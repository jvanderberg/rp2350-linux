//! Interactive Linux on the RP2350B: the guest's UART is a USB CDC-ACM serial
//! port on the target's USB (`screen /dev/cu.usbmodem*`), translator only.
//!
//! Guest kernel / initramfs / DTB come from the flash blob, and the root
//! filesystem, when there is one, from a squashfs at `DISK_ADDR` behind a
//! read-only virtio disk (scripts/pico-shell.sh loads them and this firmware
//! over BOOTSEL); guest RAM is the PSRAM. While the guest idles in WFI the firmware waits in real time
//! up to the next timer deadline, like the Mac host's interactive mode.
//!
//! core0 runs only the guest. core1 (`core1.rs`) does all the I/O: the USB
//! console (`usbcon.rs`) and the RM2 Wi-Fi radio behind the guest's
//! virtio-net (`wifi.rs`; scripts/pico-wifi.sh stores the network to join).
//! They exchange console bytes and frames through rings in SRAM. Console
//! output is kept while no terminal is open, so connecting late shows the
//! end of the boot log (`dmesg` has all of the kernel's).
//!
//! Back to BOOTSEL: open the port at 1200 baud and close it
//! (`stty -f /dev/cu.usbmodemXXXX 1200`), or the SWD request in
//! `rp2350-board::bootsel_if_requested`. RTT carries status lines for a
//! debugger but never blocks.
#![no_std]
#![no_main]

use core::mem::MaybeUninit;
use core::ptr::addr_of_mut;
use core::sync::atomic::Ordering;

use panic_rtt_target as _;
use rp2350_board as board;
use rp235x_hal as hal;
use rtt_target::{ChannelMode, rprintln, rtt_init_print};
use rvcore::dev::{Console, Platform};
use rvcore::nor::{NorDev, NorFlash};
use rvcore::swapdev::FlashSwap;
use rvcore::virtio::{Disk as _, FRAME_MAX, GuestMem, MemDisk, NetLink, VirtioBlk, VirtioNet};
use rvcore::{Bus, Cpu, DTB_RNG_SEED, Halt, TIMEBASE_HZ};
use rvjit::{JcEntry, Jit, JitMem};
use usb_device::class_prelude::UsbBusAllocator;
use usb_device::prelude::*;
use usbd_serial::SerialPort;

#[path = "../guest_blob.rs"]
mod guest_blob;
use guest_blob::{BLOB_ADDR, BlobHeader};

#[path = "../core1.rs"]
mod core1;
#[path = "../usbcon.rs"]
mod usbcon;
use usbcon::{CON_IN, CON_OUT};
#[path = "../wifi.rs"]
mod wifi;

#[path = "../flash.rs"]
mod flash;
#[path = "../swap_flash.rs"]
mod swap_flash;
use swap_flash::SwapFlash;
use wifi::{Creds, RadioPins};

#[path = "../pins.rs"]
mod pins;
use pins::{BoardPins, Gp, PinSet};

#[unsafe(link_section = ".start_block")]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

/// PSRAM: guest RAM, then the translator's backing store. The store is off:
/// on the board its traffic shares the 16 KB XIP cache with guest RAM, and a
/// 1 MiB store made the boot slower (9.9 s against 8.0 s; XIP misses 3.7 M
/// -> 12.7 M). With `STORE_WORDS = (1 << 20) / 4` the guest gets 7 MiB.
const STORE_WORDS: usize = 0;
#[cfg(feature = "pv-buffers")]
const NATIVE_BYTES: usize = 64 * 1024;
#[cfg(not(feature = "pv-buffers"))]
const NATIVE_BYTES: usize = 0;
#[cfg(feature = "pv-buffers")]
const NATIVE_BASE: usize = board::PSRAM_CACHED + (8 << 20) - NATIVE_BYTES;
#[cfg(feature = "pv-buffers")]
const _: () = assert!(core::mem::size_of::<rvcore::NativeWorkspace>() <= NATIVE_BYTES);
/// The benchmark log (`Bench`) sits in the same reservation, after the
/// workspace.
#[cfg(feature = "pv-buffers")]
const BENCH_BASE: usize = (NATIVE_BASE + core::mem::size_of::<rvcore::NativeWorkspace>()).next_multiple_of(16);
#[cfg(feature = "pv-buffers")]
const _: () = assert!(BENCH_BASE + core::mem::size_of::<Bench>() <= NATIVE_BASE + NATIVE_BYTES);
const RAM_MAX: u32 = (8 << 20) - (STORE_WORDS * 4 + NATIVE_BYTES) as u32;
const STORE: usize = board::PSRAM_CACHED + RAM_MAX as usize;

/// Root filesystem image in flash, after the 2 MB blob: a squashfs, sized by
/// its superblock, up to the kernel text at the end of flash.
const DISK_ADDR: usize = 0x1060_0000;
const DISK_MAX: usize = guest_blob::TEXT_END - guest_blob::TEXT_MAX as usize - DISK_ADDR;
/// Flash's uncached, non-allocating XIP alias (0x14000000): disk reads then
/// don't evict guest RAM from the 16 KB XIP cache the two share.
const XIP_NOCACHE_NOALLOC: usize = 0x1400_0000;

/// The squashfs at `DISK_ADDR`, if there is one.
fn flash_disk() -> Option<MemDisk> {
    let base = DISK_ADDR - 0x1000_0000 + XIP_NOCACHE_NOALLOC;
    // SAFETY: flash is mapped; the superblock is in the first 96 bytes.
    let (magic, used) = unsafe {
        let p = base as *const u32;
        (p.read_volatile(), u64::from(p.add(10).read_volatile()) | u64::from(p.add(11).read_volatile()) << 32)
    };
    const SQUASHFS_MAGIC: u32 = 0x7371_7368; // "hsqs"
    let len = usize::try_from(used).ok()?.next_multiple_of(4096);
    (magic == SQUASHFS_MAGIC && len <= DISK_MAX)
        // SAFETY: `len` bytes of mapped flash, never written while we run.
        .then(|| unsafe { MemDisk::new(base as *const u8, len) })
}

/// 32 bytes from the RP2350's TRNG (its ring oscillator, through the block's
/// von Neumann corrector and health checks; a failed check discards that
/// batch). The guest kernel hashes the seed into its pool anyway.
fn trng_seed(trng: &hal::pac::TRNG, resets: &mut hal::pac::RESETS) -> [u8; 32] {
    resets.reset().modify(|_, w| w.trng().clear_bit());
    while resets.reset_done().read().trng().bit_is_clear() {}
    trng.rng_icr().write(|w| unsafe { w.bits(0xF) });
    trng.rnd_source_enable().write(|w| w.rnd_src_en().set_bit());
    let mut out = [0u8; 32];
    let mut n = 0;
    while n < out.len() {
        let isr = trng.rng_isr().read();
        if isr.autocorr_err().bit_is_set() || isr.crngt_err().bit_is_set() || isr.vn_err().bit_is_set() {
            trng.rng_icr().write(|w| unsafe { w.bits(0xF) });
            continue;
        }
        if isr.ehr_valid().bit_is_clear() {
            continue;
        }
        // 192 bits; reading them starts the next batch
        for w in [
            trng.ehr_data0().read().bits(),
            trng.ehr_data1().read().bits(),
            trng.ehr_data2().read().bits(),
            trng.ehr_data3().read().bits(),
            trng.ehr_data4().read().bits(),
            trng.ehr_data5().read().bits(),
        ] {
            for b in w.to_le_bytes() {
                if n < out.len() {
                    out[n] = b;
                    n += 1;
                }
            }
        }
        trng.rng_icr().write(|w| w.ehr_valid().set_bit());
    }
    trng.rnd_source_enable().write(|w| w.rnd_src_en().clear_bit());
    out
}

/// Replace the device tree's fixed `rng-seed` (`DTB_RNG_SEED`) with `seed`.
fn patch_rng_seed(dtb: &mut [u8], seed: &[u8; 32]) -> bool {
    match dtb.windows(32).position(|w| w == DTB_RNG_SEED) {
        Some(i) => {
            dtb[i..i + 32].copy_from_slice(seed);
            true
        }
        None => false,
    }
}

/// The guest's /home: `flash::STORAGE` (1 MB) as the NOR flash device.
struct BoardNor;

impl NorFlash for BoardNor {
    fn size(&self) -> u32 {
        flash::STORAGE.end - flash::STORAGE.start
    }
    fn read(&mut self, off: u32, buf: &mut [u8]) -> bool {
        flash::read(flash::STORAGE.start + off, buf);
        true
    }
    fn program(&mut self, off: u32, data: &[u8]) -> bool {
        flash::program(flash::STORAGE.start + off, data).is_ok()
    }
    fn erase(&mut self, off: u32) -> bool {
        flash::erase_sector(flash::STORAGE.start + off).is_ok()
    }
    fn stat(&self, i: u32) -> u32 {
        let s = flash::stats();
        let mhz = u64::from(flash::sys_hz() / 1_000_000).max(1);
        match i {
            0 => s.erases,
            1 => s.erases_skipped,
            2 => s.pages,
            3 => (u64::from(s.stall_cycles_max) / mhz) as u32,
            4 => (s.stall_cycles_total / mhz / 1000) as u32,
            _ => 0,
        }
    }
}

// ------------------------------------------------------------------ translator storage (SRAM)

/// Translated code and its block headers: what SRAM has left once core1's
/// code and buffers (`core1.rs`), the pin devices and core0's stack (about
/// 10 KiB, what is left above .bss; `setup` and the run each need about 6) have
/// theirs. core0's stack is painted at boot; core1 reports its depth on RTT.
const CODE_KIB: usize = 326;
const CODE_WORDS: usize = CODE_KIB * 256;
const NBUCKETS: usize = 1024;
const NJC: usize = 1024;
/// Ring of 8 regions (simulator: 7.9 s to login: against 8.8 s flush-all);
/// the boot needs at most ~75 logged chains per region.
const REGIONS: usize = 8;

static mut CODE: [u32; CODE_WORDS] = [0; CODE_WORDS];
static mut BUCKETS: [u32; NBUCKETS] = [0; NBUCKETS];
/// Guest RAM as Linux sees it, at most: flash-backed kernel text + PSRAM.
const GUEST_MAX: u32 = RAM_MAX + guest_blob::TEXT_MAX;
static mut CODE_PAGES: [u8; (GUEST_MAX >> 15) as usize] = [0; (GUEST_MAX >> 15) as usize];
static mut JUMP_CACHE: [JcEntry; NJC] = [JcEntry::EMPTY; NJC];
static mut CHAIN_LOG: [u32; REGIONS * 128] = [0; REGIONS * 128];
static mut STORE_EPOCHS: [u8; (GUEST_MAX >> 12) as usize] = [0; (GUEST_MAX >> 12) as usize];
static mut CPU: MaybeUninit<Cpu> = MaybeUninit::uninit();
static mut USB_BUS: MaybeUninit<UsbBusAllocator<hal::usb::UsbBus>> = MaybeUninit::uninit();
// The bus and the translator live here rather than on main's stack: `setup`
// builds them in its own frame, which is gone before the guest runs, so
// core0's stack (what SRAM leaves above .bss, about 10 KB) is only the run.
static mut BUS: MaybeUninit<PicoBus> = MaybeUninit::uninit();
static mut JIT: MaybeUninit<Jit<'static>> = MaybeUninit::uninit();
/// Sectors kept free for bursts of swap writes: 448 of the 511 are swap.
const SWAP_SPARE: u32 = 63;
/// With the swap device (`pico-shell.sh -w`), its backend (about 2.2 KB of
/// tables) lives in the top page of guest RAM, which that DTB reserves
/// (`rvemu -D -W`). Without it, swap costs no SRAM and no guest RAM.
const SWAP_TABLES: usize = 0x1000;
const _: () = assert!(core::mem::size_of::<FlashSwap<SwapFlash>>() <= SWAP_TABLES);

/// Whether the guest's DTB has the swap device's node.
fn dtb_has_swap(h: &BlobHeader) -> bool {
    const NODE: &[u8] = b"virtio@f8000600";
    // SAFETY: the header was validated, so the DTB lies inside the blob,
    // in mapped flash.
    let dtb = unsafe { core::slice::from_raw_parts((BLOB_ADDR + h.dtb_off as usize) as *const u8, h.dtb_len as usize) };
    dtb.windows(NODE.len()).any(|w| w == NODE)
}

/// How long the radio may take to come up (and the guest wait for its MAC).
const RADIO_BRING_UP_MS: u32 = 5000;

// ------------------------------------------------------------------ core0's devices

/// The UART's host side: core1's rings.
struct RingConsole {
    /// How long `putc` waits for room before dropping a byte (core1 parked
    /// or wedged).
    wait_cycles: u64,
}

impl Console for RingConsole {
    #[unsafe(link_section = ".data.rvemu_hot")]
    fn putc(&mut self, b: u8) {
        if !CON_OUT.push(b) {
            // full: core1 is sending to a terminal (or about to drop old
            // output)
            let t0 = board::mcycle64();
            while !CON_OUT.push(b) && board::mcycle64() - t0 < self.wait_cycles {
                core1::notify_core1();
            }
        }
        core1::notify_core1();
    }
    fn rx_ready(&self) -> bool {
        !CON_IN.is_empty()
    }
    fn getc(&mut self) -> Option<u8> {
        CON_IN.pop()
    }
}

/// The guest's Ethernet: frames through core1's rings to the radio.
struct RingLink {
    /// How long the guest's first look at the MAC may wait for the radio.
    wait_cycles: u64,
}

impl NetLink for RingLink {
    fn mac(&self) -> [u8; 6] {
        let t0 = board::mcycle64();
        while wifi::RADIO.load(Ordering::Acquire) == wifi::RADIO_STARTING && board::mcycle64() - t0 < self.wait_cycles {
            core::hint::spin_loop();
        }
        if wifi::RADIO.load(Ordering::Acquire) != wifi::RADIO_UP {
            return [0; 6];
        }
        let (a, b) = (wifi::MAC[0].load(Ordering::Relaxed).to_le_bytes(), wifi::MAC[1].load(Ordering::Relaxed).to_le_bytes());
        [a[0], a[1], a[2], a[3], b[0], b[1]]
    }
    fn link_up(&self) -> bool {
        wifi::LINK_UP.load(Ordering::Acquire)
    }
    /// Dropped when core1 is behind by two frames, as a busy NIC would.
    fn send(&mut self, frame: &[u8]) {
        wifi::NET_TX.push_with(|slot| {
            let n = frame.len().min(slot.len());
            slot[..n].copy_from_slice(&frame[..n]);
            Some(n)
        });
        core1::notify_core1();
    }
    /// A freed slot lets core1 hand over the next frame: wake it.
    fn recv(&mut self, buf: &mut [u8; FRAME_MAX]) -> Option<usize> {
        let r = wifi::NET_RX.pop_with(|f| {
            let n = f.len().min(FRAME_MAX);
            buf[..n].copy_from_slice(&f[..n]);
            n
        });
        if r.is_some() {
            core1::notify_core1();
        }
        r
    }
}

struct PicoBus {
    plat: Platform<RingConsole, MemDisk, RingLink, BoardNor, &'static mut FlashSwap<SwapFlash>, BoardPins>,
    sys_hz: u32,
    /// The last `core1::events()` acted on.
    seen: u32,
    #[cfg(feature = "pv-buffers")]
    bench: &'static mut Bench,
}

/// XIP cache counters (hits, accesses; 32 bits, any write clears both).
#[cfg(feature = "pv-buffers")]
const XIP_CTR_HIT: usize = 0x400c_800c;
#[cfg(feature = "pv-buffers")]
const XIP_CTR_ACC: usize = 0x400c_8010;

/// The board's side of the guest's benchmark points (SBI 0x0a000002): each
/// point's cycle and instruction counters and the XIP cache's, kept in the
/// PSRAM reservation and printed on the console only when the guest asks
/// (`benchmark_report`), outside any measured interval. XIP counts are
/// accumulated into 64 bits at every `poll` (about every millisecond).
#[cfg(feature = "pv-buffers")]
struct Bench {
    log: rvcore::benchmark::Log,
    host: [[u64; 4]; 32],
    xip_hit: u64,
    xip_acc: u64,
}

#[cfg(feature = "pv-buffers")]
impl Bench {
    #[inline]
    fn sample_xip(&mut self) {
        // SAFETY: XIP counter registers; a write clears the one written (the
        // few accesses between the reads and the writes are lost).
        unsafe {
            let hit = core::ptr::read_volatile(XIP_CTR_HIT as *const u32);
            let acc = core::ptr::read_volatile(XIP_CTR_ACC as *const u32);
            core::ptr::write_volatile(XIP_CTR_HIT as *mut u32, 0);
            core::ptr::write_volatile(XIP_CTR_ACC as *mut u32, 0);
            self.xip_hit += u64::from(hit);
            self.xip_acc += u64::from(acc);
        }
    }
}

/// `core::fmt::Write` onto the guest console's output ring.
#[cfg(feature = "pv-buffers")]
struct ConWriter<'a>(&'a mut RingConsole);

#[cfg(feature = "pv-buffers")]
impl core::fmt::Write for ConWriter<'_> {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                self.0.putc(b'\r');
            }
            self.0.putc(b);
        }
        Ok(())
    }
}

impl PicoBus {
    /// Refresh the guest's interrupt lines (which also delivers received
    /// frames) when core1 has handed over something new.
    #[unsafe(link_section = ".data.rvemu_hot")]
    #[inline]
    fn poll(&mut self) {
        let e = core1::events();
        if e != self.seen {
            self.seen = e;
            self.plat.update_irqs();
        }
        // GPIO edges (only lines the guest armed; one register read otherwise)
        self.plat.poll_pins();
        #[cfg(feature = "pv-buffers")]
        self.bench.sample_xip();
    }
}

impl Bus for PicoBus {
    #[cfg(feature = "pv-buffers")]
    fn benchmark_supported(&self) -> bool {
        true
    }
    /// `host` is core0's cycle count, `ns` the same in nanoseconds.
    #[cfg(feature = "pv-buffers")]
    #[inline(never)]
    fn benchmark_record(&mut self, cpu: &Cpu, tag: u32) -> bool {
        let cycles = board::mcycle64();
        let instret = board::minstret64();
        self.bench.sample_xip();
        let n = self.bench.log.points().len();
        let ns = u64::try_from(u128::from(cycles) * 1_000_000_000 / u128::from(self.sys_hz)).unwrap_or(u64::MAX);
        if !self.bench.log.record(cpu, tag, cycles, ns) {
            return false;
        }
        self.bench.host[n] = [cycles, instret, self.bench.xip_hit, self.bench.xip_acc];
        true
    }
    /// The points so far, the native operations' stack peaks and both
    /// cores' stack depth, on the console; then an empty log.
    #[cfg(feature = "pv-buffers")]
    #[inline(never)]
    fn benchmark_report(&mut self, cpu: &Cpu) -> Option<u32> {
        use core::fmt::Write as _;
        let b = &mut *self.bench;
        let n = b.log.points().len();
        let mut w = ConWriter(&mut self.plat.con);
        let _ = writeln!(w, "\nbenchmark board sys_hz={} points={}", self.sys_hz, n);
        for (p, h) in b.log.points().iter().zip(b.host.iter()) {
            let _ = writeln!(w, "{p}");
            let _ = writeln!(
                w,
                "benchmark board tag={} cycles={} host_instret={} xip_hit={} xip_acc={}",
                p.tag, h[0], h[1], h[2], h[3]
            );
        }
        let _ = writeln!(w, "native buffer calls {:?}", cpu.stats.native_buffer_calls);
        if let Some(peaks) = cpu.native_stack_peaks() {
            let _ = writeln!(w, "native stack peaks {:?} of {} B", peaks, rvaccel_stack_bytes());
        }
        if let Some(erased) = cpu.native_stack_erased() {
            let _ = writeln!(w, "native stack erased {erased}");
        }
        let (lo, hi) = core0_stack();
        let _ = writeln!(
            w,
            "firmware stack use: core0 {} of {} B, core1 {} of {} B",
            core1::painted_stack_used(lo, hi),
            hi - lo,
            core1::stack_used(),
            core1::STACK_BYTES
        );
        b.log.clear();
        Some(n as u32)
    }
    fn mmio_load(&mut self, pa: u32, size: u32) -> Result<u32, u32> {
        self.plat.mmio_load(pa, size)
    }
    fn mmio_store(&mut self, pa: u32, size: u32, val: u32) -> Result<(), u32> {
        self.plat.mmio_store(pa, size, val)
    }
    fn external_irq(&self) -> bool {
        self.plat.plic.irq
    }
    fn console_putc(&mut self, b: u8) {
        self.plat.con.putc(b)
    }
    fn console_getc(&mut self) -> Option<u8> {
        self.plat.con.getc()
    }
    fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.plat.take_dma_write()
    }
    /// Interactive policy (as the Mac host's): wait in real time, at most
    /// 100 ms, for input or the timer deadline, then skip to the deadline.
    /// core1 bumps its event word when input or a frame arrives, which ends
    /// the wait at once.
    fn wfi(&mut self, cpu: &mut Cpu) {
        self.poll();
        if cpu.sip_value(self) & cpu.sie != 0 {
            return;
        }
        let now = cpu.time();
        if cpu.stimecmp <= now {
            return;
        }
        let hz = u64::from(self.sys_hz);
        let ticks = cpu.stimecmp - now;
        let wait = (ticks.saturating_mul(hz) / u64::from(TIMEBASE_HZ)).min(hz / 10);
        let t0 = board::mcycle64();
        // Erase one dirty swap sector (about 45 ms) unless the guest wakes
        // within 2 ms. Erasing stalls everything, so idle time is the place.
        let mut erased = false;
        if wait >= hz / 500
            && let Some(w) = &mut self.plat.swap
            && w.disk.wants_erase()
        {
            w.disk.idle_erase();
            erased = true;
        }
        while board::mcycle64() - t0 < wait {
            if core1::events() != self.seen {
                self.poll();
                if cpu.sip_value(self) & cpu.sie != 0 {
                    return;
                }
            }
            // a GPIO edge the guest armed ends the wait too
            self.plat.poll_pins();
            if self.plat.plic.irq && cpu.sip_value(self) & cpu.sie != 0 {
                return;
            }
        }
        if cpu.stimecmp == u64::MAX {
            return; // no timer armed: keep waiting for input
        }
        // an erase can outlast the wait: keep guest time with wall time
        let ticks = if erased {
            ticks.max((board::mcycle64() - t0) * u64::from(TIMEBASE_HZ) / hz)
        } else {
            ticks
        };
        cpu.time_skip += ticks;
        cpu.stats.idle_ticks += ticks;
    }
}

/// What core1 runs: the console, the radio, a status line on RTT.
struct Io<F> {
    console: usbcon::UsbConsole,
    radio: wifi::RadioService<F>,
    status: Status,
}

impl<F: core::future::Future<Output = ()>> core1::Service for Io<F> {
    #[unsafe(link_section = ".data.core1")]
    fn poll(&mut self) {
        self.console.poll();
        self.radio.poll();
        self.status.poll();
    }
}

unsafe extern "C" {
    /// core0's stack (riscv-rt's link.x): from `_estack` up to `_sstack`.
    static _estack: u32;
    static _sstack: u32;
}

fn core0_stack() -> (usize, usize) {
    (&raw const _estack as usize, &raw const _sstack as usize)
}

/// Every `STATUS_S` seconds: how deep both stacks have been, on RTT.
struct Status {
    next: u64,
    period: u64,
}

const STATUS_S: u32 = 10;

impl Status {
    #[unsafe(link_section = ".data.core1")]
    fn poll(&mut self) {
        let now = core1::now();
        if now < self.next {
            return;
        }
        self.next = now + self.period;
        let (lo, hi) = core0_stack();
        rprintln!(
            "[core1] stack use: core0 {} of {} B, core1 {} of {} B",
            core1::painted_stack_used(lo, hi),
            hi - lo,
            core1::stack_used(),
            core1::STACK_BYTES
        );
    }
}

// ------------------------------------------------------------------ host traps

#[unsafe(export_name = "ExceptionHandler")]
fn host_exception(_tf: &riscv_rt::TrapFrame) -> ! {
    let (cause, epc, tval): (u32, u32, u32);
    // SAFETY: reading trap CSRs.
    unsafe {
        core::arch::asm!("csrr {0}, mcause", "csrr {1}, mepc", "csrr {2}, mtval",
            out(reg) cause, out(reg) epc, out(reg) tval);
    }
    rprintln!("\n[rvemu-pico] HOST TRAP mcause={:#x} mepc={:#010x} mtval={:#010x}", cause, epc, tval);
    loop {
        hal::arch::wfi();
    }
}

fn fail(msg: &str) -> ! {
    rprintln!("[rvemu-pico] {}; rebooting to USB BOOTSEL", msg);
    board::reboot_to_bootsel();
}

// ------------------------------------------------------------------ run

#[inline(never)]
#[unsafe(link_section = ".data.rvemu_hot")]
fn jit_loop(jit: &mut Jit, cpu: &mut Cpu, bus: &mut PicoBus) {
    let mut events: u32 = 0;
    while cpu.halt.is_none() {
        jit.run_one(cpu, bus);
        events = events.wrapping_add(1);
        // ~1-2 ms at today's dispatch rate; one SRAM load unless core1 has
        // something for the guest, plus the pins' pending check
        if events & 0xFF == 0 {
            bus.poll();
        }
    }
}

#[hal::entry]
fn main() -> ! {
    let (jit, cpu, bus, sys_hz) = setup();
    jit_loop(jit, cpu, bus);
    let why = match cpu.halt {
        Some(Halt::Shutdown) => "guest shut down",
        _ => "guest stopped",
    };
    // let the last output reach the terminal
    let t0 = board::mcycle64();
    while !CON_OUT.is_empty() && board::mcycle64() - t0 < u64::from(sys_hz) {}
    fail(why);
}

/// Everything before the guest runs: the board, core1, the guest's devices
/// and the translator. Not inlined, so its temporaries (pin setup, device
/// construction) leave the stack before `jit_loop`.
#[inline(never)]
fn setup() -> (&'static mut Jit<'static>, &'static mut Cpu, &'static mut PicoBus, u32) {
    board::bootsel_if_requested();
    rtt_init_print!(ChannelMode::NoBlockSkip, 1024);
    let mut pac = hal::pac::Peripherals::take().unwrap();
    let mut watchdog = hal::Watchdog::new(pac.WATCHDOG);
    let clocks =
        board::init_clocks(pac.XOSC, pac.CLOCKS, pac.PLL_SYS, pac.PLL_USB, &mut pac.RESETS, &mut watchdog);
    board::timer_ignore_debug_halt();
    board::enable_counters();
    let mut sio = hal::Sio::new(pac.SIO);
    let pins = hal::gpio::Pins::new(pac.IO_BANK0, pac.PADS_BANK0, sio.gpio_bank0, &mut pac.RESETS);
    use hal::Clock as _;
    let sys_hz = clocks.system_clock.freq().to_Hz();
    flash::set_sys_hz(sys_hz);
    rprintln!("\n=== rvemu-pico shell: Hazard3 @ {} MHz{} ===", sys_hz / 1_000_000, board::CLOCK_NOTE);

    let psram = match board::psram_init(pins.gpio0, sys_hz) {
        Ok(p) => p,
        Err(_) => fail("PSRAM init failed"),
    };
    let h = BlobHeader::read();
    if psram.size < 8 << 20 || !h.valid(RAM_MAX) {
        rprintln!("no valid guest blob at {:#010x}", BLOB_ADDR);
        fail("run scripts/pico-shell.sh");
    }
    let seed = trng_seed(&pac.TRNG, &mut pac.RESETS);

    let radio_pins = RadioPins { pwr: pins.gpio23, dio: pins.gpio24, cs: pins.gpio25, clk: pins.gpio29 };
    let creds = Creds::from_flash();
    if let Some(c) = creds {
        rprintln!("[wifi] joining {:?} in the background", c.ssid());
    }

    // The guest's GPIO, I2C, SPI, PWM and ADC (rvcore::periph, pins.rs).
    let peri_hz = clocks.peripheral_clock.freq().to_Hz();
    let mut gp: [Option<Gp>; rvcore::periph::NGPIO as usize] = [const { None }; rvcore::periph::NGPIO as usize];
    macro_rules! free {
        ($($n:literal $f:ident),*) => { $( gp[$n] = Some(Gp::new(pins.$f)); )* };
    }
    free!(1 gpio1, 2 gpio2, 3 gpio3, 8 gpio8, 9 gpio9, 10 gpio10, 11 gpio11, 12 gpio12, 13 gpio13, 14 gpio14,
        15 gpio15, 20 gpio20, 21 gpio21, 22 gpio22, 26 gpio26, 27 gpio27, 28 gpio28, 30 gpio30, 31 gpio31,
        32 gpio32, 33 gpio33, 34 gpio34, 35 gpio35, 36 gpio36, 37 gpio37, 38 gpio38, 39 gpio39);
    debug_assert!((0..rvcore::periph::NGPIO as usize).all(|n| gp[n].is_some() == (rvcore::periph::FREE_PINS >> n & 1 != 0)));
    use hal::fugit::RateExtU32;
    let i2c0 = hal::I2C::i2c0(
        pac.I2C0,
        pins.gpio4.reconfigure(),
        pins.gpio5.reconfigure(),
        pins::i2c_hz(),
        &mut pac.RESETS,
        sys_hz.Hz(),
    );
    let i2c1 = hal::I2C::i2c1(
        pac.I2C1,
        pins.gpio6.reconfigure(),
        pins.gpio7.reconfigure(),
        pins::i2c_hz(),
        &mut pac.RESETS,
        sys_hz.Hz(),
    );
    let spi = hal::spi::Spi::<_, _, _, 8>::new(
        pac.SPI0,
        (pins.gpio19.reconfigure(), pins.gpio16.reconfigure(), pins.gpio18.reconfigure()),
    )
    .init(&mut pac.RESETS, peri_hz.Hz(), 1.MHz(), pins::SPI_MODE);
    let adc_pins = [
        pins.gpio40.into_floating_disabled().into_dyn_pin(),
        pins.gpio41.into_floating_disabled().into_dyn_pin(),
        pins.gpio42.into_floating_disabled().into_dyn_pin(),
        pins.gpio43.into_floating_disabled().into_dyn_pin(),
        pins.gpio44.into_floating_disabled().into_dyn_pin(),
        pins.gpio45.into_floating_disabled().into_dyn_pin(),
        pins.gpio46.into_floating_disabled().into_dyn_pin(),
        pins.gpio47.into_floating_disabled().into_dyn_pin(),
    ];
    let set = PinSet { gp, i2c0, i2c1, spi, spi_cs: pins.gpio17.reconfigure(), adc_pins };
    let board_pins = BoardPins::new(set, pac.PWM, pac.ADC, &mut pac.RESETS, sys_hz, peri_hz);

    // SAFETY: initialised once, here, before anything borrows it.
    let usb_bus: &'static UsbBusAllocator<hal::usb::UsbBus> = unsafe {
        (*addr_of_mut!(USB_BUS)).write(UsbBusAllocator::new(hal::usb::UsbBus::new(
            pac.USB,
            pac.USB_DPRAM,
            clocks.usb_clock,
            true,
            &mut pac.RESETS,
        )))
    };
    let serial = SerialPort::new(usb_bus);
    let usb = UsbDeviceBuilder::new(usb_bus, UsbVidPid(0x2e8a, 0x000a))
        .strings(&[StringDescriptors::default()
            .manufacturer("rp2350-linux")
            .product("rvemu Linux console")
            .serial_number("rvemu")])
        .unwrap()
        .device_class(usbd_serial::USB_CLASS_CDC)
        .build();
    let ms = |n: u32| u64::from(sys_hz / 1000) * u64::from(n);
    core1::paint_stack(core0_stack().0, 256);
    // The radio comes up on core1 while the guest boots; the guest waits
    // for its MAC (at most `RADIO_BRING_UP_MS`) when it probes the NIC.
    let resets = &mut pac.RESETS;
    core1::launch(&mut sio.fifo, &mut pac.PSM, sys_hz, move || Io {
        console: usbcon::UsbConsole::new(usb, serial, sys_hz),
        radio: wifi::RadioService::new(
            wifi::driver(pac.PIO0, resets, sys_hz, radio_pins, creds),
            sys_hz,
            RADIO_BRING_UP_MS,
        ),
        status: Status { next: 0, period: u64::from(sys_hz) * u64::from(STATUS_S) },
    });
    // Self-test of what flash writes use.
    let t0 = board::mcycle64();
    core1::park();
    core1::unpark();
    rprintln!("core1 running; park/unpark took {} cycles", board::mcycle64() - t0);
    let con = RingConsole { wait_cycles: ms(1000) };
    let disk = flash_disk();
    let disk_kib = disk.as_ref().map_or(0, |d| d.sectors() / 2);
    if disk.is_none() && h.initrd_len == 0 {
        fail("no root filesystem: no initramfs in the blob and no squashfs at 0x10600000");
    }
    let gmem = GuestMem { ptr: h.ram_host() as *mut u8, base: 0, size: h.ram_size, ro: h.text_len };
    // SAFETY: written once, here, before anything borrows it.
    let bus = unsafe {
        (*addr_of_mut!(BUS)).write(PicoBus {
            plat: Platform::new(con),
            sys_hz,
            seen: core1::events().wrapping_sub(1),
            // PSRAM is up (psram_init above); the reservation is outside
            // guest RAM (h.valid(RAM_MAX)) and used by nothing else.
            #[cfg(feature = "pv-buffers")]
            bench: {
                let at = BENCH_BASE as *mut Bench;
                core::ptr::write_bytes(at, 0, 1);
                &mut *at
            },
        })
    };
    add_devices(bus, board_pins, disk, gmem, ms(RADIO_BRING_UP_MS));

    // SAFETY: guest RAM is the mapped, writable PSRAM; the header was checked.
    let dtb = unsafe { h.load() };
    // SAFETY: the DTB `load` just copied into guest RAM (PSRAM).
    let dtb_bytes = unsafe { core::slice::from_raw_parts_mut((h.ram_host() + dtb as usize) as *mut u8, h.dtb_len as usize) };
    if patch_rng_seed(dtb_bytes, &seed) {
        rprintln!("[rvemu-pico] rng-seed filled from the TRNG");
    } else {
        rprintln!("[rvemu-pico] the DTB has no rng-seed to fill from the TRNG");
    }
    // The swap device, only when the DTB has it; after `load`, which clears
    // guest RAM. Writes start at a different sector each boot, which spreads
    // erases over the region across boots.
    let swap_on = dtb_has_swap(&h) && h.dtb_len as usize <= 0x10000 - SWAP_TABLES;
    if swap_on {
        let at = (h.ram_host() + h.ram_size as usize - SWAP_TABLES) as *mut FlashSwap<SwapFlash>;
        // SAFETY: the top page of guest RAM (PSRAM, page aligned), reserved
        // from the guest by its DTB and above the DTB itself.
        let swap = unsafe { FlashSwap::init_at(at, SwapFlash::new(sys_hz), SWAP_SPARE) };
        swap.set_head(board::mcycle64() as u32);
        bus.plat.swap = Some(VirtioBlk::new(swap, gmem));
    }
    // SAFETY: CPU and the translator tables are used only from here on.
    let cpu = unsafe {
        let p = (*addr_of_mut!(CPU)).as_mut_ptr();
        let c = Cpu::init(p, h.ram_host() as *mut u8, 0, h.ram_size, 0, dtb);
        #[cfg(feature = "pv-buffers")]
        // PSRAM has been initialized and checked. h.valid(RAM_MAX) excludes
        // this reservation from both guest RAM and the translator store.
        c.set_native_workspace(rvcore::NativeWorkspace::init_at(NATIVE_BASE as *mut _));
        c.set_readonly_prefix(h.text_len);
        c
    };
    let jit = make_jit(cpu);
    // Guest RAM is PSRAM, where Hazard3 AMOs fault.
    jit.native_amo = false;
    rprintln!(
        "guest RAM {} KiB, translator {} KiB code cache + {} KiB PSRAM store, disk {} KiB, swap {}; console on USB",
        h.ram_size >> 10,
        CODE_WORDS * 4 / 1024,
        STORE_WORDS * 4 / 1024,
        disk_kib,
        if swap_on { "on" } else { "off" }
    );
    (jit, cpu, bus, sys_hz)
}

/// The guest's devices, built straight into the static bus (a `with_*`
/// chain would keep a copy of the whole Platform per step on the stack), in
/// a frame of their own.
#[inline(never)]
fn add_devices(bus: &mut PicoBus, pins: BoardPins, disk: Option<MemDisk>, gmem: GuestMem, radio_wait: u64) {
    bus.plat.net = Some(VirtioNet::new(RingLink { wait_cycles: radio_wait }, gmem));
    bus.plat.nor = Some(NorDev::new(BoardNor, gmem));
    bus.plat.io = Some(rvcore::periph::Io::new(pins, gmem));
    if let Some(d) = disk {
        bus.plat.blk = Some(VirtioBlk::new(d, gmem));
    }
}

/// The translator, in the static `JIT`, in a frame of its own.
#[inline(never)]
fn make_jit(cpu: &mut Cpu) -> &'static mut Jit<'static> {
    // SAFETY: the translator tables are used only by this Jit, from here on.
    unsafe {
        (*addr_of_mut!(JIT)).write(Jit::new(
            cpu,
            JitMem {
                code: &mut *addr_of_mut!(CODE),
                buckets: &mut *addr_of_mut!(BUCKETS),
                code_pages: &mut *addr_of_mut!(CODE_PAGES),
                jump_cache: &mut *addr_of_mut!(JUMP_CACHE),
                regions: REGIONS,
                chain_log: &mut *addr_of_mut!(CHAIN_LOG),
                // SAFETY: the top MiB of the PSRAM, above guest RAM (psram_init
                // found 8 MB; the blob's RAM size is at most RAM_MAX).
                store: core::slice::from_raw_parts_mut(STORE as *mut u32, STORE_WORDS),
                store_epochs: &mut *addr_of_mut!(STORE_EPOCHS),
            },
        ))
    }
}

#[unsafe(link_section = ".bi_entries")]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_cargo_bin_name!(),
    hal::binary_info::rp_program_description!(c"rvemu: interactive Linux, console on USB"),
];

#[cfg(feature = "pv-buffers")]
fn rvaccel_stack_bytes() -> usize {
    rvcore::NATIVE_STACK_BYTES
}
