//! A second guest hart on core1 (rvemu-pico-shell), when the guest's DTB
//! has two CPUs.
//!
//! Hart 0 runs on core0 as before. Hart 1 runs on core1 in slices after
//! each of core1's I/O passes (`core1::set_hook`), through the same
//! translator and code cache (`Jit::add_hart`): its tables change only
//! under its lock, translated code runs outside it.
//!
//! - **Atomics.** Guest RAM is PSRAM, where the RP2350 has no exclusives, so
//!   an AMO or SC is a load and a store. To keep the other hart's accesses
//!   (plain stores too, as a ticket lock's unlock is) out of the middle, the
//!   hart doing it keeps the other core off XIP (`core1::xip_exclusive`):
//!   the other core's next XIP access takes an access fault, and its trap
//!   entry here (in SRAM) waits for its ACCESSCTRL bit and retries the
//!   access.
//! - **Hart 1's state** (its `Cpu`, jump cache and host stack) is in SRAM,
//!   like hart 0's: the top of the code buffer, which the translator then
//!   doesn't have.
//! - **Fences.** A remote fence.i needs nothing on any hart (a store to
//!   translated code drops its translations as it happens); a remote
//!   sfence.vma waits only while hart 1 runs guest code (`hart1_enter`).
//! - **Devices** are shared, behind one lock (`BusGuard`, a ticket lock, as
//!   an idle hart 0 takes it over and over). Device
//!   interrupts go to the hart the guest routes them to (PLIC contexts 0
//!   and 1). Native operations run on hart 0 only (the guest's driver pins
//!   them to CPU 0; hart 1 has no workspace).
//! - **Idle.** Hart 1 in WFI leaves core1 to its I/O; core1 looks again
//!   after every pass (a doorbell, I/O, or its 1 ms tick). Hart 0 in WFI
//!   waits for hart 1's clock while hart 1 runs, and for the earlier of both
//!   deadlines while both wait.
//! - **Flash code.** A core locked out while it fetches from flash cannot be
//!   resumed (`core1::xip_hold`), so each core runs flash code only while it
//!   holds the lockout's spinlock: the device models behind `BusGuard`,
//!   core1's I/O pass, a native operation (`Bus::cold_begin`), the
//!   translator's LR/SC emitter. Everything else either core runs is in
//!   SRAM.
//! - **Waits** on core1 call `core1::pass()`: core0 may be waiting for core1
//!   to drain the console.

use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use crate::core1;
use rvcore::{Bus, Cpu, HSM_START_PENDING, HSM_STARTED, HSM_STOPPED, Halt, Smp};
use rvjit::{AmoLock, JitSmp};

/// The stack core1 runs hart 1 on (SRAM, with hart 1's Cpu and jump cache).
pub const HART1_STACK_BYTES: usize = 10 * 1024;
/// Most guest instructions one entry into translated code retires: short,
/// so a hart sees the other's IPIs and fence requests soon.
pub const BUDGET_CAP: u64 = 20_000;

/// What translated AMOs lock with; `AMO_DENY[h]` is hart `h`'s
/// `JitRegs::amo_deny`.
pub const AMO_LOCK: AmoLock =
    AmoLock { spin: core1::XIP_SPINLOCK, ctrl: core1::ACCESSCTRL_XIP_MAIN, allow: core1::XIP_ALLOW };
pub const AMO_DENY: [u32; 2] = core1::XIP_DENY_OTHER;

pub static SMP: Smp = Smp::new(2);
pub static JIT_SMP: JitSmp = JitSmp::new();
/// Set once hart 1 is configured: from then on the bus locks.
static ON: AtomicBool = AtomicBool::new(false);
/// Hart 1 waits in WFI.
static HART1_PARKED: AtomicBool = AtomicBool::new(false);
/// Core1 runs hart 1's guest code (in a slice, `hart1_enter`).
static HART1_IN_GUEST: AtomicBool = AtomicBool::new(false);
/// Hart 1's timer deadline while it waits (guest time: lo, hi).
static HART1_DEADLINE: [AtomicU32; 2] = [AtomicU32::new(u32::MAX), AtomicU32::new(u32::MAX)];
/// How hart 1 stopped the machine (`Halt` + 1; 0: it didn't).
static HALT1: AtomicU32 = AtomicU32::new(0);
/// The device lock, a ticket lock (an idle core0 takes it over and over;
/// a plain one would keep hart 1 waiting): the next ticket and the one
/// served.
static BUS_NEXT: AtomicU32 = AtomicU32::new(0);
static BUS_SERVED: AtomicU32 = AtomicU32::new(0);

#[inline(always)]
pub fn on() -> bool {
    ON.load(Ordering::Relaxed)
}

/// Turn the second hart on (after `install_trap0`, before hart 1 runs).
pub fn enable() {
    core1::set_xip_guards();
    ON.store(true, Ordering::Release);
}

// ------------------------------------------------------------------ locks

/// The device lock, held while it lives (only once hart 1 is configured),
/// and with it the permission to run flash code (`core1::xip_hold`; the
/// device models are in flash). The device lock is always taken first.
pub struct BusGuard;

impl BusGuard {
    #[inline(always)]
    pub fn new() -> Self {
        if on() {
            let t = BUS_NEXT.fetch_add(1, Ordering::Relaxed);
            if BUS_SERVED.load(Ordering::Acquire) != t {
                bus_lock_wait(t);
            }
            core1::xip_hold();
        }
        BusGuard
    }
}

impl Drop for BusGuard {
    #[inline(always)]
    fn drop(&mut self) {
        if on() {
            core1::xip_unhold();
            BUS_SERVED.store(BUS_SERVED.load(Ordering::Relaxed).wrapping_add(1), Ordering::Release);
        }
    }
}

#[cold]
#[inline(never)]
#[unsafe(link_section = ".data.rvemu_hot")]
fn bus_lock_wait(ticket: u32) {
    let c1 = core1::on_core1();
    while BUS_SERVED.load(Ordering::Acquire) != ticket {
        if c1 {
            // core0 may hold it while it waits for core1 (park, console)
            core1::pass();
        } else {
            core::hint::spin_loop();
        }
    }
}

/// An interpreted AMO's or SC's read-modify-write by hart `hart` (on core
/// `hart`).
#[inline(always)]
pub fn atomic_begin(hart: usize) {
    core1::xip_exclusive(hart);
}

#[inline(always)]
pub fn atomic_end() {
    core1::xip_release();
}

/// Free the lockout's spinlock and open XIP to everyone (both survive a
/// core reset).
pub fn init_locks() {
    core1::xip_release();
}

// ------------------------------------------------------------------ traps

/// Per-core scratch for the trap entry: t1, t2; then, for faults that find
/// XIP open: how many in a row came straight back at the same pc, that pc,
/// and mcycle at the last one.
#[unsafe(no_mangle)]
static mut SMP_TRAP_SCRATCH0: [u32; 5] = [0; 5];
#[unsafe(no_mangle)]
static mut SMP_TRAP_SCRATCH1: [u32; 5] = [0; 5];

// Each core's trap entry, in SRAM. An access fault (fetch, load, store)
// while the other core holds XIP: wait for this core's ACCESSCTRL bit, then
// return to retry the access. (Hazard3 has no mtval. A fault that finds XIP
// open again, as a short lockout usually does by then, is retried too; a
// real fault comes straight back at the same pc, and 100,000 of those in a
// row, each within 256 cycles of the last, go to riscv-rt's handler, which
// reports them.)
// core0's machine timer (a native operation's slice, the only interrupt it
// takes) is handled here too, in SRAM: a native operation runs holding the
// lockout's spinlock (`Bus::cold_begin`), so no lockout comes in the middle;
// mepc and mstatus are kept anyway. Anything else goes to riscv-rt's
// handler.
macro_rules! trap_entry {
    ($name:literal, $scratch:literal, $bit:literal, $mti:literal) => {
        core::arch::global_asm!(
            concat!(".section .data.rvemu_hot.", $name, ", \"ax\""),
            ".balign 64",
            concat!(".global ", $name),
            concat!($name, ":"),
            "csrw mscratch, t0",
            concat!("la t0, ", $scratch),
            "sw t1, 0(t0)",
            "sw t2, 4(t0)",
            "csrr t1, mcause",
            "li t2, 1",
            "beq t1, t2, 10f",
            "li t2, 5",
            "beq t1, t2, 10f",
            "li t2, 7",
            "beq t1, t2, 10f",
            concat!(".if ", $mti),
            "li t2, 0x80000007",
            "beq t1, t2, 20f",
            ".endif",
            // anything else: riscv-rt's handler, as its own trap entry
            // calls it (that is in flash, out of `j`'s reach)
            "30:",
            "lw t2, 4(t0)",
            "lw t1, 0(t0)",
            "csrr t0, mscratch",
            "addi sp, sp, -64",
            "sw ra, 0(sp)",
            "sw t0, 4(sp)",
            "sw t1, 8(sp)",
            "sw t2, 12(sp)",
            "sw t3, 16(sp)",
            "sw t4, 20(sp)",
            "sw t5, 24(sp)",
            "sw t6, 28(sp)",
            "sw a0, 32(sp)",
            "sw a1, 36(sp)",
            "sw a2, 40(sp)",
            "sw a3, 44(sp)",
            "sw a4, 48(sp)",
            "sw a5, 52(sp)",
            "sw a6, 56(sp)",
            "sw a7, 60(sp)",
            "mv a0, sp",
            "call _start_trap_rust",
            "lw ra, 0(sp)",
            "lw t0, 4(sp)",
            "lw t1, 8(sp)",
            "lw t2, 12(sp)",
            "lw t3, 16(sp)",
            "lw t4, 20(sp)",
            "lw t5, 24(sp)",
            "lw t6, 28(sp)",
            "lw a0, 32(sp)",
            "lw a1, 36(sp)",
            "lw a2, 40(sp)",
            "lw a3, 44(sp)",
            "lw a4, 48(sp)",
            "lw a5, 52(sp)",
            "lw a6, 56(sp)",
            "lw a7, 60(sp)",
            "addi sp, sp, 64",
            "mret",
            // access fault
            "10:",
            "li t1, 0x40060018",
            "lw t2, 0(t1)",
            concat!("andi t2, t2, ", $bit),
            "bnez t2, 14f",
            "13:",
            "lw t2, 0(t1)",
            concat!("andi t2, t2, ", $bit),
            "beqz t2, 13b",
            "j 15f",
            // XIP open: the lockout just ended, or a real fault
            "14:",
            "csrr t1, mepc",
            "lw t2, 12(t0)",
            "sw t1, 12(t0)",
            "bne t1, t2, 16f",
            "csrr t1, mcycle",
            "lw t2, 16(t0)",
            "sw t1, 16(t0)",
            "sub t2, t1, t2",
            "li t1, 256",
            "bgeu t2, t1, 17f",
            "lw t2, 8(t0)",
            "addi t2, t2, 1",
            "sw t2, 8(t0)",
            "li t1, 100000",
            "bgeu t2, t1, 30b",
            "j 15f",
            "16:",
            "csrr t1, mcycle",
            "sw t1, 16(t0)",
            "17:",
            "sw zero, 8(t0)",
            "15:",
            "lw t2, 4(t0)",
            "lw t1, 0(t0)",
            "csrr t0, mscratch",
            "mret",
            // machine timer (core0)
            "20:",
            "lw t2, 4(t0)",
            "lw t1, 0(t0)",
            "csrr t0, mscratch",
            "addi sp, sp, -80",
            "sw ra, 0(sp)",
            "sw t0, 4(sp)",
            "sw t1, 8(sp)",
            "sw t2, 12(sp)",
            "sw t3, 16(sp)",
            "sw t4, 20(sp)",
            "sw t5, 24(sp)",
            "sw t6, 28(sp)",
            "sw a0, 32(sp)",
            "sw a1, 36(sp)",
            "sw a2, 40(sp)",
            "sw a3, 44(sp)",
            "sw a4, 48(sp)",
            "sw a5, 52(sp)",
            "sw a6, 56(sp)",
            "sw a7, 60(sp)",
            "csrr t0, mepc",
            "sw t0, 64(sp)",
            "csrr t0, mstatus",
            "sw t0, 68(sp)",
            "call MachineTimer",
            "lw t1, 64(sp)",
            "csrw mepc, t1",
            "lw t1, 68(sp)",
            "csrw mstatus, t1",
            "lw ra, 0(sp)",
            "lw t1, 8(sp)",
            "lw t2, 12(sp)",
            "lw t3, 16(sp)",
            "lw t4, 20(sp)",
            "lw t5, 24(sp)",
            "lw t6, 28(sp)",
            "lw a0, 32(sp)",
            "lw a1, 36(sp)",
            "lw a2, 40(sp)",
            "lw a3, 44(sp)",
            "lw a4, 48(sp)",
            "lw a5, 52(sp)",
            "lw a6, 56(sp)",
            "lw a7, 60(sp)",
            "lw t0, 4(sp)",
            "addi sp, sp, 80",
            "mret",
        );
    };
}
trap_entry!("smp_trap0", "SMP_TRAP_SCRATCH0", "0x10", "1");
trap_entry!("smp_trap1", "SMP_TRAP_SCRATCH1", "0x20", "0");
const _: () = assert!(core1::ACCESSCTRL_XIP_MAIN == 0x4006_0018);

unsafe extern "C" {
    fn smp_trap0();
    fn smp_trap1();
}

/// Core `core`'s trap entry (direct mode).
pub fn trap_entry(core: usize) -> usize {
    if core == 0 { smp_trap0 as *const () as usize } else { smp_trap1 as *const () as usize }
}

/// Point core0's trap vector at its entry.
pub fn install_trap0() {
    // SAFETY: the entry handles everything or hands it to riscv-rt's.
    unsafe { core::arch::asm!("csrw mtvec, {0}", in(reg) trap_entry(0)) };
}

// ------------------------------------------------------------------ hart 1

/// Hart 1 has been started and doesn't wait in WFI (or is about to start).
#[inline]
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_active() -> bool {
    match SMP.hsm[1].load(Ordering::Acquire) {
        HSM_START_PENDING => true,
        HSM_STARTED => !HART1_PARKED.load(Ordering::Acquire),
        _ => false,
    }
}

/// Hart 1's timer deadline while it waits (u64::MAX: it runs, or has none).
#[inline]
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_deadline() -> u64 {
    if !HART1_PARKED.load(Ordering::Acquire) || SMP.hsm[1].load(Ordering::Acquire) != HSM_STARTED {
        return u64::MAX;
    }
    let hi = HART1_DEADLINE[1].load(Ordering::Acquire);
    let lo = HART1_DEADLINE[0].load(Ordering::Relaxed);
    (u64::from(hi) << 32) | u64::from(lo)
}

/// Hart 1's WFI: wait unless something is pending (core1 looks again after
/// each pass, `hart1_runnable`).
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_wfi<B: Bus>(cpu: &mut Cpu, bus: &B) {
    cpu.smp_poll();
    if cpu.sip_value(bus) & cpu.sie != 0 {
        return;
    }
    let t = cpu.stimecmp;
    HART1_DEADLINE[0].store(t as u32, Ordering::Relaxed);
    HART1_DEADLINE[1].store((t >> 32) as u32, Ordering::Release);
    HART1_PARKED.store(true, Ordering::Release);
}

/// Whether hart 1 may be running guest code: out of a slice it takes its
/// fence requests before it runs again (`hart1_enter`).
#[inline]
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_in_guest() -> bool {
    HART1_IN_GUEST.load(Ordering::Relaxed)
}

/// Start of a slice of hart 1's guest code: marked running, then the fence
/// requests sent before (a hart sending one after sees the mark and waits;
/// rvcore `smp_sfence_vma`).
#[inline]
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_enter(cpu: &mut Cpu) {
    HART1_IN_GUEST.store(true, Ordering::Relaxed);
    core::sync::atomic::fence(Ordering::SeqCst);
    cpu.smp_poll();
}

/// End of the slice.
#[inline]
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_leave() {
    HART1_IN_GUEST.store(false, Ordering::Release);
}

/// Whether hart 1 waits in WFI (its slice ends).
#[inline]
pub fn hart1_parked() -> bool {
    HART1_PARKED.load(Ordering::Relaxed)
}

/// Before a slice: start a pending hart 1, take what was sent to a waiting
/// one; whether it can run.
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_runnable<B: Bus>(cpu: &mut Cpu, bus: &B) -> bool {
    match SMP.hsm[1].load(Ordering::Acquire) {
        HSM_START_PENDING => {
            cpu.hsm_start(SMP.start_pc[1].load(Ordering::Acquire), SMP.start_arg[1].load(Ordering::Acquire));
            // its clock starts at the machine's
            cpu.smp_poll();
            HART1_PARKED.store(false, Ordering::Release);
            SMP.hsm[1].store(HSM_STARTED, Ordering::Release);
            true
        }
        HSM_STARTED => {
            if !HART1_PARKED.load(Ordering::Acquire) {
                return true;
            }
            cpu.smp_poll();
            if cpu.sip_value(bus) & cpu.sie != 0 {
                HART1_PARKED.store(false, Ordering::Release);
                return true;
            }
            false
        }
        _ => false,
    }
}

/// After a slice: a hart_stop leaves hart 1 stopped until the next start;
/// anything else that stopped it (a shutdown, a reset) is hart 0's to act
/// on (`take_halt`).
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart1_after(cpu: &mut Cpu) {
    match cpu.halt.take() {
        None => {}
        Some(Halt::Stopped) => SMP.hsm[1].store(HSM_STOPPED, Ordering::Release),
        Some(h) => {
            HALT1.store(halt_code(h), Ordering::Release);
            SMP.hsm[1].store(HSM_STOPPED, Ordering::Release);
        }
    }
}

const HALTS: [Halt; 7] =
    [Halt::Shutdown, Halt::Until, Halt::MaxInsns, Halt::Idle, Halt::User, Halt::Panic, Halt::Stopped];

fn halt_code(h: Halt) -> u32 {
    HALTS.iter().position(|&x| x == h).map_or(1, |i| i as u32 + 1)
}

/// How hart 1 stopped the machine, if it did.
#[inline]
pub fn take_halt() -> Option<Halt> {
    match HALT1.swap(0, Ordering::Acquire) {
        0 => None,
        n => HALTS.get(n as usize - 1).copied(),
    }
}

/// Whether a hart 0 waiting in WFI has to look again: an IPI or fence for
/// it, hart 1's clock past its deadline, hart 1 starting or stopping to
/// wait (which moves the deadline to wait for), or a halt from hart 1.
#[inline]
#[unsafe(link_section = ".data.rvemu_hot")]
pub fn hart0_wake(cpu: &Cpu, hart1_was_active: bool) -> bool {
    SMP.ipi[0].load(Ordering::Relaxed) != 0
        || SMP.fence[0].load(Ordering::Relaxed) != 0
        || HALT1.load(Ordering::Relaxed) != 0
        || hart1_active() != hart1_was_active
        || (hart1_was_active && SMP.time_of(1) >= cpu.stimecmp)
}
