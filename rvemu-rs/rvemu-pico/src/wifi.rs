//! Wi-Fi for the guest: the RM2 module's CYW43439 (stock Pico 2 W wiring:
//! WL_ON GP23, data/IRQ GP24, CS GP25, clock GP29) run by the `cyw43` driver,
//! its Ethernet frames passed to and from the guest's virtio-net device.
//!
//! The bus is a port of `cyw43-pio` (embassy) to rp235x-hal: the same PIO
//! program and per-transaction sequence, with the CPU feeding the FIFOs where
//! the original uses DMA. There is no executor: core1's service loop
//! (`RadioService::poll`) polls the driver's futures with a no-op waker, and
//! `embassy-time` reads core1's timer (`core1::now()`, at the system clock).
//!
//! All of this runs on core1 (`core1.rs`), from SRAM. Frames reach the
//! guest's virtio-net on core0 through two `FrameRing`s (`NET_RX`, `NET_TX`);
//! the MAC and link state are published in `RADIO`, `MAC` and `LINK_UP`.

use core::future::{Future, poll_fn};
use core::pin::Pin;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use core::task::{Context, Poll, Waker};

use crate::core1::{self, FrameRing};

use cyw43::SpiBusCyw43;
use embassy_net_driver::{Driver, HardwareAddress, LinkState, RxToken, TxToken};
use hal::gpio::bank0::{Gpio23, Gpio24, Gpio25, Gpio29};
use hal::gpio::{
    FunctionNull, FunctionPio0, FunctionSioOutput, OutputDriveStrength, OutputSlewRate, Pin as GpioPin, PullDown,
    PullNone,
};
use hal::pio::{PIOBuilder, PIOExt, PinDir, PinState, SM0, ShiftDirection, StateMachine, Stopped};
use rp235x_hal as hal;
use rtt_target::rprintln;

// ------------------------------------------------------------------ time

/// 2^32 / (cycles per microsecond): `now` multiplies instead of making a
/// u64 division (a libcall) on every call.
static US_PER_CYCLE_Q32: AtomicU32 = AtomicU32::new(u32::MAX / 150);

struct CycleClock;

impl embassy_time_driver::Driver for CycleClock {
    fn now(&self) -> u64 {
        let c = core1::now();
        let m = u64::from(US_PER_CYCLE_Q32.load(Ordering::Relaxed));
        (c >> 32) * m + (((c & 0xFFFF_FFFF) * m) >> 32)
    }
    /// Nothing to arm: every future is polled again on core1's next service pass.
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
/// it stopped answering), so core1 never hangs on the bus.
const XFER_TIMEOUT_MS: u32 = 20;

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
    _dio: GpioPin<Gpio24, FunctionPio0, PullNone>,
    _clk: GpioPin<Gpio29, FunctionPio0, PullDown>,
    timeouts: u32,
    /// `XFER_TIMEOUT_MS` in core cycles.
    timeout_cycles: u64,
    /// `set pindirs, 1` and `jmp wrap_target`, encoded once.
    set_pindir_out: u16,
    jmp_wrap: u16,
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
        let set_pindir_out =
            pio::InstructionOperands::SET { destination: pio::SetDestination::PINDIRS, data: 1 }.encode();
        let jmp_wrap =
            pio::InstructionOperands::JMP { address: wrap_target, condition: pio::JmpCondition::Always }.encode();
        let timeout_cycles = u64::from(sys_hz / 1000 * XFER_TIMEOUT_MS);
        PioSpi { cs, _sm: sm, _dio: dio, _clk: clk, timeouts: 0, timeout_cycles, set_pindir_out, jmp_wrap }
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

    /// Restart the program with X = bits to write - 1, Y = bits to read - 1.
    fn start(&mut self, write_bits: u32, read_bits: u32) {
        self.set_enable(false);
        self.set_x(write_bits);
        self.set_y(read_bits);
        self.exec(self.set_pindir_out);
        self.exec(self.jmp_wrap);
        self.set_enable(true);
    }

    /// Spin until `ready` or the transfer deadline (in cycles); false on
    /// timeout.
    fn wait(&mut self, deadline: u64, ready: impl Fn() -> bool) -> bool {
        while !ready() {
            if core1::now() > deadline {
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
        let deadline = core1::now() + self.timeout_cycles;
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
        let deadline = core1::now() + self.timeout_cycles;
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
/// The driver's network device once it is up (core1 only).
static mut NET: Option<cyw43::NetDriver<'static>> = None;
static READY: AtomicBool = AtomicBool::new(false);

fn net() -> Option<&'static mut cyw43::NetDriver<'static>> {
    // SAFETY: only core1's radio service touches it, from one place at a
    // time (the driver future publishes it before the service reads it).
    unsafe { (*addr_of_mut!(NET)).as_mut() }
}

/// The same bytes through flash's uncached, non-allocating XIP alias, so
/// uploading the firmware doesn't sweep the guest's data out of the XIP
/// cache.
fn uncached(b: &'static cyw43::Aligned<cyw43::A4, [u8]>) -> &'static cyw43::Aligned<cyw43::A4, [u8]> {
    const XIP_NOCACHE_NOALLOC_OFFSET: usize = 0x1400_0000 - 0x1000_0000;
    let p = core::ptr::from_ref(b);
    let a = p.cast::<u8>() as usize;
    if !(0x1000_0000..0x1400_0000).contains(&a) {
        return b;
    }
    let q = core::ptr::slice_from_raw_parts((a + XIP_NOCACHE_NOALLOC_OFFSET) as *const u8, b.len());
    // SAFETY: the same flash bytes at another address with the same
    // alignment, never written while we run; `Aligned` is repr(C) around
    // the slice, so the pointer metadata carries over.
    unsafe { &*(q as *const cyw43::Aligned<cyw43::A4, [u8]>) }
}

/// Bring the chip up, then join `creds` (retrying) while serving the bus.
async fn run(pwr: Pwr, spi: PioSpi, creds: Option<Creds>) {
    let fw = uncached(cyw43::aligned_bytes!("../firmware/43439A0.bin"));
    let clm = uncached(cyw43::aligned_bytes!("../firmware/43439A0_clm.bin"));
    let nvram = uncached(cyw43::aligned_bytes!("../firmware/nvram_rp2040.bin"));
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

/// The RM2's pins, as `hal::gpio::Pins::new` hands them out.
pub struct RadioPins {
    pub pwr: GpioPin<Gpio23, FunctionNull, PullDown>,
    pub dio: GpioPin<Gpio24, FunctionNull, PullDown>,
    pub cs: GpioPin<Gpio25, FunctionNull, PullDown>,
    pub clk: GpioPin<Gpio29, FunctionNull, PullDown>,
}

/// The driver future for `RadioService` (set up on core0, run on core1).
pub fn driver(
    pio0: hal::pac::PIO0,
    resets: &mut hal::pac::RESETS,
    sys_hz: u32,
    pins: RadioPins,
    creds: Option<Creds>,
) -> impl Future<Output = ()> + use<> {
    US_PER_CYCLE_Q32.store(u32::MAX / (sys_hz / 1_000_000), Ordering::Relaxed);
    let spi = PioSpi::new(pio0, resets, sys_hz, pins.dio, pins.clk, pins.cs);
    let pwr = pins.pwr.into_push_pull_output_in_state(hal::gpio::PinState::Low);
    run(pwr, spi, creds)
}

// ------------------------------------------------------------------ the guest's link (shared with core0)

/// Frames from the radio to the guest (core1 pushes, core0 pops). More
/// wait in the driver's own buffers (4).
#[unsafe(link_section = ".sram89.net_rx")]
pub static NET_RX: FrameRing<2> = FrameRing::new();
/// Frames from the guest to the radio (core0 pushes, core1 pops).
#[unsafe(link_section = ".sram89.net_tx")]
pub static NET_TX: FrameRing<2> = FrameRing::new();

pub const RADIO_STARTING: u32 = 0;
pub const RADIO_UP: u32 = 1;
pub const RADIO_ABSENT: u32 = 2;
/// `RADIO_*`; `MAC` is valid once it is `RADIO_UP`.
pub static RADIO: AtomicU32 = AtomicU32::new(RADIO_STARTING);
/// The MAC, bytes 0-3 and 4-5, little-endian.
pub static MAC: [AtomicU32; 2] = [AtomicU32::new(0), AtomicU32::new(0)];
pub static LINK_UP: AtomicBool = AtomicBool::new(false);

/// core1's radio service: drives the driver, moves frames between it and
/// the rings, publishes the MAC and the link state.
pub struct RadioService<F> {
    /// Pinned in place: services never move (`core1::launch`).
    fut: F,
    /// When bring-up started, and how long it may take.
    t0: u64,
    bring_up_cycles: u64,
}

// SAFETY: built on core0 before core1 starts, then used by core1 only.
unsafe impl<F> Send for RadioService<F> {}

impl<F: Future<Output = ()>> RadioService<F> {
    /// `bring_up_ms`: how long the chip may take to come up before the
    /// guest's NIC is reported absent.
    pub fn new(fut: F, sys_hz: u32, bring_up_ms: u32) -> Self {
        RadioService { fut, t0: 0, bring_up_cycles: u64::from(sys_hz / 1000) * u64::from(bring_up_ms) }
    }

    fn poll_driver(&mut self) {
        let mut cx = Context::from_waker(core1::waker());
        // SAFETY: `fut` never moves (see the field).
        let _ = unsafe { Pin::new_unchecked(&mut self.fut) }.poll(&mut cx);
    }
}

impl<F: Future<Output = ()>> core1::Service for RadioService<F> {
    fn poll(&mut self) {
        let mut cx = Context::from_waker(core1::waker());
        // guest -> radio: as many frames as the driver takes
        if let Some(d) = net() {
            while !NET_TX.is_empty() {
                let Some(tx) = d.transmit(&mut cx) else { break };
                NET_TX.pop_with(|f| tx.consume(f.len(), |b| b.copy_from_slice(f)));
            }
        }
        self.poll_driver();
        if RADIO.load(Ordering::Relaxed) == RADIO_STARTING && !READY.load(Ordering::Relaxed) {
            let now = core1::now();
            if self.t0 == 0 {
                self.t0 = now;
            } else if now - self.t0 > self.bring_up_cycles {
                rprintln!("[wifi] radio did not come up; the guest's eth0 stays down");
                RADIO.store(RADIO_ABSENT, Ordering::Release);
                core1::notify_core0();
            }
            return;
        }
        let Some(d) = net() else { return };
        // radio -> guest, while there's room (the rest waits in the driver)
        let mut got = false;
        while NET_RX.has_room() {
            let Some((rx, _tx)) = d.receive(&mut cx) else { break };
            NET_RX.push_with(|slot| {
                Some(rx.consume(|b| {
                    let n = b.len().min(slot.len());
                    slot[..n].copy_from_slice(&b[..n]);
                    n
                }))
            });
            got = true;
        }
        if RADIO.load(Ordering::Relaxed) == RADIO_STARTING {
            // READY: the chip is up and the driver knows its MAC
            if let HardwareAddress::Ethernet(m) = d.hardware_address() {
                MAC[0].store(u32::from_le_bytes([m[0], m[1], m[2], m[3]]), Ordering::Relaxed);
                MAC[1].store(u32::from_le_bytes([m[4], m[5], 0, 0]), Ordering::Relaxed);
            }
            RADIO.store(RADIO_UP, Ordering::Release);
            got = true;
        }
        let up = d.link_state(&mut cx) == LinkState::Up;
        if up != LINK_UP.load(Ordering::Relaxed) {
            LINK_UP.store(up, Ordering::Release);
            got = true;
        }
        if got {
            core1::notify_core0();
        }
    }
}
