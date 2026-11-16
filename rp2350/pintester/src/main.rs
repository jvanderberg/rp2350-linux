//! Pin test fixture: a second RP2350B board (the old Pico Stretch, the SWD
//! probe's board) wired to the Linux board's pins, driven over USB serial
//! (`/dev/cu.usbmodempintest1`) by `../tools/pin-test.py`.
//!
//! One command per line; every reply ends with a line `ok` or `err WHY`.
//!
//! | Command | Does |
//! |---|---|
//! | `id` | `pintester` |
//! | `out P V` | GP`P` an output at `V` (0/1) |
//! | `in P [u\|d\|n]` | GP`P` an input with a pull-up, pull-down or neither (default); prints `P=V` |
//! | `get P` | GP`P`'s level, `P=V` |
//! | `pulse P N US` | toggle output GP`P` `N` times, `US` microseconds apart |
//! | `meas P MS` | watch GP`P` for `MS` ms: `rises=R falls=F hz=H duty=D` (duty in %, of the samples) |
//! | `regs` | the I2C target's first 16 registers |
//! | `i2c` | the I2C target's counters |
//! | `bootsel` | reboot into USB BOOTSEL (to load debugprobe back) |
//!
//! The I2C target is always on: I2C1, GP14 SDA / GP15 SCL, address 0x42,
//! with internal pull-ups. It is a 256-byte register file like a 24C02: a
//! write's first byte sets the register pointer and the rest are stored from
//! there; a read returns bytes from the pointer. Both advance the pointer.
//! Opening the port at 1200 baud and closing it also reboots into BOOTSEL.
#![no_std]
#![no_main]

use core::fmt::Write as _;

use panic_rtt_target as _;
use rp2350_board as board;
use rp235x_hal as hal;
use usb_device::class_prelude::UsbBusAllocator;
use usb_device::prelude::*;
use usbd_serial::SerialPort;

#[link_section = ".start_block"]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

#[link_section = ".bi_entries"]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_program_name!(c"pintester"),
    hal::binary_info::rp_program_description!(c"GPIO/PWM/I2C test fixture over USB serial"),
];

/// 7-bit (a `u16` would make the HAL set the target up for 10-bit addressing).
const I2C_ADDR: u8 = 0x42;
/// Pins the I2C target owns.
const I2C_SDA: u32 = 14;
const I2C_SCL: u32 = 15;

/// Pad bits (PADS_BANK0 GPIOn).
const PAD_SCHMITT: u32 = 1 << 1;
const PAD_PDE: u32 = 1 << 2;
const PAD_PUE: u32 = 1 << 3;
const PAD_DRIVE_4MA: u32 = 1 << 4;
const PAD_IE: u32 = 1 << 6;
/// IO_BANK0 GPIOn_CTRL FUNCSEL: SIO.
const FUNC_SIO: u32 = 5;

/// DesignWare I2C registers (offsets from I2C1's base).
const IC_DATA_CMD: usize = 0x10;
const IC_RAW_INTR_STAT: usize = 0x34;
const IC_CLR_RD_REQ: usize = 0x50;
const IC_CLR_TX_ABRT: usize = 0x54;
const IC_CLR_STOP_DET: usize = 0x60;
const IC_STATUS: usize = 0x70;
const RAW_RD_REQ: u32 = 1 << 5;
const RAW_TX_ABRT: u32 = 1 << 6;
const RAW_STOP_DET: u32 = 1 << 9;
const STATUS_RFNE: u32 = 1 << 3;
const DATA_FIRST_BYTE: u32 = 1 << 11;

static mut USB_BUS: core::mem::MaybeUninit<UsbBusAllocator<hal::usb::UsbBus>> = core::mem::MaybeUninit::uninit();

fn sio() -> &'static hal::pac::sio::RegisterBlock {
    // SAFETY: SIO's GPIO registers; the set/clear ones are atomic.
    unsafe { &*hal::pac::SIO::ptr() }
}

/// GP`p` as an SIO pin with these pad bits (input enabled, isolation off).
fn pin_setup(p: u32, pad: u32) {
    // SAFETY: one pin's pad and function registers, ours.
    unsafe {
        (*hal::pac::PADS_BANK0::ptr()).gpio(p as usize).write(|w| w.bits(pad | PAD_IE | PAD_SCHMITT | PAD_DRIVE_4MA));
        (*hal::pac::IO_BANK0::ptr()).gpio(p as usize).gpio_ctrl().write(|w| w.bits(FUNC_SIO));
    }
}

fn set_oe(p: u32, on: bool) {
    let s = sio();
    // SAFETY: atomic set/clear registers; any mask is valid.
    unsafe {
        match (p < 32, on) {
            (true, true) => s.gpio_oe_set().write(|w| w.bits(1 << p)),
            (true, false) => s.gpio_oe_clr().write(|w| w.bits(1 << p)),
            (false, true) => s.gpio_hi_oe_set().write(|w| w.bits(1 << (p - 32))),
            (false, false) => s.gpio_hi_oe_clr().write(|w| w.bits(1 << (p - 32))),
        }
    }
}

fn set_out(p: u32, v: bool) {
    let s = sio();
    // SAFETY: as in `set_oe`.
    unsafe {
        match (p < 32, v) {
            (true, true) => s.gpio_out_set().write(|w| w.bits(1 << p)),
            (true, false) => s.gpio_out_clr().write(|w| w.bits(1 << p)),
            (false, true) => s.gpio_hi_out_set().write(|w| w.bits(1 << (p - 32))),
            (false, false) => s.gpio_hi_out_clr().write(|w| w.bits(1 << (p - 32))),
        }
    }
}

#[inline(always)]
fn level(p: u32) -> bool {
    let s = sio();
    if p < 32 {
        s.gpio_in().read().bits() >> p & 1 != 0
    } else {
        s.gpio_hi_in().read().bits() >> (p - 32) & 1 != 0
    }
}

/// The I2C target's state: the register file and counters.
struct Target {
    regs: [u8; 256],
    ptr: u8,
    writes: u32,
    reads: u32,
    stops: u32,
    aborts: u32,
}

fn i2c_reg(off: usize) -> *mut u32 {
    (hal::pac::I2C1::ptr() as usize + off) as *mut u32
}

impl Target {
    /// Serve the bus: take received bytes, answer a read request with the
    /// next register (one byte per request, so the pointer stays exact).
    fn poll(&mut self) {
        // SAFETY: I2C1's registers; the block is ours (configured in main).
        unsafe {
            while i2c_reg(IC_STATUS).read_volatile() & STATUS_RFNE != 0 {
                let d = i2c_reg(IC_DATA_CMD).read_volatile();
                if d & DATA_FIRST_BYTE != 0 {
                    self.ptr = d as u8;
                } else {
                    self.regs[self.ptr as usize] = d as u8;
                    self.ptr = self.ptr.wrapping_add(1);
                    self.writes += 1;
                }
            }
            let raw = i2c_reg(IC_RAW_INTR_STAT).read_volatile();
            if raw & RAW_TX_ABRT != 0 {
                let _ = i2c_reg(IC_CLR_TX_ABRT).read_volatile();
                self.aborts += 1;
            }
            if raw & RAW_RD_REQ != 0 {
                i2c_reg(IC_DATA_CMD).write_volatile(u32::from(self.regs[self.ptr as usize]));
                self.ptr = self.ptr.wrapping_add(1);
                self.reads += 1;
                let _ = i2c_reg(IC_CLR_RD_REQ).read_volatile();
            }
            if raw & RAW_STOP_DET != 0 {
                let _ = i2c_reg(IC_CLR_STOP_DET).read_volatile();
                self.stops += 1;
            }
        }
    }
}

/// A reply being built.
struct Out {
    buf: [u8; 1024],
    len: usize,
}

impl core::fmt::Write for Out {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        let b = s.as_bytes();
        let n = b.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&b[..n]);
        self.len += n;
        Ok(())
    }
}

struct Clock {
    timer: hal::Timer<hal::timer::CopyableTimer0>,
}

impl Clock {
    fn us(&self) -> u64 {
        self.timer.get_counter().ticks()
    }
    fn wait_until(&self, t: u64) {
        while self.us() < t {}
    }
}

fn parse(s: Option<&str>) -> Option<u32> {
    let s = s?;
    if let Some(h) = s.strip_prefix("0x") {
        u32::from_str_radix(h, 16).ok()
    } else {
        s.parse().ok()
    }
}

fn pin_arg(s: Option<&str>) -> Result<u32, &'static str> {
    match parse(s) {
        Some(p) if p < 48 && p != I2C_SDA && p != I2C_SCL => Ok(p),
        Some(_) => Err("pin out of range (0-47, not the I2C target's 14/15)"),
        None => Err("pin?"),
    }
}

/// Watch `p` for `ms`: edges, the frequency from the first to the last
/// rising edge, and the share of samples it was high.
fn measure(clock: &Clock, o: &mut Out, p: u32, ms: u32) {
    let start = clock.us();
    let end = start + u64::from(ms) * 1000;
    let mut last = level(p);
    let (mut rises, mut falls, mut high, mut samples) = (0u32, 0u32, 0u64, 0u64);
    let (mut first_rise, mut last_rise) = (0u64, 0u64);
    loop {
        let now = clock.us();
        if now >= end {
            break;
        }
        let v = level(p);
        samples += 1;
        high += u64::from(v);
        if v != last {
            if v {
                if rises == 0 {
                    first_rise = now;
                }
                last_rise = now;
                rises += 1;
            } else {
                falls += 1;
            }
            last = v;
        }
    }
    let hz = if rises > 1 && last_rise > first_rise {
        (rises - 1) as f32 * 1e6 / (last_rise - first_rise) as f32
    } else {
        0.0
    };
    let duty = if samples != 0 { high as f32 * 100.0 / samples as f32 } else { 0.0 };
    let _ = writeln!(o, "rises={rises} falls={falls} hz={hz:.1} duty={duty:.1} samples={samples}");
}

fn run(line: &str, o: &mut Out, clock: &Clock, t: &mut Target) -> Result<(), &'static str> {
    let mut a = line.split_ascii_whitespace();
    match a.next() {
        None => Ok(()),
        Some("id") => {
            let _ = writeln!(o, "pintester");
            Ok(())
        }
        Some("out") => {
            let p = pin_arg(a.next())?;
            let v = parse(a.next()).ok_or("value?")? != 0;
            set_out(p, v);
            pin_setup(p, 0);
            set_oe(p, true);
            Ok(())
        }
        Some("in") => {
            let p = pin_arg(a.next())?;
            let pad = match a.next() {
                Some("u") => PAD_PUE,
                Some("d") => PAD_PDE,
                None | Some("n") => 0,
                _ => return Err("pull is u, d or n"),
            };
            set_oe(p, false);
            pin_setup(p, pad);
            clock.wait_until(clock.us() + 50);
            let _ = writeln!(o, "{p}={}", u8::from(level(p)));
            Ok(())
        }
        Some("get") => {
            let p = pin_arg(a.next())?;
            let _ = writeln!(o, "{p}={}", u8::from(level(p)));
            Ok(())
        }
        Some("pulse") => {
            let p = pin_arg(a.next())?;
            let n = parse(a.next()).ok_or("count?")?;
            let us = u64::from(parse(a.next()).ok_or("microseconds?")?);
            let mut v = level(p);
            let mut next = clock.us();
            for _ in 0..n {
                next += us;
                clock.wait_until(next);
                v = !v;
                set_out(p, v);
            }
            Ok(())
        }
        Some("meas") => {
            let p = pin_arg(a.next())?;
            let ms = parse(a.next()).unwrap_or(100).min(5000);
            measure(clock, o, p, ms);
            Ok(())
        }
        Some("regs") => {
            for r in &t.regs[..16] {
                let _ = write!(o, "{r:02x} ");
            }
            let _ = writeln!(o);
            Ok(())
        }
        Some("i2c") => {
            let _ = writeln!(o, "addr=0x{I2C_ADDR:02x} writes={} reads={} stops={} aborts={} ptr={}", t.writes, t.reads, t.stops, t.aborts, t.ptr);
            Ok(())
        }
        Some("bootsel") => board::reboot_to_bootsel(),
        Some(_) => Err("unknown command"),
    }
}

#[hal::entry]
fn main() -> ! {
    let mut pac = hal::pac::Peripherals::take().unwrap();
    let mut watchdog = hal::Watchdog::new(pac.WATCHDOG);
    let clocks = board::init_clocks(pac.XOSC, pac.CLOCKS, pac.PLL_SYS, pac.PLL_USB, &mut pac.RESETS, &mut watchdog);
    let sio = hal::Sio::new(pac.SIO);
    let pins = hal::gpio::Pins::new(pac.IO_BANK0, pac.PADS_BANK0, sio.gpio_bank0, &mut pac.RESETS);
    let clock = Clock { timer: hal::Timer::new_timer0(pac.TIMER0, &mut pac.RESETS, &clocks) };

    let sda: hal::gpio::Pin<_, hal::gpio::FunctionI2C, hal::gpio::PullUp> = pins.gpio14.reconfigure();
    let scl: hal::gpio::Pin<_, hal::gpio::FunctionI2C, hal::gpio::PullUp> = pins.gpio15.reconfigure();
    let _i2c = hal::I2C::new_peripheral_event_iterator(pac.I2C1, sda, scl, &mut pac.RESETS, I2C_ADDR);
    let mut target = Target { regs: [0; 256], ptr: 0, writes: 0, reads: 0, stops: 0, aborts: 0 };
    for (i, r) in target.regs.iter_mut().enumerate() {
        *r = i as u8;
    }

    // SAFETY: initialised once, here, before anything borrows it.
    let usb_bus: &'static UsbBusAllocator<hal::usb::UsbBus> = unsafe {
        (*core::ptr::addr_of_mut!(USB_BUS)).write(UsbBusAllocator::new(hal::usb::UsbBus::new(
            pac.USB,
            pac.USB_DPRAM,
            clocks.usb_clock,
            true,
            &mut pac.RESETS,
        )))
    };
    let mut serial = SerialPort::new(usb_bus);
    let mut usb = UsbDeviceBuilder::new(usb_bus, UsbVidPid(0x2e8a, 0x000a))
        .strings(&[StringDescriptors::default()
            .manufacturer("rp2350-linux")
            .product("pin tester")
            .serial_number("pintest")])
        .unwrap()
        .device_class(usbd_serial::USB_CLASS_CDC)
        .build();

    let mut line = [0u8; 128];
    let mut n = 0;
    loop {
        target.poll();
        usb.poll(&mut [&mut serial]);
        if serial.line_coding().data_rate() == 1200 && !serial.dtr() {
            board::reboot_to_bootsel();
        }
        let mut buf = [0u8; 64];
        let got = match serial.read(&mut buf) {
            Ok(k) => k,
            Err(_) => 0,
        };
        for &c in &buf[..got] {
            if c != b'\n' && c != b'\r' {
                if n < line.len() {
                    line[n] = c;
                    n += 1;
                }
                continue;
            }
            let mut o = Out { buf: [0; 1024], len: 0 };
            let cmd = core::str::from_utf8(&line[..n]).unwrap_or("");
            if !cmd.trim().is_empty() {
                match run(cmd, &mut o, &clock, &mut target) {
                    Ok(()) => {
                        let _ = writeln!(o, "ok");
                    }
                    Err(e) => {
                        let _ = writeln!(o, "err {e}");
                    }
                }
            }
            n = 0;
            // send it all (or give up after a second without a reader)
            let mut sent = 0;
            let give_up = clock.us() + 1_000_000;
            while sent < o.len && clock.us() < give_up {
                usb.poll(&mut [&mut serial]);
                target.poll();
                if let Ok(k) = serial.write(&o.buf[sent..o.len]) {
                    sent += k;
                }
            }
        }
    }
}
