//! Hart state and the RV32IMA interpreter.
//!
//! There is no M-mode: the kernel runs in S-mode and its ecalls are handled
//! in `sbi.rs`. Every memory access goes through the soft TLB in `mmu.rs`.

use crate::instrument::{Instrument, Stats};

/// Guest timebase. Time is 1 tick per retired instruction plus ticks skipped
/// while idle, so runs are deterministic.
pub const TIMEBASE_HZ: u32 = 25_000_000;

pub const TLB_BITS: u32 = 8;
pub const TLB_SIZE: usize = 1 << TLB_BITS;

pub const ACC_R: usize = 0;
pub const ACC_W: usize = 1;
pub const ACC_X: usize = 2;
pub const ACC_N: usize = 3;

/// Permission contexts with separate TLB sets.
pub const MMU_U: usize = 0;
pub const MMU_S: usize = 1;
pub const MMU_S_SUM: usize = 2;
pub const MMU_N: usize = 3;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Priv {
    U = 0,
    S = 1,
}

pub mod exc {
    pub const INSN_MISALIGN: u32 = 0;
    pub const INSN_FAULT: u32 = 1;
    pub const ILLEGAL: u32 = 2;
    pub const BREAKPOINT: u32 = 3;
    pub const LOAD_MISALIGN: u32 = 4;
    pub const LOAD_FAULT: u32 = 5;
    pub const STORE_MISALIGN: u32 = 6;
    pub const STORE_FAULT: u32 = 7;
    pub const ECALL_U: u32 = 8;
    pub const INSN_PFAULT: u32 = 12;
    pub const LOAD_PFAULT: u32 = 13;
    pub const STORE_PFAULT: u32 = 15;
}

pub const CAUSE_IRQ: u32 = 0x8000_0000;
pub const IRQ_SSI: u32 = 1;
pub const IRQ_STI: u32 = 5;
pub const IRQ_SEI: u32 = 9;
pub const MIP_SSIP: u32 = 1 << IRQ_SSI;
pub const MIP_STIP: u32 = 1 << IRQ_STI;
pub const MIP_SEIP: u32 = 1 << IRQ_SEI;

/// Harts a machine can have.
pub const MAX_HARTS: usize = 2;
/// SBI HSM states (`Smp::hsm`).
pub const HSM_STARTED: u32 = 0;
pub const HSM_STOPPED: u32 = 1;
pub const HSM_START_PENDING: u32 = 2;
/// Remote fence requests (`Smp::fence`): an sfence.vma.
pub const FENCE_VMA: u32 = 1;

/// What the harts of a multi-hart machine share: words only, so each hart
/// may run on its own host core. A single-hart machine has none
/// (`Cpu::smp` is null) and never looks at any of this.
#[repr(C)]
pub struct Smp {
    pub nharts: u32,
    /// An IPI is pending for the hart (sets its SSIP at its next check).
    pub ipi: [core::sync::atomic::AtomicU32; MAX_HARTS],
    /// Remote fence requests for the hart (`FENCE_*`), cleared when done.
    pub fence: [core::sync::atomic::AtomicU32; MAX_HARTS],
    /// HSM state (`HSM_*`), and where a pending start begins.
    pub hsm: [core::sync::atomic::AtomicU32; MAX_HARTS],
    pub start_pc: [core::sync::atomic::AtomicU32; MAX_HARTS],
    pub start_arg: [core::sync::atomic::AtomicU32; MAX_HARTS],
    /// Each hart's guest time at its last check (lo, hi; hi written last).
    /// A hart's time only moves forward, and never trails another's by more
    /// than one check: `time` is `max` over the harts.
    pub time: [[core::sync::atomic::AtomicU32; 2]; MAX_HARTS],
}

impl Smp {
    pub const fn new(nharts: u32) -> Self {
        use core::sync::atomic::AtomicU32 as A;
        Smp {
            nharts,
            ipi: [const { A::new(0) }; MAX_HARTS],
            fence: [const { A::new(0) }; MAX_HARTS],
            hsm: [A::new(HSM_STARTED), A::new(HSM_STOPPED)],
            start_pc: [const { A::new(0) }; MAX_HARTS],
            start_arg: [const { A::new(0) }; MAX_HARTS],
            time: [const { [A::new(0), A::new(0)] }; MAX_HARTS],
        }
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn time_of(&self, h: usize) -> u64 {
        use core::sync::atomic::Ordering::Acquire;
        let hi = self.time[h][1].load(Acquire);
        let lo = self.time[h][0].load(Acquire);
        (u64::from(hi) << 32) | u64::from(lo)
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn publish_time(&self, h: usize, t: u64) {
        use core::sync::atomic::Ordering::Release;
        self.time[h][0].store(t as u32, Release);
        self.time[h][1].store((t >> 32) as u32, Release);
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Halt {
    Shutdown,
    Until,
    MaxInsns,
    Idle,
    User,
    /// The guest kernel panicked (the host saw its message).
    Panic,
    /// SBI HSM hart_stop (a secondary hart; the host parks it).
    Stopped,
}

/// One soft-TLB slot: `host = va + addend` for the page `tag`. An empty slot
/// holds a page number that cannot index it, so a hit is one xor + shift.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct TlbEntry {
    pub tag: u32,
    pub addend: usize,
}

/// Everything outside the hart: MMIO devices, console, idle policy.
pub trait Bus {
    /// Optional host-only measurement service; independent of native crypto.
    fn benchmark_supported(&self) -> bool { false }
    fn benchmark_record(&mut self, _cpu: &Cpu, _tag: u32) -> bool { false }
    /// Optional: print the recorded points and host diagnostics, then clear
    /// the log. `None`: not provided (the SBI call fails NOT_SUPPORTED).
    fn benchmark_report(&mut self, _cpu: &Cpu) -> Option<u32> { None }
    /// Whether a native operation that may pend runs preemptibly: the bus
    /// arms an interrupt in `native_slice_begin` whose handler calls
    /// `rvaccel::Workspace::preempt`.
    fn native_preemptible(&self) -> bool { false }
    /// Around every slice of a native operation (also one that runs to
    /// completion): arm and disarm the preemption timer, account the time.
    fn native_slice_begin(&mut self) {}
    fn native_slice_end(&mut self, _cpu: &mut Cpu) {}
    fn mmio_load(&mut self, pa: u32, size: u32) -> Result<u32, u32>;
    fn mmio_store(&mut self, pa: u32, size: u32, val: u32) -> Result<(), u32>;
    /// Level of the PLIC's S-mode external interrupt output.
    fn external_irq(&self) -> bool;
    /// The same for hart `hart`'s context (multi-hart machines).
    #[inline(always)]
    fn external_irq_hart(&self, hart: u32) -> bool {
        hart == 0 && self.external_irq()
    }
    /// Multi-hart: hart `hart` has something new (an IPI, a fence request,
    /// a start): wake it if it waits.
    fn kick(&mut self, _hart: u32) {}
    /// Multi-hart: make the next few accesses to guest RAM atomic with
    /// respect to every other hart's accesses, plain stores included (an
    /// AMO's or SC's read-modify-write), and end that.
    fn atomic_begin(&mut self) {}
    fn atomic_end(&mut self) {}
    /// Multi-hart: whether the harts run at once, on host cores of their
    /// own. Then a hart that asks another for a fence waits until it is
    /// done (`Smp::fence` back to 0), calling `fence_wait` meanwhile. A host
    /// that runs the harts in turn needs no wait: the other hart takes the
    /// request before its next instruction.
    fn concurrent_harts(&self) -> bool {
        false
    }
    /// One pause in such a wait.
    fn fence_wait(&mut self) {}
    /// Multi-hart: whether hart `hart` may be running guest code now. One
    /// that isn't takes its fence requests (`Cpu::smp_poll`) before it runs
    /// again, so a fence for it needn't wait.
    fn hart_running(&self, _hart: u32) -> bool {
        true
    }
    /// Around rarely used host code (a native operation, HSM calls) that a
    /// host may keep out of fast memory and need to bracket.
    fn cold_begin(&mut self) {}
    fn cold_end(&mut self) {}
    fn console_putc(&mut self, b: u8);
    fn console_getc(&mut self) -> Option<u8>;
    /// Guest executed WFI. May advance `cpu.time_skip` or set `cpu.halt`.
    fn wfi(&mut self, cpu: &mut Cpu);
    /// Guest RAM a device wrote since the last call (guest physical [lo, hi)),
    /// so a translator can drop code translated from it.
    fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        None
    }
}

/// Fields the block translator's generated code reads and writes. Kept
/// right after `x`/`pc` so they sit at small offsets from the state pointer.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JitRegs {
    /// Instructions the current entry into translated code may still retire.
    pub budget: i32,
    /// Guest pc and exit reason written by the exit stub.
    pub exit_pc: u32,
    pub exit_info: u32,
    /// Host stack pointer saved by the entry trampoline.
    pub host_sp: usize,
    /// `&tlb[mmu_idx][acc][0]` for the current permission context.
    pub tlb_cur: [usize; ACC_N],
    /// Translator's jump cache (owned by the translator; 0 if none).
    pub jc_base: usize,
    /// Pending and enabled interrupts (`sip & sie`) at the last
    /// `check_irq`, which the dispatcher calls before entering translated
    /// code; they cannot change while it runs. Translated code that sets
    /// sstatus.SIE exits to the dispatcher if any.
    pub irq_pending: u32,
    /// Translator dispatcher state of this hart: interpret the next
    /// instruction; the block the next step starts in (pc, host, code), as
    /// chain() looked it up, valid while the translator's generation is
    /// `next_gen`.
    pub force_interp: bool,
    pub next_valid: bool,
    pub next_pc: u32,
    pub next_host: usize,
    pub next_code: u32,
    pub next_gen: u32,
    /// Multi-hart AMOs in translated code: what the lock routine stores to
    /// lock the other harts out (the platform's choice; 0 if unused).
    pub amo_deny: u32,
}

#[repr(C)]
pub struct Cpu {
    pub x: [u32; 32],
    pub pc: u32,
    pub jit: JitRegs,
    pub privilege: Priv,
    pub(crate) mmu_idx: usize,

    pub sie_bit: bool,
    pub spie: bool,
    pub spp: bool,
    pub sum: bool,
    pub mxr: bool,
    pub sie: u32,
    /// Software-held pending bits (SSIP). STIP is derived from the timer and
    /// SEIP from the bus when read.
    pub sip: u32,
    pub stvec: u32,
    pub sscratch: u32,
    pub sepc: u32,
    pub scause: u32,
    pub stval: u32,
    pub satp: u32,
    pub scounteren: u32,
    pub stimecmp: u64,
    pub instret: u64,
    pub time_skip: u64,
    /// LR/SC reservation (a translator does LR/SC itself)
    pub res_valid: bool,
    pub res_addr: u32,
    /// The value LR.W loaded: with several harts SC.W succeeds only if the
    /// word still holds it (checked inside the bus's atomic section).
    pub res_val: u32,
    /// This hart's id, and what it shares with the others (null: the only
    /// hart).
    pub hartid: u32,
    pub smp: *const Smp,

    pub(crate) ram: *mut u8,
    pub(crate) ram_base: u32,
    pub(crate) ram_size: u32,
    /// The first `ro_size` bytes of RAM are read-only (the RP2350 backs the
    /// kernel's text with flash there): stores to them fault.
    pub(crate) ro_size: u32,

    /// Translator support: one bit per RAM page that holds translated code.
    /// Null when no translator is attached. Writes to such pages never get
    /// a write-TLB entry; the first one sets `smc_page`.
    pub code_pages: *const u8,
    pub smc_page: Option<u32>,
    #[cfg(feature = "pv-buffers")]
    pub(crate) native_write: Option<(u32, u32)>,
    /// Output range of the native operation in progress (pv-buffers).
    pub(crate) native_job_dst: Option<(u32, u32)>,
    #[cfg(feature = "pv-buffers")]
    pub(crate) native_workspace: *mut rvaccel::Workspace,

    pub halt: Option<Halt>,
    pub stats: Stats,
    pub inst: Instrument,

    pub(crate) tlb: [[[TlbEntry; TLB_SIZE]; ACC_N]; MMU_N],
}

#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_i(insn: u32) -> u32 {
    ((insn as i32) >> 20) as u32
}
#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_s(insn: u32) -> u32 {
    ((((insn as i32) >> 25) << 5) as u32) | ((insn >> 7) & 0x1F)
}
#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_b(insn: u32) -> u32 {
    ((((insn as i32) >> 31) << 12) as u32)
        | ((insn << 4) & 0x800)
        | ((insn >> 20) & 0x7E0)
        | ((insn >> 7) & 0x1E)
}
#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_j(insn: u32) -> u32 {
    ((((insn as i32) >> 31) << 20) as u32)
        | (insn & 0xF_F000)
        | ((insn >> 9) & 0x800)
        | ((insn >> 20) & 0x7FE)
}

/// A fresh `Cpu`'s fields but its TLB, handed to `$m` (`Cpu::fresh` builds
/// the struct from them, `Cpu::init` writes them in place).
macro_rules! cpu_fields {
    ($m:ident, $ram:expr, $ram_base:expr, $ram_size:expr, $entry:expr) => {
        $m! {
            #[cfg(feature = "pv-buffers")]
            native_workspace: core::ptr::null_mut(),
            x: [0; 32],
            pc: $entry,
            jit: JitRegs {
                budget: 0,
                exit_pc: 0,
                exit_info: 0,
                host_sp: 0,
                tlb_cur: [0; ACC_N],
                jc_base: 0,
                irq_pending: 0,
                force_interp: false,
                next_valid: false,
                next_pc: 0,
                next_host: 0,
                next_code: 0,
                next_gen: 0,
                amo_deny: 0,
            },
            privilege: Priv::S,
            mmu_idx: MMU_S,
            sie_bit: false,
            spie: false,
            spp: false,
            sum: false,
            mxr: false,
            sie: 0,
            sip: 0,
            stvec: 0,
            sscratch: 0,
            sepc: 0,
            scause: 0,
            stval: 0,
            satp: 0,
            scounteren: 0,
            stimecmp: u64::MAX,
            instret: 0,
            time_skip: 0,
            res_valid: false,
            res_addr: 0,
            res_val: 0,
            hartid: 0,
            smp: core::ptr::null(),
            ram: $ram,
            ram_base: $ram_base,
            ram_size: $ram_size,
            ro_size: 0,
            code_pages: core::ptr::null(),
            smc_page: None,
            #[cfg(feature = "pv-buffers")]
            native_write: None,
            native_job_dst: None,
            halt: None,
            stats: Stats::new(),
            inst: Instrument::new($ram_size),
        }
    };
}

impl Cpu {
    /// # Safety
    /// `ram` must point to `ram_size` writable bytes that outlive the `Cpu`
    /// and are not accessed through other references while it runs.
    pub unsafe fn new(ram: *mut u8, ram_base: u32, ram_size: u32, entry: u32, dtb: u32) -> Self {
        let mut c = Self::fresh(ram, ram_base, ram_size, entry);
        c.boot_regs(dtb);
        c
    }

    /// `new`, written straight into `place` and returned from there. The
    /// struct is ~20 KB (mostly TLB): it is written field by field and the
    /// TLB entry by entry, so no frame or temporary of that size is needed
    /// whatever the inliner does (the RP2350's stacks are a few KB).
    ///
    /// # Safety
    /// As `new`; `place` must be valid for writes and not in use.
    pub unsafe fn init<'a>(place: *mut Cpu, ram: *mut u8, ram_base: u32, ram_size: u32, entry: u32, dtb: u32) -> &'a mut Cpu {
        macro_rules! put {
            ($($(#[$a:meta])* $f:ident: $v:expr,)*) => {
                $(
                    $(#[$a])*
                    // SAFETY: a field of `place` (per the contract).
                    unsafe { core::ptr::addr_of_mut!((*place).$f).write($v) };
                )*
            };
        }
        cpu_fields!(put, ram, ram_base, ram_size, entry);
        // SAFETY: as above.
        let tlb = unsafe { core::ptr::addr_of_mut!((*place).tlb).cast::<TlbEntry>() };
        for i in 0..MMU_N * ACC_N * TLB_SIZE {
            // SAFETY: inside the TLB array.
            unsafe { tlb.add(i).write(TlbEntry { tag: 0, addend: 0 }) };
        }
        // SAFETY: every field is written.
        let c = unsafe { &mut *place };
        c.boot_regs(dtb);
        c
    }

    #[inline(always)]
    fn fresh(ram: *mut u8, ram_base: u32, ram_size: u32, entry: u32) -> Self {
        macro_rules! lit {
            ($($(#[$a:meta])* $f:ident: $v:expr,)*) => {
                Cpu { $($(#[$a])* $f: $v,)* tlb: [[[TlbEntry { tag: 0, addend: 0 }; TLB_SIZE]; ACC_N]; MMU_N] }
            };
        }
        cpu_fields!(lit, ram, ram_base, ram_size, entry)
    }

    /// A native buffer call wrote guest RAM (`take_native_write` has it).
    #[inline(always)]
    pub fn has_native_write(&self) -> bool {
        #[cfg(feature = "pv-buffers")]
        {
            self.native_write.is_some()
        }
        #[cfg(not(feature = "pv-buffers"))]
        {
            false
        }
    }

    /// Physical output range from a native buffer call, including failed calls.
    #[inline(always)]
    pub fn take_native_write(&mut self) -> Option<(u32, u32)> {
        #[cfg(feature = "pv-buffers")]
        {
            self.native_write.take()
        }
        #[cfg(not(feature = "pv-buffers"))]
        {
            None
        }
    }

    fn boot_regs(&mut self, dtb: u32) {
        self.x[10] = 0; // hart id
        self.x[11] = dtb;
        self.tlb_flush_all();
        self.stats.tlb_flushes = 0;
    }

    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn time(&self) -> u64 {
        self.instret + self.time_skip
    }

    /// Pending interrupt bits as software sees them in `sip`.
    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn sip_value<B: Bus>(&self, bus: &B) -> u32 {
        let mut p = self.sip;
        if self.time() >= self.stimecmp {
            p |= MIP_STIP;
        }
        if bus.external_irq_hart(self.hartid) {
            p |= MIP_SEIP;
        }
        p
    }

    pub(crate) fn update_mmu_idx(&mut self) {
        self.mmu_idx = match (self.privilege, self.sum) {
            (Priv::U, _) => MMU_U,
            (Priv::S, false) => MMU_S,
            (Priv::S, true) => MMU_S_SUM,
        };
        self.sync_tlb_ptrs();
    }

    /// Refresh `jit.tlb_cur`. Needed after the `Cpu` is moved.
    pub fn sync_tlb_ptrs(&mut self) {
        for acc in 0..ACC_N {
            self.jit.tlb_cur[acc] = self.tlb[self.mmu_idx][acc].as_ptr() as usize;
        }
    }

    /// Instructions that can retire before a timer interrupt could become
    /// pending (u64::MAX if none can, or one already is).
    pub fn timer_deadline(&self) -> u64 {
        let t = self.time();
        if self.stimecmp > t { self.stimecmp - t } else { u64::MAX }
    }

    pub fn ram_ptr(&self) -> *mut u8 {
        self.ram
    }
    pub fn ram_base(&self) -> u32 {
        self.ram_base
    }
    pub fn ram_size(&self) -> u32 {
        self.ram_size
    }
    /// Make the first `bytes` of RAM (page aligned) read-only to the guest.
    pub fn set_readonly_prefix(&mut self, bytes: u32) {
        self.ro_size = bytes & !0xFFF;
        self.tlb_flush_all();
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn trap(&mut self, cause: u32, tval: u32) {
        if cause & CAUSE_IRQ != 0 {
            self.stats.interrupts += 1;
        } else {
            self.stats.exceptions += 1;
        }
        self.scause = cause;
        self.stval = tval;
        self.sepc = self.pc;
        self.spp = self.privilege == Priv::S;
        self.spie = self.sie_bit;
        self.sie_bit = false;
        self.privilege = Priv::S;
        self.update_mmu_idx();
        self.res_valid = false;
        let base = self.stvec & !3;
        self.pc = if self.stvec & 1 != 0 && cause & CAUSE_IRQ != 0 {
            base.wrapping_add(4 * (cause & !CAUSE_IRQ))
        } else {
            base
        };
    }

    /// Take a pending, enabled interrupt if there is one.
    #[inline]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn check_irq<B: Bus>(&mut self, bus: &B) -> bool {
        if !self.smp.is_null() {
            self.smp_poll();
        }
        let p = self.sip_value(bus) & self.sie;
        // for translated code that sets SIE (rvjit)
        self.jit.irq_pending = p;
        if p == 0 || (self.privilege == Priv::S && !self.sie_bit) {
            return false;
        }
        let irq = if p & MIP_SEIP != 0 {
            IRQ_SEI
        } else if p & MIP_SSIP != 0 {
            IRQ_SSI
        } else {
            IRQ_STI
        };
        self.trap(CAUSE_IRQ | irq, 0);
        true
    }

    /// Multi-hart: take IPIs and remote fences sent to this hart, and keep
    /// its time with the other harts'.
    #[inline(never)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn smp_poll(&mut self) {
        use core::sync::atomic::Ordering::{AcqRel, Relaxed};
        // SAFETY: non-null `smp` points to the machine's shared block.
        let smp = unsafe { &*self.smp };
        let h = self.hartid as usize;
        if smp.ipi[h].load(Relaxed) != 0 && smp.ipi[h].swap(0, AcqRel) != 0 {
            self.sip |= MIP_SSIP;
        }
        if smp.fence[h].load(Relaxed) != 0 {
            let f = smp.fence[h].swap(0, AcqRel);
            if f & FENCE_VMA != 0 {
                self.tlb_flush_all();
            }
        }
        self.smp_sync_time(smp);
    }

    /// `time = max` over the harts; publish this hart's.
    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn smp_sync_time(&mut self, smp: &Smp) {
        let h = self.hartid as usize;
        let mut t = self.time();
        for o in 0..smp.nharts as usize {
            if o != h {
                let ot = smp.time_of(o);
                if ot > t {
                    self.time_skip += ot - t;
                    t = ot;
                }
            }
        }
        smp.publish_time(h, t);
    }

    /// Start this (secondary) hart as SBI HSM hart_start asked: S-mode,
    /// translation off, a0 = hartid, a1 = the opaque argument.
    pub fn hsm_start(&mut self, pc: u32, arg: u32) {
        self.pc = pc;
        self.x = [0; 32];
        self.x[10] = self.hartid;
        self.x[11] = arg;
        self.privilege = Priv::S;
        self.sie_bit = false;
        self.spie = false;
        self.spp = false;
        self.sum = false;
        self.mxr = false;
        self.sie = 0;
        self.sip = 0;
        self.satp = 0;
        self.stimecmp = u64::MAX;
        self.res_valid = false;
        self.halt = None;
        self.update_mmu_idx();
        self.tlb_flush_all();
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn sret(&mut self) {
        self.privilege = if self.spp { Priv::S } else { Priv::U };
        self.sie_bit = self.spie;
        self.spie = true;
        self.spp = false;
        self.update_mmu_idx();
        if self.privilege == Priv::U && self.stats.user_instret == 0 {
            self.stats.user_instret = self.instret + 1;
            self.stats.user_unique_insns = self.stats.unique_insns;
        }
    }

    // ------------------------------------------------------------ CSRs

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn csr_read<B: Bus>(&self, bus: &B, csr: u32) -> Result<u32, u32> {
        match csr {
            0xC00 | 0xC02 => return Ok(self.instret as u32),
            0xC80 | 0xC82 => return Ok((self.instret >> 32) as u32),
            0xC01 => return Ok(self.time() as u32),
            0xC81 => return Ok((self.time() >> 32) as u32),
            _ => {}
        }
        if self.privilege != Priv::S {
            return Err(exc::ILLEGAL);
        }
        Ok(match csr {
            0x100 => self.sstatus(),
            0x104 => self.sie,
            0x105 => self.stvec,
            0x106 => self.scounteren,
            0x140 => self.sscratch,
            0x141 => self.sepc,
            0x142 => self.scause,
            0x143 => self.stval,
            0x144 => self.sip_value(bus),
            0x180 => self.satp,
            _ => return Err(exc::ILLEGAL),
        })
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn sstatus(&self) -> u32 {
        (self.sie_bit as u32) << 1
            | (self.spie as u32) << 5
            | (self.spp as u32) << 8
            | (self.sum as u32) << 18
            | (self.mxr as u32) << 19
    }

    /// `csrrw` (op 1), `csrrs` (2) or `csrrc` (3) of sstatus with operand
    /// `src`, in S-mode, for a translator.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn sstatus_op(&mut self, op: u32, src: u32) {
        let old = self.sstatus();
        let nv = match op {
            1 => src,
            2 => old | src,
            _ => old & !src,
        };
        let _ = self.csr_write(0x100, nv);
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn csr_write(&mut self, csr: u32, v: u32) -> Result<(), u32> {
        if self.privilege != Priv::S || (csr >> 10) == 3 {
            return Err(exc::ILLEGAL);
        }
        match csr {
            0x100 => {
                let old_mxr = self.mxr;
                self.sie_bit = v & (1 << 1) != 0;
                self.spie = v & (1 << 5) != 0;
                self.spp = v & (1 << 8) != 0;
                self.sum = v & (1 << 18) != 0;
                self.mxr = v & (1 << 19) != 0;
                if self.mxr != old_mxr {
                    self.tlb_flush_all();
                }
                self.update_mmu_idx();
            }
            0x104 => self.sie = v & (MIP_SSIP | MIP_STIP | MIP_SEIP),
            0x105 => self.stvec = v & !2,
            0x106 => self.scounteren = v,
            0x140 => self.sscratch = v,
            0x141 => self.sepc = v & !3,
            0x142 => self.scause = v,
            0x143 => self.stval = v,
            0x144 => self.sip = (self.sip & !MIP_SSIP) | (v & MIP_SSIP),
            0x180 => {
                // Sv32 or bare; ASID bits not implemented (read back as 0)
                self.satp = if v >> 31 != 0 { v & !(0x1FF << 22) } else { 0 };
                self.tlb_flush_all();
            }
            _ => return Err(exc::ILLEGAL),
        }
        Ok(())
    }

    // ------------------------------------------------------------ execute

    /// Execute one instruction (or take the exception it raises).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn step<B: Bus>(&mut self, bus: &mut B) {
        let (insn, off) = match self.fetch(self.pc) {
            Ok(v) => v,
            Err(e) => return self.trap(e, self.pc),
        };
        self.inst.fetch(&mut self.stats, off);
        if let Err(e) = self.exec(bus, insn) {
            let (cause, tval) = e;
            self.trap(cause, tval);
        }
    }

    /// Ok(()) retires the instruction; Err((cause, tval)) traps.
    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn exec<B: Bus>(&mut self, bus: &mut B, insn: u32) -> Result<(), (u32, u32)> {
        let opcode = insn & 0x7F;
        let rd = ((insn >> 7) & 31) as usize;
        let rs1 = ((insn >> 15) & 31) as usize;
        let rs2 = ((insn >> 20) & 31) as usize;
        let f3 = (insn >> 12) & 7;
        let f7 = insn >> 25;
        let a = self.x[rs1];
        let b = self.x[rs2];
        #[cfg(feature = "instrument")]
        {
            let (r, s1, s2) = match opcode {
                0x37 | 0x17 | 0x6F => (true, false, false),
                0x67 | 0x03 | 0x13 => (true, true, false),
                0x63 | 0x23 => (false, true, true),
                0x33 | 0x2F => (true, true, true),
                _ => (false, false, false),
            };
            if r { self.stats.reg_use[rd] += 1; }
            if s1 { self.stats.reg_use[rs1] += 1; }
            if s2 { self.stats.reg_use[rs2] += 1; }
        }
        let pc = self.pc;
        let mut next = pc.wrapping_add(4);
        let illegal = Err((exc::ILLEGAL, insn));

        let val: Option<u32> = match opcode {
            0x37 => Some(insn & 0xFFFF_F000),
            0x17 => Some(pc.wrapping_add(insn & 0xFFFF_F000)),
            0x6F => {
                let link = next;
                next = pc.wrapping_add(imm_j(insn));
                Some(link)
            }
            0x67 => {
                if f3 != 0 {
                    return illegal;
                }
                let link = next;
                next = a.wrapping_add(imm_i(insn)) & !1;
                Some(link)
            }
            0x63 => {
                let t = match f3 {
                    0 => a == b,
                    1 => a != b,
                    4 => (a as i32) < (b as i32),
                    5 => (a as i32) >= (b as i32),
                    6 => a < b,
                    7 => a >= b,
                    _ => return illegal,
                };
                if t {
                    next = pc.wrapping_add(imm_b(insn));
                }
                None
            }
            0x03 => {
                let va = a.wrapping_add(imm_i(insn));
                let size = match f3 {
                    0 | 4 => 1,
                    1 | 5 => 2,
                    2 => 4,
                    _ => return illegal,
                };
                let v = self.mem_load(bus, va, size).map_err(|e| (e, va))?;
                Some(match f3 {
                    0 => v as u8 as i8 as i32 as u32,
                    1 => v as u16 as i16 as i32 as u32,
                    _ => v,
                })
            }
            0x23 => {
                let va = a.wrapping_add(imm_s(insn));
                if f3 > 2 {
                    return illegal;
                }
                self.mem_store(bus, va, 1 << f3, b).map_err(|e| (e, va))?;
                None
            }
            0x13 => {
                let imm = imm_i(insn);
                let sh = rs2 as u32;
                Some(match f3 {
                    0 => a.wrapping_add(imm),
                    1 => {
                        if f7 != 0 {
                            return illegal;
                        }
                        a << sh
                    }
                    2 => ((a as i32) < (imm as i32)) as u32,
                    3 => (a < imm) as u32,
                    4 => a ^ imm,
                    5 => match f7 {
                        0 => a >> sh,
                        0x20 => ((a as i32) >> sh) as u32,
                        _ => return illegal,
                    },
                    6 => a | imm,
                    _ => a & imm,
                })
            }
            0x33 => {
                if f7 == 1 {
                    Some(match f3 {
                        0 => a.wrapping_mul(b),
                        1 => (((a as i32 as i64) * (b as i32 as i64)) >> 32) as u32,
                        2 => (((a as i32 as i64) * (b as u64 as i64)) >> 32) as u32,
                        3 => (((a as u64) * (b as u64)) >> 32) as u32,
                        4 => {
                            if b == 0 {
                                u32::MAX
                            } else if a == 0x8000_0000 && b == u32::MAX {
                                a
                            } else {
                                ((a as i32) / (b as i32)) as u32
                            }
                        }
                        5 => a.checked_div(b).unwrap_or(u32::MAX),
                        6 => {
                            if b == 0 {
                                a
                            } else if a == 0x8000_0000 && b == u32::MAX {
                                0
                            } else {
                                ((a as i32) % (b as i32)) as u32
                            }
                        }
                        _ => a.checked_rem(b).unwrap_or(a),
                    })
                } else {
                    if f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5)) {
                        return illegal;
                    }
                    Some(match f3 {
                        0 => {
                            if f7 != 0 {
                                a.wrapping_sub(b)
                            } else {
                                a.wrapping_add(b)
                            }
                        }
                        1 => a << (b & 31),
                        2 => ((a as i32) < (b as i32)) as u32,
                        3 => (a < b) as u32,
                        4 => a ^ b,
                        5 => {
                            if f7 != 0 {
                                ((a as i32) >> (b & 31)) as u32
                            } else {
                                a >> (b & 31)
                            }
                        }
                        6 => a | b,
                        _ => a & b,
                    })
                }
            }
            0x0F => {
                // FENCE / FENCE.I: no-ops for a single in-order hart
                if f3 > 1 {
                    return illegal;
                }
                None
            }
            0x73 => {
                if f3 == 0 {
                    match insn {
                        0x0000_0073 => {
                            if self.privilege == Priv::U {
                                return Err((exc::ECALL_U, 0));
                            }
                            self.sbi_call(bus);
                        }
                        0x0010_0073 => return Err((exc::BREAKPOINT, pc)),
                        0x1020_0073 => {
                            if self.privilege != Priv::S {
                                return illegal;
                            }
                            next = self.sepc;
                            self.sret();
                        }
                        0x1050_0073 => {
                            if self.privilege != Priv::S {
                                return illegal;
                            }
                            self.stats.wfi += 1;
                            bus.wfi(self);
                        }
                        _ if f7 == 0x09 && rd == 0 => {
                            // SFENCE.VMA: rs1 = 0 flushes everything, otherwise one page
                            if self.privilege != Priv::S {
                                return illegal;
                            }
                            if rs1 == 0 {
                                self.tlb_flush_all();
                            } else {
                                self.tlb_flush_page(a);
                            }
                        }
                        _ => return illegal,
                    }
                    None
                } else if f3 != 4 {
                    let csr = insn >> 20;
                    let src = if f3 & 4 != 0 { rs1 as u32 } else { a };
                    let op = f3 & 3;
                    let mut old = 0;
                    if !(op == 1 && rd == 0) {
                        old = self.csr_read(bus, csr).map_err(|_| (exc::ILLEGAL, insn))?;
                    }
                    if op == 1 || rs1 != 0 {
                        let nv = match op {
                            1 => src,
                            2 => old | src,
                            _ => old & !src,
                        };
                        self.csr_write(csr, nv).map_err(|_| (exc::ILLEGAL, insn))?;
                    }
                    Some(old)
                } else {
                    return illegal;
                }
            }
            0x2F => {
                let f5 = insn >> 27;
                if f3 != 2 {
                    return illegal;
                }
                if a & 3 != 0 {
                    let cause = if f5 == 2 { exc::LOAD_MISALIGN } else { exc::STORE_MISALIGN };
                    return Err((cause, a));
                }
                match f5 {
                    0x02 => {
                        if rs2 != 0 {
                            return illegal;
                        }
                        let v = self.mem_load(bus, a, 4).map_err(|e| (e, a))?;
                        self.res_valid = true;
                        self.res_addr = a;
                        self.res_val = v;
                        Some(v)
                    }
                    0x03 if !self.smp.is_null() => {
                        let r = if self.res_valid && self.res_addr == a {
                            let h = self.amo_ptr(a).map_err(|e| (e, a))?;
                            bus.atomic_begin();
                            // SAFETY: as for the AMOs below.
                            let ok = unsafe { core::ptr::read_volatile(h as *const u32) } == self.res_val;
                            if ok {
                                unsafe { core::ptr::write_volatile(h as *mut u32, b) };
                            }
                            bus.atomic_end();
                            u32::from(!ok)
                        } else {
                            1
                        };
                        self.res_valid = false;
                        Some(r)
                    }
                    0x03 => {
                        let r = if self.res_valid && self.res_addr == a {
                            self.mem_store(bus, a, 4, b).map_err(|e| (e, a))?;
                            0
                        } else {
                            1
                        };
                        self.res_valid = false;
                        Some(r)
                    }
                    _ => {
                        let h = self.amo_ptr(a).map_err(|e| (e, a))?;
                        self.inst.store(self.host_off(h), 4);
                        let smp = !self.smp.is_null();
                        if smp {
                            bus.atomic_begin();
                        }
                        // SAFETY: amo_ptr returned a pointer into guest RAM,
                        // 4-byte aligned within one page.
                        let old = unsafe { core::ptr::read_volatile(h as *const u32) };
                        let nv = match f5 {
                            0x00 => old.wrapping_add(b),
                            0x01 => b,
                            0x04 => old ^ b,
                            0x08 => old | b,
                            0x0C => old & b,
                            0x10 => (old as i32).min(b as i32) as u32,
                            0x14 => (old as i32).max(b as i32) as u32,
                            0x18 => old.min(b),
                            0x1C => old.max(b),
                            _ => {
                                if smp {
                                    bus.atomic_end();
                                }
                                return illegal;
                            }
                        };
                        unsafe { core::ptr::write_volatile(h as *mut u32, nv) };
                        if smp {
                            bus.atomic_end();
                        }
                        Some(old)
                    }
                }
            }
            _ => return illegal,
        };

        if next & 3 != 0 {
            return Err((exc::INSN_MISALIGN, next));
        }
        if let Some(v) = val
            && rd != 0
        {
            self.x[rd] = v;
        }
        self.pc = next;
        self.instret += 1;
        self.stats.op[opcode as usize] += 1;
        Ok(())
    }
}
