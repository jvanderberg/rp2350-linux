//! The board's pins as guest devices: GPIO, two I2C buses, two SPI buses, PWM
//! and the ADC, in guest MMIO 0xF8001000-0xF8002FFF (only GPIO interrupts:
//! PLIC 8).
//!
//! | Device | Address | IRQ | Guest driver (guest/linux/drivers/rvemu) |
//! |---|---|---|---|
//! | GPIO | 0xF8001000 | 8 | `rvemu,gpio` |
//! | I2C0, I2C1 | 0xF8001200, 0xF8001400 | | `rvemu,i2c` |
//! | SPI0 | 0xF8001600 | | `rvemu,spi` + spidev |
//! | SPI1 | 0xF8001C00 | | `rvemu,spi` + `mmc-spi-slot` (the SD card) |
//! | PWM | 0xF8001800 | | `rvemu,pwm` |
//! | ADC | 0xF8001A00 | | `rvemu,adc` (IIO) |
//!
//! The models keep register state; the host's `Pins` does the I/O (the
//! rp235x-hal drivers on the board, a simulated board on the Mac). Every
//! operation completes synchronously, inside the guest's register write:
//! I2C and SPI read their messages from guest RAM and write the replies
//! there, so neither needs a queue, an interrupt or a guest thread.

use crate::cpu::exc;
use crate::virtio::GuestMem;

pub const IO_BASE: u32 = 0xF800_1000;
pub const IO_SIZE: u32 = 0x2000;
pub const GPIO_BASE: u32 = 0xF800_1000;
pub const GPIO_SIZE: u32 = 0x200;
pub const GPIO_IRQ: u32 = 8;
pub const I2C_BASE: [u32; 2] = [0xF800_1200, 0xF800_1400];
pub const I2C_SIZE: u32 = 0x100;
pub const SPI_BASE: [u32; 2] = [0xF800_1600, 0xF800_1C00];
pub const SPI_SIZE: u32 = 0x100;
pub const PWM_BASE: u32 = 0xF800_1800;
pub const PWM_SIZE: u32 = 0x100;
pub const ADC_BASE: u32 = 0xF800_1A00;
pub const ADC_SIZE: u32 = 0x100;

/// GPIO lines (the RP2350B's GP0-GP47); PWM channel N is GPIO N.
pub const NGPIO: u32 = 48;
/// PWM channels: up to GP39, the last free pin (GP40-GP47 are the ADC's).
/// Fewer than 48 also keeps the guest's PWM table in one 4 KiB slab.
pub const NPWM: u32 = 40;
/// The fastest SPI clock the guest may ask for.
pub const SPI_MAX_HZ: u32 = 50_000_000;
/// The slowest SPI clock: the device runs a transfer while the guest waits,
/// so 4 KiB (spidev's buffer) takes at most about 0.3 s.
pub const SPI_MIN_HZ: u32 = 100_000;
/// ADC channels: 0-7 are GP40-GP47, 8 the temperature sensor.
pub const ADC_CHANNELS: u32 = 9;

/// The board's GPIO lines: GPN for a free pin, otherwise what it is for. The
/// free pins are the GPIO lines (and PWM channels) the guest may use; the
/// DT's `gpio-line-names` comes from here.
pub const LINE_NAMES: [&str; NGPIO as usize] = [
    "PSRAM_CS", "GP1", "GP2", "GP3", "I2C0_SDA", "I2C0_SCL", "I2C1_SDA", "I2C1_SCL", //
    "SPI1_RX", "SPI1_CS", "SPI1_SCK", "SPI1_TX", "GP12", "GP13", "GP14", "GP15", //
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

/// Why an I2C transaction failed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum I2cError {
    /// The address was not acknowledged.
    Nack,
    /// Anything else (a data NACK, arbitration, a message the controller
    /// can't do).
    Bus,
    /// The bus stayed busy past the backend's deadline (a device holding SCL
    /// or SDA low); the backend has aborted and tried to free the bus.
    Timeout,
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
    /// device at 7-bit `addr` on `bus`.
    fn i2c(&mut self, bus: usize, addr: u8, ops: &mut [I2cOp<'_>]) -> Result<(), I2cError>;
    /// Full-duplex SPI on `bus`, MSB first: send `buf` and replace it with
    /// what came back, at `hz` (or the nearest rate below), in mode `mode`
    /// (CPOL << 1 | CPHA).
    fn spi_transfer(&mut self, bus: usize, hz: u32, mode: u8, buf: &mut [u8]);
    /// Drive `bus`'s chip select (active low) to active or not.
    fn spi_cs(&mut self, bus: usize, active: bool);
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
    fn i2c(&mut self, _bus: usize, _addr: u8, _ops: &mut [I2cOp<'_>]) -> Result<(), I2cError> {
        Err(I2cError::Nack)
    }
    fn spi_transfer(&mut self, _bus: usize, _hz: u32, _mode: u8, _buf: &mut [u8]) {}
    fn spi_cs(&mut self, _bus: usize, _active: bool) {}
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

// ------------------------------------------------------------------ DMA

/// Guest RAM the devices wrote, for `take_dma_write`.
struct Dirty {
    r: [(u32, u32); 8],
    n: usize,
}

impl Dirty {
    fn note(&mut self, pa: u64, len: usize) {
        let (lo, hi) = (pa as u32, (pa + len as u64) as u32);
        let n = self.n;
        match self.r[..n].iter().position(|&(a, b)| lo <= b && a <= hi).or((n == self.r.len()).then_some(n - 1)) {
            Some(i) => self.r[i] = (self.r[i].0.min(lo), self.r[i].1.max(hi)),
            None => {
                self.r[n] = (lo, hi);
                self.n += 1;
            }
        }
    }
    fn take(&mut self) -> Option<(u32, u32)> {
        self.n = self.n.checked_sub(1)?;
        Some(self.r[self.n])
    }
}

// ------------------------------------------------------------------ I2C

/// Messages in one transfer, at most (Linux's i2c-dev allows 42).
pub const I2C_MSGS: usize = 16;
const I2C_RESULT_NACK: u32 = 1 << 31;
const I2C_RESULT_TIMEOUT: u32 = 1 << 30;

/// `rvemu,i2c`: MSGS (0x00) is the guest physical address of a table of
/// 16-byte messages { u16 addr, u16 flags (bit 0 read), u32 len, u32 buf,
/// u32 0 }; writing their number to NMSGS (0x04) runs them as one transfer
/// and RESULT (0x08) is then the number done, plus bit 31 when the address
/// wasn't acknowledged or bit 30 when the bus stayed busy (timeout). Consecutive messages to one address are one
/// transaction, with repeated starts.
#[derive(Default)]
struct I2c {
    msgs: u32,
    result: u32,
}

#[derive(Clone, Copy, Default)]
struct I2cMsg {
    addr: u8,
    read: bool,
    buf: u64,
    len: usize,
}

impl I2c {
    fn load(&self, off: u32) -> u32 {
        match off {
            0x00 => self.msgs,
            0x08 => self.result,
            _ => 0,
        }
    }

    fn store(&mut self, bus: usize, off: u32, v: u32, mem: &GuestMem, dirty: &mut Dirty, pins: &mut impl Pins) {
        match off {
            0x00 => self.msgs = v,
            0x04 => self.result = Self::run(bus, u64::from(self.msgs), v as usize, mem, dirty, pins),
            _ => {}
        }
    }

    fn run(bus: usize, tab: u64, n: usize, mem: &GuestMem, dirty: &mut Dirty, pins: &mut impl Pins) -> u32 {
        if n == 0 || n > I2C_MSGS {
            return 0;
        }
        let mut m = [I2cMsg::default(); I2C_MSGS];
        for (k, msg) in m[..n].iter_mut().enumerate() {
            let e = tab + 16 * k as u64;
            let (Some(a), Some(len), Some(buf)) = (mem.u32(e), mem.u32(e + 4), mem.u32(e + 8)) else { return 0 };
            *msg = I2cMsg { addr: (a & 0x7F) as u8, read: a >> 16 & 1 != 0, buf: u64::from(buf), len: len as usize };
            if a & 0xFF80 != 0 {
                return 0; // 10-bit addresses aren't offered
            }
        }
        let mut done = 0;
        while done < n {
            let addr = m[done].addr;
            let end = m[done..n].iter().position(|x| x.addr != addr).map_or(n, |k| done + k);
            let mut ops: [I2cOp<'_>; I2C_MSGS] = core::array::from_fn(|_| I2cOp::Write(&[]));
            for (op, x) in ops.iter_mut().zip(&m[done..end]) {
                *op = if x.len == 0 {
                    if x.read { I2cOp::Read(&mut []) } else { I2cOp::Write(&[]) }
                } else if x.read {
                    let Some(p) = mem.range_mut(x.buf, x.len) else { return done as u32 };
                    dirty.note(x.buf, x.len);
                    // SAFETY: in guest RAM (checked); the driver's buffer.
                    I2cOp::Read(unsafe { core::slice::from_raw_parts_mut(p, x.len) })
                } else {
                    let Some(p) = mem.range(x.buf, x.len) else { return done as u32 };
                    // SAFETY: as above.
                    I2cOp::Write(unsafe { core::slice::from_raw_parts(p, x.len) })
                };
            }
            match pins.i2c(bus, addr, &mut ops[..end - done]) {
                Ok(()) => done = end,
                Err(I2cError::Nack) => return done as u32 | I2C_RESULT_NACK,
                Err(I2cError::Bus) => return done as u32,
                Err(I2cError::Timeout) => return done as u32 | I2C_RESULT_TIMEOUT,
            }
        }
        n as u32
    }
}

// ------------------------------------------------------------------ SPI

const SPI_MODE_LSB_FIRST: u32 = 8;
/// Bytes sent per backend transfer when nothing is received.
const SPI_CHUNK: usize = 64;
/// The most bytes one SKIP clocks.
pub const SPI_SKIP_MAX: u32 = 4096;
/// SKIP's result when every byte matched.
pub const SPI_SKIP_NONE: u32 = 0x100;

/// `rvemu,spi`: TX (0x00) and RX (0x04) guest physical buffers (0: zeros
/// out, nothing in), LEN (0x08), HZ (0x0c), MODE (0x10: Linux SPI_CPHA,
/// SPI_CPOL, SPI_LSB_FIRST), CS (0x14: 1 drives the pin low) and GO (0x18),
/// which runs the transfer. SKIP (0x1c): a write of BYTE | MAX << 8 sends
/// 0xff a byte at a time until a byte other than BYTE comes back, at most
/// MAX bytes (an SD card's wait for a token, or for busy to end); a read
/// gives that byte, or `SPI_SKIP_NONE`.
#[derive(Default)]
struct Spi {
    tx: u32,
    rx: u32,
    len: u32,
    hz: u32,
    mode: u32,
    cs: u32,
    skip: u32,
}

impl Spi {
    fn load(&self, off: u32) -> u32 {
        match off {
            0x00 => self.tx,
            0x04 => self.rx,
            0x08 => self.len,
            0x0c => self.hz,
            0x10 => self.mode,
            0x14 => self.cs,
            0x1c => self.skip,
            _ => 0,
        }
    }

    fn store(&mut self, bus: usize, off: u32, v: u32, mem: &GuestMem, dirty: &mut Dirty, pins: &mut impl Pins) {
        match off {
            0x00 => self.tx = v,
            0x04 => self.rx = v,
            0x08 => self.len = v,
            0x0c => self.hz = v.clamp(SPI_MIN_HZ, SPI_MAX_HZ),
            0x10 => self.mode = v & 0xB,
            0x14 => {
                self.cs = v & 1;
                pins.spi_cs(bus, self.cs != 0);
            }
            0x18 => self.go(bus, mem, dirty, pins),
            0x1c => self.skip = self.skip_until(bus, v as u8, (v >> 8).clamp(1, SPI_SKIP_MAX), pins),
            _ => {}
        }
    }

    fn skip_until(&self, bus: usize, byte: u8, max: u32, pins: &mut impl Pins) -> u32 {
        let lsb = self.mode & SPI_MODE_LSB_FIRST != 0;
        for _ in 0..max {
            let mut b = [0xFF];
            pins.spi_transfer(bus, self.hz, (self.mode & 3) as u8, &mut b);
            let got = if lsb { b[0].reverse_bits() } else { b[0] };
            if got != byte {
                return u32::from(got);
            }
        }
        SPI_SKIP_NONE
    }

    fn go(&mut self, bus: usize, mem: &GuestMem, dirty: &mut Dirty, pins: &mut impl Pins) {
        let (len, tx, rx) = (self.len as usize, u64::from(self.tx), u64::from(self.rx));
        if (tx != 0 && mem.range(tx, len).is_none()) || (rx != 0 && mem.range_mut(rx, len).is_none()) {
            return;
        }
        let lsb = self.mode & SPI_MODE_LSB_FIRST != 0;
        if rx != 0 {
            // in place, in the guest's RX buffer: what goes out first, then
            // the whole transfer at once
            let p = mem.range_mut(rx, len).unwrap();
            if tx == 0 {
                // SAFETY: `len` bytes of guest RAM (checked above).
                unsafe { core::ptr::write_bytes(p, 0, len) };
            } else if tx != rx {
                // SAFETY: as above; `copy` allows the buffers to overlap.
                unsafe { core::ptr::copy(mem.range(tx, len).unwrap(), p, len) };
            }
            // SAFETY: as above, and nothing else holds guest RAM during the
            // device's register write.
            let buf = unsafe { core::slice::from_raw_parts_mut(p, len) };
            if lsb {
                buf.iter_mut().for_each(|b| *b = b.reverse_bits());
            }
            pins.spi_transfer(bus, self.hz, (self.mode & 3) as u8, buf);
            if lsb {
                buf.iter_mut().for_each(|b| *b = b.reverse_bits());
            }
            if len != 0 {
                dirty.note(rx, len);
            }
            return;
        }
        // nothing comes back: send from TX through a small buffer
        let mut at = 0;
        while at < len {
            let n = (len - at).min(SPI_CHUNK);
            let mut buf = [0u8; SPI_CHUNK];
            if tx != 0 {
                // SAFETY: in guest RAM (checked above).
                unsafe { core::ptr::copy_nonoverlapping(mem.range(tx + at as u64, n).unwrap(), buf.as_mut_ptr(), n) };
            }
            let buf = &mut buf[..n];
            if lsb {
                buf.iter_mut().for_each(|b| *b = b.reverse_bits());
            }
            pins.spi_transfer(bus, self.hz, (self.mode & 3) as u8, buf);
            at += n;
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
            0x18 => NPWM,
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
    mem: GuestMem,
    dirty: Dirty,
    gpio: Gpio,
    i2c: [I2c; 2],
    spi: [Spi; 2],
    pwm: PwmDev,
}

impl<P: Pins> Io<P> {
    pub fn new(pins: P, mem: GuestMem) -> Self {
        Io {
            pins,
            mem,
            dirty: Dirty { r: [(0, 0); 8], n: 0 },
            // the RP2350's pads come out of reset pulled down
            gpio: Gpio { line: [Line { pull_down: true, ..Line::default() }; NGPIO as usize], pend: 0 },
            i2c: [I2c::default(), I2c::default()],
            spi: [(); 2].map(|_| Spi { hz: 1_000_000, ..Spi::default() }),
            pwm: PwmDev { sel: 0, staged: Pwm::default(), ch: [Pwm::default(); NGPIO as usize] },
        }
    }

    pub fn load(&mut self, pa: u32, size: u32) -> Result<u32, u32> {
        if size != 4 {
            return Err(exc::LOAD_FAULT);
        }
        let (dev, off) = (pa & !0x1FF, pa & 0xFF);
        let pins = &mut self.pins;
        Ok(match dev {
            GPIO_BASE => self.gpio.load(pa & 0x1FF, pins),
            b if b == I2C_BASE[0] => self.i2c[0].load(off),
            b if b == I2C_BASE[1] => self.i2c[1].load(off),
            b if b == SPI_BASE[0] => self.spi[0].load(off),
            b if b == SPI_BASE[1] => self.spi[1].load(off),
            PWM_BASE => self.pwm.load(off, pins),
            ADC_BASE if off < 4 * ADC_CHANNELS => pins.adc(off / 4).map_or(1 << 31, u32::from),
            _ => 0,
        })
    }

    pub fn store(&mut self, pa: u32, size: u32, v: u32) -> Result<(), u32> {
        if size != 4 {
            return Err(exc::STORE_FAULT);
        }
        let (dev, off) = (pa & !0x1FF, pa & 0xFF);
        let (pins, mem, dirty) = (&mut self.pins, &self.mem, &mut self.dirty);
        match dev {
            GPIO_BASE => self.gpio.store(pa & 0x1FF, v, pins),
            b if b == I2C_BASE[0] => self.i2c[0].store(0, off, v, mem, dirty, pins),
            b if b == I2C_BASE[1] => self.i2c[1].store(1, off, v, mem, dirty, pins),
            b if b == SPI_BASE[0] => self.spi[0].store(0, off, v, mem, dirty, pins),
            b if b == SPI_BASE[1] => self.spi[1].store(1, off, v, mem, dirty, pins),
            PWM_BASE => self.pwm.store(off, v, pins),
            _ => {}
        }
        Ok(())
    }

    /// Pick up GPIO edges.
    pub fn poll(&mut self) {
        self.gpio.poll(&mut self.pins);
    }

    /// The GPIO controller's interrupt line (PLIC `GPIO_IRQ`).
    pub fn irq_line(&self) -> bool {
        self.gpio.pend != 0
    }

    pub fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.dirty.take()
    }
}
