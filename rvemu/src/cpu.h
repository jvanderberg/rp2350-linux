/* RV32IMA + Sv32 guest CPU state shared by the interpreter and (later) the
 * block translator. Layout notes:
 *   - x[] and pc first so generated code can reach them with small offsets.
 *   - The soft TLB is split by access type (R/W/X) and by permission context
 *     (U, S, S+SUM), so privilege changes and SUM toggles never need a flush.
 *   - A TLB entry maps a guest virtual page to host memory: host = va + addend.
 *     An empty slot holds a page number that cannot index that slot, so the
 *     hit test is a single xor/shift/compare with no separate valid bit.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Guest timebase. Time is derived from retired instructions (1 tick per
 * instruction, plus ticks skipped while idle in WFI), which makes every run
 * deterministic: same inputs, same instruction trace. */
#define TIMEBASE_HZ 25000000u

#define TLB_BITS 8
#define TLB_SIZE (1u << TLB_BITS)

enum { ACC_R, ACC_W, ACC_X, ACC_N };
enum { MMU_U, MMU_S, MMU_S_SUM, MMU_N };
enum { PRIV_U = 0, PRIV_S = 1 };

#define EXC_INSN_MISALIGN 0
#define EXC_INSN_FAULT 1
#define EXC_ILLEGAL 2
#define EXC_BREAKPOINT 3
#define EXC_LOAD_MISALIGN 4
#define EXC_LOAD_FAULT 5
#define EXC_STORE_MISALIGN 6
#define EXC_STORE_FAULT 7
#define EXC_ECALL_U 8
#define EXC_ECALL_S 9
#define EXC_INSN_PFAULT 12
#define EXC_LOAD_PFAULT 13
#define EXC_STORE_PFAULT 15
#define CAUSE_IRQ 0x80000000u

#define IRQ_SSI 1
#define IRQ_STI 5
#define IRQ_SEI 9
#define MIP_SSIP (1u << IRQ_SSI)
#define MIP_STIP (1u << IRQ_STI)
#define MIP_SEIP (1u << IRQ_SEI)

enum {
    HALT_NONE,
    HALT_SHUTDOWN,
    HALT_UNTIL,
    HALT_MAX_INSNS,
    HALT_IDLE,
    HALT_USER,
    HALT_FATAL,
};

typedef struct {
    uint32_t tag;      /* page-aligned guest virtual address */
    uintptr_t addend;  /* host pointer = guest va + addend */
} tlb_entry_t;

/* Simple model of the RP2350 XIP cache (which fronts PSRAM), used only to
 * estimate PSRAM miss counts for the timing model. */
typedef struct {
    uint32_t sets, ways, line_shift;
    uint32_t *tags;
    uint8_t *mru;
    uint64_t accesses, misses;
} xip_cache_t;

typedef struct {
    uint64_t op[128]; /* retired instructions by major opcode */
    uint64_t tlb_miss[ACC_N];
    uint64_t tlb_flushes, tlb_page_flushes, exceptions, interrupts, sbi_calls, wfi, idle_ticks;
    uint64_t unique_insns, unique_code_pages;
    /* snapshots at milestones */
    uint64_t user_instret, user_unique_insns; /* first sret into U-mode */
    uint64_t until_instret, until_unique_insns; /* --until string seen */
} stats_t;

typedef struct cpu {
    uint32_t x[32];
    uint32_t pc;
    uint32_t priv;
    tlb_entry_t (*tlb)[TLB_SIZE]; /* = tlb_sets[mmu_idx]; index [ACC_*][slot] */
    uint32_t mmu_idx;

    /* supervisor state */
    bool sie_bit, spie, spp, sum, mxr;
    uint32_t sie, sip, stvec, sscratch, sepc, scause, stval, satp, scounteren;
    uint64_t stimecmp;
    uint64_t instret, time_skip;
    bool res_valid;
    uint32_t res_addr;

    /* guest physical RAM */
    uint8_t *ram;
    uint32_t ram_base, ram_size;

    int halt;

    /* instrumentation */
    uint8_t *code_bits; /* 1 bit per RAM word: executed at least once */
    uint8_t *page_bits; /* 1 bit per RAM page: contains executed code */
    bool model;
    xip_cache_t xip_interp; /* sees guest fetches + data (interpreter) */
    xip_cache_t xip_jit;    /* sees guest data only (translated code in SRAM) */
    stats_t stats;

    tlb_entry_t tlb_sets[MMU_N][ACC_N][TLB_SIZE];
} cpu_t;

void cpu_init(cpu_t *c, uint8_t *ram, uint32_t ram_base, uint32_t ram_size,
              uint32_t entry, uint32_t dtb_addr);
bool cpu_check_irq(cpu_t *c);
void cpu_step(cpu_t *c);
void cpu_trap(cpu_t *c, uint32_t cause, uint32_t tval);
void tlb_flush_all(cpu_t *c);
void xip_init(xip_cache_t *x, uint32_t size, uint32_t ways, uint32_t line);

static inline uint64_t cpu_time(const cpu_t *c)
{
    return c->instret + c->time_skip;
}
