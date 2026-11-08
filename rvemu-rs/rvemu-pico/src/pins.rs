//! The board's pins behind the guest's GPIO, I2C, SPI, PWM and ADC devices
//! (`rvcore::periph`), on the rp235x-hal drivers:
//! - GPIO: the free pins (`periph::FREE_PINS`) as SIO inputs and outputs.
//!   Edges latch in IO_BANK0's raw interrupt registers; the device poll
//!   looks at them only when the IO_IRQ_BANK0 line is pending (one CSR
//!   read), and the interrupt itself is never taken.
//! - I2C0 on GP4/GP5 and I2C1 on GP6/GP7, 100 kHz, internal pull-ups. The
//!   HAL sets the controllers up; transfers run in `i2c_run`, on their
//!   registers, because the HAL's own waits have no time limit and a device
//!   holding SCL or SDA low would stop the emulator for good.
//! - SPI0: GP16 RX (MISO), GP17 chip select (driven as a GPIO), GP18 SCK,
//!   GP19 TX (MOSI).
//! - PWM: the slice and channel that can reach the pin (`periph::pwm_slice`).
//! - ADC: GP40-GP47 and the temperature sensor.
//!
//! rp235x-hal 0.4's ADC knows only the QFN-60 package (channels on
//! GP26-GP29, the temperature sensor on channel 4). This is the QFN-80
//! RP2350B, so the HAL brings the ADC up and enables the temperature sensor,
//! and `adc` selects the channel itself (AINSEL, then START_ONCE, as the
//! HAL's own conversion does).

use embedded_hal::digital::{InputPin as _, OutputPin as _};
use embedded_hal::spi::{MODE_0, Mode, Phase, Polarity, SpiBus as _};
use hal::fugit::RateExtU32;
use hal::gpio::bank0::{Gpio4, Gpio5, Gpio6, Gpio7, Gpio16, Gpio17, Gpio18, Gpio19};
use hal::gpio::{
    DynPinId, DynPullType, FunctionI2C, FunctionNull, FunctionPwm, FunctionSioInput, FunctionSioOutput,
    FunctionSpi, Interrupt, Pin, PinState, PullDown, PullNone, PullUp,
};
use hal::pac;
use hal::pwm::{Slices, ValidSliceMode};
use hal::spi::{Enabled, FrameFormat, Spi};
use rp235x_hal as hal;
use rp2350_board as board;
use rvcore::periph::{FREE_PINS, I2cError, I2cOp, Line, NGPIO, Pins, Pwm, pwm_slice, pwm_timing};

type DPin<F> = Pin<DynPinId, F, DynPullType>;

/// A free pin, in the function the guest last asked for.
pub enum Gp {
    In(DPin<FunctionSioInput>),
    Out(DPin<FunctionSioOutput>),
    Pwm(DPin<FunctionPwm>),
}

macro_rules! with_pin {
    ($g:expr, $p:ident => $e:expr) => {
        match $g {
            Gp::In($p) => $e,
            Gp::Out($p) => $e,
            Gp::Pwm($p) => $e,
        }
    };
}

impl Gp {
    /// Take a pin from `hal::gpio::Pins` as an input pulled down (as it
    /// comes out of reset).
    pub fn new<I: hal::gpio::PinId, F: hal::gpio::Function, P: hal::gpio::PullType>(p: Pin<I, F, P>) -> Self {
        let mut p = p.into_dyn_pin().into_pull_type::<DynPullType>();
        p.set_pull_type(DynPullType::Down);
        match p.try_into_function::<FunctionSioInput>() {
            Ok(p) => Gp::In(p),
            Err(_) => unreachable!("every bank 0 pin has SIO"),
        }
    }

    fn into_input(self) -> Self {
        match self {
            Gp::In(p) => Gp::In(p),
            Gp::Out(p) => Gp::In(p.try_into_function().ok().unwrap()),
            Gp::Pwm(p) => Gp::In(p.try_into_function().ok().unwrap()),
        }
    }

    /// (A dynamic pin can't take its level before it becomes an output, as
    /// a typed one can: it is set right after.)
    fn into_output(self, high: bool) -> Self {
        let mut p: DPin<FunctionSioOutput> = match self {
            Gp::Out(p) => p,
            Gp::In(p) => p.try_into_function().ok().unwrap(),
            Gp::Pwm(p) => p.try_into_function().ok().unwrap(),
        };
        let _ = p.set_state(if high { PinState::High } else { PinState::Low });
        Gp::Out(p)
    }

    fn into_pwm(self) -> Self {
        match self {
            Gp::Pwm(p) => Gp::Pwm(p),
            Gp::In(p) => Gp::Pwm(p.try_into_function().ok().unwrap()),
            Gp::Out(p) => Gp::Pwm(p.try_into_function().ok().unwrap()),
        }
    }
}

pub type I2c0 = hal::I2C<pac::I2C0, (Pin<Gpio4, FunctionI2C, PullUp>, Pin<Gpio5, FunctionI2C, PullUp>)>;
pub type I2c1 = hal::I2C<pac::I2C1, (Pin<Gpio6, FunctionI2C, PullUp>, Pin<Gpio7, FunctionI2C, PullUp>)>;
pub type Spi0 = Spi<
    Enabled,
    pac::SPI0,
    (Pin<Gpio19, FunctionSpi, PullDown>, Pin<Gpio16, FunctionSpi, PullDown>, Pin<Gpio18, FunctionSpi, PullDown>),
    8,
>;
pub type SpiCs = Pin<Gpio17, FunctionSioOutput, PullNone>;

/// The pins `BoardPins::new` takes over.
pub struct PinSet {
    /// Every free pin, at its number.
    pub gp: [Option<Gp>; NGPIO as usize],
    pub i2c0: I2c0,
    pub i2c1: I2c1,
    pub spi: Spi0,
    pub spi_cs: SpiCs,
    /// GP40-GP47, analog.
    pub adc_pins: [Pin<DynPinId, FunctionNull, PullNone>; 8],
}

pub struct BoardPins {
    gp: [Option<Gp>; NGPIO as usize],
    /// Lines with an edge interrupt enabled.
    edge_lines: u64,
    /// Ours, set up by the HAL; `i2c_run` drives their registers.
    _i2c0: I2c0,
    _i2c1: I2c1,
    spi: Spi0,
    spi_cs: SpiCs,
    spi_hz: u32,
    spi_mode: u8,
    peri_hz: u32,
    pwm: Slices,
    /// Pins with PWM on, and each slice's (div16, top).
    pwm_on: u64,
    slice_cfg: [(u32, u32); 12],
    sys_hz: u32,
    _adc: hal::adc::Adc,
    _temp: hal::adc::TempSense,
    _adc_pins: [Pin<DynPinId, FunctionNull, PullNone>; 8],
}

impl BoardPins {
    pub fn new(set: PinSet, pwm: pac::PWM, adc: pac::ADC, resets: &mut pac::RESETS, sys_hz: u32, peri_hz: u32) -> Self {
        let mut adc = hal::adc::Adc::new(adc, resets);
        let temp = adc.take_temp_sensor().unwrap();
        let mut spi_cs = set.spi_cs;
        let _ = spi_cs.set_high();
        let mut adc_pins = set.adc_pins;
        for p in adc_pins.iter_mut() {
            p.set_input_enable(false);
            p.set_output_disable(true);
        }
        BoardPins {
            gp: set.gp,
            edge_lines: 0,
            _i2c0: set.i2c0,
            _i2c1: set.i2c1,
            spi: set.spi,
            spi_cs,
            spi_hz: 0,
            spi_mode: 0,
            peri_hz,
            pwm: Slices::new(pwm, resets),
            pwm_on: 0,
            slice_cfg: [(0, 0); 12],
            sys_hz,
            _adc: adc,
            _temp: temp,
            _adc_pins: adc_pins,
        }
    }

    /// Set up slice `s`, channel `ch` (0 A, 1 B); None turns the channel off
    /// (and the slice, with `slice_off`).
    fn slice_set(&mut self, s: u32, ch: u32, t: Option<(u32, u32, u32, bool)>, slice_off: bool) {
        fn apply<I: hal::pwm::SliceId, M: ValidSliceMode<I>>(
            sl: &mut hal::pwm::Slice<I, M>,
            ch: u32,
            t: Option<(u32, u32, u32, bool)>,
            slice_off: bool,
        ) {
            use embedded_hal::pwm::SetDutyCycle as _;
            match t {
                Some((div16, top, cc, inv)) => {
                    sl.set_div_int((div16 >> 4) as u8);
                    sl.set_div_frac((div16 & 15) as u8);
                    sl.set_top(top as u16);
                    if ch == 0 {
                        let c = &mut sl.channel_a;
                        let _ = c.set_duty_cycle(cc as u16);
                        if inv { c.set_inverted() } else { c.clr_inverted() }
                        c.set_enabled(true);
                    } else {
                        let c = &mut sl.channel_b;
                        let _ = c.set_duty_cycle(cc as u16);
                        if inv { c.set_inverted() } else { c.clr_inverted() }
                        c.set_enabled(true);
                    }
                    sl.enable();
                }
                None => {
                    if ch == 0 {
                        sl.channel_a.set_enabled(false);
                    } else {
                        sl.channel_b.set_enabled(false);
                    }
                    if slice_off {
                        sl.disable();
                    }
                }
            }
        }
        let p = &mut self.pwm;
        match s {
            0 => apply(&mut p.pwm0, ch, t, slice_off),
            1 => apply(&mut p.pwm1, ch, t, slice_off),
            2 => apply(&mut p.pwm2, ch, t, slice_off),
            3 => apply(&mut p.pwm3, ch, t, slice_off),
            4 => apply(&mut p.pwm4, ch, t, slice_off),
            5 => apply(&mut p.pwm5, ch, t, slice_off),
            6 => apply(&mut p.pwm6, ch, t, slice_off),
            7 => apply(&mut p.pwm7, ch, t, slice_off),
            8 => apply(&mut p.pwm8, ch, t, slice_off),
            9 => apply(&mut p.pwm9, ch, t, slice_off),
            10 => apply(&mut p.pwm10, ch, t, slice_off),
            _ => apply(&mut p.pwm11, ch, t, slice_off),
        }
    }

    /// Other pins with PWM on that share pin `n`'s slice.
    fn slice_users(&self, n: u32) -> u64 {
        let s = pwm_slice(n).0;
        (0..NGPIO).filter(|&k| k != n && self.pwm_on >> k & 1 != 0 && pwm_slice(k).0 == s).fold(0, |m, k| m | 1 << k)
    }
}

/// How long one I2C byte, or the STOP, may take: 9 bit times at 100 kHz
/// are 90 us, and a device may stretch the clock, but not past this.
const I2C_BYTE_US: u64 = 5_000;

/// Spin until `ready()` or `us` microseconds have passed (`cyc_us`: core
/// cycles per microsecond); false on the timeout.
fn spin(cyc_us: u64, us: u64, mut ready: impl FnMut() -> bool) -> bool {
    let end = board::mcycle64() + cyc_us * us;
    while !ready() {
        if board::mcycle64() > end {
            return ready();
        }
    }
    true
}

/// One I2C controller: its registers, its reset bit and its SDA/SCL pins.
struct I2cBus {
    regs: &'static pac::i2c0::RegisterBlock,
    reset_bit: u32,
    sda: usize,
    scl: usize,
}

/// RESETS (0x4002_0000): RESET, its atomic set and clear aliases, and
/// RESET_DONE.
const RESETS_SET: *mut u32 = 0x4002_2000 as *mut u32;
const RESETS_CLR: *mut u32 = 0x4002_3000 as *mut u32;
const RESET_DONE: *const u32 = 0x4002_0008 as *const u32;

impl I2cBus {
    fn new(bus: usize) -> Self {
        // SAFETY: the controllers are ours (`BoardPins::_i2c0`/`_i2c1`); only
        // `i2c_run` uses their registers once the HAL has set them up.
        if bus == 0 {
            I2cBus { regs: unsafe { &*pac::I2C0::ptr() }, reset_bit: 1 << 4, sda: 4, scl: 5 }
        } else {
            I2cBus { regs: unsafe { &*pac::I2C1::ptr() }, reset_bit: 1 << 5, sda: 6, scl: 7 }
        }
    }

    /// The error for an abort the controller reported (and clear it).
    fn abort_error(&self) -> I2cError {
        let src = self.regs.ic_tx_abrt_source().read();
        self.regs.ic_clr_tx_abrt().read();
        if src.abrt_7b_addr_noack().bit_is_set() { I2cError::Nack } else { I2cError::Bus }
    }

    /// After an abort: the controller ends the transfer with a STOP.
    fn aborted(&self, cyc_us: u64) -> I2cError {
        let r = self.regs;
        let e = self.abort_error();
        if spin(cyc_us, I2C_BYTE_US, || r.ic_raw_intr_stat().read().stop_det().is_active()) {
            r.ic_clr_stop_det().read();
        }
        e
    }

    /// The bus stayed busy past the deadline. A controller that is waiting
    /// on a held line can't finish an abort either, so reset it (that also
    /// lets go of the lines) and put back the HAL's setup. If a device still
    /// holds SDA low, clock it free first: up to 9 SCL pulses, then a STOP,
    /// with the pins on SIO for the moment.
    fn timed_out(&self, cyc_us: u64) -> I2cError {
        let r = self.regs;
        let saved = [
            r.ic_con().read().bits(),
            r.ic_tx_tl().read().bits(),
            r.ic_rx_tl().read().bits(),
            r.ic_fs_scl_hcnt().read().bits(),
            r.ic_fs_scl_lcnt().read().bits(),
            r.ic_fs_spklen().read().bits(),
            r.ic_sda_hold().read().bits(),
        ];
        // SAFETY: the atomic aliases touch only this controller's bit.
        unsafe {
            RESETS_SET.write_volatile(self.reset_bit);
            RESETS_CLR.write_volatile(self.reset_bit);
        }
        spin(cyc_us, 100, || unsafe { RESET_DONE.read_volatile() } & self.reset_bit != 0);
        // SAFETY: our own two pins; their function goes back to I2C below.
        let (io, sio) = unsafe { (&*pac::IO_BANK0::ptr(), &*pac::SIO::ptr()) };
        let (sda, scl) = (1u32 << self.sda, 1u32 << self.scl);
        let level = |m: u32| sio.gpio_in().read().bits() & m != 0;
        if !level(sda) {
            // open drain: output enable drives the line low, off lets the
            // pull-up take it high
            let low = |m: u32| sio.gpio_oe_set().write(|w| unsafe { w.bits(m) });
            let release = |m: u32| sio.gpio_oe_clr().write(|w| unsafe { w.bits(m) });
            let half = || spin(cyc_us, 5, || false);
            release(sda | scl);
            sio.gpio_out_clr().write(|w| unsafe { w.bits(sda | scl) });
            for n in [self.sda, self.scl] {
                io.gpio(n).gpio_ctrl().modify(|_, w| w.funcsel().sio());
            }
            for _ in 0..9 {
                if level(sda) {
                    break;
                }
                low(scl);
                half();
                release(scl);
                half();
            }
            low(sda);
            half();
            release(scl);
            half();
            release(sda);
            half();
            for n in [self.sda, self.scl] {
                io.gpio(n).gpio_ctrl().modify(|_, w| w.funcsel().i2c());
            }
        }
        // SAFETY: the values the HAL wrote, read back above.
        unsafe {
            r.ic_con().write(|w| w.bits(saved[0]));
            r.ic_tx_tl().write(|w| w.bits(saved[1]));
            r.ic_rx_tl().write(|w| w.bits(saved[2]));
            r.ic_fs_scl_hcnt().write(|w| w.bits(saved[3]));
            r.ic_fs_scl_lcnt().write(|w| w.bits(saved[4]));
            r.ic_fs_spklen().write(|w| w.bits(saved[5]));
            r.ic_sda_hold().write(|w| w.bits(saved[6]));
        }
        I2cError::Timeout
    }
}

/// One I2C transaction (`Pins::i2c`) on the controller's registers, as the
/// pico-sdk's blocking transfers do it, one byte in flight at a time, but
/// with every wait bounded: a device that holds the bus makes it fail with
/// `Timeout` after at most a few `I2C_BYTE_US`, never hang the emulator.
fn i2c_run(bus: &I2cBus, cyc_us: u64, addr: u8, ops: &mut [I2cOp<'_>]) -> Result<(), I2cError> {
    // The controller can't send an address alone (SMBus quick, i2cdetect's
    // probe): a one-byte read asks the same question.
    if let [I2cOp::Write([])] = ops {
        return i2c_xfer(bus, cyc_us, addr, &mut [I2cOp::Read(&mut [0])]);
    }
    if ops.iter().any(|op| matches!(op, I2cOp::Read([]) | I2cOp::Write([]))) {
        return Err(I2cError::Bus);
    }
    i2c_xfer(bus, cyc_us, addr, ops)
}

fn i2c_xfer(bus: &I2cBus, cyc_us: u64, addr: u8, ops: &mut [I2cOp<'_>]) -> Result<(), I2cError> {
    let r = bus.regs;
    r.ic_enable().write(|w| w.enable().disabled());
    if !spin(cyc_us, I2C_BYTE_US, || r.ic_enable_status().read().ic_en().bit_is_clear()) {
        return Err(bus.timed_out(cyc_us));
    }
    r.ic_tar().write(|w| unsafe { w.ic_tar().bits(u16::from(addr)) });
    r.ic_clr_tx_abrt().read();
    r.ic_clr_stop_det().read();
    r.ic_enable().write(|w| w.enable().enabled());
    let n = ops.len();
    let mut prev_read = None;
    for (k, op) in ops.iter_mut().enumerate() {
        let read = matches!(op, I2cOp::Read(_));
        // a repeated start where the direction changes
        let restart = prev_read.is_some_and(|p| p != read);
        prev_read = Some(read);
        let len = match op {
            I2cOp::Read(b) => b.len(),
            I2cOp::Write(b) => b.len(),
        };
        for i in 0..len {
            let stop = k + 1 == n && i + 1 == len;
            let first = restart && i == 0;
            match op {
                I2cOp::Write(b) => {
                    r.ic_data_cmd().write(|w| unsafe { w.restart().bit(first).stop().bit(stop).dat().bits(b[i]) });
                    // TX_EMPTY_CTRL (the HAL sets it): empty once the byte
                    // has left the shift register
                    if !spin(cyc_us, I2C_BYTE_US, || r.ic_raw_intr_stat().read().tx_empty().is_active()) {
                        return Err(bus.timed_out(cyc_us));
                    }
                    if r.ic_tx_abrt_source().read().bits() != 0 {
                        return Err(bus.aborted(cyc_us));
                    }
                }
                I2cOp::Read(b) => {
                    r.ic_data_cmd().write(|w| w.restart().bit(first).stop().bit(stop).cmd().read());
                    let ok = spin(cyc_us, I2C_BYTE_US, || {
                        r.ic_rxflr().read().rxflr().bits() != 0 || r.ic_tx_abrt_source().read().bits() != 0
                    });
                    if r.ic_tx_abrt_source().read().bits() != 0 {
                        return Err(bus.aborted(cyc_us));
                    }
                    if !ok {
                        return Err(bus.timed_out(cyc_us));
                    }
                    b[i] = r.ic_data_cmd().read().dat().bits();
                }
            }
            if stop {
                if !spin(cyc_us, I2C_BYTE_US, || r.ic_raw_intr_stat().read().stop_det().is_active()) {
                    return Err(bus.timed_out(cyc_us));
                }
                r.ic_clr_stop_det().read();
                // an abort can show only once the STOP is out
                if r.ic_tx_abrt_source().read().bits() != 0 {
                    return Err(bus.abort_error());
                }
            }
        }
    }
    Ok(())
}

impl BoardPins {
    /// Collect and clear the latched edges of the armed lines.
    #[cold]
    #[inline(never)]
    fn scan_edges(&mut self) -> u64 {
        let mut m = 0;
        for n in 0..NGPIO as usize {
            if self.edge_lines >> n & 1 == 0 {
                continue;
            }
            let Some(g) = &mut self.gp[n] else { continue };
            with_pin!(g, p => {
                for irq in [Interrupt::EdgeHigh, Interrupt::EdgeLow] {
                    if p.interrupt_status(irq) {
                        p.clear_interrupt(irq);
                        m |= 1 << n;
                    }
                }
            });
        }
        m
    }
}

impl Pins for BoardPins {
    fn gpio_valid(&self) -> u64 {
        FREE_PINS
    }

    fn gpio_set(&mut self, n: u32, l: Line) {
        let Some(g) = self.gp.get_mut(n as usize).and_then(Option::take) else { return };
        let mut g = if l.oe { g.into_output(l.out) } else { g.into_input() };
        self.pwm_on &= !(1 << n);
        let pull = match (l.pull_up, l.pull_down) {
            (true, true) => DynPullType::BusKeep,
            (true, false) => DynPullType::Up,
            (false, true) => DynPullType::Down,
            (false, false) => DynPullType::None,
        };
        with_pin!(&mut g, p => {
            p.set_pull_type(pull);
            for (on, irq) in [(l.rise, Interrupt::EdgeHigh), (l.fall, Interrupt::EdgeLow)] {
                if on && !p.is_interrupt_enabled(irq) {
                    p.clear_interrupt(irq);
                }
                p.set_interrupt_enabled(irq, on);
            }
        });
        if l.rise || l.fall {
            self.edge_lines |= 1 << n;
        } else {
            self.edge_lines &= !(1 << n);
        }
        self.gp[n as usize] = Some(g);
    }

    fn gpio_levels(&mut self) -> u64 {
        let mut m = 0;
        for (n, g) in self.gp.iter().enumerate() {
            if let Some(g) = g
                && with_pin!(g, p => p.as_input().is_high().unwrap_or(false))
            {
                m |= 1 << n;
            }
        }
        m
    }

    /// The guest's poll runs this every millisecond or so from SRAM: keep the
    /// nothing-happened check inline there and the scan out of line.
    #[inline(always)]
    fn gpio_edges(&mut self) -> u64 {
        if self.edge_lines == 0 || !hal::xh3irq::is_pending(pac::Interrupt::IO_IRQ_BANK0) {
            return 0;
        }
        self.scan_edges()
    }

    fn i2c(&mut self, bus: usize, addr: u8, ops: &mut [I2cOp<'_>]) -> Result<(), I2cError> {
        i2c_run(&I2cBus::new(bus), u64::from(self.sys_hz / 1_000_000), addr, ops)
    }

    fn spi_transfer(&mut self, hz: u32, mode: u8, buf: &mut [u8]) {
        if hz != self.spi_hz {
            self.spi.set_baudrate(self.peri_hz.Hz(), hz.Hz());
            self.spi_hz = hz;
        }
        if mode != self.spi_mode {
            let m = Mode {
                polarity: if mode & 2 != 0 { Polarity::IdleHigh } else { Polarity::IdleLow },
                phase: if mode & 1 != 0 { Phase::CaptureOnSecondTransition } else { Phase::CaptureOnFirstTransition },
            };
            self.spi.set_format(FrameFormat::MotorolaSpi(m));
            self.spi_mode = mode;
        }
        let _ = self.spi.transfer_in_place(buf);
        let _ = self.spi.flush();
    }

    fn spi_cs(&mut self, active: bool) {
        let _ = if active { self.spi_cs.set_low() } else { self.spi_cs.set_high() };
    }

    fn pwm_set(&mut self, n: u32, want: Pwm) -> Pwm {
        let (s, ch) = pwm_slice(n);
        let users = self.slice_users(n);
        if !want.enable {
            if self.pwm_on >> n & 1 != 0 {
                // another pin on the same slice channel keeps it running
                let same_ch = (0..NGPIO).any(|k| users >> k & 1 != 0 && pwm_slice(k).1 == ch);
                if !same_ch {
                    self.slice_set(s, ch, None, users == 0);
                }
                self.pwm_on &= !(1 << n);
                if let Some(g) = self.gp[n as usize].take() {
                    self.gp[n as usize] = Some(g.into_input());
                }
            }
            return want;
        }
        let Some(t) = pwm_timing(self.sys_hz, want.period_ns, want.duty_ns) else {
            return Pwm { period_ns: 0, ..want };
        };
        // pins sharing the slice share its counter: the period can't change
        if users != 0 && self.slice_cfg[s as usize] != (t.div16, t.top) {
            return Pwm { period_ns: 0, ..want };
        }
        self.slice_set(s, ch, Some((t.div16, t.top, t.cc, want.invert)), false);
        self.slice_cfg[s as usize] = (t.div16, t.top);
        self.pwm_on |= 1 << n;
        if let Some(g) = self.gp[n as usize].take() {
            self.gp[n as usize] = Some(g.into_pwm());
        }
        Pwm { period_ns: t.period_ns, duty_ns: t.duty_ns, ..want }
    }

    fn adc(&mut self, ch: u32) -> Option<u16> {
        // SAFETY: the ADC is ours (`_adc`); this is the HAL's own one-shot
        // conversion, with the QFN-80 channel numbers.
        let adc = unsafe { &*pac::ADC::ptr() };
        adc.cs().modify(|_, w| unsafe { w.ainsel().bits(ch as u8).start_once().set_bit() });
        while adc.cs().read().ready().bit_is_clear() {}
        let r = adc.result().read().result().bits();
        (!adc.cs().read().err().bit_is_set()).then_some(r)
    }
}

/// The default SPI mode before the guest picks one.
pub const SPI_MODE: Mode = MODE_0;

/// 100 kHz, the I2C bus speed.
pub fn i2c_hz() -> hal::fugit::HertzU32 {
    100.kHz()
}
