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
use rvcore::virtio::{Disk as _, FRAME_MAX, GuestMem, MemDisk, NetLink};
use rvcore::{Bus, Cpu, Halt, TIMEBASE_HZ};
use rvjit::{Block, JcEntry, Jit, JitMem};
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
use wifi::{Creds, RadioPins};

#[unsafe(link_section = ".start_block")]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

/// PSRAM: guest RAM, then the translator's backing store. The store is off:
/// on the board its traffic shares the 16 KB XIP cache with guest RAM, and a
/// 1 MiB store made the boot slower (9.9 s against 8.0 s; XIP misses 3.7 M
/// -> 12.7 M). With `STORE_WORDS = (1 << 20) / 4` the guest gets 7 MiB.
const STORE_WORDS: usize = 0;
const RAM_MAX: u32 = (8 << 20) - (STORE_WORDS * 4) as u32;
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

// ------------------------------------------------------------------ translator storage (SRAM)

/// The translator's code cache: what SRAM has left once core1's code and
/// buffers (`core1.rs`) and core0's stack (9 KB deep after a gate run;
/// 11.6 KB here) have theirs.
const CODE_KIB: usize = 272;
const CODE_WORDS: usize = CODE_KIB * 256;
const NBLOCKS: usize = 4096;
const NBUCKETS: usize = 1024;
const NJC: usize = 1024;
/// Ring of 8 regions (simulator: 7.9 s to login: against 8.8 s flush-all);
/// the boot needs at most ~75 logged chains per region.
const REGIONS: usize = 8;

static mut CODE: [u32; CODE_WORDS] = [0; CODE_WORDS];
static mut BLOCKS: [Block; NBLOCKS] = [Block::EMPTY; NBLOCKS];
static mut BUCKETS: [u32; NBUCKETS] = [0; NBUCKETS];
/// Guest RAM as Linux sees it, at most: flash-backed kernel text + PSRAM.
const GUEST_MAX: u32 = RAM_MAX + guest_blob::TEXT_MAX;
static mut CODE_PAGES: [u8; (GUEST_MAX >> 15) as usize] = [0; (GUEST_MAX >> 15) as usize];
static mut JUMP_CACHE: [JcEntry; NJC] = [JcEntry::EMPTY; NJC];
static mut CHAIN_LOG: [u32; REGIONS * 128] = [0; REGIONS * 128];
static mut STORE_EPOCHS: [u8; (GUEST_MAX >> 12) as usize] = [0; (GUEST_MAX >> 12) as usize];
static mut CPU: MaybeUninit<Cpu> = MaybeUninit::uninit();
static mut USB_BUS: MaybeUninit<UsbBusAllocator<hal::usb::UsbBus>> = MaybeUninit::uninit();

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
    plat: Platform<RingConsole, MemDisk, RingLink>,
    sys_hz: u32,
    /// The last `core1::events()` acted on.
    seen: u32,
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
    }
}

impl Bus for PicoBus {
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
        while board::mcycle64() - t0 < wait {
            if core1::events() != self.seen {
                self.poll();
                if cpu.sip_value(self) & cpu.sie != 0 {
                    return;
                }
            }
        }
        if cpu.stimecmp == u64::MAX {
            return; // no timer armed: keep waiting for input
        }
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
        // something for the guest
        if events & 0xFF == 0 {
            bus.poll();
        }
    }
}

#[hal::entry]
fn main() -> ! {
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

    let radio_pins = RadioPins { pwr: pins.gpio23, dio: pins.gpio24, cs: pins.gpio25, clk: pins.gpio29 };
    let creds = Creds::from_flash();
    if let Some(c) = creds {
        rprintln!("[wifi] joining {:?} in the background", c.ssid());
    }

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
    let mut plat = Platform::new(con).with_net(RingLink { wait_cycles: ms(RADIO_BRING_UP_MS) }, gmem);
    if let Some(d) = disk {
        plat = plat.with_disk(d, gmem);
    }
    let mut bus = PicoBus { plat, sys_hz, seen: core1::events().wrapping_sub(1) };

    // SAFETY: guest RAM is the mapped, writable PSRAM; the header was checked.
    let dtb = unsafe { h.load() };
    // SAFETY: CPU and the translator tables are used only from here on.
    let cpu = unsafe {
        let p = (*addr_of_mut!(CPU)).as_mut_ptr();
        let c = Cpu::init(p, h.ram_host() as *mut u8, 0, h.ram_size, 0, dtb);
        c.set_readonly_prefix(h.text_len);
        c
    };
    // SAFETY: as above.
    let mut jit = unsafe {
        Jit::new(
            cpu,
            JitMem {
                code: &mut *addr_of_mut!(CODE),
                buckets: &mut *addr_of_mut!(BUCKETS),
                blocks: &mut *addr_of_mut!(BLOCKS),
                code_pages: &mut *addr_of_mut!(CODE_PAGES),
                jump_cache: &mut *addr_of_mut!(JUMP_CACHE),
                regions: REGIONS,
                chain_log: &mut *addr_of_mut!(CHAIN_LOG),
                // SAFETY: the top MiB of the PSRAM, above guest RAM (psram_init
                // found 8 MB; the blob's RAM size is at most RAM_MAX).
                store: core::slice::from_raw_parts_mut(STORE as *mut u32, STORE_WORDS),
                store_epochs: &mut *addr_of_mut!(STORE_EPOCHS),
            },
        )
    };
    // Guest RAM is PSRAM, where Hazard3 AMOs fault.
    jit.native_amo = false;
    rprintln!(
        "guest RAM {} KiB, translator {} KiB code cache + {} KiB PSRAM store, disk {} KiB; console on USB",
        h.ram_size >> 10,
        CODE_WORDS * 4 / 1024,
        STORE_WORDS * 4 / 1024,
        disk_kib
    );
    jit_loop(&mut jit, cpu, &mut bus);
    let why = match cpu.halt {
        Some(Halt::Shutdown) => "guest shut down",
        _ => "guest stopped",
    };
    // let the last output reach the terminal
    let t0 = board::mcycle64();
    while !CON_OUT.is_empty() && board::mcycle64() - t0 < u64::from(sys_hz) {}
    fail(why);
}

#[unsafe(link_section = ".bi_entries")]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_cargo_bin_name!(),
    hal::binary_info::rp_program_description!(c"rvemu: interactive Linux, console on USB"),
];
