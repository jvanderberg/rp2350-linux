//! Block translator: RV32IMA guest code -> RV32 host code (Hazard3).
//!
//! Guest register xN lives in host register xN, except five host registers
//! the translator reserves; the guest values of those live in `cpu.x[]`:
//!   x3  S   pointer to the `Cpu`
//!   x4  K0  scratch: guest address / host address / exit pc
//!   x7  K1  scratch / exit info
//!   x23 K2  scratch
//!   x24 K3  link register for the TLB helpers
//! (the five least-used guest registers in a boot, ~1.5% of operand uses).
//!
//! Every block starts with `budget -= N; if budget < 0 exit`. The dispatcher
//! sets the budget to the instructions left before the next timer deadline,
//! which keeps `instret` exact and makes interrupt timing identical to the
//! interpreter's. Anything other than ALU / M / loads / stores / branches /
//! jumps ends the block and is run by the interpreter (`Cpu::step`).
//!
//! Loads and stores call a shared soft-TLB helper (`jal K3`) that returns
//! the host address in K0 and skips the following `j miss_stub` on a hit. A
//! miss, MMIO, a fault or a misaligned access exits, and the interpreter
//! executes that one instruction.
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
//! Block descriptors are a ring in the same order. A chain from a block into
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

use core::mem::offset_of;
use emit::*;
use rvcore::{ACC_R, ACC_W, ACC_X, Bus, Cpu, JitRegs, TlbEntry};

pub const S: Reg = 3;
pub const K0: Reg = 4;
pub const K1: Reg = 7;
pub const K2: Reg = 23;
pub const K3: Reg = 24;

#[inline(always)]
pub const fn spilled(r: u32) -> bool {
    r == S || r == K0 || r == K1 || r == K2 || r == K3
}

/// Guest registers held in host registers while in translated code.
const GUEST_REGS: [Reg; 26] = [
    1, 2, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 25, 26, 27, 28, 29, 30, 31,
];
/// Host registers the entry trampoline must preserve for the Rust caller
/// (ra, callee-saved s0-s11, and gp/tp which we repurpose).
const SAVED: [Reg; 15] = [1, 3, 4, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27];
const FRAME: i32 = 64;

pub const EXIT_JUMP: u32 = 0; // info = chain slot address, or 0
pub const EXIT_INTERP: u32 = 1; // interpret the instruction at exit_pc
pub const EXIT_BUDGET: u32 = 2; // budget exhausted before the block ran

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
pub struct Block {
    pub va: u32,
    pub pa: u32,
    /// Word index of the code, or NO_CODE for "always interpret".
    pub code: u32,
    pub next: u32,
}

impl Block {
    pub const EMPTY: Block = Block { va: 0, pa: 0, code: NO_CODE, next: EMPTY };
}

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

/// Pass 1 of translation: the instructions of the block starting at `pc0`.
struct Scan {
    insns: [u32; MAX_BLOCK],
    kinds: [Kind; MAX_BLOCK],
    n: usize,
    /// stopped before an instruction only the interpreter runs
    end_interp: bool,
}

#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn scan(cpu: &Cpu, pc0: u32, pa0: u32) -> Scan {
    let page_end = (pc0 & !0xFFF).wrapping_add(0x1000);
    let host0 = cpu.ram_ptr() as usize + (pa0 - cpu.ram_base()) as usize;
    let mut s = Scan { insns: [0; MAX_BLOCK], kinds: [Kind::Interp; MAX_BLOCK], n: 0, end_interp: false };
    let mut pc = pc0;
    while s.n < MAX_BLOCK && pc != page_end {
        // SAFETY: within the same guest RAM page as pa0.
        let insn = unsafe { ((host0 + (pc - pc0) as usize) as *const u32).read() };
        let k = classify(insn, pc);
        if k == Kind::Interp {
            s.end_interp = true;
            break;
        }
        s.insns[s.n] = insn;
        s.kinds[s.n] = k;
        s.n += 1;
        pc = pc.wrapping_add(4);
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
    pub code: &'m mut [u32],
    /// Power of two.
    pub buckets: &'m mut [u32],
    pub blocks: &'m mut [Block],
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
    blocks: &'m mut [Block],
    /// descriptor ring: oldest live descriptor, live count
    bhead: usize,
    nblocks: usize,
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
    /// descriptors per region (they are contiguous in the descriptor ring)
    nblk: [u32; MAX_REGIONS],
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
    /// from the block prologue: `jal K1, budget_exit` (K1 = entry + 16)
    budget_exit: usize,
    /// `jal K3, ...; .word meta` - interpret the instruction meta names
    mem_miss: usize,
    /// TLB helpers, [store][log2 size]
    helper: [[usize; 3]; 2],
}

/// Metadata word after a helper call: distance back to the block header,
/// index of the instruction, and block length.
const fn meta(dist: usize, k: usize, n: usize) -> u32 {
    (dist as u32) | ((k as u32) << 10) | ((n as u32) << 16)
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
            blocks: mem.blocks,
            bhead: 0,
            nblocks: 0,
            code_pages: mem.code_pages,
            ring: Ring {
                regions: mem.regions,
                region_words: 0,
                cur: 0,
                seq: [0; MAX_REGIONS],
                seq_next: 1,
                nblk: [0; MAX_REGIONS],
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
        a.jalr(ZERO, K0, 0);

        // exit: K0 = guest pc, K1 = info
        r.exit = a.pos();
        for &reg in &GUEST_REGS {
            a.sw(reg, S, xoff(reg));
        }
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

        // budget exhausted: K1 = entry + 16, block header (pc0) at entry - 4
        r.budget_exit = a.pos();
        a.lw(K0, K1, -20);
        a.addi(K1, ZERO, EXIT_BUDGET as i32);
        jump!(r.exit);

        // K3 = address of a meta word: interpret instruction k of the block,
        // refunding the n - k instructions that did not run
        r.mem_miss = a.pos();
        a.lw(K1, K3, 0); // meta
        a.andi(K2, K1, 0x3FF);
        a.slli(K2, K2, 2);
        a.sub(K2, K3, K2);
        a.lw(K0, K2, 0); // pc0 from the header
        a.srli(K2, K1, 10);
        a.andi(K2, K2, 63); // k
        a.slli(K3, K2, 2);
        a.add(K0, K0, K3); // pc = pc0 + 4k
        a.srli(K1, K1, 16); // n
        a.sub(K1, K1, K2);
        a.lw(K2, S, OFF_BUDGET);
        a.add(K2, K2, K1);
        a.sw(K2, S, OFF_BUDGET);
        a.addi(K1, ZERO, EXIT_INTERP as i32);
        jump!(r.exit);

        // indirect jump with the target in the word at K1
        r.ijmp_imm = a.pos();
        a.lw(K0, K1, 0);
        // falls through into ijmp: indirect jump to guest K0 via the
        // fetch-TLB check and the jump cache
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
        let miss = a.pos();
        a.fix(miss1, miss);
        a.fix(miss2, miss);
        a.fix(miss3, miss);
        a.addi(K1, ZERO, EXIT_JUMP as i32);
        jump!(r.exit);

        // TLB helpers: K0 = guest va -> host address, K3 = meta word.
        // Hit returns to K3 + 4; miss goes to mem_miss.
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
                a.jalr(ZERO, K3, 4);
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
        self.bhead = 0;
        self.nblocks = 0;
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
        g.nblk = [0; MAX_REGIONS];
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
            if self.pos + need > self.code.len() || self.nblocks == self.blocks.len() {
                self.flush();
            }
            return;
        }
        let end = self.code_start + (self.ring.cur + 1) * self.ring.region_words;
        if self.pos + need <= end && self.nblocks < self.blocks.len() {
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
        if self.nblocks == self.blocks.len() {
            // one region holds every descriptor (no-code blocks)
            self.flush();
        }
    }

    /// Drop region `r`'s blocks: descriptors out of the hash table, logged
    /// chains into it back to chain requests, jump-cache entries into it.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn evict_region(&mut self, r: usize) {
        let nb = self.ring.nblk[r] as usize;
        for k in 0..nb {
            let d = (self.bhead + k) % self.blocks.len();
            let (va, pa) = (self.blocks[d].va, self.blocks[d].pa);
            let h = self.hash(va, pa);
            let mut prev = EMPTY;
            let mut i = self.buckets[h];
            while i != EMPTY {
                let next = self.blocks[i as usize].next;
                if i as usize == d {
                    if prev == EMPTY {
                        self.buckets[h] = next;
                    } else {
                        self.blocks[prev as usize].next = next;
                    }
                    break;
                }
                prev = i;
                i = next;
            }
        }
        self.bhead = (self.bhead + nb) % self.blocks.len();
        self.nblocks -= nb;
        self.ring.nblk[r] = 0;
        let base = r * self.ring.log_per;
        for k in 0..self.ring.nlog[r] as usize {
            let slot = self.ring.log[base + k] as usize;
            self.code[slot] = j_type(K1, (self.r.chain_req as i32 - slot as i32) * 4);
        }
        self.ring.nlog[r] = 0;
        self.ring.seq[r] = 0;
        let lo = self.code_addr((self.code_start + r * self.ring.region_words) as u32) as u32;
        let hi = lo + (self.ring.region_words * 4) as u32;
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
        ((va ^ pa.rotate_left(16)) >> 2).wrapping_mul(0x9E37_79B1) as usize >> self.bucket_shift
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn lookup(&self, va: u32, pa: u32) -> Option<u32> {
        let mut i = self.buckets[self.hash(va, pa)];
        while i != EMPTY {
            let b = &self.blocks[i as usize];
            if b.va == va && b.pa == pa {
                return Some(b.code);
            }
            i = b.next;
        }
        None
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn insert(&mut self, va: u32, pa: u32, code: u32) {
        let h = self.hash(va, pa);
        let i = (self.bhead + self.nblocks) % self.blocks.len();
        self.blocks[i] = Block { va, pa, code, next: self.buckets[h] };
        self.buckets[h] = i as u32;
        self.nblocks += 1;
        self.ring.nblk[self.ring.cur] += 1;
    }

    /// Drop every block translated from physical page `ppage`.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn invalidate_page(&mut self, cpu: &Cpu, ppage: u32) {
        for h in 0..self.buckets.len() {
            let mut prev = EMPTY;
            let mut i = self.buckets[h];
            while i != EMPTY {
                let next = self.blocks[i as usize].next;
                if self.blocks[i as usize].pa & !0xFFF == ppage {
                    if prev == EMPTY {
                        self.buckets[h] = next;
                    } else {
                        self.blocks[prev as usize].next = next;
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
                self.store_append(cpu, pc0, pa0, r as usize - 1);
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
        // data word after a chain / jump / interp request or a helper call
        let r = self.r;
        let mut i = 1;
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
            if rd == K3 || (rd == K1 && (t == r.chain_req || t == r.ijmp_imm || t == r.interp_exit)) {
                i += 1;
            }
        }
        self.pos = at + words;
        fence_i();
        self.stats.restores += 1;
        self.stats.restore_words += words as u64;
        self.insert(va, pa, (at + 1) as u32);
        (at + 1) as u32
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn translate_inner(&mut self, cpu: &mut Cpu, pc0: u32, pa0: u32) -> u32 {
        // pass 1: find the block
        let Scan { insns, kinds, n, end_interp } = scan(cpu, pc0, pa0);
        let pc = pc0.wrapping_add(4 * n as u32);

        // room for the worst case (measuring never keeps the block)
        if !self.measure_only {
            self.make_room(n * 12 + 16);
        }
        if n == 0 {
            self.insert(pc0, pa0, NO_CODE);
            return NO_CODE;
        }
        if !self.measure_only {
            self.mark_code_page(cpu, pa0);
        }

        let r = self.r;
        let mut a = Asm::at(self.code, self.code_base, self.pos);

        // header word (guest pc), then the entry point
        let header = a.emit(pc0);
        let start = a.pos();
        let same_page = |t: u32| t & !0xFFF == pc0 & !0xFFF;

        macro_rules! call {
            ($link:expr, $to:expr) => {{
                let c = a.emit(j_type($link, 0));
                a.fix(c, $to);
            }};
        }
        // jump to a known guest pc: chainable slot if same page, else jump cache
        macro_rules! goto_pc {
            ($target:expr) => {{
                let t: u32 = $target;
                call!(K1, if same_page(t) { r.chain_req } else { r.ijmp_imm });
                a.emit(t);
            }};
        }
        // `jal K3, to; .word meta` for instruction k
        macro_rules! call_meta {
            ($to:expr, $k:expr) => {{
                call!(K3, $to);
                let here = a.pos();
                a.emit(meta(here - header, $k, n));
            }};
        }

        // prologue: budget check
        a.lw(K0, S, OFF_BUDGET);
        a.addi(K0, K0, -(n as i32));
        a.emit(b_type(5, K0, ZERO, 8)); // bge K0, zero, +8
        call!(K1, r.budget_exit);
        a.sw(K0, S, OFF_BUDGET);

        for k in 0..n {
            let insn = insns[k];
            let pc = pc0.wrapping_add(4 * k as u32);
            let rd = (insn >> 7) & 31;
            let rs1 = (insn >> 15) & 31;
            let rs2 = (insn >> 20) & 31;
            let f3 = (insn >> 12) & 7;
            match kinds[k] {
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
                    let store = kinds[k] == Kind::Store;
                    let imm = if store { imm_s(insn) } else { imm_i(insn) };
                    if spilled(rs1) {
                        a.lw(K0, S, xoff(rs1));
                        if imm != 0 {
                            a.addi(K0, K0, imm);
                        }
                    } else {
                        a.addi(K0, rs1, imm);
                    }
                    call_meta!(r.helper[store as usize][(f3 & 3) as usize], k);
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
                    a.emit(b_type(0, K1, ZERO, 12)); // beq K1, zero, +12
                    call_meta!(r.mem_miss, k);
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
                    a.emit(b_type(0, K1, ZERO, 12)); // beq K1, zero, +12
                    call_meta!(r.mem_miss, k);
                    call_meta!(r.helper[1][2], k);
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
        let last = kinds[n - 1];
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
        assert!(end - header < 1024, "block too long for meta words");
        self.pos = end;
        fence_i();

        self.stats.blocks += 1;
        self.stats.guest_insns_translated += n as u64;
        self.stats.code_words += (end - header) as u64;
        self.insert(pc0, pa0, start as u32);
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
        let executed = (budget - cpu.jit.budget) as u64;
        cpu.instret += executed;
        self.stats.jit_insns += executed;
        self.stats.entries += 1;
        cpu.pc = cpu.jit.exit_pc;
        let info = cpu.jit.exit_info;
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
        let sc = scan(cpu, va, pa);
        let end = match sc.n {
            0 => BlockEnd::Interp,
            n => match sc.kinds[n - 1] {
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
        let (pos, nblocks, nblk) = (self.pos, self.nblocks, self.ring.nblk);
        let h = self.hash(va, pa);
        let head = self.buckets[h];
        let stats = self.stats;
        self.measure_only = true;
        self.translate_inner(cpu, va, pa);
        self.measure_only = false;
        let words = (self.pos - pos) as u32;
        self.pos = pos;
        self.nblocks = nblocks;
        self.ring.nblk = nblk;
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
    pub fn blocks_live(&self) -> usize {
        self.nblocks
    }
}
