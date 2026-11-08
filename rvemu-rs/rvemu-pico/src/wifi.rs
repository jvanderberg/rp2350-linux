//! Wi-Fi for the guest: the RM2 module's CYW43439 (stock Pico 2 W wiring:
//! WL_ON GP23, data/IRQ GP24, CS GP25, clock GP29) run by the `cyw43` driver,
//! its Ethernet frames passed to and from the guest's virtio-net device.
//!
//! The bus is a port of `cyw43-pio` (embassy) to rp235x-hal: the same PIO
//! program and per-transaction sequence, with the CPU feeding the FIFOs where
//! the original uses DMA. There is no executor: `Radio::poll` polls the
//! driver's futures with a no-op waker from the emulator's device poll, and
//! `embassy-time` reads the core's cycle counter.

use core::future::{Future, poll_fn};
use core::pin::Pin;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use core::task::{Context, Poll, Waker};

use cyw43::SpiBusCyw43;
use embassy_net_driver::{Driver, HardwareAddress, LinkState, RxToken, TxToken};
use hal::gpio::bank0::{Gpio23, Gpio24, Gpio25, Gpio29};
use hal::gpio::{
    FunctionNull, FunctionPio0, FunctionSioOutput, OutputDriveStrength, OutputSlewRate, Pin as GpioPin, PullDown,
    PullNone,
};
use hal::pio::{PIOBuilder, PIOExt, PinDir, PinState, SM0, ShiftDirection, StateMachine, Stopped};
use rp2350_board as board;
use rp235x_hal as hal;
use rtt_target::rprintln;
use rvcore::virtio::{FRAME_MAX, NetLink};

// ------------------------------------------------------------------ time

static CYCLES_PER_US: AtomicU32 = AtomicU32::new(150);

struct CycleClock;

impl embassy_time_driver::Driver for CycleClock {
    fn now(&self) -> u64 {
        board::mcycle64() / u64::from(CYCLES_PER_US.load(Ordering::Relaxed))
    }
    /// Nothing to arm: every future is polled again on the next device poll.
    fn schedule_wake(&self, _at: u64, _waker: &Waker) {}
}

embassy_time_driver::time_driver_impl!(static CLOCK: CycleClock = CycleClock);

// ------------------------------------------------------------------ PIO gSPI bus

const PIO0: usize = 0x5020_0000;
const PIO_CTRL: usize = PIO0;
const PIO_FSTAT: usize = PIO0 + 0x004;
const PIO_TXF0: usize = PIO0 + 0x010;
const PIO_RXF0: usize = PIO0 + 0x020;
const PIO_IRQ: usize = PIO0 + 0x030;
const PIO_INPUT_SYNC_BYPASS: usize = PIO0 + 0x038;
const PIO_SM0_INSTR: usize = PIO0 + 0x0d8;
/// Atomic set / clear register aliases.
const SET: usize = 0x2000;
const CLR: usize = 0x3000;
const FSTAT_RXEMPTY0: u32 = 1 << 8;
const FSTAT_TXFULL0: u32 = 1 << 16;

const PIN_DIO: u8 = 24;
const PIN_CLK: u8 = 29;
/// gSPI clock for the RM2: cyw43-pio's RM2_CLOCK_DIVIDER gives a 50 MHz PIO
/// clock (25 MHz bus) at the stock 150 MHz; pick the divider that does the
/// same at our clock.
const PIO_HZ_MAX: u32 = 50_000_000;
/// A transfer that takes longer than this is abandoned (no radio fitted, or
/// it stopped answering), so the emulator never hangs on the bus.
const XFER_TIMEOUT_US: u64 = 20_000;

fn rd(a: usize) -> u32 {
    // SAFETY: PIO0 register.
    unsafe { read_volatile(a as *const u32) }
}
fn wr(a: usize, v: u32) {
    // SAFETY: PIO0 register.
    unsafe { write_volatile(a as *mut u32, v) }
}

pub struct PioSpi {
    cs: GpioPin<Gpio25, FunctionSioOutput, PullDown>,
    /// Owned so nothing else claims SM0; driven through its registers.
    _sm: StateMachine<(hal::pac::PIO0, SM0), Stopped>,
    wrap_target: u8,
    _dio: GpioPin<Gpio24, FunctionPio0, PullNone>,
    _clk: GpioPin<Gpio29, FunctionPio0, PullDown>,
    timeouts: u32,
}

impl PioSpi {
    pub fn new(
        pio0: hal::pac::PIO0,
        resets: &mut hal::pac::RESETS,
        sys_hz: u32,
        dio: GpioPin<Gpio24, FunctionNull, PullDown>,
        clk: GpioPin<Gpio29, FunctionNull, PullDown>,
        cs: GpioPin<Gpio25, FunctionNull, PullDown>,
    ) -> Self {
        let div = sys_hz.div_ceil(PIO_HZ_MAX);
        // cyw43-pio picks its program by PIO clock; below 75 MHz it is this
        // one ("low speed").
        let program = pio::pio_asm!(
            ".side_set 1"

            ".wrap_target"
            // write out x-1 bits
            "lp:"
            "out pins, 1    side 0"
            "jmp x-- lp     side 1"
            // switch directions
            "set pindirs, 0 side 0"
            "nop            side 0"
            // read in y-1 bits
            "lp2:"
            "in pins, 1     side 1"
            "jmp y-- lp2    side 0"

            // wait for event and irq host
            "wait 1 pin 0   side 0"
            "irq 0          side 0"

            ".wrap"
        );
        let (mut pio, sm0, _, _, _) = pio0.split(resets);
        let installed = pio.install(&program.program).unwrap();
        let wrap_target = installed.wrap_target();

        let mut dio = dio.into_pull_type::<PullNone>().into_function::<FunctionPio0>();
        dio.set_schmitt_enabled(true);
        dio.set_drive_strength(OutputDriveStrength::TwelveMilliAmps);
        dio.set_slew_rate(OutputSlewRate::Fast);
        let mut clk = clk.into_function::<FunctionPio0>();
        clk.set_drive_strength(OutputDriveStrength::TwelveMilliAmps);
        clk.set_slew_rate(OutputSlewRate::Fast);
        let mut cs = cs.into_push_pull_output_in_state(hal::gpio::PinState::High);
        cs.set_drive_strength(OutputDriveStrength::TwelveMilliAmps);

        let (mut sm, _rx, _tx) = PIOBuilder::from_installed_program(installed)
            .side_set_pin_base(PIN_CLK)
            .out_pins(PIN_DIO, 1)
            .in_pin_base(PIN_DIO)
            .set_pins(PIN_DIO, 1)
            .out_shift_direction(ShiftDirection::Left)
            .autopull(true)
            .in_shift_direction(ShiftDirection::Left)
            .autopush(true)
            .clock_divisor_fixed_point(div as u16, 0)
            .build(sm0);
        wr(PIO_INPUT_SYNC_BYPASS + SET, 1 << PIN_DIO);
        sm.set_pindirs([(PIN_CLK, PinDir::Output), (PIN_DIO, PinDir::Output)]);
        sm.set_pins([(PIN_CLK, PinState::Low), (PIN_DIO, PinState::Low)]);
        rprintln!("[wifi] gSPI: PIO clock {} MHz (divider {})", sys_hz / div / 1_000_000, div);
        PioSpi { cs, _sm: sm, wrap_target, _dio: dio, _clk: clk, timeouts: 0 }
    }

    fn set_enable(&mut self, on: bool) {
        wr(PIO_CTRL + if on { SET } else { CLR }, 1);
    }

    fn exec(&mut self, instr: u16) {
        wr(PIO_SM0_INSTR, u32::from(instr));
    }

    fn set_x(&mut self, v: u32) {
        const OUT: u16 = pio::InstructionOperands::OUT { destination: pio::OutDestination::X, bit_count: 32 }.encode();
        wr(PIO_TXF0, v);
        self.exec(OUT);
    }

    fn set_y(&mut self, v: u32) {
        const OUT: u16 = pio::InstructionOperands::OUT { destination: pio::OutDestination::Y, bit_count: 32 }.encode();
        wr(PIO_TXF0, v);
        self.exec(OUT);
    }

    fn set_pindir(&mut self, data: u8) {
        let set = pio::InstructionOperands::SET { destination: pio::SetDestination::PINDIRS, data }.encode();
        self.exec(set);
    }

    fn exec_jmp(&mut self, address: u8) {
        let jmp = pio::InstructionOperands::JMP { address, condition: pio::JmpCondition::Always }.encode();
        self.exec(jmp);
    }

    /// Restart the program with X = bits to write - 1, Y = bits to read - 1.
    fn start(&mut self, write_bits: u32, read_bits: u32) {
        self.set_enable(false);
        self.set_x(write_bits);
        self.set_y(read_bits);
        self.set_pindir(0b1);
        self.exec_jmp(self.wrap_target);
        self.set_enable(true);
    }

    /// Spin until `ready` or the transfer deadline; false on timeout.
    fn wait(&mut self, deadline: u64, ready: impl Fn() -> bool) -> bool {
        while !ready() {
            if embassy_time_driver::now() > deadline {
                self.timeouts += 1;
                return false;
            }
        }
        true
    }

    fn push(&mut self, deadline: u64, w: u32) -> bool {
        let ok = self.wait(deadline, || rd(PIO_FSTAT) & FSTAT_TXFULL0 == 0);
        if ok {
            wr(PIO_TXF0, w);
        }
        ok
    }

    fn pull(&mut self, deadline: u64) -> Option<u32> {
        self.wait(deadline, || rd(PIO_FSTAT) & FSTAT_RXEMPTY0 == 0).then(|| rd(PIO_RXF0))
    }

    /// Write data to peripheral and return status.
    fn write(&mut self, write: &[u32]) -> u32 {
        self.start(write.len() as u32 * 32 - 1, 31);
        let deadline = embassy_time_driver::now() + XFER_TIMEOUT_US;
        for &w in write {
            if !self.push(deadline, w) {
                return 0;
            }
        }
        self.pull(deadline).unwrap_or(0)
    }

    /// Send command and read response into buffer.
    fn read(&mut self, cmd: u32, read: &mut [u32]) -> u32 {
        self.start(31, read.len() as u32 * 32 + 32 - 1);
        let deadline = embassy_time_driver::now() + XFER_TIMEOUT_US;
        if !self.push(deadline, cmd) {
            return 0;
        }
        for r in read.iter_mut() {
            match self.pull(deadline) {
                Some(w) => *r = w,
                None => return 0,
            }
        }
        self.pull(deadline).unwrap_or(0)
    }
}

impl SpiBusCyw43 for PioSpi {
    async fn cmd_write(&mut self, write: &[u32]) -> u32 {
        self.cs.set_low_infallible();
        let status = self.write(write);
        self.cs.set_high_infallible();
        status
    }

    async fn cmd_read(&mut self, write: u32, read: &mut [u32]) -> u32 {
        self.cs.set_low_infallible();
        let status = self.read(write, read);
        self.cs.set_high_infallible();
        status
    }

    /// The program raises PIO IRQ 0 once the chip pulls the data line high
    /// after a transfer (its interrupt).
    async fn wait_for_event(&mut self) {
        poll_fn(|_| {
            if rd(PIO_IRQ) & 1 != 0 {
                wr(PIO_IRQ, 1);
                Poll::Ready(())
            } else {
                Poll::Pending
            }
        })
        .await
    }
}

trait InfallibleOutput {
    fn set_low_infallible(&mut self);
    fn set_high_infallible(&mut self);
}

impl<P: embedded_hal::digital::OutputPin<Error = core::convert::Infallible>> InfallibleOutput for P {
    fn set_low_infallible(&mut self) {
        let Ok(()) = self.set_low();
    }
    fn set_high_infallible(&mut self) {
        let Ok(()) = self.set_high();
    }
}

// ------------------------------------------------------------------ credentials

/// Wi-Fi network to join, stored in flash (the last 4 KB below the guest
/// blob) by scripts/pico-wifi.sh:
/// magic "WIFI", SSID length, passphrase length, SSID[32], passphrase[64].
const CREDS_ADDR: usize = 0x103F_F000;
const CREDS_MAGIC: u32 = 0x4946_4957; // "WIFI"

#[derive(Clone, Copy)]
pub struct Creds {
    ssid: &'static str,
    pass: &'static [u8],
}

impl Creds {
    pub fn from_flash() -> Option<Creds> {
        // SAFETY: flash is mapped (XIP) and never written while we run.
        let w = |i: usize| unsafe { read_volatile((CREDS_ADDR + 4 * i) as *const u32) };
        let (slen, plen) = (w(1) as usize, w(2) as usize);
        if w(0) != CREDS_MAGIC || slen == 0 || slen > 32 || plen > 64 {
            return None;
        }
        // SAFETY: as above; lengths checked.
        let (s, p) = unsafe {
            (
                core::slice::from_raw_parts((CREDS_ADDR + 12) as *const u8, slen),
                core::slice::from_raw_parts((CREDS_ADDR + 44) as *const u8, plen),
            )
        };
        Some(Creds { ssid: core::str::from_utf8(s).ok()?, pass: p })
    }

    pub fn ssid(&self) -> &str {
        self.ssid
    }
}

// ------------------------------------------------------------------ driver task

type Pwr = GpioPin<Gpio23, FunctionSioOutput, PullDown>;

static mut STATE: cyw43::State = cyw43::State::new();
/// The driver's network device once it is up; the guest's link reads it.
static mut NET: Option<cyw43::NetDriver<'static>> = None;
static READY: AtomicBool = AtomicBool::new(false);

fn net() -> Option<&'static mut cyw43::NetDriver<'static>> {
    // SAFETY: single core, no interrupts: only the emulator loop touches it.
    unsafe { (*addr_of_mut!(NET)).as_mut() }
}

/// Bring the chip up, then join `creds` (retrying) while serving the bus.
async fn run(pwr: Pwr, spi: PioSpi, creds: Option<Creds>) {
    let fw = cyw43::aligned_bytes!("../firmware/43439A0.bin");
    let clm = cyw43::aligned_bytes!("../firmware/43439A0_clm.bin");
    let nvram = cyw43::aligned_bytes!("../firmware/nvram_rp2040.bin");
    // SAFETY: STATE is borrowed here only, once.
    let state = unsafe { &mut *addr_of_mut!(STATE) };
    let (device, mut control, runner) = cyw43::new(state, pwr, spi, fw, nvram).await;
    // SAFETY: as in `net`.
    unsafe { *addr_of_mut!(NET) = Some(device) };
    let control_task = async move {
        control.init(clm).await;
        control.set_power_management(cyw43::PowerManagementMode::None).await;
        let mac = control.address().await;
        rprintln!(
            "[wifi] CYW43439 up, MAC {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
        );
        READY.store(true, Ordering::Relaxed);
        let Some(c) = creds else {
            rprintln!("[wifi] no network configured (scripts/pico-wifi.sh)");
            return;
        };
        loop {
            match control.join(c.ssid, cyw43::JoinOptions::new(c.pass)).await {
                Ok(()) => {
                    rprintln!("[wifi] joined {:?}", c.ssid);
                    return;
                }
                Err(e) => {
                    rprintln!("[wifi] join {:?} failed: {:?}; retrying", c.ssid, e);
                    embassy_time::Timer::after_secs(3).await;
                }
            }
        }
    };
    embassy_futures::join::join(runner.run(), control_task).await;
}

/// The driver, polled from the emulator's device poll.
pub struct Radio<'a> {
    fut: Pin<&'a mut dyn Future<Output = ()>>,
}

impl<'a> Radio<'a> {
    pub fn poll(&mut self) {
        let mut cx = Context::from_waker(Waker::noop());
        let _ = self.fut.as_mut().poll(&mut cx);
    }

    /// Poll until the chip is up (its MAC known) or `timeout_us` passes.
    pub fn bring_up(&mut self, timeout_us: u64) -> bool {
        let t0 = embassy_time_driver::now();
        while !READY.load(Ordering::Relaxed) && embassy_time_driver::now() - t0 < timeout_us {
            self.poll();
        }
        READY.load(Ordering::Relaxed)
    }
}

/// The RM2's pins, as `hal::gpio::Pins::new` hands them out.
pub struct RadioPins {
    pub pwr: GpioPin<Gpio23, FunctionNull, PullDown>,
    pub dio: GpioPin<Gpio24, FunctionNull, PullDown>,
    pub cs: GpioPin<Gpio25, FunctionNull, PullDown>,
    pub clk: GpioPin<Gpio29, FunctionNull, PullDown>,
}

/// The driver future for `Radio`; pin it where it lives
/// (`core::pin::pin!`).
pub fn driver(
    pio0: hal::pac::PIO0,
    resets: &mut hal::pac::RESETS,
    sys_hz: u32,
    pins: RadioPins,
    creds: Option<Creds>,
) -> impl Future<Output = ()> + use<> {
    CYCLES_PER_US.store(sys_hz / 1_000_000, Ordering::Relaxed);
    let spi = PioSpi::new(pio0, resets, sys_hz, pins.dio, pins.clk, pins.cs);
    let pwr = pins.pwr.into_push_pull_output_in_state(hal::gpio::PinState::Low);
    run(pwr, spi, creds)
}

impl<'a> Radio<'a> {
    pub fn new(fut: Pin<&'a mut dyn Future<Output = ()>>) -> Self {
        Radio { fut }
    }
}

// ------------------------------------------------------------------ guest link

/// The guest's Ethernet: frames to and from the driver's network device.
pub struct WifiLink;

fn cx() -> Context<'static> {
    Context::from_waker(Waker::noop())
}

impl NetLink for WifiLink {
    fn mac(&self) -> [u8; 6] {
        match net().map(|d| d.hardware_address()) {
            Some(HardwareAddress::Ethernet(m)) => m,
            _ => [0; 6],
        }
    }
    fn link_up(&self) -> bool {
        net().is_some_and(|d| d.link_state(&mut cx()) == LinkState::Up)
    }
    fn send(&mut self, frame: &[u8]) {
        if let Some(tx) = net().and_then(|d| d.transmit(&mut cx())) {
            tx.consume(frame.len(), |b| b.copy_from_slice(frame));
        }
    }
    fn recv(&mut self, buf: &mut [u8; FRAME_MAX]) -> Option<usize> {
        let (rx, _tx) = net()?.receive(&mut cx())?;
        Some(rx.consume(|b| {
            let n = b.len().min(FRAME_MAX);
            buf[..n].copy_from_slice(&b[..n]);
            n
        }))
    }
}
