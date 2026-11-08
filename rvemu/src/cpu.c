/* RV32IMA interpreter with Sv32 MMU, S/U privilege and a built-in SBI.
 *
 * There is no M-mode: the kernel runs in S-mode and its ecalls are handled
 * here in C (timer, IPI, remote fence, HSM, reset, legacy console).
 *
 * Every guest memory access goes through the soft TLB. A miss walks the guest
 * page tables, sets A/D bits, checks permissions and installs an entry; that
 * fill path is what the block translator's slow path will call too.
 */
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev.h"

#define PTE_V 0x01u
#define PTE_R 0x02u
#define PTE_W 0x04u
#define PTE_X 0x08u
#define PTE_U 0x10u
#define PTE_A 0x40u
#define PTE_D 0x80u

#define PAGE_MASK 0xFFFu

/* ---------------------------------------------------------------- XIP model */

void xip_init(xip_cache_t *x, uint32_t size, uint32_t ways, uint32_t line)
{
    uint32_t shift = 0;
    while ((1u << shift) < line)
        shift++;
    x->line_shift = shift;
    x->ways = ways;
    x->sets = size / (line * ways);
    x->tags = malloc(sizeof(uint32_t) * x->sets * ways);
    x->mru = calloc(x->sets, 1);
    if (!x->tags || !x->mru) {
        fprintf(stderr, "xip_init: out of memory\n");
        exit(1);
    }
    memset(x->tags, 0xFF, sizeof(uint32_t) * x->sets * ways);
    x->accesses = x->misses = 0;
}

static inline void xip_access(xip_cache_t *x, uint32_t pa)
{
    uint32_t line = pa >> x->line_shift;
    uint32_t set = line & (x->sets - 1);
    uint32_t *t = &x->tags[set * x->ways];
    x->accesses++;
    for (uint32_t w = 0; w < x->ways; w++) {
        if (t[w] == line) {
            x->mru[set] = (uint8_t) w;
            return;
        }
    }
    x->misses++;
    uint32_t victim = x->ways == 1 ? 0 : (uint32_t) (x->mru[set] ^ 1u);
    t[victim] = line;
    x->mru[set] = (uint8_t) victim;
}

static inline void model_data(cpu_t *c, const uint8_t *host)
{
    if (c->model) {
        uint32_t pa = (uint32_t) (host - c->ram);
        xip_access(&c->xip_interp, pa);
        xip_access(&c->xip_jit, pa);
    }
}

/* ---------------------------------------------------------------- TLB */

void tlb_flush_all(cpu_t *c)
{
    for (int m = 0; m < MMU_N; m++)
        for (int a = 0; a < ACC_N; a++)
            for (uint32_t i = 0; i < TLB_SIZE; i++) {
                /* a page whose index bits point at a different slot */
                c->tlb_sets[m][a][i].tag = ((i + 1) & (TLB_SIZE - 1)) << 12;
                c->tlb_sets[m][a][i].addend = 0;
            }
    c->stats.tlb_flushes++;
}

/* Drop the entries for one virtual page (all contexts and access types). */
static void tlb_flush_page(cpu_t *c, uint32_t va)
{
    uint32_t slot = (va >> 12) & (TLB_SIZE - 1);
    for (int m = 0; m < MMU_N; m++)
        for (int a = 0; a < ACC_N; a++) {
            tlb_entry_t *e = &c->tlb_sets[m][a][slot];
            if (((e->tag ^ va) >> 12) == 0) {
                e->tag = ((slot + 1) & (TLB_SIZE - 1)) << 12;
                e->addend = 0;
            }
        }
    c->stats.tlb_page_flushes++;
}

static void update_mmu_idx(cpu_t *c)
{
    uint32_t m = c->priv == PRIV_U ? MMU_U : (c->sum ? MMU_S_SUM : MMU_S);
    c->mmu_idx = m;
    c->tlb = c->tlb_sets[m];
}

static inline uint8_t *tlb_lookup(cpu_t *c, uint32_t va, int acc)
{
    tlb_entry_t *t = &c->tlb[acc][(va >> 12) & (TLB_SIZE - 1)];
    if (((t->tag ^ va) >> 12) == 0)
        return (uint8_t *) (va + t->addend);
    return NULL;
}

static inline uint32_t *ram_word(cpu_t *c, uint64_t pa)
{
    if (pa >= c->ram_base && pa + 4 <= (uint64_t) c->ram_base + c->ram_size)
        return (uint32_t *) (c->ram + (pa - c->ram_base));
    return NULL;
}

/* Sv32 page walk. Returns 0 and the physical address, or an exception
 * cause. Updates A (and D for writes) in the PTE, like hardware would. */
static int translate(cpu_t *c, uint32_t va, int acc, uint32_t *pa)
{
    static const int pfault[ACC_N] = {EXC_LOAD_PFAULT, EXC_STORE_PFAULT,
                                      EXC_INSN_PFAULT};
    static const int afault[ACC_N] = {EXC_LOAD_FAULT, EXC_STORE_FAULT,
                                      EXC_INSN_FAULT};

    if (!(c->satp >> 31)) {
        *pa = va;
        return 0;
    }

    uint64_t table = (uint64_t) (c->satp & 0x3FFFFFu) << 12;
    for (int level = 1; level >= 0; level--) {
        uint32_t vpn = (va >> (12 + 10 * level)) & 0x3FFu;
        uint32_t *pte_p = ram_word(c, table + vpn * 4u);
        if (!pte_p)
            return afault[acc];
        uint32_t pte = *pte_p;
        if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W)))
            return pfault[acc];
        if (!(pte & (PTE_R | PTE_X))) { /* pointer to next level */
            table = (uint64_t) (pte >> 10) << 12;
            continue;
        }

        uint32_t ppn = pte >> 10;
        if (level == 1 && (ppn & 0x3FFu)) /* misaligned superpage */
            return pfault[acc];

        if (acc == ACC_X) {
            if (!(pte & PTE_X))
                return pfault[acc];
        } else if (acc == ACC_R) {
            if (!(pte & PTE_R) && !(c->mxr && (pte & PTE_X)))
                return pfault[acc];
        } else if (!(pte & PTE_W)) {
            return pfault[acc];
        }

        if (c->priv == PRIV_U) {
            if (!(pte & PTE_U))
                return pfault[acc];
        } else if (pte & PTE_U) {
            if (acc == ACC_X || !c->sum)
                return pfault[acc];
        }

        uint32_t updated = pte | PTE_A | (acc == ACC_W ? PTE_D : 0);
        if (updated != pte)
            *pte_p = updated;

        uint64_t p = (uint64_t) ppn << 12;
        p |= level == 1 ? (va & 0x3FFFFFu) : (va & PAGE_MASK);
        if (p >> 32)
            return afault[acc];
        *pa = (uint32_t) p;
        return 0;
    }
    return pfault[acc];
}

/* TLB miss path: translate, and if the target is RAM install an entry.
 * *host is set to the host pointer, or NULL for MMIO. */
static int tlb_fill(cpu_t *c, uint32_t va, int acc, uint32_t *pa,
                    uint8_t **host)
{
    c->stats.tlb_miss[acc]++;
    int e = translate(c, va, acc, pa);
    if (e)
        return e;
    uint32_t ppage = *pa & ~PAGE_MASK;
    if (ppage >= c->ram_base && ppage - c->ram_base < c->ram_size) {
        uint8_t *hp = c->ram + (ppage - c->ram_base);
        uint32_t vpage = va & ~PAGE_MASK;
        tlb_entry_t *t = &c->tlb[acc][(va >> 12) & (TLB_SIZE - 1)];
        t->tag = vpage;
        t->addend = (uintptr_t) hp - (uintptr_t) vpage;
        *host = hp + (va & PAGE_MASK);
    } else {
        *host = NULL;
    }
    return 0;
}

/* ---------------------------------------------------------------- memory */

static int mem_load(cpu_t *c, uint32_t va, int size, uint32_t *out)
{
    if ((va & PAGE_MASK) + (uint32_t) size <= 0x1000u) {
        uint8_t *h = tlb_lookup(c, va, ACC_R);
        if (!h) {
            uint32_t pa;
            int e = tlb_fill(c, va, ACC_R, &pa, &h);
            if (e)
                return e;
            if (!h)
                return mmio_load(c, pa, size, out);
        }
        model_data(c, h);
        switch (size) {
        case 1:
            *out = *h;
            break;
        case 2: {
            uint16_t v;
            memcpy(&v, h, 2);
            *out = v;
            break;
        }
        default:
            memcpy(out, h, 4);
            break;
        }
        return 0;
    }
    /* misaligned access crossing a page boundary */
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        uint32_t b;
        int e = mem_load(c, va + (uint32_t) i, 1, &b);
        if (e)
            return e;
        v |= b << (8 * i);
    }
    *out = v;
    return 0;
}

static int mem_store(cpu_t *c, uint32_t va, int size, uint32_t val)
{
    if ((va & PAGE_MASK) + (uint32_t) size <= 0x1000u) {
        uint8_t *h = tlb_lookup(c, va, ACC_W);
        if (!h) {
            uint32_t pa;
            int e = tlb_fill(c, va, ACC_W, &pa, &h);
            if (e)
                return e;
            if (!h)
                return mmio_store(c, pa, size, val);
        }
        model_data(c, h);
        memcpy(h, &val, (size_t) size); /* host is little-endian */
        return 0;
    }
    for (int i = 0; i < size; i++) {
        int e = mem_store(c, va + (uint32_t) i, 1, (val >> (8 * i)) & 0xFF);
        if (e)
            return e;
    }
    return 0;
}

/* Host pointer for an AMO (needs write permission; MMIO not supported). */
static int amo_ptr(cpu_t *c, uint32_t va, uint8_t **h)
{
    *h = tlb_lookup(c, va, ACC_W);
    if (*h)
        return 0;
    uint32_t pa;
    int e = tlb_fill(c, va, ACC_W, &pa, h);
    if (e)
        return e;
    return *h ? 0 : EXC_STORE_FAULT;
}

static int fetch(cpu_t *c, uint32_t pc, uint32_t *insn, uint8_t **host)
{
    uint8_t *h = tlb_lookup(c, pc, ACC_X);
    if (!h) {
        uint32_t pa;
        int e = tlb_fill(c, pc, ACC_X, &pa, &h);
        if (e)
            return e;
        if (!h)
            return EXC_INSN_FAULT;
    }
    memcpy(insn, h, 4);
    *host = h;
    return 0;
}

/* ---------------------------------------------------------------- traps */

void cpu_trap(cpu_t *c, uint32_t cause, uint32_t tval)
{
    if (cause & CAUSE_IRQ)
        c->stats.interrupts++;
    else
        c->stats.exceptions++;
    c->scause = cause;
    c->stval = tval;
    c->sepc = c->pc;
    c->spp = c->priv == PRIV_S;
    c->spie = c->sie_bit;
    c->sie_bit = false;
    c->priv = PRIV_S;
    update_mmu_idx(c);
    c->res_valid = false;
    uint32_t base = c->stvec & ~3u;
    if ((c->stvec & 1) && (cause & CAUSE_IRQ))
        c->pc = base + 4 * (cause & ~CAUSE_IRQ);
    else
        c->pc = base;
}

bool cpu_check_irq(cpu_t *c)
{
    if (cpu_time(c) >= c->stimecmp)
        c->sip |= MIP_STIP;
    else
        c->sip &= ~MIP_STIP;

    uint32_t p = c->sip & c->sie;
    if (!p || (c->priv == PRIV_S && !c->sie_bit))
        return false;
    /* priority: SEI > SSI > STI */
    uint32_t irq = (p & MIP_SEIP) ? IRQ_SEI : (p & MIP_SSIP) ? IRQ_SSI : IRQ_STI;
    cpu_trap(c, CAUSE_IRQ | irq, 0);
    return true;
}

static void do_sret(cpu_t *c)
{
    c->priv = c->spp ? PRIV_S : PRIV_U;
    c->sie_bit = c->spie;
    c->spie = true;
    c->spp = false;
    update_mmu_idx(c);
    if (c->priv == PRIV_U && !c->stats.user_instret) {
        c->stats.user_instret = c->instret + 1;
        c->stats.user_unique_insns = c->stats.unique_insns;
    }
}

/* ---------------------------------------------------------------- CSRs */

#define SSTATUS_SIE (1u << 1)
#define SSTATUS_SPIE (1u << 5)
#define SSTATUS_SPP (1u << 8)
#define SSTATUS_SUM (1u << 18)
#define SSTATUS_MXR (1u << 19)
#define SIE_MASK (MIP_SSIP | MIP_STIP | MIP_SEIP)

static int csr_read(cpu_t *c, uint32_t csr, uint32_t *v)
{
    switch (csr) {
    case 0xC00: /* cycle */
    case 0xC02: /* instret */
        *v = (uint32_t) c->instret;
        return 0;
    case 0xC80:
    case 0xC82:
        *v = (uint32_t) (c->instret >> 32);
        return 0;
    case 0xC01: /* time */
        *v = (uint32_t) cpu_time(c);
        return 0;
    case 0xC81:
        *v = (uint32_t) (cpu_time(c) >> 32);
        return 0;
    default:
        break;
    }
    if (c->priv != PRIV_S)
        return EXC_ILLEGAL;
    switch (csr) {
    case 0x100:
        *v = (c->sie_bit ? SSTATUS_SIE : 0) | (c->spie ? SSTATUS_SPIE : 0) |
             (c->spp ? SSTATUS_SPP : 0) | (c->sum ? SSTATUS_SUM : 0) |
             (c->mxr ? SSTATUS_MXR : 0);
        return 0;
    case 0x104:
        *v = c->sie;
        return 0;
    case 0x105:
        *v = c->stvec;
        return 0;
    case 0x106:
        *v = c->scounteren;
        return 0;
    case 0x140:
        *v = c->sscratch;
        return 0;
    case 0x141:
        *v = c->sepc;
        return 0;
    case 0x142:
        *v = c->scause;
        return 0;
    case 0x143:
        *v = c->stval;
        return 0;
    case 0x144:
        *v = c->sip;
        return 0;
    case 0x180:
        *v = c->satp;
        return 0;
    default:
        return EXC_ILLEGAL;
    }
}

static int csr_write(cpu_t *c, uint32_t csr, uint32_t v)
{
    if (c->priv != PRIV_S || (csr >> 10) == 3) /* 0xC00-0xFFF read-only */
        return EXC_ILLEGAL;
    switch (csr) {
    case 0x100: {
        bool old_mxr = c->mxr;
        c->sie_bit = v & SSTATUS_SIE;
        c->spie = v & SSTATUS_SPIE;
        c->spp = v & SSTATUS_SPP;
        c->sum = v & SSTATUS_SUM;
        c->mxr = v & SSTATUS_MXR;
        if (c->mxr != old_mxr)
            tlb_flush_all(c);
        update_mmu_idx(c);
        return 0;
    }
    case 0x104:
        c->sie = v & SIE_MASK;
        return 0;
    case 0x105:
        c->stvec = v & ~2u;
        return 0;
    case 0x106:
        c->scounteren = v;
        return 0;
    case 0x140:
        c->sscratch = v;
        return 0;
    case 0x141:
        c->sepc = v & ~3u;
        return 0;
    case 0x142:
        c->scause = v;
        return 0;
    case 0x143:
        c->stval = v;
        return 0;
    case 0x144: /* only SSIP is software-writable */
        c->sip = (c->sip & ~MIP_SSIP) | (v & MIP_SSIP);
        return 0;
    case 0x180:
        /* Sv32 or bare; no ASID bits implemented (reads back as 0) */
        c->satp = (v >> 31) ? (v & ~(0x1FFu << 22)) : 0;
        tlb_flush_all(c);
        return 0;
    default:
        return EXC_ILLEGAL;
    }
}

/* ---------------------------------------------------------------- SBI */

#define SBI_ERR_FAILED (-1)
#define SBI_ERR_NOT_SUPPORTED (-2)
#define SBI_ERR_INVALID_PARAM (-3)
#define SBI_ERR_ALREADY_AVAILABLE (-6)

#define SBI_EXT_BASE 0x10u
#define SBI_EXT_TIME 0x54494D45u
#define SBI_EXT_IPI 0x735049u
#define SBI_EXT_RFENCE 0x52464E43u
#define SBI_EXT_HSM 0x48534Du
#define SBI_EXT_SRST 0x53525354u

static bool hart0_in_mask(uint32_t mask, uint32_t base)
{
    return base == 0xFFFFFFFFu || (base == 0 && (mask & 1));
}

static void sbi_call(cpu_t *c)
{
    uint32_t eid = c->x[17], fid = c->x[16];
    int32_t err = 0;
    uint32_t val = 0;
    c->stats.sbi_calls++;

    switch (eid) {
    /* legacy v0.1 calls return a single value in a0 */
    case 0x00:
        c->stimecmp = ((uint64_t) c->x[11] << 32) | c->x[10];
        c->x[10] = 0;
        return;
    case 0x01:
        console_putc(c, (uint8_t) c->x[10]);
        c->x[10] = 0;
        return;
    case 0x02:
        c->x[10] = (uint32_t) console_getc(c);
        return;
    case 0x08:
        c->halt = HALT_SHUTDOWN;
        c->x[10] = 0;
        return;

    case SBI_EXT_BASE:
        switch (fid) {
        case 0: val = 0x02000000u; break; /* spec v2.0 */
        case 1: val = 0x52503233u; break; /* impl id: "RP23" */
        case 2: val = 1; break;
        case 3: {
            uint32_t e = c->x[10];
            val = e == SBI_EXT_BASE || e == SBI_EXT_TIME || e == SBI_EXT_IPI ||
                  e == SBI_EXT_RFENCE || e == SBI_EXT_HSM || e == SBI_EXT_SRST;
            break;
        }
        case 4: case 5: case 6: val = 0; break;
        default: err = SBI_ERR_NOT_SUPPORTED; break;
        }
        break;
    case SBI_EXT_TIME:
        if (fid == 0)
            c->stimecmp = ((uint64_t) c->x[11] << 32) | c->x[10];
        else
            err = SBI_ERR_NOT_SUPPORTED;
        break;
    case SBI_EXT_IPI:
        if (fid == 0) {
            if (hart0_in_mask(c->x[10], c->x[11]))
                c->sip |= MIP_SSIP;
        } else {
            err = SBI_ERR_NOT_SUPPORTED;
        }
        break;
    case SBI_EXT_RFENCE:
        switch (fid) {
        case 0: break; /* remote fence.i: nothing cached yet */
        case 1: case 2: tlb_flush_all(c); break;
        default: err = SBI_ERR_NOT_SUPPORTED; break;
        }
        break;
    case SBI_EXT_HSM:
        switch (fid) {
        case 0: err = c->x[10] == 0 ? SBI_ERR_ALREADY_AVAILABLE
                                    : SBI_ERR_INVALID_PARAM; break;
        case 2: if (c->x[10] == 0) val = 0; /* STARTED */
                else err = SBI_ERR_INVALID_PARAM; break;
        default: err = SBI_ERR_NOT_SUPPORTED; break;
        }
        break;
    case SBI_EXT_SRST:
        if (fid == 0)
            c->halt = HALT_SHUTDOWN;
        else
            err = SBI_ERR_NOT_SUPPORTED;
        break;
    default:
        err = SBI_ERR_NOT_SUPPORTED;
        break;
    }
    c->x[10] = (uint32_t) err;
    c->x[11] = val;
}

/* ---------------------------------------------------------------- execute */

void cpu_init(cpu_t *c, uint8_t *ram, uint32_t ram_base, uint32_t ram_size,
              uint32_t entry, uint32_t dtb_addr)
{
    memset(c, 0, sizeof(*c));
    c->ram = ram;
    c->ram_base = ram_base;
    c->ram_size = ram_size;
    c->priv = PRIV_S;
    c->pc = entry;
    c->x[10] = 0;        /* hart id */
    c->x[11] = dtb_addr; /* device tree */
    c->stimecmp = UINT64_MAX;
    c->code_bits = calloc(ram_size / 32, 1);
    c->page_bits = calloc(ram_size / 4096 / 8 + 1, 1);
    if (!c->code_bits || !c->page_bits) {
        fprintf(stderr, "cpu_init: out of memory\n");
        exit(1);
    }
    update_mmu_idx(c);
    tlb_flush_all(c);
    c->stats.tlb_flushes = 0;
}

static inline void mark_code(cpu_t *c, const uint8_t *h)
{
    size_t off = (size_t) (h - c->ram);
    size_t w = off >> 2;
    uint8_t bit = (uint8_t) (1u << (w & 7));
    if (!(c->code_bits[w >> 3] & bit)) {
        c->code_bits[w >> 3] |= bit;
        c->stats.unique_insns++;
        size_t p = off >> 12;
        uint8_t pbit = (uint8_t) (1u << (p & 7));
        if (!(c->page_bits[p >> 3] & pbit)) {
            c->page_bits[p >> 3] |= pbit;
            c->stats.unique_code_pages++;
        }
    }
    if (c->model)
        xip_access(&c->xip_interp, (uint32_t) off);
}

static inline int32_t imm_i(uint32_t insn) { return (int32_t) insn >> 20; }
static inline int32_t imm_s(uint32_t insn)
{
    return ((int32_t) insn >> 25 << 5) | (int32_t) ((insn >> 7) & 0x1F);
}
static inline int32_t imm_b(uint32_t insn)
{
    return ((int32_t) insn >> 31 << 12) | (int32_t) ((insn << 4) & 0x800) |
           (int32_t) ((insn >> 20) & 0x7E0) | (int32_t) ((insn >> 7) & 0x1E);
}
static inline int32_t imm_j(uint32_t insn)
{
    return ((int32_t) insn >> 31 << 20) | (int32_t) (insn & 0xFF000) |
           (int32_t) ((insn >> 9) & 0x800) | (int32_t) ((insn >> 20) & 0x7FE);
}

void cpu_step(cpu_t *c)
{
    uint32_t insn;
    uint8_t *ih;
    int e = fetch(c, c->pc, &insn, &ih);
    if (e) {
        cpu_trap(c, (uint32_t) e, c->pc);
        return;
    }
    mark_code(c, ih);

    const uint32_t opcode = insn & 0x7F;
    const uint32_t rd = (insn >> 7) & 31;
    const uint32_t rs1 = (insn >> 15) & 31;
    const uint32_t rs2 = (insn >> 20) & 31;
    const uint32_t f3 = (insn >> 12) & 7;
    const uint32_t f7 = insn >> 25;
    const uint32_t a = c->x[rs1], b = c->x[rs2];
    uint32_t next = c->pc + 4;
    uint32_t val = 0;
    bool wb = true;

    switch (opcode) {
    case 0x37: /* LUI */
        val = insn & 0xFFFFF000u;
        break;
    case 0x17: /* AUIPC */
        val = c->pc + (insn & 0xFFFFF000u);
        break;
    case 0x6F: /* JAL */
        val = next;
        next = c->pc + (uint32_t) imm_j(insn);
        break;
    case 0x67: /* JALR */
        if (f3)
            goto illegal;
        val = next;
        next = (a + (uint32_t) imm_i(insn)) & ~1u;
        break;
    case 0x63: { /* BRANCH */
        bool t;
        switch (f3) {
        case 0: t = a == b; break;
        case 1: t = a != b; break;
        case 4: t = (int32_t) a < (int32_t) b; break;
        case 5: t = (int32_t) a >= (int32_t) b; break;
        case 6: t = a < b; break;
        case 7: t = a >= b; break;
        default: goto illegal;
        }
        if (t)
            next = c->pc + (uint32_t) imm_b(insn);
        wb = false;
        break;
    }
    case 0x03: { /* LOAD */
        uint32_t va = a + (uint32_t) imm_i(insn), v;
        int size;
        switch (f3) {
        case 0: case 4: size = 1; break;
        case 1: case 5: size = 2; break;
        case 2: size = 4; break;
        default: goto illegal;
        }
        e = mem_load(c, va, size, &v);
        if (e) {
            cpu_trap(c, (uint32_t) e, va);
            return;
        }
        switch (f3) {
        case 0: val = (uint32_t) (int8_t) v; break;
        case 1: val = (uint32_t) (int16_t) v; break;
        default: val = v; break;
        }
        break;
    }
    case 0x23: { /* STORE */
        uint32_t va = a + (uint32_t) imm_s(insn);
        if (f3 > 2)
            goto illegal;
        e = mem_store(c, va, 1 << f3, b);
        if (e) {
            cpu_trap(c, (uint32_t) e, va);
            return;
        }
        wb = false;
        break;
    }
    case 0x13: { /* OP-IMM */
        uint32_t imm = (uint32_t) imm_i(insn);
        uint32_t sh = rs2; /* shamt */
        switch (f3) {
        case 0: val = a + imm; break;
        case 1: if (f7) goto illegal; val = a << sh; break;
        case 2: val = (int32_t) a < (int32_t) imm; break;
        case 3: val = a < imm; break;
        case 4: val = a ^ imm; break;
        case 5:
            if (f7 == 0) val = a >> sh;
            else if (f7 == 0x20) val = (uint32_t) ((int32_t) a >> sh);
            else goto illegal;
            break;
        case 6: val = a | imm; break;
        default: val = a & imm; break;
        }
        break;
    }
    case 0x33: /* OP */
        if (f7 == 1) {
            switch (f3) {
            case 0: val = a * b; break;
            case 1: val = (uint32_t) (((int64_t) (int32_t) a * (int32_t) b) >> 32); break;
            case 2: val = (uint32_t) (((int64_t) (int32_t) a * (int64_t) (uint64_t) b) >> 32); break;
            case 3: val = (uint32_t) (((uint64_t) a * b) >> 32); break;
            case 4:
                if (b == 0) val = 0xFFFFFFFFu;
                else if (a == 0x80000000u && b == 0xFFFFFFFFu) val = a;
                else val = (uint32_t) ((int32_t) a / (int32_t) b);
                break;
            case 5: val = b ? a / b : 0xFFFFFFFFu; break;
            case 6:
                if (b == 0) val = a;
                else if (a == 0x80000000u && b == 0xFFFFFFFFu) val = 0;
                else val = (uint32_t) ((int32_t) a % (int32_t) b);
                break;
            default: val = b ? a % b : a; break;
            }
            break;
        }
        if (f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5)))
            goto illegal;
        switch (f3) {
        case 0: val = f7 ? a - b : a + b; break;
        case 1: val = a << (b & 31); break;
        case 2: val = (int32_t) a < (int32_t) b; break;
        case 3: val = a < b; break;
        case 4: val = a ^ b; break;
        case 5: val = f7 ? (uint32_t) ((int32_t) a >> (b & 31)) : a >> (b & 31); break;
        case 6: val = a | b; break;
        default: val = a & b; break;
        }
        break;
    case 0x0F: /* FENCE / FENCE.I: no-ops for a single in-order hart */
        if (f3 > 1)
            goto illegal;
        wb = false;
        break;
    case 0x73: /* SYSTEM */
        if (f3 == 0) {
            wb = false;
            if (insn == 0x00000073u) { /* ECALL */
                if (c->priv == PRIV_U) {
                    cpu_trap(c, EXC_ECALL_U, 0);
                    return;
                }
                sbi_call(c);
            } else if (insn == 0x00100073u) { /* EBREAK */
                cpu_trap(c, EXC_BREAKPOINT, c->pc);
                return;
            } else if (insn == 0x10200073u) { /* SRET */
                if (c->priv != PRIV_S)
                    goto illegal;
                next = c->sepc;
                do_sret(c);
            } else if (insn == 0x10500073u) { /* WFI */
                if (c->priv != PRIV_S)
                    goto illegal;
                c->stats.wfi++;
                dev_wfi(c);
            } else if (f7 == 0x09 && rd == 0) { /* SFENCE.VMA */
                if (c->priv != PRIV_S)
                    goto illegal;
                if (rs1 == 0)
                    tlb_flush_all(c);
                else
                    tlb_flush_page(c, a);
            } else {
                goto illegal;
            }
        } else if (f3 != 4) {
            uint32_t csr = insn >> 20;
            uint32_t src = (f3 & 4) ? rs1 : a;
            uint32_t op = f3 & 3, old = 0;
            if (!(op == 1 && rd == 0)) {
                if (csr_read(c, csr, &old))
                    goto illegal;
            }
            if (op == 1 || rs1 != 0) {
                uint32_t nv = op == 1 ? src : op == 2 ? (old | src) : (old & ~src);
                if (csr_write(c, csr, nv))
                    goto illegal;
            }
            val = old;
        } else {
            goto illegal;
        }
        break;
    case 0x2F: { /* AMO */
        uint32_t f5 = insn >> 27;
        if (f3 != 2)
            goto illegal;
        if (a & 3) {
            cpu_trap(c, f5 == 2 ? EXC_LOAD_MISALIGN : EXC_STORE_MISALIGN, a);
            return;
        }
        if (f5 == 2) { /* LR.W */
            if (rs2)
                goto illegal;
            e = mem_load(c, a, 4, &val);
            if (e) {
                cpu_trap(c, (uint32_t) e, a);
                return;
            }
            c->res_valid = true;
            c->res_addr = a;
        } else if (f5 == 3) { /* SC.W */
            if (c->res_valid && c->res_addr == a) {
                e = mem_store(c, a, 4, b);
                if (e) {
                    cpu_trap(c, (uint32_t) e, a);
                    return;
                }
                val = 0;
            } else {
                val = 1;
            }
            c->res_valid = false;
        } else {
            uint8_t *h;
            e = amo_ptr(c, a, &h);
            if (e) {
                cpu_trap(c, (uint32_t) e, a);
                return;
            }
            model_data(c, h);
            uint32_t old, nv;
            memcpy(&old, h, 4);
            switch (f5) {
            case 0x00: nv = old + b; break;
            case 0x01: nv = b; break;
            case 0x04: nv = old ^ b; break;
            case 0x08: nv = old | b; break;
            case 0x0C: nv = old & b; break;
            case 0x10: nv = (int32_t) old < (int32_t) b ? old : b; break;
            case 0x14: nv = (int32_t) old > (int32_t) b ? old : b; break;
            case 0x18: nv = old < b ? old : b; break;
            case 0x1C: nv = old > b ? old : b; break;
            default: goto illegal;
            }
            memcpy(h, &nv, 4);
            val = old;
        }
        break;
    }
    default:
        goto illegal;
    }

    if (next & 3) {
        cpu_trap(c, EXC_INSN_MISALIGN, next);
        return;
    }
    if (wb && rd)
        c->x[rd] = val;
    c->pc = next;
    c->instret++;
    c->stats.op[opcode]++;
    return;

illegal:
    cpu_trap(c, EXC_ILLEGAL, insn);
}
