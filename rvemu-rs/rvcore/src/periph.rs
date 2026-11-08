//! The board's pins as guest devices: GPIO, two I2C buses, SPI, PWM and the
//! ADC, in guest MMIO 0xF8001000-0xF8002FFF on PLIC inputs 8-11.
//!
//! | Device | Address | IRQ | Guest driver |
//! |---|---|---|---|
//! | GPIO | 0xF8001000 | 8 | `rvemu,gpio` (guest/linux/drivers/rvemu) |
//! | I2C0, I2C1 | 0xF8001200, 0xF8001400 | 9, 10 | virtio-i2c |
//! | SPI0 | 0xF8001600 | 11 | `sifive,spi0` + spidev |
//! | PWM | 0xF8001800 | | `rvemu,pwm` |
//! | ADC | 0xF8001A00 | | `rvemu,adc` (IIO) |
//!
//! The models here only keep register state; the host's `Pins` does the I/O
//! (rp235x-hal drivers on the board, a simulated board on the Mac). Every
//! operation completes synchronously, inside the guest's register access.

use crate::cpu::exc;
use crate::virtio::{CHAIN_MAX, DESC_F_WRITE, Desc, GuestMem, Kick, NO_DESC, Transport};

pub const IO_BASE: u32 = 0xF800_1000;
pub const IO_SIZE: u32 = 0x2000;
pub const GPIO_BASE: u32 = 0xF800_1000;
pub const GPIO_SIZE: u32 = 0x200;
pub const GPIO_IRQ: u32 = 8;
pub const I2C_BASE: [u32; 2] = [0xF800_1200, 0xF800_1400];
pub const I2C_SIZE: u32 = 0x200;
pub const I2C_IRQ: [u32; 2] = [9, 10];
pub const SPI_BASE: u32 = 0xF800_1600;
pub const SPI_SIZE: u32 = 0x100;
pub const SPI_IRQ: u32 = 11;
pub const PWM_BASE: u32 = 0xF800_1800;
pub const PWM_SIZE: u32 = 0x100;
pub const ADC_BASE: u32 = 0xF800_1A00;
pub const ADC_SIZE: u32 = 0x100;

/// GPIO lines (the RP2350B's GP0-GP47); PWM channel N is GPIO N.
pub const NGPIO: u32 = 48;
/// The SPI controller's FIFO depth (DT `sifive,fifo-depth`).
pub const SPI_FIFO: usize = 32;
/// The SPI controller's input clock (DT fixed-clock): SCK = this / 2(div + 1).
pub const SPI_CLOCK_HZ: u32 = 100_000_000;
/// ADC channels: 0-7 are GP40-GP47, 8 the temperature sensor.
pub const ADC_CHANNELS: u32 = 9;

/// The board's GPIO lines: GPN for a free pin, otherwise what it is for. The
/// free pins are the GPIO lines (and PWM channels) the guest may use; the
/// DT's `gpio-line-names` comes from here.
pub const LINE_NAMES: [&str; NGPIO as usize] = [
    "PSRAM_CS", "GP1", "GP2", "GP3", "I2C0_SDA", "I2C0_SCL", "I2C1_SDA", "I2C1_SCL", //
    "GP8", "GP9", "GP10", "GP11", "GP12", "GP13", "GP14", "GP15", //
    "SPI0_RX", "SPI0_CS", "SPI0_SCK", "SPI0_TX", "GP20", "GP21", "GP22", "WL_ON", //
    "WL_D", "WL_CS", "GP26", "GP27", "GP28", "WL_CLK", "GP30", "GP31", //
    "GP32", "GP33", "GP34", "GP35", "GP36", "GP37", "GP38", "GP39", //
    "ADC0", "ADC1", "ADC2", "ADC3", "ADC4", "ADC5", "ADC6", "ADC7",
];

/// The free pins (bit N = GPN).
pub const FREE_PINS: u64 = {
    let mut m = 0u64;
    let mut n = 0;
    while n < LINE_NAMES.len() {
        let b = LINE_NAMES[n].as_bytes();
        if b[0] == b'G' && b[1] == b'P' {
            m |= 1 << n;
        }
        n += 1;
    }
    m
};

/// The RP2350 PWM slice and channel (0 A, 1 B) that can drive pin `n`.
pub fn pwm_slice(n: u32) -> (u32, u32) {
    let slice = if n < 32 { (n >> 1) & 7 } else { 8 + ((n >> 1) & 3) };
    (slice, n & 1)
}

/// An RP2350 PWM slice setting: counter clock `clk / (div16 / 16)`, wrap at
/// `top`, compare value `cc`, and the period and duty that gives.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct PwmTiming {
    pub div16: u32,
    pub top: u32,
    pub cc: u32,
    pub period_ns: u32,
    pub duty_ns: u32,
}

/// The longest period at or below `period_ns`, and the longest duty at or
/// below `duty_ns`, a slice clocked at `clk_hz` can do (None: too short).
pub fn pwm_timing(clk_hz: u32, period_ns: u32, duty_ns: u32) -> Option<PwmTiming> {
    const NS: u64 = 1_000_000_000;
    let clk = u64::from(clk_hz);
    // far above the longest period (65536 * 256 ticks), and keeps the
    // products below in 64 bits
    let period_ns = period_ns.min(NS as u32);
    // counter ticks at divider 1, in 1/16 ticks
    let t16 = u64::from(period_ns) * clk * 16 / NS;
    let div16 = t16.div_ceil(1 << 16).clamp(16, 0xFFF);
    let wrap = (t16 / div16).min(1 << 16); // TOP + 1
    if wrap < 2 {
        return None;
    }
    let tick_ns = |n: u64| (n * div16 * NS / 16 / clk) as u32;
    let duty = u64::from(duty_ns.min(period_ns));
    let cc = (duty * clk * 16 / NS / div16).min(wrap).min(0xFFFF);
    Some(PwmTiming { div16: div16 as u32, top: (wrap - 1) as u32, cc: cc as u32, period_ns: tick_ns(wrap), duty_ns: tick_ns(cc) })
}

// ------------------------------------------------------------------ host side

/// One GPIO line as the guest configured it.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Line {
    pub out: bool,
    pub oe: bool,
    pub pull_up: bool,
    pub pull_down: bool,
    /// Edges that latch an interrupt.
    pub rise: bool,
    pub fall: bool,
}

impl Line {
    fn from_bits(v: u32) -> Self {
        Line {
            out: v & 1 != 0,
            oe: v & 2 != 0,
            pull_up: v & 4 != 0,
            pull_down: v & 8 != 0,
            rise: v & 0x10 != 0,
            fall: v & 0x20 != 0,
        }
    }
    fn bits(self) -> u32 {
        self.out as u32
            | (self.oe as u32) << 1
            | (self.pull_up as u32) << 2
            | (self.pull_down as u32) << 3
            | (self.rise as u32) << 4
            | (self.fall as u32) << 5
    }
}

/// One message of an I2C transaction.
pub enum I2cOp<'a> {
    Read(&'a mut [u8]),
    Write(&'a [u8]),
}

/// One PWM channel: period and duty in ns.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Pwm {
    pub period_ns: u32,
    pub duty_ns: u32,
    pub enable: bool,
    pub invert: bool,
}

/// The board behind the devices.
pub trait Pins {
    /// GPIO lines the guest may use (bit N = GPN).
    fn gpio_valid(&self) -> u64;
    /// Pins the guest may drive with PWM.
    fn pwm_valid(&self) -> u64 {
        self.gpio_valid()
    }
    /// Apply line `n`'s configuration (only for valid lines). Valid lines
    /// start as inputs pulled down, as the RP2350's pads come out of reset.
    fn gpio_set(&mut self, n: u32, line: Line);
    /// The pad level of every line (bit N = GPN).
    fn gpio_levels(&mut self) -> u64;
    /// Edges latched since the last call on lines with `rise` / `fall` set.
    /// Called on every device poll: make it cheap when nothing happened.
    fn gpio_edges(&mut self) -> u64;
    /// One transaction (repeated starts between the messages) with the
    /// device at 7-bit `addr` on `bus`; false on a NACK or a bus error.
    fn i2c(&mut self, bus: usize, addr: u8, ops: &mut [I2cOp<'_>]) -> bool;
    /// Full-duplex SPI: send `buf` and replace it with what came back, at
    /// `hz` (or the nearest rate below), in mode `mode` (CPOL << 1 | CPHA).
    fn spi_transfer(&mut self, hz: u32, mode: u8, buf: &mut [u8]);
    /// Drive the SPI chip select to `high`.
    fn spi_cs(&mut self, high: bool);
    /// Configure PWM on pin `n` (only for valid pins) and return what the
    /// hardware does instead (`period_ns` 0: not possible).
    fn pwm_set(&mut self, n: u32, pwm: Pwm) -> Pwm;
    /// Convert ADC channel `ch`: 12 bits, or None on a conversion error.
    fn adc(&mut self, ch: u32) -> Option<u16>;
}

/// No board (the devices are left out of the platform).
pub struct NoPins;

impl Pins for NoPins {
    fn gpio_valid(&self) -> u64 {
        0
    }
    fn gpio_set(&mut self, _n: u32, _line: Line) {}
    fn gpio_levels(&mut self) -> u64 {
        0
    }
    fn gpio_edges(&mut self) -> u64 {
        0
    }
    fn i2c(&mut self, _bus: usize, _addr: u8, _ops: &mut [I2cOp<'_>]) -> bool {
        false
    }
    fn spi_transfer(&mut self, _hz: u32, _mode: u8, _buf: &mut [u8]) {}
    fn spi_cs(&mut self, _high: bool) {}
    fn pwm_set(&mut self, _n: u32, _pwm: Pwm) -> Pwm {
        Pwm::default()
    }
    fn adc(&mut self, _ch: u32) -> Option<u16> {
        None
    }
}

// ------------------------------------------------------------------ GPIO

/// `rvemu,gpio`: a control register per line at 4N (bits 0-5 as `Line`,
/// bit 8 the pad level), then NGPIO, VALID, PEND (write 1 to clear), INPUT,
/// SET and CLR, as 32-bit words for lines 0-31 and 32-63.
struct Gpio {
    line: [Line; NGPIO as usize],
    pend: u64,
}

impl Gpio {
    fn word(v: u64, hi: bool) -> u32 {
        (if hi { v >> 32 } else { v }) as u32
    }

    fn load(&mut self, off: u32, pins: &mut impl Pins) -> u32 {
        let hi = off & 4 != 0;
        match off {
            0..0x100 => match self.line.get((off / 4) as usize) {
                Some(l) => l.bits() | ((pins.gpio_levels() >> (off / 4)) as u32 & 1) << 8,
                None => 0,
            },
            0x100 => NGPIO,
            0x104 | 0x108 => Self::word(pins.gpio_valid(), off == 0x108),
            0x110 | 0x114 => Self::word(self.pend, hi),
            0x120 | 0x124 => Self::word(pins.gpio_levels(), hi),
            _ => 0,
        }
    }

    fn set(&mut self, n: usize, l: Line, pins: &mut impl Pins) {
        if pins.gpio_valid() >> n & 1 != 0 && self.line[n] != l {
            self.line[n] = l;
            pins.gpio_set(n as u32, l);
        }
    }

    fn store(&mut self, off: u32, v: u32, pins: &mut impl Pins) {
        let shift = if off & 4 != 0 { 32 } else { 0 };
        match off {
            0..0x100 => {
                if ((off / 4) as usize) < self.line.len() {
                    self.set((off / 4) as usize, Line::from_bits(v), pins);
                }
            }
            0x110 | 0x114 => self.pend &= !(u64::from(v) << shift),
            // SET 0x130/0x134, CLR 0x138/0x13c
            0x130..0x140 => {
                let mask = u64::from(v) << shift;
                for n in 0..self.line.len() {
                    if mask >> n & 1 != 0 {
                        let l = Line { out: off < 0x138, ..self.line[n] };
                        self.set(n, l, pins);
                    }
                }
            }
            _ => {}
        }
    }

    fn poll(&mut self, pins: &mut impl Pins) {
        self.pend |= pins.gpio_edges();
    }
}

// ------------------------------------------------------------------ I2C

const DEVICE_I2C: u32 = 34;
/// Each message takes three descriptors; Linux allows 42 per transfer.
const I2C_QUEUE: u32 = 128;
/// Messages run as one transaction; longer groups are refused.
const I2C_GROUP: usize = 16;
const I2C_F_ZERO_LENGTH: u32 = 1;
const I2C_FLAGS_FAIL_NEXT: u32 = 1;
const I2C_FLAGS_M_RD: u32 = 2;

/// One parsed request: the message and where its status byte goes.
#[derive(Clone, Copy)]
struct I2cReq {
    head: u16,
    addr: u8,
    read: bool,
    buf: Desc,
    status: u64,
    ok: bool,
    fail_next: bool,
}

/// virtio-i2c: one request queue; each message is out header, buffer (none
/// for a zero-length message), status. A group of messages chained by
/// FAIL_NEXT is one transaction.
struct VirtioI2c {
    bus: usize,
    t: Transport,
}

impl VirtioI2c {
    fn new(bus: usize, mem: GuestMem) -> Self {
        VirtioI2c { bus, t: Transport::new(mem, DEVICE_I2C, &[I2C_QUEUE], I2C_F_ZERO_LENGTH) }
    }

    fn load(&mut self, off: u32, size: u32) -> Result<u32, u32> {
        if off >= 0x100 {
            return Ok(0); // no config space
        }
        if size != 4 {
            return Err(exc::LOAD_FAULT);
        }
        Ok(self.t.load(off))
    }

    fn store(&mut self, off: u32, size: u32, v: u32, pins: &mut impl Pins) -> Result<(), u32> {
        if off >= 0x100 {
            return Ok(());
        }
        if size != 4 {
            return Err(exc::STORE_FAULT);
        }
        if let Kick::Notify(0) = self.t.store(off, v) {
            while self.t.peek_avail(0).is_some() {
                self.group(pins);
            }
        }
        Ok(())
    }

    fn parse(&self, head: u16) -> I2cReq {
        let mut c = [NO_DESC; CHAIN_MAX];
        let n = self.t.chain(0, head, &mut c);
        let mut r = I2cReq { head, addr: 0, read: false, buf: NO_DESC, status: 0, ok: false, fail_next: false };
        if !(2..=3).contains(&n) || c[0].len < 8 || c[n - 1].flags & DESC_F_WRITE == 0 {
            return r;
        }
        let (Some(addr), Some(flags)) = (self.t.mem.u16(c[0].addr), self.t.mem.u32(c[0].addr + 4)) else { return r };
        r.addr = (addr >> 1) as u8 & 0x7F;
        r.read = flags & I2C_FLAGS_M_RD != 0;
        r.fail_next = flags & I2C_FLAGS_FAIL_NEXT != 0;
        r.status = c[n - 1].addr;
        if n == 3 {
            r.buf = c[1];
            // a read buffer is device-writable, a write buffer isn't
            if (r.buf.flags & DESC_F_WRITE != 0) != r.read {
                return r;
            }
        }
        r.ok = true;
        r
    }

    /// Take the next group of messages off the queue and run it.
    fn group(&mut self, pins: &mut impl Pins) {
        let mut reqs = [I2cReq { head: 0, addr: 0, read: false, buf: NO_DESC, status: 0, ok: false, fail_next: false };
            I2C_GROUP];
        let mut n = 0;
        while let Some(head) = self.t.avail_nth(0, n as u16) {
            reqs[n] = self.parse(head);
            n += 1;
            if !reqs[n - 1].fail_next || n == I2C_GROUP {
                break;
            }
        }
        let reqs = &reqs[..n];
        // runs to one address are one transaction each; a failure fails
        // the rest of the group
        let mut done = 0;
        let mut ok = reqs.len() < I2C_GROUP || !reqs[n - 1].fail_next;
        while ok && done < n {
            let addr = reqs[done].addr;
            let end = reqs[done..].iter().position(|r| r.addr != addr).map_or(n, |k| done + k);
            ok = self.run(addr, &reqs[done..end], pins);
            if ok {
                done = end;
            }
        }
        for (i, r) in reqs.iter().enumerate() {
            let good = i < done;
            let mut written = 1;
            if r.ok && r.read {
                // even a failed read may have written part of the buffer
                self.t.note_dirty(r.buf.addr, r.buf.len as usize);
                if good {
                    written += r.buf.len;
                }
            }
            self.t.dma_write(r.status, &[if good { 0 } else { 1 }]);
            self.t.push_used(0, r.head, written);
        }
    }

    /// One transaction to `addr`.
    fn run(&mut self, addr: u8, reqs: &[I2cReq], pins: &mut impl Pins) -> bool {
        let mut ops: [I2cOp<'_>; I2C_GROUP] = core::array::from_fn(|_| I2cOp::Write(&[]));
        for (op, r) in ops.iter_mut().zip(reqs) {
            if !r.ok {
                return false;
            }
            let len = r.buf.len as usize;
            *op = if r.read {
                let Some(p) = self.t.mem.range_mut(r.buf.addr, len) else { return false };
                // SAFETY: in guest RAM (checked); the buffer is the driver's
                // and nothing else touches it during the request.
                I2cOp::Read(unsafe { core::slice::from_raw_parts_mut(p, len) })
            } else if len == 0 {
                I2cOp::Write(&[])
            } else {
                let Some(p) = self.t.mem.range(r.buf.addr, len) else { return false };
                // SAFETY: as above.
                I2cOp::Write(unsafe { core::slice::from_raw_parts(p, len) })
            };
        }
        pins.i2c(self.bus, addr, &mut ops[..reqs.len()])
    }
}

// ------------------------------------------------------------------ SPI

const SPI_CSMODE_AUTO: u32 = 0;
const SPI_CSMODE_HOLD: u32 = 2;
const SPI_FMT_ENDIAN: u32 = 1 << 2;
const SPI_FMT_DIR: u32 = 1 << 3;
const SPI_TXDATA_FULL: u32 = 1 << 31;
const SPI_RXDATA_EMPTY: u32 = 1 << 31;
const SPI_IP_TXWM: u32 = 1;
const SPI_IP_RXWM: u32 = 2;

/// The SiFive SPI controller's registers (Linux `spi-sifive`), one chip
/// select. Transmitted bytes wait in the FIFO and go out in one transfer
/// when the driver looks for the result (RXDATA, IP, IE) or ends the
/// message (CSMODE), or when the FIFO is full.
struct Spi {
    sckdiv: u32,
    sckmode: u32,
    csid: u32,
    csdef: u32,
    csmode: u32,
    delay: [u32; 2],
    fmt: u32,
    txmark: u32,
    rxmark: u32,
    ie: u32,
    tx: [u8; SPI_FIFO],
    ntx: usize,
    rx: [u8; SPI_FIFO],
    rx_head: usize,
    nrx: usize,
}

impl Spi {
    fn new() -> Self {
        Spi {
            sckdiv: 3,
            sckmode: 0,
            csid: 0,
            csdef: 1,
            csmode: SPI_CSMODE_AUTO,
            delay: [0x1_0001, 1],
            fmt: 8 << 16,
            txmark: 0,
            rxmark: 0,
            ie: 0,
            tx: [0; SPI_FIFO],
            ntx: 0,
            rx: [0; SPI_FIFO],
            rx_head: 0,
            nrx: 0,
        }
    }

    fn ip(&self) -> u32 {
        let tx = if (self.ntx as u32) < self.txmark { SPI_IP_TXWM } else { 0 };
        let rx = if self.nrx as u32 > self.rxmark { SPI_IP_RXWM } else { 0 };
        tx | rx
    }

    fn irq_line(&self) -> bool {
        self.ip() & self.ie != 0
    }

    fn cs_active(&self) -> bool {
        self.csdef & 1 == 0
    }

    /// Clock out the bytes waiting in the FIFO.
    fn flush(&mut self, pins: &mut impl Pins) {
        if self.ntx == 0 {
            return;
        }
        let mut buf = self.tx;
        let buf = &mut buf[..self.ntx];
        self.ntx = 0;
        let lsb = self.fmt & SPI_FMT_ENDIAN != 0;
        if lsb {
            buf.iter_mut().for_each(|b| *b = b.reverse_bits());
        }
        let hz = SPI_CLOCK_HZ / (2 * (self.sckdiv + 1));
        let auto = self.csmode == SPI_CSMODE_AUTO && self.csid == 0;
        if auto {
            pins.spi_cs(self.cs_active());
        }
        pins.spi_transfer(hz, self.sckmode as u8, buf);
        if auto {
            pins.spi_cs(!self.cs_active());
        }
        if self.fmt & SPI_FMT_DIR != 0 {
            return; // transmit only: nothing goes to the receive FIFO
        }
        for &b in buf.iter() {
            if self.nrx < SPI_FIFO {
                self.rx[(self.rx_head + self.nrx) % SPI_FIFO] = if lsb { b.reverse_bits() } else { b };
                self.nrx += 1;
            }
        }
    }

    fn load(&mut self, off: u32, pins: &mut impl Pins) -> u32 {
        match off {
            0x00 => self.sckdiv,
            0x04 => self.sckmode,
            0x10 => self.csid,
            0x14 => self.csdef,
            0x18 => self.csmode,
            0x28 => self.delay[0],
            0x2c => self.delay[1],
            0x40 => self.fmt,
            0x48 => {
                if self.ntx == SPI_FIFO {
                    SPI_TXDATA_FULL
                } else {
                    0
                }
            }
            0x4c => {
                self.flush(pins);
                if self.nrx == 0 {
                    return SPI_RXDATA_EMPTY;
                }
                let b = self.rx[self.rx_head];
                self.rx_head = (self.rx_head + 1) % SPI_FIFO;
                self.nrx -= 1;
                u32::from(b)
            }
            0x50 => self.txmark,
            0x54 => self.rxmark,
            0x70 => self.ie,
            0x74 => {
                self.flush(pins);
                self.ip()
            }
            _ => 0,
        }
    }

    fn store(&mut self, off: u32, v: u32, pins: &mut impl Pins) {
        match off {
            0x00 => self.sckdiv = v & 0xFFF,
            0x04 => self.sckmode = v & 3,
            0x10 => self.csid = v,
            0x14 => {
                // one chip select: the probe finds CSDEF's single bit
                self.csdef = v & 1;
                if self.csmode != SPI_CSMODE_HOLD {
                    pins.spi_cs(!self.cs_active());
                }
            }
            0x18 => {
                self.flush(pins);
                self.csmode = v & 3;
                let hold = self.csmode == SPI_CSMODE_HOLD && self.csid == 0;
                pins.spi_cs(if hold { self.cs_active() } else { !self.cs_active() });
            }
            0x28 => self.delay[0] = v,
            0x2c => self.delay[1] = v,
            0x40 => self.fmt = v,
            0x48 => {
                if self.ntx == SPI_FIFO {
                    self.flush(pins);
                }
                self.tx[self.ntx] = v as u8;
                self.ntx += 1;
            }
            0x50 => self.txmark = v & 0xFF,
            0x54 => self.rxmark = v & 0xFF,
            0x70 => {
                self.flush(pins);
                self.ie = v & 3;
            }
            _ => {}
        }
    }
}

// ------------------------------------------------------------------ PWM

/// `rvemu,pwm`: SEL picks channel N (= GPIO N); PERIOD and DUTY (ns) and
/// CTRL (bit 0 enable, bit 1 inverted) are its. Writing CTRL applies them,
/// and PERIOD / DUTY then read back what the hardware does. VALID at
/// 0x10/0x14, NPWM at 0x18.
struct PwmDev {
    sel: usize,
    staged: Pwm,
    ch: [Pwm; NGPIO as usize],
}

impl PwmDev {
    fn load(&self, off: u32, pins: &impl Pins) -> u32 {
        match off {
            0x00 => self.sel as u32,
            0x04 => self.staged.period_ns,
            0x08 => self.staged.duty_ns,
            0x0c => {
                let c = self.ch[self.sel];
                c.enable as u32 | (c.invert as u32) << 1
            }
            0x10 => pins.pwm_valid() as u32,
            0x14 => (pins.pwm_valid() >> 32) as u32,
            0x18 => NGPIO,
            _ => 0,
        }
    }

    fn store(&mut self, off: u32, v: u32, pins: &mut impl Pins) {
        match off {
            0x00 => {
                self.sel = (v as usize).min(self.ch.len() - 1);
                self.staged = self.ch[self.sel];
            }
            0x04 => self.staged.period_ns = v,
            0x08 => self.staged.duty_ns = v,
            0x0c => {
                let want = Pwm { enable: v & 1 != 0, invert: v & 2 != 0, ..self.staged };
                let got = if pins.pwm_valid() >> self.sel & 1 != 0 {
                    pins.pwm_set(self.sel as u32, want)
                } else {
                    Pwm::default()
                };
                self.ch[self.sel] = got;
                self.staged = got;
            }
            _ => {}
        }
    }
}

// ------------------------------------------------------------------ all of them

/// Every device in IO_BASE..IO_BASE + IO_SIZE, on the host's `Pins`.
pub struct Io<P: Pins> {
    pub pins: P,
    gpio: Gpio,
    i2c: [VirtioI2c; 2],
    spi: Spi,
    pwm: PwmDev,
}

impl<P: Pins> Io<P> {
    pub fn new(pins: P, mem: GuestMem) -> Self {
        Io {
            pins,
            // the RP2350's pads come out of reset pulled down
            gpio: Gpio { line: [Line { pull_down: true, ..Line::default() }; NGPIO as usize], pend: 0 },
            i2c: [VirtioI2c::new(0, mem), VirtioI2c::new(1, mem)],
            spi: Spi::new(),
            pwm: PwmDev { sel: 0, staged: Pwm::default(), ch: [Pwm::default(); NGPIO as usize] },
        }
    }

    /// The device at `pa` and the offset in it.
    fn at(pa: u32) -> (u32, u32) {
        let off = pa - IO_BASE;
        (off & !0x1FF, off & 0x1FF)
    }

    pub fn load(&mut self, pa: u32, size: u32) -> Result<u32, u32> {
        let (dev, off) = Self::at(pa);
        let pins = &mut self.pins;
        if let Some(k) = I2C_BASE.iter().position(|&b| b - IO_BASE == dev) {
            return self.i2c[k].load(off, size);
        }
        if size != 4 {
            return Err(exc::LOAD_FAULT);
        }
        Ok(match dev + IO_BASE {
            GPIO_BASE => self.gpio.load(off, pins),
            SPI_BASE => self.spi.load(off, pins),
            PWM_BASE => self.pwm.load(off, pins),
            ADC_BASE if off < 4 * ADC_CHANNELS => pins.adc(off / 4).map_or(1 << 31, u32::from),
            _ => 0,
        })
    }

    pub fn store(&mut self, pa: u32, size: u32, v: u32) -> Result<(), u32> {
        let (dev, off) = Self::at(pa);
        let pins = &mut self.pins;
        if let Some(k) = I2C_BASE.iter().position(|&b| b - IO_BASE == dev) {
            return self.i2c[k].store(off, size, v, pins);
        }
        if size != 4 {
            return Err(exc::STORE_FAULT);
        }
        match dev + IO_BASE {
            GPIO_BASE => self.gpio.store(off, v, pins),
            SPI_BASE => self.spi.store(off, v, pins),
            PWM_BASE => self.pwm.store(off, v, pins),
            _ => {}
        }
        Ok(())
    }

    /// Pick up GPIO edges.
    pub fn poll(&mut self) {
        self.gpio.poll(&mut self.pins);
    }

    /// (PLIC input, level) of each device that interrupts.
    pub fn irq_lines(&self) -> [(u32, bool); 4] {
        [
            (GPIO_IRQ, self.gpio.pend != 0),
            (I2C_IRQ[0], self.i2c[0].t.int_status != 0),
            (I2C_IRQ[1], self.i2c[1].t.int_status != 0),
            (SPI_IRQ, self.spi.irq_line()),
        ]
    }

    pub fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.i2c[0].t.take_dma_write().or_else(|| self.i2c[1].t.take_dma_write())
    }
}
