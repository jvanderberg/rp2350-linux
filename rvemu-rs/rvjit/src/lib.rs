//! Block translator: RV32IMA guest code -> RV32 host code (Hazard3).
//!
//! Guest register xN lives in host register xN, except six host registers
//! the translator reserves; the guest values of those live in `cpu.x[]`:
//!   x3  S   pointer to the `Cpu`
//!   x4  K0  scratch: guest address / host address / exit pc
//!   x7  K1  scratch / exit info
//!   x22 KB  instruction budget (`cpu.jit.budget` while in translated code)
//!   x23 K2  scratch
//!   x24 K3  link register for the TLB helpers
//! (the six least-used guest registers in a boot, ~2% of operand uses).
//!
//! A block is `pc0; jal K1, budget_exit; addi KB, KB, -N; blt KB, zero, -8`
//! and then its body; it is entered at the `addi`. The dispatcher sets the
//! budget to the instructions left before the next timer deadline, which
//! keeps `instret` exact and makes interrupt timing identical to the
//! interpreter's. Anything other than ALU / M / loads / stores / branches /
//! jumps ends the block and is run by the interpreter (`Cpu::step`).
//!
//! Loads and stores call a shared soft-TLB helper (`jal K3`) that returns
//! the host address in K0. A miss, MMIO, a fault or a misaligned access
//! exits with the call site, and the interpreter executes that one
//! instruction: the dispatcher finds the block from the call site (the
//! `jal K1, budget_exit` before its entry; data words in a block are guest
//! pcs, 4-aligned, so they never look like an instruction) and which
//! instruction it was (counting `jal K3` sites).
//!
//! Blocks are keyed by (virtual pc, physical pc) and never cross a page.
//! Exits to the same page are chained by patching a `jal`. Other jumps go
//! through an inline jump cache, validated against the fetch TLB, before
//! falling back to the dispatcher. Pages holding translated code never get
//! write-TLB entries, so a store to one reaches the interpreter and the
//! page's blocks are dropped.
//!
//! When the code buffer is full it is either flushed (`regions` = 1) or used
//! as a ring of `regions` equal parts: blocks fill one region after another,
//! and moving on to the next region evicts the blocks it holds, the oldest.
//! Each block's descriptor (hash-chain link, physical and virtual pc) is a
//! header in the code buffer in front of its code, so the buffer has no
//! separate block limit. A chain from a block into
//! an older region is logged in that region's `chain_log` and reset to a
//! chain request when the region is evicted; chains into the same or a newer
//! region need nothing, because the source is evicted first.
//!
//! Optionally every translated block is also appended to a backing store
//! (`JitMem::store`, e.g. PSRAM): a block missing from the code buffer is
//! then copied back from there instead of translated again. Stored blocks
//! keep the words as emitted plus their original position; restoring
//! re-points the `jal`s into the shared routines. The log is a ring: when
//! full it overwrites its oldest entries, and an index slot is live while
//! its entry is less than one lap behind the write position. A store to a
//! code page bumps that page's epoch, which retires its stored blocks.
#![no_std]

pub mod emit;

use core::mem::{MaybeUninit, offset_of};
use emit::*;
use rvcore::{ACC_R, ACC_W, ACC_X, Bus, Cpu, JitRegs, TlbEntry};

pub const S: Reg = 3;
pub const K0: Reg = 4;
pub const K1: Reg = 7;
pub const K2: Reg = 23;
pub const K3: Reg = 24;
pub const KB: Reg = 22;

#[inline(always)]
pub const fn spilled(r: u32) -> bool {
    r == S || r == K0 || r == K1 || r == K2 || r == K3 || r == KB
}

/// Guest registers held in host registers while in translated code.
const GUEST_REGS: [Reg; 25] = [
    1, 2, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 25, 26, 27, 28, 29, 30, 31,
];
/// Host registers the entry trampoline must preserve for the Rust caller
/// (ra, callee-saved s0-s11, and gp/tp which we repurpose).
const SAVED: [Reg; 15] = [1, 3, 4, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27];
const FRAME: i32 = 64;

pub const EXIT_JUMP: u32 = 0; // info = chain slot address, or 0
pub const EXIT_INTERP: u32 = 1; // interpret the instruction at exit_pc
pub const EXIT_BUDGET: u32 = 2; // budget exhausted before the block ran
pub const EXIT_MEM: u32 = 3; // info = 3 + the return address of a `jal K3` site

/// A block in the code buffer: a header of `HDR_NEXT` (the next block in its
/// hash chain, as the word index of its header, or EMPTY), `HDR_PA` (its
/// physical pc) and `HDR_VA` (its virtual pc), then `jal K1, budget_exit`,
/// then the entry point (`ENTRY_OFF`).
const HDR_NEXT: usize = 0;
const HDR_PA: usize = 1;
const HDR_VA: usize = 2;
const ENTRY_OFF: usize = 4;
const HASH_MUL: u32 = 0x9E37_79B1;

pub const MAX_BLOCK: usize = 32;
/// Most code-buffer regions (`JitMem::regions`).
pub const MAX_REGIONS: usize = 16;
const BUDGET_CAP: u64 = 1 << 20;
const NO_CODE: u32 = u32::MAX;
const EMPTY: u32 = u32::MAX;

/// Jump cache entry: guest va, fetch-TLB addend at fill time, host code.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct JcEntry {
    pub va: u32,
    pub addend: u32,
    pub code: u32,
    pub _pad: u32,
}
impl JcEntry {
    pub const EMPTY: JcEntry = JcEntry { va: 1, addend: 0, code: 0, _pad: 0 };
}

const fn xoff(r: u32) -> i32 {
    (offset_of!(Cpu, x) + 4 * r as usize) as i32
}
const OFF_BUDGET: i32 = (offset_of!(Cpu, jit) + offset_of!(JitRegs, budget)) as i32;
const OFF_EXIT_PC: i32 = (offset_of!(Cpu, jit) + offset_of!(JitRegs, exit_pc)) as i32;
const OFF_EXIT_INFO: i32 = (offset_of!(Cpu, jit) + offset_of!(JitRegs, exit_info)) as i32;
const OFF_HOST_SP: i32 = (offset_of!(Cpu, jit) + offset_of!(JitRegs, host_sp)) as i32;
const OFF_JC: i32 = (offset_of!(Cpu, jit) + offset_of!(JitRegs, jc_base)) as i32;
const fn off_tlb_cur(acc: usize) -> i32 {
    (offset_of!(Cpu, jit) + offset_of!(JitRegs, tlb_cur) + acc * core::mem::size_of::<usize>()) as i32
}
const OFF_ADDEND: i32 = offset_of!(TlbEntry, addend) as i32;

#[derive(Clone, Copy)]
pub struct JitStats {
    pub blocks: u64,
    pub guest_insns_translated: u64,
    pub code_words: u64,
    pub entries: u64,
    pub jit_insns: u64,
    pub interp_steps: u64,
    pub exit_jump: u64,
    pub exit_interp: u64,
    pub exit_budget: u64,
    pub chains: u64,
    pub flushes: u64,
    /// code-buffer regions evicted (ring mode)
    pub evictions: u64,
    pub smc_invalidations: u64,
    /// blocks copied back from the backing store, and their words
    pub restores: u64,
    pub restore_words: u64,
    /// laps of the backing store's log
    pub store_resets: u64,
    /// Interpreted instructions by major opcode.
    pub interp_op: [u64; 128],
    /// Host instructions spent (riscv32 hosts; minstret based).
    pub h_translate: u64,
    pub h_interp: u64,
    pub h_code: u64,
    pub h_total: u64,
}

/// Host retired-instruction counter (0 elsewhere).
#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
pub fn host_instret() -> u64 {
    #[cfg(target_arch = "riscv32")]
    {
        let (lo, hi): (u32, u32);
        unsafe { core::arch::asm!("csrr {0}, minstret", "csrr {1}, minstreth", out(reg) lo, out(reg) hi) };
        ((hi as u64) << 32) | lo as u64
    }
    #[cfg(not(target_arch = "riscv32"))]
    0
}

impl Default for JitStats {
    fn default() -> Self {
        JitStats {
            blocks: 0,
            guest_insns_translated: 0,
            code_words: 0,
            entries: 0,
            jit_insns: 0,
            interp_steps: 0,
            exit_jump: 0,
            exit_interp: 0,
            exit_budget: 0,
            chains: 0,
            flushes: 0,
            evictions: 0,
            smc_invalidations: 0,
            restores: 0,
            restore_words: 0,
            store_resets: 0,
            interp_op: [0; 128],
            h_translate: 0,
            h_interp: 0,
            h_code: 0,
            h_total: 0,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Alu { rs1: bool, rs2: bool },
    Auipc,
    Load,
    Store,
    Branch,
    Jal,
    Jalr,
    /// FENCE / FENCE.I: nothing to do for one in-order hart (stores to
    /// translated pages are caught by the write-TLB refusal).
    Nop,
    /// AMO*.W except LR/SC: done with the host AMO on the host address.
    Amo,
    Interp,
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_i(insn: u32) -> i32 {
    (insn as i32) >> 20
}
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_s(insn: u32) -> i32 {
    (((insn as i32) >> 25) << 5) | ((insn >> 7) & 0x1F) as i32
}
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_b(insn: u32) -> i32 {
    (((insn as i32) >> 31) << 12)
        | ((insn << 4) & 0x800) as i32
        | ((insn >> 20) & 0x7E0) as i32
        | ((insn >> 7) & 0x1E) as i32
}
/// Byte offset of a `jal`.
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn jal_off(insn: u32) -> i32 {
    imm_j(insn)
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_j(insn: u32) -> i32 {
    (((insn as i32) >> 31) << 20)
        | (insn & 0xF_F000) as i32
        | ((insn >> 9) & 0x800) as i32
        | ((insn >> 20) & 0x7FE) as i32
}

/// Which instructions the translator handles itself. Encodings are checked
/// as strictly as the interpreter does, so anything copied verbatim is a
/// valid RV32IM instruction with identical semantics on the host.
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn classify(insn: u32, pc: u32) -> Kind {
    let f3 = (insn >> 12) & 7;
    let f7 = insn >> 25;
    match insn & 0x7F {
        0x37 => Kind::Alu { rs1: false, rs2: false },
        0x17 => Kind::Auipc,
        0x13 => match f3 {
            1 if f7 != 0 => Kind::Interp,
            5 if f7 != 0 && f7 != 0x20 => Kind::Interp,
            _ => Kind::Alu { rs1: true, rs2: false },
        },
        0x33 => {
            if f7 == 0 || f7 == 1 || (f7 == 0x20 && (f3 == 0 || f3 == 5)) {
                Kind::Alu { rs1: true, rs2: true }
            } else {
                Kind::Interp
            }
        }
        0x03 if matches!(f3, 0 | 1 | 2 | 4 | 5) => Kind::Load,
        0x23 if f3 <= 2 => Kind::Store,
        0x63 if !matches!(f3, 2 | 3) => {
            if pc.wrapping_add(imm_b(insn) as u32) & 3 != 0 { Kind::Interp } else { Kind::Branch }
        }
        0x6F => {
            if pc.wrapping_add(imm_j(insn) as u32) & 3 != 0 { Kind::Interp } else { Kind::Jal }
        }
        0x67 if f3 == 0 => Kind::Jalr,
        0x0F if f3 <= 1 => Kind::Nop,
        0x2F if f3 == 2 && matches!(insn >> 27, 0x00 | 0x01 | 0x04 | 0x08 | 0x0C | 0x10 | 0x14 | 0x18 | 0x1C) => {
            Kind::Amo
        }
        _ => Kind::Interp,
    }
}

/// Pass 1 of translation: the extent of the block starting at `pc0`, and
/// the kind of each instruction (pass 2 reads the instructions again).
struct Scan {
    /// host address of the instruction at `pc0`
    host0: usize,
    n: usize,
    /// kind of the last instruction (Interp if n == 0)
    last: Kind,
    /// stopped before an instruction only the interpreter runs
    end_interp: bool,
}

/// Instruction `k` of the block at host address `host0`.
#[inline(always)]
fn insn_at(host0: usize, k: usize) -> u32 {
    // SAFETY: callers stay within the block's guest RAM page.
    unsafe { ((host0 + 4 * k) as *const u32).read() }
}

/// Kinds of a block's instructions; scan() writes the first `n` (left
/// uninitialized otherwise: zero-filling it cost more than the scan).
type Kinds = [MaybeUninit<Kind>; MAX_BLOCK];

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn scan(cpu: &Cpu, pc0: u32, pa0: u32, kinds: &mut Kinds) -> Scan {
    let host0 = cpu.ram_ptr() as usize + (pa0 - cpu.ram_base()) as usize;
    // instructions left in the page
    let max = (((0x1000 - (pc0 & 0xFFF)) >> 2) as usize).min(MAX_BLOCK);
    let mut s = Scan { host0, n: 0, last: Kind::Interp, end_interp: false };
    while s.n < max {
        let k = classify(insn_at(host0, s.n), pc0.wrapping_add(4 * s.n as u32));
        if k == Kind::Interp {
            s.end_interp = true;
            break;
        }
        kinds[s.n] = MaybeUninit::new(k);
        s.last = k;
        s.n += 1;
        if matches!(k, Kind::Branch | Kind::Jal | Kind::Jalr) {
            break;
        }
    }
    s
}

/// How a block ends (for the board-cost simulator in the Mac host).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlockEnd {
    Branch,
    Jal,
    Jalr,
    /// stopped before an instruction only the interpreter runs
    Interp,
    /// MAX_BLOCK or the page end: falls through to the next pc
    Fall,
}

/// Where the shared routines sit in the code buffer (word indices).
#[derive(Clone, Copy, Debug)]
pub struct PreludeLayout {
    /// first word available for blocks
    pub code_start: usize,
    /// routines whose `jal K1, routine` is followed by a guest pc word
    pub chain_req: usize,
    pub ijmp_imm: usize,
    pub interp_exit: usize,
}

/// Shape and exact translated size of one block.
#[derive(Clone, Copy, Debug)]
pub struct BlockInfo {
    /// guest instructions (0: the first one needs the interpreter)
    pub n: u32,
    /// words of translated code including the header (0 if n == 0)
    pub words: u32,
    pub end: BlockEnd,
}

type EnterFn = unsafe extern "C" fn(*mut Cpu, usize);

/// What one dispatcher step did (used by lockstep checking).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Event {
    /// Took an interrupt; no instruction executed.
    Irq,
    /// Interpreted one instruction (which may have trapped).
    Interp,
    /// Ran translated code entered at `va`; every instruction retired.
    Block { va: u32 },
}

/// Memory the translator works in, supplied by the platform.
pub struct JitMem<'m> {
    /// Translated code, block headers included.
    pub code: &'m mut [u32],
    /// Hash chains of blocks. Power of two.
    pub buckets: &'m mut [u32],
    /// One bit per RAM page.
    pub code_pages: &'m mut [u8],
    /// Power of two.
    pub jump_cache: &'m mut [JcEntry],
    /// 1: flush the whole buffer when full; 2..=MAX_REGIONS: ring of regions.
    pub regions: usize,
    /// Ring mode: room for logged cross-region chains, split evenly between
    /// the regions (a chain that does not fit is left unpatched).
    pub chain_log: &'m mut [u32],
    /// Backing store for translated blocks (empty: none). The first 1/16
    /// (rounded down to a power of two) is the index, the rest an
    /// append-only log; a boot's blocks average ~25 words with the header,
    /// so this keeps the index about 60% full.
    pub store: &'m mut [u32],
    /// One epoch per RAM page (with a store).
    pub store_epochs: &'m mut [u8],
}

pub struct Jit<'m> {
    code: &'m mut [u32],
    code_base: usize,
    pos: usize,
    code_start: usize,
    r: Routines,
    enter: usize,
    buckets: &'m mut [u32],
    bucket_shift: u32,
    /// host address - guest physical address in guest RAM
    ram_off: u32,
    code_pages: &'m mut [u8],
    ring: Ring<'m>,
    store: Store<'m>,
    jc: &'m mut [JcEntry],
    generation: u32,
    force_interp: bool,
    /// Translate AMOs to the host AMO (true) or to a plain load / operate /
    /// store sequence (false). RP2350 only supports exclusives, and so
    /// Hazard3 AMOs, on main SRAM: an AMO to guest RAM in PSRAM is a
    /// Store/AMO fault. With one guest hart and no host interrupts the plain
    /// sequence is equivalent. Set before running any code.
    pub native_amo: bool,
    /// set while `block_info` measures a block: translate without side effects
    measure_only: bool,
    pub stats: JitStats,
}

/// Backing store (see the crate docs). Log entry: va, pa,
/// words | epoch << 16, original code position, then the words. Index slots
/// hold 1 + the entry's absolute log position (0 = empty).
struct Store<'m> {
    mem: &'m mut [u32],
    epochs: &'m mut [u8],
    /// index slots (power of two); the log follows them
    nidx: usize,
    /// log words
    len: usize,
    /// absolute write position (log words written so far, wrapping skips
    /// included)
    wabs: u32,
}

const STORE_HDR: usize = 4;
const STORE_PROBES: usize = 8;

/// Ring-mode state (see the crate docs).
struct Ring<'m> {
    regions: usize,
    region_words: usize,
    /// region being filled
    cur: usize,
    /// allocation sequence of each region (0 = empty)
    seq: [u32; MAX_REGIONS],
    seq_next: u32,
    log: &'m mut [u32],
    log_per: usize,
    nlog: [u32; MAX_REGIONS],
}

/// Word indices of the shared routines in the prelude.
#[derive(Clone, Copy, Default)]
struct Routines {
    exit: usize,
    /// `jal K1, chain_req; .word target` - request chaining of this slot
    chain_req: usize,
    /// K0 = guest target
    ijmp: usize,
    /// `jal K1, ijmp_imm; .word target`
    ijmp_imm: usize,
    /// `jal K1, interp_exit; .word pc`
    interp_exit: usize,
    /// from the block prologue: `jal K1, budget_exit` (K1 = entry)
    budget_exit: usize,
    /// `jal K3, mem_miss` - interpret the instruction of that call site
    mem_miss: usize,
    /// TLB helpers, [store][log2 size]
    helper: [[usize; 3]; 2],
}

#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn fence_i() {
    #[cfg(target_arch = "riscv32")]
    unsafe {
        core::arch::asm!("fence.i")
    };
}

impl<'m> Jit<'m> {
    pub fn new(cpu: &mut Cpu, mem: JitMem<'m>) -> Self {
        assert!(mem.buckets.len().is_power_of_two() && mem.jump_cache.len().is_power_of_two());
        assert!(mem.code_pages.len() * 8 >= (cpu.ram_size() >> 12) as usize);
        // all jumps between blocks and the prelude are `jal` (+-1 MiB)
        assert!(mem.code.len() * 4 <= 1 << 20);
        assert!((1..=MAX_REGIONS).contains(&mem.regions));
        let log_per = mem.chain_log.len() / mem.regions;
        let nidx = if mem.store.is_empty() {
            0
        } else {
            assert!(mem.store_epochs.len() >= (cpu.ram_size() >> 12) as usize);
            let n = 1usize << (mem.store.len() / 16).max(1).ilog2();
            assert!(n * 2 <= mem.store.len());
            n
        };
        #[cfg(target_arch = "riscv32")]
        assert!(core::mem::size_of::<TlbEntry>() == 8 && rvcore::TLB_SIZE == 256);
        let code_base = mem.code.as_ptr() as usize;
        let bucket_shift = 32 - mem.buckets.len().trailing_zeros();
        let mut j = Jit {
            code: mem.code,
            code_base,
            pos: 0,
            code_start: 0,
            r: Routines::default(),
            enter: 0,
            buckets: mem.buckets,
            bucket_shift,
            ram_off: (cpu.ram_ptr() as usize as u32).wrapping_sub(cpu.ram_base()),
            code_pages: mem.code_pages,
            ring: Ring {
                regions: mem.regions,
                region_words: 0,
                cur: 0,
                seq: [0; MAX_REGIONS],
                seq_next: 1,
                log: mem.chain_log,
                log_per,
                nlog: [0; MAX_REGIONS],
            },
            store: Store { len: mem.store.len() - nidx, mem: mem.store, epochs: mem.store_epochs, nidx, wabs: 0 },
            jc: mem.jump_cache,
            generation: 0,
            force_interp: false,
            native_amo: true,
            measure_only: false,
            stats: JitStats::default(),
        };
        j.emit_prelude();
        j.ring.region_words = (j.code.len() - j.code_start) / j.ring.regions;
        assert!(j.ring.region_words >= MAX_BLOCK * 12 + 16, "code buffer too small for its regions");
        j.reset_tables();
        j.code_pages.fill(0);
        j.store_reset();
        cpu.code_pages = j.code_pages.as_ptr();
        cpu.jit.jc_base = j.jc.as_ptr() as usize;
        cpu.sync_tlb_ptrs();
        j
    }

    /// Shared routines at the start of the code buffer.
    fn emit_prelude(&mut self) {
        let jc_bits = self.jc.len().trailing_zeros();
        let mut a = Asm::at(self.code, self.code_base, 0);
        let mut r = Routines::default();
        macro_rules! jump {
            ($to:expr) => {{
                let j = a.jump_fwd();
                a.fix(j, $to);
            }};
        }

        // enter(cpu: a0, code: a1)
        let enter = a.pos();
        a.addi(SP, SP, -FRAME);
        for (i, &reg) in SAVED.iter().enumerate() {
            a.sw(reg, SP, 4 * i as i32);
        }
        a.sw(SP, 10, OFF_HOST_SP);
        a.addi(S, 10, 0);
        a.addi(K0, 11, 0);
        for &reg in &GUEST_REGS {
            a.lw(reg, S, xoff(reg));
        }
        a.lw(KB, S, OFF_BUDGET);
        a.jalr(ZERO, K0, 0);

        // exit: K0 = guest pc, K1 = info
        r.exit = a.pos();
        for &reg in &GUEST_REGS {
            a.sw(reg, S, xoff(reg));
        }
        a.sw(KB, S, OFF_BUDGET);
        a.sw(K0, S, OFF_EXIT_PC);
        a.sw(K1, S, OFF_EXIT_INFO);
        a.lw(SP, S, OFF_HOST_SP);
        for (i, &reg) in SAVED.iter().enumerate().rev() {
            a.lw(reg, SP, 4 * i as i32);
        }
        a.addi(SP, SP, FRAME);
        a.ret();

        // chain request: K1 = slot + 4 = address of the target word
        r.chain_req = a.pos();
        a.lw(K0, K1, 0);
        a.addi(K1, K1, -4);
        jump!(r.exit);

        // interpret the instruction at the pc in the word at K1
        r.interp_exit = a.pos();
        a.lw(K0, K1, 0);
        a.addi(K1, ZERO, EXIT_INTERP as i32);
        jump!(r.exit);

        // budget exhausted: K1 = entry, which is `addi KB, KB, -n`; undo it.
        // The block header (pc0) is at entry - 8.
        r.budget_exit = a.pos();
        a.lw(K2, K1, 0);
        a.srai(K2, K2, 20);
        a.sub(KB, KB, K2);
        a.lw(K0, K1, -8);
        a.addi(K1, ZERO, EXIT_BUDGET as i32);
        jump!(r.exit);

        // K3 = return address of a `jal K3` site: the dispatcher works out
        // the instruction and the refund (`mem_exit`)
        r.mem_miss = a.pos();
        a.addi(K1, K3, EXIT_MEM as i32);
        jump!(r.exit);

        // indirect jump with the target in the word at K1
        r.ijmp_imm = a.pos();
        a.lw(K0, K1, 0);
        // falls through into ijmp: indirect jump to guest K0 via the
        // fetch-TLB check and the jump cache, and on a jump-cache miss the
        // block's hash chain (refilling the jump cache), before giving up
        // to the dispatcher
        r.ijmp = a.pos();
        a.srli(K1, K0, 9);
        a.andi(K1, K1, 0x7F8);
        a.lw(K2, S, off_tlb_cur(ACC_X));
        a.add(K1, K1, K2);
        a.lw(K2, K1, 0);
        a.sub(K2, K0, K2);
        a.srli(K2, K2, 12);
        let miss1 = a.branch_fwd(1, K2, ZERO);
        a.lw(K3, K1, OFF_ADDEND); // fetch-TLB addend for the target page
        a.srli(K1, K0, 2);
        a.slli(K1, K1, 32 - jc_bits);
        a.srli(K1, K1, 32 - jc_bits - 4); // index * 16
        a.lw(K2, S, OFF_JC);
        a.add(K1, K1, K2);
        a.lw(K2, K1, 0);
        let miss2 = a.branch_fwd(1, K2, K0); // va
        a.lw(K2, K1, 4);
        let miss3 = a.branch_fwd(1, K2, K3); // same mapping
        a.lw(K2, K1, 8);
        a.jalr(ZERO, K2, 0);
        // jump-cache miss: K0 = va, K3 = addend. K3 = pa - va from here on.
        let walk = a.pos();
        a.fix(miss2, walk);
        a.fix(miss3, walk);
        a.li(K1, self.ram_off);
        a.sub(K3, K3, K1);
        a.add(K1, K0, K3); // pa
        a.srli(K1, K1, 12);
        a.srli(K2, K0, 2);
        a.xor(K1, K1, K2);
        a.li(K2, HASH_MUL);
        a.mul(K1, K1, K2);
        a.srli(K1, K1, self.bucket_shift);
        a.slli(K1, K1, 2);
        a.li(K2, self.buckets.as_ptr() as usize as u32);
        a.add(K1, K1, K2);
        a.lw(K2, K1, 0); // first header (word index) or EMPTY
        let lp = a.pos();
        a.addi(K1, K2, 1);
        let end = a.branch_fwd(0, K1, ZERO); // EMPTY
        a.slli(K2, K2, 2);
        a.li(K1, self.code_base as u32);
        a.add(K2, K2, K1); // header address
        a.lw(K1, K2, (4 * HDR_VA) as i32);
        let nx1 = a.branch_fwd(1, K1, K0);
        a.lw(K1, K2, (4 * HDR_PA) as i32);
        a.sub(K1, K1, K0);
        let nx2 = a.branch_fwd(1, K1, K3);
        // found: fill the jump-cache entry and go
        a.addi(K2, K2, (4 * ENTRY_OFF) as i32);
        a.li(K1, self.ram_off);
        a.add(K3, K3, K1); // the addend again
        a.srli(K1, K0, 2);
        a.slli(K1, K1, 32 - jc_bits);
        a.srli(K1, K1, 32 - jc_bits - 4);
        // (exit_info is free until the exit routine writes it)
        a.sw(K3, S, OFF_EXIT_INFO);
        a.lw(K3, S, OFF_JC);
        a.add(K1, K1, K3);
        a.lw(K3, S, OFF_EXIT_INFO);
        a.sw(K0, K1, 0);
        a.sw(K3, K1, 4);
        a.sw(K2, K1, 8);
        a.jalr(ZERO, K2, 0);
        let next = a.pos();
        a.fix(nx1, next);
        a.fix(nx2, next);
        a.lw(K2, K2, (4 * HDR_NEXT) as i32);
        let back = a.jump_fwd();
        a.fix(back, lp);
        let miss = a.pos();
        a.fix(miss1, miss);
        a.fix(end, miss);
        a.addi(K1, ZERO, EXIT_JUMP as i32);
        jump!(r.exit);

        // TLB helpers: K0 = guest va -> host address, K3 = return address.
        // A miss goes to mem_miss.
        //
        // Misaligned accesses also take the miss path, so the interpreter
        // does them in software: Hazard3 has no misaligned loads/stores (on
        // the PSRAM XIP window one wedged the hart). An aligned access of at
        // most 4 bytes cannot cross a page, so no page-crossing check is
        // needed after the alignment check.
        for store in 0..2 {
            for lg in 0..3 {
                r.helper[store][lg] = a.pos();
                let misaligned = if lg > 0 {
                    a.andi(K2, K0, (1 << lg) - 1);
                    Some(a.branch_fwd(1, K2, ZERO))
                } else {
                    None
                };
                a.srli(K1, K0, 9);
                a.andi(K1, K1, 0x7F8);
                a.lw(K2, S, off_tlb_cur(if store == 1 { ACC_W } else { ACC_R }));
                a.add(K1, K1, K2);
                a.lw(K2, K1, 0);
                a.sub(K2, K0, K2);
                a.srli(K2, K2, 12);
                let m = a.branch_fwd(1, K2, ZERO);
                a.lw(K1, K1, OFF_ADDEND);
                a.add(K0, K0, K1);
                a.jalr(ZERO, K3, 0);
                let here = a.pos();
                a.fix(m, here);
                if let Some(mis) = misaligned {
                    a.fix(mis, here);
                }
                jump!(r.mem_miss);
            }
        }

        assert!(!a.overflowed());
        self.enter = self.code_base + enter * 4;
        self.code_start = a.pos();
        self.pos = self.code_start;
        self.r = r;
        fence_i();
    }

    fn reset_tables(&mut self) {
        self.buckets.fill(EMPTY);
        // with a backing store, pages keep their write protection: stored
        // blocks from them must still see stores to them
        if self.store.nidx == 0 {
            self.code_pages.fill(0);
        }
        self.jc.fill(JcEntry::EMPTY);
        self.pos = self.code_start;
        self.generation = self.generation.wrapping_add(1);
        let g = &mut self.ring;
        g.cur = 0;
        g.seq = [0; MAX_REGIONS];
        g.seq[0] = g.seq_next;
        g.seq_next += 1;
        g.nlog = [0; MAX_REGIONS];
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn region_of(&self, word: usize) -> usize {
        (word - self.code_start) / self.ring.region_words
    }

    /// Make room for a block of up to `need` words: flush, or (ring mode)
    /// move on to the next region, evicting its blocks.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn make_room(&mut self, need: usize) {
        if self.ring.regions == 1 {
            if self.pos + need > self.code.len() {
                self.flush();
            }
            return;
        }
        let end = self.code_start + (self.ring.cur + 1) * self.ring.region_words;
        if self.pos + need <= end {
            return;
        }
        let next = (self.ring.cur + 1) % self.ring.regions;
        self.evict_region(next);
        let g = &mut self.ring;
        g.cur = next;
        g.seq[next] = g.seq_next;
        g.seq_next += 1;
        self.pos = self.code_start + next * self.ring.region_words;
        self.generation = self.generation.wrapping_add(1);
        self.stats.evictions += 1;
    }

    /// Drop region `r`'s blocks: out of the hash chains, logged chains into
    /// it back to chain requests, jump-cache entries into it.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn evict_region(&mut self, r: usize) {
        // Chains are newest first, and `r` is the oldest region still
        // holding blocks, so its blocks are the tail of every chain: cut
        // each chain at the first one.
        let lo = (self.code_start + r * self.ring.region_words) as u32;
        let hi = lo + self.ring.region_words as u32;
        let inr = |i: u32| i.wrapping_sub(lo) < hi - lo;
        for h in 0..self.buckets.len() {
            let mut i = self.buckets[h];
            if i == EMPTY {
                continue;
            }
            if inr(i) {
                self.buckets[h] = EMPTY;
                continue;
            }
            loop {
                let next = self.code[i as usize + HDR_NEXT];
                if next == EMPTY {
                    break;
                }
                if inr(next) {
                    self.code[i as usize + HDR_NEXT] = EMPTY;
                    break;
                }
                i = next;
            }
        }
        let base = r * self.ring.log_per;
        for k in 0..self.ring.nlog[r] as usize {
            let slot = self.ring.log[base + k] as usize;
            self.code[slot] = j_type(K1, (self.r.chain_req as i32 - slot as i32) * 4);
        }
        self.ring.nlog[r] = 0;
        self.ring.seq[r] = 0;
        let (lo, hi) = (self.code_addr(lo) as u32, self.code_addr(hi) as u32);
        for e in self.jc.iter_mut() {
            if e.code >= lo && e.code < hi {
                *e = JcEntry::EMPTY;
            }
        }
        fence_i();
    }

    pub fn flush(&mut self) {
        self.reset_tables();
        self.stats.flushes += 1;
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn hash(&self, va: u32, pa: u32) -> usize {
        // (the ijmp routine computes the same in host code)
        ((va >> 2) ^ (pa >> 12)).wrapping_mul(HASH_MUL) as usize >> self.bucket_shift
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    /// Entry point (code word index) of block (va, pa).
    fn lookup(&self, va: u32, pa: u32) -> Option<u32> {
        let mut i = self.buckets[self.hash(va, pa)];
        while i != EMPTY {
            let h = i as usize;
            if self.code[h + HDR_VA] == va && self.code[h + HDR_PA] == pa {
                return Some((h + ENTRY_OFF) as u32);
            }
            i = self.code[h + HDR_NEXT];
        }
        None
    }

    /// Fill in the header at word `h` and put the block in its hash chain.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn insert(&mut self, va: u32, pa: u32, h: usize) {
        let b = self.hash(va, pa);
        self.code[h + HDR_NEXT] = self.buckets[b];
        self.code[h + HDR_PA] = pa;
        self.code[h + HDR_VA] = va;
        self.buckets[b] = h as u32;
    }

    /// Drop every block translated from physical page `ppage`.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn invalidate_page(&mut self, cpu: &Cpu, ppage: u32) {
        for b in 0..self.buckets.len() {
            let mut prev = EMPTY;
            let mut i = self.buckets[b];
            while i != EMPTY {
                let next = self.code[i as usize + HDR_NEXT];
                if self.code[i as usize + HDR_PA] & !0xFFF == ppage {
                    if prev == EMPTY {
                        self.buckets[b] = next;
                    } else {
                        self.code[prev as usize + HDR_NEXT] = next;
                    }
                } else {
                    prev = i;
                }
                i = next;
            }
        }
        self.jc.fill(JcEntry::EMPTY);
        let n = ((ppage - cpu.ram_base()) >> 12) as usize;
        self.code_pages[n >> 3] &= !(1 << (n & 7));
        self.stats.smc_invalidations += 1;
        if self.store.nidx != 0 {
            self.store.epochs[n] = self.store.epochs[n].wrapping_add(1);
        }
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn mark_code_page(&mut self, cpu: &mut Cpu, pa: u32) {
        let n = ((pa - cpu.ram_base()) >> 12) as usize;
        if self.code_pages[n >> 3] & (1 << (n & 7)) == 0 {
            self.code_pages[n >> 3] |= 1 << (n & 7);
            let host_page = cpu.ram_ptr() as usize + (n << 12);
            cpu.tlb_evict_writes_to(host_page);
        }
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn code_addr(&self, code: u32) -> usize {
        self.code_base + code as usize * 4
    }

    // ------------------------------------------------------------ translate

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn translate(&mut self, cpu: &mut Cpu, pc0: u32, pa0: u32) -> u32 {
        let t0 = host_instret();
        let r = if self.store.nidx == 0 {
            self.translate_inner(cpu, pc0, pa0)
        } else if let Some(e) = self.store_find(cpu, pc0, pa0) {
            self.restore(cpu, e)
        } else {
            let r = self.translate_inner(cpu, pc0, pa0);
            if r != NO_CODE {
                self.store_append(cpu, pc0, pa0, r as usize - ENTRY_OFF);
            }
            r
        };
        self.stats.h_translate += host_instret() - t0;
        r
    }

    // ------------------------------------------------------------ backing store

    fn store_reset(&mut self) {
        let n = self.store.nidx;
        self.store.mem[..n].fill(0);
        self.store.wabs = 0;
    }

    /// `mem` index of the entry an index slot value names, if it has not
    /// been overwritten.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn store_entry(&self, v: u32) -> Option<usize> {
        let abs = v.wrapping_sub(1);
        (v != 0 && self.store.wabs.wrapping_sub(abs) as usize <= self.store.len)
            .then(|| self.store.nidx + abs as usize % self.store.len)
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn store_slot(&self, va: u32, pa: u32, i: usize) -> usize {
        let h = ((va ^ pa.rotate_left(16)) >> 2).wrapping_mul(0x9E37_79B1) >> 7;
        (h as usize + i) & (self.store.nidx - 1)
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn page_epoch(&self, cpu: &Cpu, pa: u32) -> u32 {
        u32::from(self.store.epochs[((pa - cpu.ram_base()) >> 12) as usize])
    }

    /// Log entry of a current stored copy of block (va, pa).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn store_find(&self, cpu: &Cpu, va: u32, pa: u32) -> Option<usize> {
        for i in 0..STORE_PROBES {
            let v = self.store.mem[self.store_slot(va, pa, i)];
            if v == 0 {
                return None;
            }
            let Some(e) = self.store_entry(v) else { continue };
            let m = &self.store.mem;
            if m[e] == va && m[e + 1] == pa {
                return (m[e + 2] >> 16 == self.page_epoch(cpu, pa)).then_some(e);
            }
        }
        None
    }

    /// Append the block whose header is at code word `header` (just
    /// translated, so its words end at `self.pos`).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn store_append(&mut self, cpu: &Cpu, va: u32, pa: u32, header: usize) {
        let words = self.pos - header;
        let len = STORE_HDR + words;
        let mut off = self.store.wabs as usize % self.store.len;
        if off + len > self.store.len {
            // entries are contiguous: skip the tail, start the next lap
            self.store.wabs = self.store.wabs.wrapping_add((self.store.len - off) as u32);
            off = 0;
            self.stats.store_resets += 1;
        }
        let abs = self.store.wabs;
        let e = self.store.nidx + off;
        let epoch = self.page_epoch(cpu, pa);
        let m = &mut self.store.mem;
        m[e] = va;
        m[e + 1] = pa;
        m[e + 2] = words as u32 | epoch << 16;
        m[e + 3] = header as u32;
        m[e + STORE_HDR..e + STORE_HDR + words].copy_from_slice(&self.code[header..self.pos]);
        self.store.wabs = abs.wrapping_add(len as u32);
        // take a free or overwritten slot, or the same key's; else the
        // first probe's
        let mut slot = self.store_slot(va, pa, 0);
        for i in 0..STORE_PROBES {
            let s = self.store_slot(va, pa, i);
            match self.store_entry(self.store.mem[s]) {
                None => {
                    slot = s;
                    break;
                }
                Some(o) if self.store.mem[o] == va && self.store.mem[o + 1] == pa => {
                    slot = s;
                    break;
                }
                Some(_) => {}
            }
        }
        self.store.mem[slot] = abs.wrapping_add(1);
    }

    /// Copy stored block `e` into the code buffer; returns its entry point.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn restore(&mut self, cpu: &mut Cpu, e: usize) -> u32 {
        let (va, pa) = (self.store.mem[e], self.store.mem[e + 1]);
        let words = (self.store.mem[e + 2] & 0xFFFF) as usize;
        let orig = self.store.mem[e + 3] as usize;
        self.make_room(words);
        self.mark_code_page(cpu, pa);
        let at = self.pos;
        self.code[at..at + words].copy_from_slice(&self.store.mem[e + STORE_HDR..e + STORE_HDR + words]);
        // re-point the jals into the shared routines, stepping over the
        // data word after a chain / jump / interp request
        let r = self.r;
        let mut i = HDR_VA + 1;
        while i < words {
            let w = self.code[at + i];
            i += 1;
            if w & 0x7F != 0x6F {
                continue;
            }
            let tgt = (orig + i - 1) as isize + (jal_off(w) / 4) as isize;
            if tgt < 0 || tgt as usize >= self.code_start {
                continue;
            }
            let rd = (w >> 7) & 31;
            self.code[at + i - 1] = j_type(rd, ((tgt - (at + i - 1) as isize) * 4) as i32);
            let t = tgt as usize;
            if rd == K1 && (t == r.chain_req || t == r.ijmp_imm || t == r.interp_exit) {
                i += 1;
            }
        }
        self.pos = at + words;
        fence_i();
        self.stats.restores += 1;
        self.stats.restore_words += words as u64;
        self.insert(va, pa, at);
        (at + ENTRY_OFF) as u32
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn translate_inner(&mut self, cpu: &mut Cpu, pc0: u32, pa0: u32) -> u32 {
        // pass 1: find the block
        let mut kinds: Kinds = [MaybeUninit::uninit(); MAX_BLOCK];
        let Scan { host0, n, last, end_interp } = scan(cpu, pc0, pa0, &mut kinds);
        let pc = pc0.wrapping_add(4 * n as u32);

        // (no-code blocks are not kept: scanning one instruction again is
        // about as cheap as looking it up)
        if n == 0 {
            return NO_CODE;
        }
        // room for the worst case (measuring never keeps the block)
        if !self.measure_only {
            self.make_room(n * 12 + 16 + ENTRY_OFF);
        }
        if !self.measure_only {
            self.mark_code_page(cpu, pa0);
        }

        let r = self.r;
        let mut a = Asm::at(self.code, self.code_base, self.pos);

        // `jal link, routine` (the buffer is at most 1 MiB, so always in range)
        macro_rules! call {
            ($link:expr, $to:expr) => {{
                let off = ($to as isize - a.pos() as isize) * 4;
                a.emit(j_type($link, off as i32));
            }};
        }

        // header word (guest pc), the budget exit, then the entry point:
        // `addi KB, KB, -n; blt KB, zero, <the budget exit>`
        let header = a.pos();
        a.emit(EMPTY); // HDR_NEXT, HDR_PA: filled in by `insert`
        a.emit(pa0);
        a.emit(pc0);
        call!(K1, r.budget_exit);
        let start = a.pos();
        a.addi(KB, KB, -(n as i32));
        a.emit(b_type(4, KB, ZERO, -8));
        let same_page = |t: u32| t & !0xFFF == pc0 & !0xFFF;
        // jump to a known guest pc: chainable slot if same page, else jump cache
        macro_rules! goto_pc {
            ($target:expr) => {{
                let t: u32 = $target;
                call!(K1, if same_page(t) { r.chain_req } else { r.ijmp_imm });
                a.emit(t);
            }};
        }

        for (k, kind) in kinds[..n].iter().enumerate() {
            let insn = insn_at(host0, k);
            let pc = pc0.wrapping_add(4 * k as u32);
            // SAFETY: scan() wrote kinds[..n].
            let kind = unsafe { kind.assume_init() };
            let rd = (insn >> 7) & 31;
            let rs1 = (insn >> 15) & 31;
            let rs2 = (insn >> 20) & 31;
            let f3 = (insn >> 12) & 7;
            match kind {
                Kind::Alu { rs1: u1, rs2: u2 } => {
                    let mut w = insn;
                    if u1 && spilled(rs1) {
                        a.lw(K0, S, xoff(rs1));
                        w = (w & !(31 << 15)) | (K0 << 15);
                    }
                    if u2 && spilled(rs2) {
                        a.lw(K1, S, xoff(rs2));
                        w = (w & !(31 << 20)) | (K1 << 20);
                    }
                    if spilled(rd) {
                        a.emit((w & !(31 << 7)) | (K2 << 7));
                        a.sw(K2, S, xoff(rd));
                    } else {
                        a.emit(w);
                    }
                }
                Kind::Auipc => {
                    if rd != 0 {
                        let v = pc.wrapping_add(insn & 0xFFFF_F000);
                        if spilled(rd) {
                            a.li(K2, v);
                            a.sw(K2, S, xoff(rd));
                        } else {
                            a.li(rd, v);
                        }
                    }
                }
                Kind::Load | Kind::Store => {
                    let store = kind == Kind::Store;
                    let imm = if store { imm_s(insn) } else { imm_i(insn) };
                    if spilled(rs1) {
                        a.lw(K0, S, xoff(rs1));
                        if imm != 0 {
                            a.addi(K0, K0, imm);
                        }
                    } else {
                        a.addi(K0, rs1, imm);
                    }
                    call!(K3, r.helper[store as usize][(f3 & 3) as usize]);
                    if store {
                        if spilled(rs2) {
                            a.lw(K1, S, xoff(rs2));
                            a.store(f3, K1, K0, 0);
                        } else {
                            a.store(f3, rs2, K0, 0);
                        }
                    } else if spilled(rd) {
                        a.load(f3, K1, K0, 0);
                        a.sw(K1, S, xoff(rd));
                    } else {
                        a.load(f3, rd, K0, 0);
                    }
                }
                Kind::Branch => {
                    let r1 = if spilled(rs1) {
                        a.lw(K0, S, xoff(rs1));
                        K0
                    } else {
                        rs1
                    };
                    let r2 = if spilled(rs2) {
                        a.lw(K1, S, xoff(rs2));
                        K1
                    } else {
                        rs2
                    };
                    a.emit(b_type(f3 ^ 1, r1, r2, 12)); // not taken: skip slot + word
                    goto_pc!(pc.wrapping_add(imm_b(insn) as u32));
                    goto_pc!(pc.wrapping_add(4));
                }
                Kind::Jal => {
                    if rd != 0 {
                        let link = pc.wrapping_add(4);
                        if spilled(rd) {
                            a.li(K2, link);
                            a.sw(K2, S, xoff(rd));
                        } else {
                            a.li(rd, link);
                        }
                    }
                    goto_pc!(pc.wrapping_add(imm_j(insn) as u32));
                }
                Kind::Jalr => {
                    if spilled(rs1) {
                        a.lw(K0, S, xoff(rs1));
                        a.addi(K0, K0, imm_i(insn));
                    } else {
                        a.addi(K0, rs1, imm_i(insn));
                    }
                    a.andi(K0, K0, -2);
                    // misaligned target: let the interpreter raise it
                    a.andi(K1, K0, 2);
                    a.emit(b_type(0, K1, ZERO, 8)); // beq K1, zero, +8
                    call!(K3, r.mem_miss);
                    if rd != 0 {
                        let link = pc.wrapping_add(4);
                        if spilled(rd) {
                            a.li(K2, link);
                            a.sw(K2, S, xoff(rd));
                        } else {
                            a.li(rd, link);
                        }
                    }
                    let j = a.jump_fwd();
                    a.fix(j, r.ijmp);
                }
                Kind::Nop => {}
                Kind::Amo => {
                    // misaligned AMOs and every slow case go to the interpreter
                    if spilled(rs1) {
                        a.lw(K0, S, xoff(rs1));
                    } else {
                        a.addi(K0, rs1, 0);
                    }
                    a.andi(K1, K0, 3);
                    a.emit(b_type(0, K1, ZERO, 8)); // beq K1, zero, +8
                    call!(K3, r.mem_miss);
                    call!(K3, r.helper[1][2]);
                    let src = if spilled(rs2) {
                        a.lw(K1, S, xoff(rs2));
                        K1
                    } else {
                        rs2
                    };
                    if self.native_amo {
                        let dst = if spilled(rd) { K2 } else { rd };
                        // same funct5, aq/rl cleared, host registers substituted
                        a.emit(r_type(0x2F, dst, 2, K0, src, (insn >> 27) << 2));
                        if spilled(rd) {
                            a.sw(K2, S, xoff(rd));
                        }
                    } else {
                        // K2 = old, K3 (free once the helper has returned) = new
                        a.lw(K2, K0, 0);
                        let new = match insn >> 27 {
                            0x01 => src,
                            0x00 => {
                                a.add(K3, K2, src);
                                K3
                            }
                            0x04 => {
                                a.emit(r_type(0x33, K3, 4, K2, src, 0));
                                K3
                            }
                            0x08 => {
                                a.emit(r_type(0x33, K3, 6, K2, src, 0));
                                K3
                            }
                            0x0C => {
                                a.emit(r_type(0x33, K3, 7, K2, src, 0));
                                K3
                            }
                            f5 => {
                                // min/max[u]: new = old; if src beats old, new = src
                                // (branch over the move when it does not)
                                let skip_f3 = match f5 {
                                    0x10 => 5, // min:  bge  src, old
                                    0x14 => 4, // max:  blt  src, old
                                    0x18 => 7, // minu: bgeu src, old
                                    _ => 6,    // maxu: bltu src, old
                                };
                                a.addi(K3, K2, 0);
                                a.emit(b_type(skip_f3, src, K2, 8));
                                a.addi(K3, src, 0);
                                K3
                            }
                        };
                        a.sw(new, K0, 0);
                        if spilled(rd) {
                            a.sw(K2, S, xoff(rd));
                        } else if rd != 0 {
                            a.addi(rd, K2, 0);
                        }
                    }
                }
                Kind::Interp => unreachable!(),
            }
        }

        // block end without a terminator
        if !matches!(last, Kind::Branch | Kind::Jal | Kind::Jalr) {
            if end_interp {
                call!(K1, r.interp_exit);
                a.emit(pc);
            } else {
                goto_pc!(pc);
            }
        }

        let end = a.pos();
        assert!(!a.overflowed(), "code buffer overflow");
        self.pos = end;
        fence_i();

        self.stats.blocks += 1;
        self.stats.guest_insns_translated += n as u64;
        self.stats.code_words += (end - header) as u64;
        self.insert(pc0, pa0, header);
        start as u32
    }

    // ------------------------------------------------------------ dispatch

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn interp<B: Bus>(&mut self, cpu: &mut Cpu, bus: &mut B) {
        let t0 = host_instret();
        self.interp_inner(cpu, bus);
        self.stats.h_interp += host_instret() - t0;
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn interp_inner<B: Bus>(&mut self, cpu: &mut Cpu, bus: &mut B) {
        if let Ok((_, h)) = cpu.translate_pc(cpu.pc) {
            // SAFETY: h is a host pointer into guest RAM from the fetch TLB.
            let op = unsafe { (h as *const u32).read() } & 0x7F;
            self.stats.interp_op[op as usize] += 1;
        }
        cpu.step(bus);
        self.stats.interp_steps += 1;
        if let Some(p) = cpu.smc_page.take() {
            self.invalidate_page(cpu, p);
        }
        // device writes to guest RAM (always from an interpreted MMIO store)
        while let Some((lo, hi)) = bus.take_dma_write() {
            let mut page = lo & !0xFFF;
            while page < hi {
                let n = (page.wrapping_sub(cpu.ram_base()) >> 12) as usize;
                if n < self.code_pages.len() * 8 && self.code_pages[n >> 3] & (1 << (n & 7)) != 0 {
                    self.invalidate_page(cpu, page);
                }
                page += 0x1000;
            }
        }
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn chain(&mut self, cpu: &mut Cpu, slot_addr: usize, target: u32) {
        let generation = self.generation;
        let Ok((pa, _)) = cpu.translate_pc(target) else { return };
        let code = match self.lookup(target, pa) {
            Some(c) => c,
            None => self.translate(cpu, target, pa),
        };
        if code == NO_CODE || generation != self.generation {
            return;
        }
        let slot = (slot_addr - self.code_base) / 4;
        let off = (code as isize - slot as isize) * 4;
        if (-(1 << 20)..(1 << 20)).contains(&off) {
            if self.ring.regions > 1 {
                // into an older region: log it, so evicting that region
                // can reset this slot
                let (src, dst) = (self.region_of(slot), self.region_of(code as usize));
                if self.ring.seq[dst] < self.ring.seq[src] {
                    let n = self.ring.nlog[dst] as usize;
                    if n == self.ring.log_per {
                        return;
                    }
                    self.ring.log[dst * self.ring.log_per + n] = slot as u32;
                    self.ring.nlog[dst] += 1;
                }
            }
            self.code[slot] = j_type(ZERO, off as i32);
            fence_i();
            self.stats.chains += 1;
        }
    }

    /// A `jal K3` site returning to host address `ret` left translated code
    /// (TLB miss, MMIO, fault, misaligned access or jump target): set
    /// `exit_pc` to its instruction and refund the budget for the
    /// instructions from that one to the end of the block, which did not run.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn mem_exit(&mut self, cpu: &mut Cpu, ret: usize) {
        let site = (ret - self.code_base) / 4 - 1;
        // back to the block's `jal K1, budget_exit`: no other word is a jal
        // there (guest pcs in data words are 4-aligned; instructions are not)
        let mut p = site - 1;
        while !(self.code[p] & 0xFFF == (K1 << 7) | 0x6F
            && p as isize + (jal_off(self.code[p]) / 4) as isize == self.r.budget_exit as isize)
        {
            p -= 1;
        }
        let entry = p + 1;
        let pc0 = self.code[p - 1];
        let n = -((self.code[entry] as i32) >> 20) as u32;
        let before = self.code[entry..site].iter().filter(|&&w| w & 0xFFF == (K3 << 7) | 0x6F).count();
        // the guest instruction with that many sites before it
        let pa0 = match cpu.tlb_probe(pc0, ACC_X) {
            Some(pa) => pa,
            None => cpu.translate_pc(pc0).map(|(pa, _)| pa).expect("mem exit: block page unmapped"),
        };
        let host0 = cpu.ram_ptr() as usize + (pa0 - cpu.ram_base()) as usize;
        let (mut k, mut seen) = (0u32, 0usize);
        loop {
            // SAFETY: instruction k of the block, in the same guest RAM page.
            let insn = unsafe { ((host0 + 4 * k as usize) as *const u32).read() };
            seen += match classify(insn, pc0.wrapping_add(4 * k)) {
                Kind::Load | Kind::Store | Kind::Jalr => 1,
                Kind::Amo => 2,
                _ => 0,
            };
            if seen > before {
                break;
            }
            k += 1;
        }
        cpu.jit.exit_pc = pc0.wrapping_add(4 * k);
        cpu.jit.budget += (n - k) as i32;
    }

    /// One dispatcher step: take an interrupt, interpret one instruction, or
    /// run translated code until it exits.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn run_one<B: Bus>(&mut self, cpu: &mut Cpu, bus: &mut B) -> Event {
        let t0 = host_instret();
        let ev = self.run_one_inner(cpu, bus);
        self.stats.h_total += host_instret() - t0;
        ev
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn run_one_inner<B: Bus>(&mut self, cpu: &mut Cpu, bus: &mut B) -> Event {
        if cpu.check_irq(bus) {
            self.force_interp = false;
            return Event::Irq;
        }
        if self.force_interp {
            self.force_interp = false;
            self.interp(cpu, bus);
            return Event::Interp;
        }
        let pc = cpu.pc;
        let Ok((pa, host)) = cpu.translate_pc(pc) else {
            self.interp(cpu, bus); // raises the fetch fault
            return Event::Interp;
        };
        let code = match self.lookup(pc, pa) {
            Some(c) => c,
            None => self.translate(cpu, pc, pa),
        };
        if code == NO_CODE {
            self.interp(cpu, bus);
            return Event::Interp;
        }
        // remember it for the inline jump cache
        let jci = ((pc >> 2) as usize) & (self.jc.len() - 1);
        self.jc[jci] = JcEntry {
            va: pc,
            addend: host.wrapping_sub(pc as usize) as u32,
            code: self.code_addr(code) as u32,
            _pad: 0,
        };

        let budget = cpu.timer_deadline().min(BUDGET_CAP) as i32;
        cpu.jit.budget = budget;
        // SAFETY: `enter` is the trampoline emitted in emit_prelude; the code
        // it jumps to was produced by `translate`.
        let t0 = host_instret();
        unsafe {
            let enter: EnterFn = core::mem::transmute(self.enter);
            enter(cpu, self.code_addr(code));
        }
        self.stats.h_code += host_instret() - t0;
        let info = cpu.jit.exit_info;
        if info & 3 == EXIT_MEM {
            self.mem_exit(cpu, (info & !3) as usize);
        }
        let executed = (budget - cpu.jit.budget) as u64;
        cpu.instret += executed;
        self.stats.jit_insns += executed;
        self.stats.entries += 1;
        cpu.pc = cpu.jit.exit_pc;
        match info & 3 {
            EXIT_JUMP => {
                self.stats.exit_jump += 1;
                if info != 0 {
                    self.chain(cpu, info as usize, cpu.pc);
                }
            }
            EXIT_INTERP => {
                self.stats.exit_interp += 1;
                self.force_interp = true;
            }
            EXIT_MEM => {
                self.stats.exit_interp += 1;
                self.force_interp = true;
            }
            _ => {
                self.stats.exit_budget += 1;
                self.force_interp = true;
            }
        }
        Event::Block { va: pc }
    }

    /// Shape and exact translated size of the block at (`va`, `pa`), without
    /// keeping it: used by the Mac host to simulate other cache policies.
    pub fn block_info(&mut self, cpu: &mut Cpu, va: u32, pa: u32) -> BlockInfo {
        self.block_code(cpu, va, pa).0
    }

    pub fn prelude_layout(&self) -> PreludeLayout {
        PreludeLayout {
            code_start: self.code_start,
            chain_req: self.r.chain_req,
            ijmp_imm: self.r.ijmp_imm,
            interp_exit: self.r.interp_exit,
        }
    }

    /// `block_info`, plus the words the translator emits for the block and
    /// the word index they were emitted at (valid until the next translation).
    pub fn block_code(&mut self, cpu: &mut Cpu, va: u32, pa: u32) -> (BlockInfo, &[u32], usize) {
        let sc = scan(cpu, va, pa, &mut [MaybeUninit::uninit(); MAX_BLOCK]);
        let end = match sc.n {
            0 => BlockEnd::Interp,
            _ => match sc.last {
                Kind::Branch => BlockEnd::Branch,
                Kind::Jal => BlockEnd::Jal,
                Kind::Jalr => BlockEnd::Jalr,
                _ if sc.end_interp => BlockEnd::Interp,
                _ => BlockEnd::Fall,
            },
        };
        if sc.n == 0 {
            return (BlockInfo { n: 0, words: 0, end }, &[], self.pos);
        }
        let pos = self.pos;
        let h = self.hash(va, pa);
        let head = self.buckets[h];
        let stats = self.stats;
        self.measure_only = true;
        self.translate_inner(cpu, va, pa);
        self.measure_only = false;
        let words = (self.pos - pos) as u32;
        self.pos = pos;
        self.buckets[h] = head;
        self.stats = stats;
        (BlockInfo { n: sc.n as u32, words, end }, &self.code[pos..pos + words as usize], pos)
    }

    /// Mark / unmark a guest physical page as holding translated code (the
    /// write-TLB refusal that catches self-modifying code), for the simulator.
    pub fn set_code_page(&mut self, cpu: &mut Cpu, pa: u32, on: bool) {
        if on {
            self.mark_code_page(cpu, pa);
        } else {
            let n = ((pa - cpu.ram_base()) >> 12) as usize;
            self.code_pages[n >> 3] &= !(1 << (n & 7));
        }
    }

    pub fn code_words_used(&self) -> usize {
        self.pos - self.code_start
    }
}
