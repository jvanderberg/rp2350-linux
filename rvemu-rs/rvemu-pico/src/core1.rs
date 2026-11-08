//! The second Hazard3 core as an I/O processor.
//!
//! core0 runs the guest and nothing else. core1 runs a `Service` (usually a
//! struct of several) in a loop: USB, the radio, whatever else the firmware
//! gives it. The two cores talk only through lock-free single-producer
//! single-consumer rings and single-writer words in main SRAM:
//! - `ByteRing`: bytes (the console);
//! - `FrameRing`: whole Ethernet frames;
//! - `EVENTS`: a sequence number core1 bumps (`notify_core0`) whenever it
//!   hands core0 something new, so core0's device poll is one load and a
//!   compare until something happens.
//!
//! Everything core1 runs must be in SRAM, not XIP flash: the cores share one
//! QMI and one 16 KB XIP cache, and the guest's RAM is behind them. Code in
//! this module, and in the modules and crates `core1.x` names, is linked
//! there; `#[unsafe(link_section = ".data.core1")]` puts any other function
//! there. `scripts/check-core1.py` checks the result.
//!
//! Flash erase and program stall XIP: bracket them with `park()` and
//! `unpark()`, which hold core1 in an SRAM spin loop meanwhile.
//!
//! Between passes core1 sleeps in `wfi` until something happens: a USB
//! interrupt, the radio's PIO interrupt, a doorbell from core0
//! (`notify_core1`, which core0 rings whenever it queues work), or a
//! `TICK_US` timer, which also runs timeouts and the driver's timers. A
//! source left asserted only makes core1 spin; none is ever needed for
//! correctness beyond the tick. Time on core1 is `now()` (the SIO RISC-V
//! timer at the system clock), since a sleeping core's `mcycle` may stop.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};
use core::task::{RawWaker, RawWakerVTable, Waker};

use rp2350_board as board;
use rp235x_hal as hal;

/// What core1 runs: `poll` is called in a loop, forever. It must not block
/// for long (the other services wait). `launch` builds it in a static slot
/// where it never moves, so it may hold pinned futures.
pub trait Service {
    fn poll(&mut self);
}

// ------------------------------------------------------------------ rings

/// A single-producer single-consumer byte ring. One core pushes, the other
/// pops; neither ever blocks the other. `N` is a power of two.
#[repr(C)]
pub struct ByteRing<const N: usize> {
    /// Consumed so far (written by the consumer only).
    head: AtomicU32,
    /// Produced so far (written by the producer only).
    tail: AtomicU32,
    buf: UnsafeCell<[u8; N]>,
}

// SAFETY: the producer only writes bytes outside [head, tail) and publishes
// them with a release store of `tail`; the consumer only reads inside it
// after an acquire load, and gives bytes back with a release store of `head`.
unsafe impl<const N: usize> Sync for ByteRing<N> {}

impl<const N: usize> ByteRing<N> {
    pub const fn new() -> Self {
        assert!(N.is_power_of_two() && N <= 1 << 31);
        ByteRing { head: AtomicU32::new(0), tail: AtomicU32::new(0), buf: UnsafeCell::new([0; N]) }
    }

    /// Bytes waiting (either side).
    #[inline]
    pub fn len(&self) -> usize {
        let t = self.tail.load(Ordering::Acquire);
        t.wrapping_sub(self.head.load(Ordering::Acquire)) as usize
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Producer: room left.
    #[inline]
    pub fn free(&self) -> usize {
        N - self.len()
    }

    /// Producer: append one byte; false if the ring is full.
    #[inline]
    pub fn push(&self, b: u8) -> bool {
        let t = self.tail.load(Ordering::Relaxed);
        if t.wrapping_sub(self.head.load(Ordering::Acquire)) as usize == N {
            return false;
        }
        // SAFETY: slot `t` is outside [head, tail), so the consumer isn't
        // reading it.
        unsafe { self.buf.get().cast::<u8>().add(t as usize & (N - 1)).write(b) };
        self.tail.store(t.wrapping_add(1), Ordering::Release);
        true
    }

    /// Producer: append as much of `s` as fits; how much did.
    pub fn push_slice(&self, s: &[u8]) -> usize {
        let t = self.tail.load(Ordering::Relaxed);
        let n = s.len().min(N - t.wrapping_sub(self.head.load(Ordering::Acquire)) as usize);
        for (i, &b) in s[..n].iter().enumerate() {
            // SAFETY: as in `push`, for each of the `n` free slots.
            unsafe { self.buf.get().cast::<u8>().add(t.wrapping_add(i as u32) as usize & (N - 1)).write(b) };
        }
        self.tail.store(t.wrapping_add(n as u32), Ordering::Release);
        n
    }

    /// Consumer: the oldest contiguous run of waiting bytes.
    #[inline]
    pub fn front(&self) -> &[u8] {
        let h = self.head.load(Ordering::Relaxed);
        let n = self.tail.load(Ordering::Acquire).wrapping_sub(h) as usize;
        let at = h as usize & (N - 1);
        // SAFETY: [head, tail) is published and the producer leaves it alone
        // until `consume` gives it back.
        unsafe { core::slice::from_raw_parts(self.buf.get().cast::<u8>().add(at), n.min(N - at)) }
    }

    /// Consumer: drop the `n` oldest bytes (at most `len()`).
    #[inline]
    pub fn consume(&self, n: usize) {
        let h = self.head.load(Ordering::Relaxed);
        self.head.store(h.wrapping_add(n as u32), Ordering::Release);
    }

    /// Consumer: take the oldest byte.
    #[inline]
    pub fn pop(&self) -> Option<u8> {
        let h = self.head.load(Ordering::Relaxed);
        if self.tail.load(Ordering::Acquire) == h {
            return None;
        }
        // SAFETY: slot `h` is published (h != tail).
        let b = unsafe { self.buf.get().cast::<u8>().add(h as usize & (N - 1)).read() };
        self.head.store(h.wrapping_add(1), Ordering::Release);
        Some(b)
    }
}

/// Room for any Ethernet frame (`rvcore::virtio::FRAME_MAX`, 1518).
pub const FRAME_SLOT: usize = 1520;

/// A single-producer single-consumer ring of `K` whole frames (a power of
/// two), each copied in and out of its slot.
#[repr(C)]
pub struct FrameRing<const K: usize> {
    head: AtomicU32,
    tail: AtomicU32,
    len: UnsafeCell<[u16; K]>,
    data: UnsafeCell<[[u8; FRAME_SLOT]; K]>,
}

// SAFETY: as for `ByteRing`, slot by slot.
unsafe impl<const K: usize> Sync for FrameRing<K> {}

impl<const K: usize> FrameRing<K> {
    pub const fn new() -> Self {
        assert!(K.is_power_of_two());
        FrameRing {
            head: AtomicU32::new(0),
            tail: AtomicU32::new(0),
            len: UnsafeCell::new([0; K]),
            data: UnsafeCell::new([[0; FRAME_SLOT]; K]),
        }
    }

    #[inline]
    pub fn is_empty(&self) -> bool {
        self.tail.load(Ordering::Acquire) == self.head.load(Ordering::Acquire)
    }

    /// Producer: is there a free slot?
    #[inline]
    pub fn has_room(&self) -> bool {
        let t = self.tail.load(Ordering::Relaxed);
        t.wrapping_sub(self.head.load(Ordering::Acquire)) as usize != K
    }

    /// Producer: fill the next free slot with `fill`, which returns the
    /// frame's length (or None to give the slot back). False if the ring
    /// was full or `fill` declined.
    #[inline]
    pub fn push_with(&self, fill: impl FnOnce(&mut [u8; FRAME_SLOT]) -> Option<usize>) -> bool {
        if !self.has_room() {
            return false;
        }
        let t = self.tail.load(Ordering::Relaxed);
        let i = t as usize & (K - 1);
        // SAFETY: slot `i` is free: the consumer doesn't touch it until the
        // release store of `tail` below.
        let (slot, len) = unsafe { (&mut *self.data.get().cast::<[u8; FRAME_SLOT]>().add(i), self.len.get().cast::<u16>().add(i)) };
        let Some(n) = fill(slot) else { return false };
        // SAFETY: as above.
        unsafe { len.write(n.min(FRAME_SLOT) as u16) };
        self.tail.store(t.wrapping_add(1), Ordering::Release);
        true
    }

    /// Consumer: hand the oldest frame to `take`, then free its slot.
    #[inline]
    pub fn pop_with<R>(&self, take: impl FnOnce(&[u8]) -> R) -> Option<R> {
        let h = self.head.load(Ordering::Relaxed);
        if self.tail.load(Ordering::Acquire) == h {
            return None;
        }
        let i = h as usize & (K - 1);
        // SAFETY: slot `i` is published, and the producer leaves it alone
        // until the release store of `head` below.
        let r = unsafe {
            let n = self.len.get().cast::<u16>().add(i).read() as usize;
            take(core::slice::from_raw_parts(self.data.get().cast::<[u8; FRAME_SLOT]>().add(i).cast::<u8>(), n))
        };
        self.head.store(h.wrapping_add(1), Ordering::Release);
        Some(r)
    }
}

// ------------------------------------------------------------------ signals

const SIO: usize = 0xd000_0000;
const DOORBELL_OUT_SET: usize = SIO + 0x180;
const DOORBELL_IN_CLR: usize = SIO + 0x18c;
const MTIME_CTRL: usize = SIO + 0x1a4;
const MTIME: usize = SIO + 0x1b0;
const MTIMEH: usize = SIO + 0x1b4;
const MTIMECMP: usize = SIO + 0x1b8;
const MTIMECMPH: usize = SIO + 0x1bc;
const PIO0_IRQ0_INTE_SET: usize = 0x5020_0000 + 0x170 + 0x2000;
/// The longest core1 sleeps without an interrupt.
const TICK_US: u32 = 1000;

fn rd(a: usize) -> u32 {
    // SAFETY: an SIO register.
    unsafe { (a as *const u32).read_volatile() }
}
fn wr(a: usize, v: u32) {
    // SAFETY: an SIO or PIO register.
    unsafe { (a as *mut u32).write_volatile(v) }
}

/// System clock cycles since boot, from the SIO RISC-V timer (`launch`
/// runs it at full speed). Either core may read it; core1 uses it for all
/// its timing.
#[inline]
pub fn now() -> u64 {
    loop {
        let (hi, lo, hi2) = (rd(MTIMEH), rd(MTIME), rd(MTIMEH));
        if hi == hi2 {
            return (u64::from(hi) << 32) | u64::from(lo);
        }
    }
}

/// core0: wake core1 (it has queued work for it).
#[inline]
pub fn notify_core1() {
    wr(DOORBELL_OUT_SET, 1);
}

/// Bumped by core1 (its only writer) whenever it gave core0 something new:
/// console input, a frame, a link change.
static EVENTS: AtomicU32 = AtomicU32::new(0);

/// core1: tell core0 to look at its rings and device state again.
#[inline]
pub fn notify_core0() {
    EVENTS.store(EVENTS.load(Ordering::Relaxed).wrapping_add(1), Ordering::Release);
}

/// core0: the current event sequence; compare it with the last one seen.
#[inline]
pub fn events() -> u32 {
    EVENTS.load(Ordering::Acquire)
}

static PARK_REQ: AtomicBool = AtomicBool::new(false);
static PARK_ACK: AtomicBool = AtomicBool::new(false);
static RUNNING: AtomicBool = AtomicBool::new(false);

/// core0: park core1 in an SRAM loop (it never touches flash there), for
/// flash erase and program; `unpark` lets it go. Waits until core1 is
/// parked (at most one pass of its service loop). Don't nest. `flash.rs`
/// calls the pair around every erase and program. Always true.
#[unsafe(link_section = ".data.core1")]
#[inline(never)]
pub fn park() -> bool {
    if RUNNING.load(Ordering::Acquire) {
        PARK_REQ.store(true, Ordering::Release);
        notify_core1();
        while !PARK_ACK.load(Ordering::Acquire) {
            core::hint::spin_loop();
        }
    }
    true
}

/// core0: end `park`.
#[unsafe(link_section = ".data.core1")]
#[inline(never)]
pub fn unpark() {
    if RUNNING.load(Ordering::Acquire) {
        PARK_REQ.store(false, Ordering::Release);
        while PARK_ACK.load(Ordering::Acquire) {
            core::hint::spin_loop();
        }
    }
}

#[unsafe(link_section = ".data.core1")]
#[inline(never)]
fn parked() {
    PARK_ACK.store(true, Ordering::Release);
    while PARK_REQ.load(Ordering::Acquire) {
        core::hint::spin_loop();
    }
    PARK_ACK.store(false, Ordering::Release);
}

// ------------------------------------------------------------------ waker

/// A no-op waker whose functions are in SRAM (`Waker::noop`'s are in flash).
/// There is no executor: every future is polled again on the next pass.
pub fn waker() -> &'static Waker {
    #[unsafe(link_section = ".data.core1")]
    fn clone(_: *const ()) -> RawWaker {
        RawWaker::new(core::ptr::null(), &VTABLE)
    }
    #[unsafe(link_section = ".data.core1")]
    fn noop(_: *const ()) {}
    static VTABLE: RawWakerVTable = RawWakerVTable::new(clone, noop, noop, noop);
    // SAFETY: every function in VTABLE is a no-op, as the contract allows.
    static WAKER: Waker = unsafe { Waker::from_raw(RawWaker::new(core::ptr::null(), &VTABLE)) };
    &WAKER
}

// ------------------------------------------------------------------ SRAM code

unsafe extern "C" {
    static mut __score1_text: u32;
    static mut __ecore1_text: u32;
    static __sicore1_text: u32;
    static mut __ssram89: u32;
    static mut __esram89: u32;
}

/// Statics placed in SRAM8/9 (two 4 KB banks of their own, outside the
/// striped main SRAM): `#[unsafe(link_section = ".sram89.NAME")]`. They start
/// zeroed.
///
/// Copy `.core1_text` (core1.x) from flash into SRAM before anything can
/// call into it: memcpy is in there too, so this runs before `.data` is set
/// up and copies word by word with volatile writes the compiler can't turn
/// into a memcpy call.
#[riscv_rt::pre_init]
unsafe fn copy_core1_text() {
    // SAFETY: linker-provided bounds of the section and its load image.
    unsafe {
        let mut d = &raw mut __score1_text;
        let end = &raw mut __ecore1_text;
        let mut s = &raw const __sicore1_text;
        while d < end {
            d.write_volatile(s.read_volatile());
            d = d.add(1);
            s = s.add(1);
        }
        core::arch::asm!("fence.i");
        // `.sram89` (SRAM8/9) is NOLOAD: zero it as riscv-rt does .bss
        let mut d = &raw mut __ssram89;
        while d < &raw mut __esram89 {
            d.write_volatile(0);
            d = d.add(1);
        }
    }
}

// ------------------------------------------------------------------ launch

/// core1's stack, in main SRAM (3.3 KB deep after a gate run).
const STACK_WORDS: usize = 1280;
pub const STACK_BYTES: usize = STACK_WORDS * 4;
#[repr(align(16))]
struct Stack(UnsafeCell<[u32; STACK_WORDS]>);
// SAFETY: core1's alone once launched; `stack_used` only reads it.
unsafe impl Sync for Stack {}
static STACK: Stack = Stack(UnsafeCell::new([0; STACK_WORDS]));
const STACK_PAINT: u32 = 0xC07E_57AC;

/// Where the service lives (`launch`): its size is checked at compile time.
/// The radio's driver future is most of it.
pub const SERVICE_BYTES: usize = 7 * 1024;
#[repr(C, align(16))]
struct ServiceSlot(UnsafeCell<MaybeUninit<[u8; SERVICE_BYTES]>>);
// SAFETY: written by core0 before core1 starts, then core1's alone.
unsafe impl Sync for ServiceSlot {}
static SERVICE: ServiceSlot = ServiceSlot(UnsafeCell::new(MaybeUninit::uninit()));

static ENTRY: AtomicUsize = AtomicUsize::new(0);
static ARG: AtomicUsize = AtomicUsize::new(0);

// core1's first instructions: the bootrom sets sp and mtvec, but not gp.
core::arch::global_asm!(
    ".section .data.core1.start, \"ax\"",
    ".balign 4",
    ".global rvemu_core1_start",
    "rvemu_core1_start:",
    ".option push",
    ".option norelax",
    "la gp, __global_pointer$",
    ".option pop",
    "la t0, {entry}",
    "lw t0, 0(t0)",
    "la a0, {arg}",
    "lw a0, 0(a0)",
    "jr t0",
    entry = sym ENTRY,
    arg = sym ARG,
);

unsafe extern "C" {
    fn rvemu_core1_start() -> !;
}

#[unsafe(link_section = ".data.core1")]
extern "C" fn trampoline<S: Service>(arg: *mut S) -> ! {
    // SAFETY: `launch` built it in SERVICE, which nothing else touches now.
    let s = unsafe { &mut *arg };
    board::enable_counters();
    // Wake sources for `wfi` (interrupts stay off: mstatus.MIE is 0, so
    // nothing is taken, wfi just returns). PIO0's IRQ 0 output follows the
    // radio program's `irq 0` (the chip's data-ready).
    wr(PIO0_IRQ0_INTE_SET, 1 << 8);
    // SAFETY: only enables wake-ups; no handler can run with MIE clear.
    unsafe {
        hal::arch::interrupt_unmask(hal::pac::Interrupt::USBCTRL_IRQ);
        hal::arch::interrupt_unmask(hal::pac::Interrupt::PIO0_IRQ_0);
        hal::arch::interrupt_unmask(hal::pac::Interrupt::SIO_IRQ_BELL);
        core::arch::asm!("csrs mie, {0}", in(reg) (1u32 << 11) | (1 << 7));
    }
    RUNNING.store(true, Ordering::Release);
    run(s)
}

#[unsafe(link_section = ".data.core1")]
#[inline(never)]
fn run<S: Service>(s: &mut S) -> ! {
    let tick = u64::from(TICK_SYS_MHZ.load(Ordering::Relaxed)) * u64::from(TICK_US);
    loop {
        // acknowledge doorbells first: one rung during the pass wakes the
        // wfi below at once
        wr(DOORBELL_IN_CLR, !0);
        if PARK_REQ.load(Ordering::Acquire) {
            parked();
        }
        s.poll();
        let t = now() + tick;
        wr(MTIMECMPH, u32::MAX);
        wr(MTIMECMP, t as u32);
        wr(MTIMECMPH, (t >> 32) as u32);
        // SAFETY: waits for a wake source; nothing is taken.
        unsafe { core::arch::asm!("wfi") };
    }
}

/// The system clock in MHz, for `TICK_US`.
static TICK_SYS_MHZ: AtomicU32 = AtomicU32::new(150);

/// Start core1 on the service `make` builds (straight into its static slot,
/// so it never sits in a stack frame). Returns once core1 is running it.
// Not inlined, so whatever `make` puts on the stack leaves core0's stack
// with this frame; and in flash (core0 runs it once), not in core1.x's SRAM.
#[inline(never)]
#[unsafe(link_section = ".text.core0_launch")]
pub fn launch<S: Service + Send>(fifo: &mut hal::sio::SioFifo, psm: &mut hal::pac::PSM, sys_hz: u32, make: impl FnOnce() -> S) {
    const {
        assert!(size_of::<S>() <= SERVICE_BYTES && align_of::<S>() <= 16, "core1's service: raise SERVICE_BYTES");
    };
    let stack = STACK.0.get().cast::<u32>();
    for i in 0..STACK_WORDS {
        // SAFETY: core1 isn't running yet.
        unsafe { stack.add(i).write_volatile(STACK_PAINT) };
    }
    // SAFETY: one past the end of the stack array.
    let stack_top = unsafe { stack.add(STACK_WORDS) } as usize;
    TICK_SYS_MHZ.store(sys_hz / 1_000_000, Ordering::Relaxed);
    // the SIO RISC-V timer at the system clock: `now()`
    wr(MTIME_CTRL, 0b11);
    let s = SERVICE.0.get().cast::<S>();
    // SAFETY: the slot is big and aligned enough (checked above), and core1
    // isn't running yet.
    unsafe { s.write(make()) };
    ENTRY.store(trampoline::<S> as *const () as usize, Ordering::Relaxed);
    ARG.store(s as usize, Ordering::Relaxed);

    // Reset core1 into the bootrom, which then waits for this sequence.
    psm.frce_off().modify(|_, w| w.proc1().set_bit());
    while !psm.frce_off().read().proc1().bit_is_set() {}
    psm.frce_off().modify(|_, w| w.proc1().clear_bit());
    let mtvec: usize;
    // SAFETY: reading a CSR.
    unsafe { core::arch::asm!("csrr {0}, mtvec", out(reg) mtvec) };
    let seq = [0, 0, 1, mtvec, stack_top, rvemu_core1_start as *const () as usize];
    let mut i = 0;
    while i < seq.len() {
        let cmd = seq[i] as u32;
        if cmd == 0 {
            fifo.drain();
            hal::arch::sev();
        }
        fifo.write_blocking(cmd);
        i = if fifo.read_blocking() == cmd { i + 1 } else { 0 };
    }
    while !RUNNING.load(Ordering::Acquire) {
        core::hint::spin_loop();
    }
}

/// core1's deepest stack use so far, in bytes (from the paint `launch` put
/// down).
pub fn stack_used() -> usize {
    let stack = STACK.0.get().cast::<u32>();
    // SAFETY: reading words of the stack array.
    let untouched = (0..STACK_WORDS).take_while(|&i| unsafe { stack.add(i).read_volatile() } == STACK_PAINT).count();
    (STACK_WORDS - untouched) * 4
}

/// The deepest use of the stack that ends at `top` and must not go below
/// `bottom`, painted by `paint_stack` (core0's, from main).
pub fn painted_stack_used(bottom: usize, top: usize) -> usize {
    let untouched = (bottom..top)
        .step_by(4)
        // SAFETY: reading SRAM inside the given stack.
        .take_while(|&a| unsafe { (a as *const u32).read_volatile() } == STACK_PAINT)
        .count();
    top - bottom - untouched * 4
}

/// Paint the stack below the caller's frame (keeping `margin` bytes clear),
/// down to `bottom`.
#[inline(never)]
pub fn paint_stack(bottom: usize, margin: usize) {
    let sp: usize;
    // SAFETY: reading sp.
    unsafe { core::arch::asm!("mv {0}, sp", out(reg) sp) };
    let mut a = bottom;
    while a < sp - margin {
        // SAFETY: below the live stack and above the statics.
        unsafe { (a as *mut u32).write_volatile(STACK_PAINT) };
        a += 4;
    }
}
