//! Interactive Linux on the RP2350B: the guest's UART is a USB CDC-ACM serial
//! port on the target's USB (`screen /dev/cu.usbmodem*`), translator only.
//!
//! Guest kernel / initramfs / DTB come from the flash blob, and the root
//! filesystem, when there is one, from a squashfs at `DISK_ADDR` behind a
//! read-only virtio disk (scripts/pico-shell.sh loads them and this firmware
//! over BOOTSEL); guest RAM is the PSRAM. While the guest idles in WFI the firmware waits in real time
//! (polling USB) up to the next timer deadline, like the Mac host's
//! interactive mode. Console output is kept in a ring while no terminal is
//! open, so connecting late shows the end of the boot log (`dmesg` has all
//! of the kernel's).
//!
//! The guest's Ethernet (virtio-net) is the RM2 Wi-Fi radio (`wifi.rs`);
//! scripts/pico-wifi.sh stores the network to join.
//!
//! Back to BOOTSEL: open the port at 1200 baud and close it
//! (`stty -f /dev/cu.usbmodemXXXX 1200`), or the SWD request in
//! `rp2350-board::bootsel_if_requested`. RTT carries status lines for a
//! debugger but never blocks.
#![no_std]
#![no_main]

use core::future::Future;
use core::mem::MaybeUninit;
use core::pin::{Pin, pin};
use core::ptr::addr_of_mut;

use panic_rtt_target as _;
use rp2350_board as board;
use rp235x_hal as hal;
use rtt_target::{ChannelMode, rprintln, rtt_init_print};
use rvcore::dev::{Console, Platform};
use rvcore::virtio::{Disk as _, GuestMem, MemDisk};
use rvcore::{Bus, Cpu, Halt, TIMEBASE_HZ};
use rvjit::{Block, JcEntry, Jit, JitMem};
use usb_device::class_prelude::UsbBusAllocator;
use usb_device::prelude::*;
use usbd_serial::SerialPort;

#[path = "../guest_blob.rs"]
mod guest_blob;
use guest_blob::{BLOB_ADDR, BlobHeader};

#[path = "../wifi.rs"]
mod wifi;
use wifi::{Creds, Radio, RadioPins, WifiLink};

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

/// 320 KiB: what the SRAM holds without the lockstep reference CPU and with
/// a small stack (the cache-policy simulator's `flush320`).
const CODE_WORDS: usize = 80 * 1024;
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

// ------------------------------------------------------------------ console

/// Byte ring; when full, `push` drops the oldest byte.
struct Ring<const N: usize> {
    buf: [u8; N],
    head: usize,
    len: usize,
}

impl<const N: usize> Ring<N> {
    const fn new() -> Self {
        Ring { buf: [0; N], head: 0, len: 0 }
    }
    fn push(&mut self, b: u8) {
        if self.len == N {
            self.head = (self.head + 1) % N;
            self.len -= 1;
        }
        self.buf[(self.head + self.len) % N] = b;
        self.len += 1;
    }
    fn pop(&mut self) -> Option<u8> {
        if self.len == 0 {
            return None;
        }
        let b = self.buf[self.head];
        self.head = (self.head + 1) % N;
        self.len -= 1;
        Some(b)
    }
    /// The oldest contiguous run of bytes.
    fn front(&self) -> &[u8] {
        let end = (self.head + self.len).min(N);
        &self.buf[self.head..end]
    }
    fn consume(&mut self, n: usize) {
        self.head = (self.head + n) % N;
        self.len -= n;
    }
}

/// Console output while no terminal is connected. A static, not a field:
/// `main` moves the console into the platform and the bus by value, and a
/// 16 KB ring in each of those stack slots overflowed the stack.
static mut TX_RING: Ring<16384> = Ring::new();

struct UsbConsole {
    tx: &'static mut Ring<16384>,
    rx: Ring<256>,
}

impl Console for UsbConsole {
    fn putc(&mut self, b: u8) {
        self.tx.push(b);
    }
    fn rx_ready(&self) -> bool {
        self.rx.len != 0
    }
    fn getc(&mut self) -> Option<u8> {
        self.rx.pop()
    }
}

struct PicoBus<'a> {
    plat: Platform<UsbConsole, MemDisk, WifiLink, BoardPins>,
    radio: Radio<'a>,
    usb: UsbDevice<'static, hal::usb::UsbBus>,
    serial: SerialPort<'static, hal::usb::UsbBus>,
    sys_hz: u32,
}

impl PicoBus<'_> {
    /// Service USB: take input, send buffered output, honour the 1200-baud
    /// "touch" that asks for BOOTSEL. Run the radio driver. Then refresh the
    /// guest's interrupt lines (which also delivers received frames).
    fn poll(&mut self) {
        self.radio.poll();
        if self.usb.poll(&mut [&mut self.serial]) {
            let mut buf = [0u8; 64];
            while let Ok(n) = self.serial.read(&mut buf) {
                if n == 0 {
                    break;
                }
                for &b in &buf[..n] {
                    self.plat.con.rx.push(b);
                }
            }
        }
        if self.serial.line_coding().data_rate() == 1200 && !self.serial.dtr() {
            board::reboot_to_bootsel();
        }
        if self.serial.dtr() {
            while self.plat.con.tx.len != 0 {
                match self.serial.write(self.plat.con.tx.front()) {
                    Ok(n) if n > 0 => self.plat.con.tx.consume(n),
                    _ => break,
                }
            }
        }
        self.plat.update_irqs();
        self.plat.poll_pins();
    }
}

impl Bus for PicoBus<'_> {
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
            self.poll();
            if cpu.sip_value(self) & cpu.sie != 0 {
                return;
            }
        }
        if cpu.stimecmp == u64::MAX {
            return; // no timer armed: keep waiting for input
        }
        cpu.time_skip += ticks;
        cpu.stats.idle_ticks += ticks;
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
fn jit_loop(jit: &mut Jit, cpu: &mut Cpu, bus: &mut PicoBus<'_>) {
    let mut events: u32 = 0;
    while cpu.halt.is_none() {
        jit.run_one(cpu, bus);
        events = events.wrapping_add(1);
        // ~1-2 ms at today's dispatch rate: often enough for USB
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
    let sio = hal::Sio::new(pac.SIO);
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

    // The radio first: the guest reads its MAC when it probes the NIC.
    let radio_pins = RadioPins { pwr: pins.gpio23, dio: pins.gpio24, cs: pins.gpio25, clk: pins.gpio29 };
    let creds = Creds::from_flash();
    let driver: Pin<&mut dyn Future<Output = ()>> =
        pin!(wifi::driver(pac.PIO0, &mut pac.RESETS, sys_hz, radio_pins, creds));
    let mut radio = Radio::new(driver);
    if !radio.bring_up(5_000_000) {
        rprintln!("[wifi] radio did not come up; the guest's eth0 stays down");
    }
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
    // SAFETY: the one reference to TX_RING.
    let con = UsbConsole { tx: unsafe { &mut *addr_of_mut!(TX_RING) }, rx: Ring::new() };
    let disk = flash_disk();
    let disk_kib = disk.as_ref().map_or(0, |d| d.sectors() / 2);
    if disk.is_none() && h.initrd_len == 0 {
        fail("no root filesystem: no initramfs in the blob and no squashfs at 0x10600000");
    }
    let gmem = GuestMem { ptr: h.ram_host() as *mut u8, base: 0, size: h.ram_size, ro: h.text_len };
    let mut plat = Platform::new(con).with_net(WifiLink, gmem).with_io(board_pins, gmem);
    if let Some(d) = disk {
        plat = plat.with_disk(d, gmem);
    }
    let mut bus = PicoBus {
        plat,
        radio,
        usb,
        serial,
        sys_hz,
    };

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
    while board::mcycle64() - t0 < u64::from(sys_hz) {
        bus.poll();
    }
    fail(why);
}

#[unsafe(link_section = ".bi_entries")]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_cargo_bin_name!(),
    hal::binary_info::rp_program_description!(c"rvemu: interactive Linux, console on USB"),
];
