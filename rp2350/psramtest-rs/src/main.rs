//! PSRAM bring-up and soak test for an RP2350B running on the Hazard3 RISC-V
//! cores, PSRAM on QMI CS1 (GP0). Rust port of ../psramtest/main.c.
//!
//! The QMI sequence follows PicoMite/FanPico psram.c (Timo Kokkonen): exit
//! QPI, read ID, reset, enter QPI, then program the M1 timing/format
//! registers. Clocks come from rp235x-hal.
//!
//! With the default `overclock` feature the core runs at 348 MHz on 1.20 V
//! with the PSRAM at 116 MHz (divider 3, above its 109 MHz rating), the
//! settings that soaked clean on the Arm side. `--no-default-features` gives
//! the stock 150 MHz with the in-spec PSRAM divider. The core voltage is set
//! the same way as the Pico SDK's vreg_set_voltage (the HAL has no setter).
//!
//! Output is RTT. Results are also kept in `RESULT` for reading over SWD.
#![no_std]
#![no_main]

use core::arch::asm;
use core::ptr::{read_volatile, write_volatile};

use panic_rtt_target as _;
use rp235x_hal as hal;
use rtt_target::{rprintln, rtt_init_print};

#[link_section = ".start_block"]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

// ---- test configuration ----------------------------------------------------

const XTAL_HZ: u32 = 12_000_000;
const SOAK_S: u64 = 60;

/// 12 MHz x 87 = 1044 MHz VCO, / 3 / 1 = 348 MHz (exact 350 is not reachable).
#[cfg(feature = "overclock")]
const PLL_SYS_OC: hal::pll::PLLConfig = hal::pll::PLLConfig {
    vco_freq: fugit::HertzU32::MHz(1044),
    refdiv: 1,
    post_div1: 3,
    post_div2: 1,
};
/// POWMAN VREG VSEL code 0b01101 = 1.20 V (SDK VREG_VOLTAGE_1_20).
#[cfg(feature = "overclock")]
const VREG_VSEL: u32 = 0b01101;
/// Forced PSRAM SCK divider; `None` picks the in-spec one (<= 109 MHz).
const PSRAM_CLKDIV: Option<u32> = if cfg!(feature = "overclock") { Some(3) } else { None };

// ---- registers (values from the Pico SDK rp2350 headers) -------------------

const SET: usize = 0x2000;
const CLR: usize = 0x3000;

const QMI: usize = 0x400d_0000;
const DIRECT_CSR: usize = QMI;
const DIRECT_TX: usize = QMI + 0x04;
const DIRECT_RX: usize = QMI + 0x08;
#[cfg(feature = "overclock")]
const M0_TIMING: usize = QMI + 0x0c;
const M1_TIMING: usize = QMI + 0x20;
const M1_RFMT: usize = QMI + 0x24;
const M1_RCMD: usize = QMI + 0x28;
const M1_WFMT: usize = QMI + 0x2c;
const M1_WCMD: usize = QMI + 0x30;

const CSR_EN: u32 = 1 << 0;
const CSR_BUSY: u32 = 1 << 1;
const CSR_ASSERT_CS1N: u32 = 1 << 3;
const CSR_TXEMPTY: u32 = 1 << 11;
const CSR_CLKDIV_LSB: u32 = 22;
const TX_OE: u32 = 1 << 19;
const TX_IWIDTH_Q: u32 = 2 << 16;

/// TIMER0.DBGPAUSE: resets to 7, which pauses the timer whenever a core is
/// halted by the debugger and skews every timing below. Cleared at start.
const TIMER0_DBGPAUSE: usize = 0x400b_002c;

const XIP_CTRL: usize = 0x400c_8000;

#[cfg(feature = "overclock")]
mod powman {
    pub const VREG_CTRL: usize = 0x4010_0004;
    pub const VREG: usize = 0x4010_000c;
    pub const PASSWORD: u32 = 0x5afe_0000;
    pub const VREG_CTRL_UNLOCK: u32 = 1 << 13;
    pub const VREG_UPDATE_IN_PROGRESS: u32 = 1 << 15;
    pub const VREG_VSEL_LSB: u32 = 4;
    pub const VREG_VSEL_BITS: u32 = 0x1f0;
}
const XIP_CTRL_WRITABLE_M1: u32 = 1 << 11;

/// M1_RFMT: quad prefix/addr/dummy/data, 8-bit prefix, 24 dummy bits.
const RFMT: u32 = 6 << 16 | 1 << 12 | 2 << 8 | 2 << 6 | 2 << 4 | 2 << 2 | 2;
/// M1_WFMT: same without dummy bits.
const WFMT: u32 = 1 << 12 | 2 << 8 | 2 << 6 | 2 << 4 | 2 << 2 | 2;
// Same values the C build programs (read back from the target over SWD).
const _: () = assert!(RFMT == 0x0006_12aa && WFMT == 0x0000_12aa);

const CMD_QUAD_READ: u32 = 0xeb;
const CMD_QUAD_WRITE: u32 = 0x38;
const CMD_ENTER_QPI: u32 = 0x35;
const CMD_EXIT_QPI: u32 = 0xf5;
const CMD_RESET_ENABLE: u32 = 0x66;
const CMD_RESET: u32 = 0x99;
const CMD_READ_ID: u32 = 0x9f;
const KGD_PASS: u8 = 0x5d;

const PSRAM_CACHED: usize = 0x1100_0000;
const PSRAM_UNCACHED: usize = 0x1500_0000;
const PSRAM_WINDOW: u32 = 16 << 20;
const PSRAM_MAX_CSR_CLK: u32 = 5_000_000;

// ---- results ---------------------------------------------------------------

#[repr(C)]
pub struct TestResult {
    pub magic: u32, // 'PSRT' once finished
    pub stage: u32,
    pub id: [u8; 8],
    pub size: u32,
    pub errors: u32,
    pub first_bad_addr: u32,
    pub first_bad_got: u32,
    pub first_bad_want: u32,
    pub rounds: u32,
}

#[no_mangle]
pub static mut RESULT: TestResult = TestResult {
    magic: 0,
    stage: 0,
    id: [0; 8],
    size: 0,
    errors: 0,
    first_bad_addr: 0,
    first_bad_got: 0,
    first_bad_want: 0,
    rounds: 0,
};

fn result() -> &'static mut TestResult {
    // SAFETY: single-threaded, no interrupts; only touched from main.
    unsafe { &mut *core::ptr::addr_of_mut!(RESULT) }
}

// ---- raw register access (always inlined so RAM functions stay in RAM) -----

#[inline(always)]
fn rd(addr: usize) -> u32 {
    // SAFETY: fixed, valid RP2350 peripheral or PSRAM address.
    unsafe { read_volatile(addr as *const u32) }
}

#[inline(always)]
fn wr(addr: usize, v: u32) {
    // SAFETY: fixed, valid RP2350 peripheral or PSRAM address.
    unsafe { write_volatile(addr as *mut u32, v) }
}

#[inline(always)]
fn csr_wait(mask: u32, want: u32) {
    while rd(DIRECT_CSR) & mask != want {}
}

/// One direct-mode transfer with CS1 asserted; returns the received byte.
#[inline(always)]
fn csr_xfer(tx: u32) -> u32 {
    wr(DIRECT_TX, tx);
    csr_wait(CSR_TXEMPTY, CSR_TXEMPTY);
    csr_wait(CSR_BUSY, 0);
    rd(DIRECT_RX)
}

#[inline(always)]
fn csr_cmd(cmd: u32) {
    wr(DIRECT_CSR + SET, CSR_ASSERT_CS1N);
    csr_xfer(cmd);
    wr(DIRECT_CSR + CLR, CSR_ASSERT_CS1N);
}

#[inline(always)]
fn direct_mode_on(csr_clkdiv: u32) {
    wr(DIRECT_CSR, csr_clkdiv << CSR_CLKDIV_LSB | CSR_EN);
    csr_wait(CSR_BUSY, 0);
}

/// Leave direct mode and wait until the QMI has actually seen it. Hazard3's
/// instruction fetch can reach the QMI ahead of this store; a flash fetch that
/// lands while direct mode is still enabled is a bus error (an instruction
/// access fault on return to flash code).
#[inline(always)]
fn direct_mode_off() {
    wr(DIRECT_CSR + CLR, CSR_EN | CSR_ASSERT_CS1N);
    while rd(DIRECT_CSR) & (CSR_EN | CSR_BUSY) != 0 {}
    // SAFETY: orders the register writes above before later fetches/accesses.
    unsafe { asm!("fence iorw, iorw") };
}

// The functions below put the QMI into direct mode or change flash timing, so
// they must not fetch from flash: they live in SRAM and call nothing.
// Interrupts are never enabled in this program, so no masking is needed.

#[inline(never)]
#[link_section = ".data.ram_func"]
fn psram_read_id(csr_clkdiv: u32) -> [u8; 8] {
    let mut id = [0u8; 8];
    direct_mode_on(csr_clkdiv);
    csr_cmd(TX_OE | TX_IWIDTH_Q | CMD_EXIT_QPI);
    wr(DIRECT_CSR + SET, CSR_ASSERT_CS1N);
    let mut i = 0;
    while i < 12 {
        let rx = csr_xfer(if i == 0 { CMD_READ_ID } else { 0 });
        if i >= 4 {
            id[i - 4] = rx as u8;
        }
        i += 1;
    }
    direct_mode_off();
    id
}

#[inline(never)]
#[link_section = ".data.ram_func"]
fn psram_qmi_setup(csr_clkdiv: u32, timing: u32) {
    direct_mode_on(csr_clkdiv);
    csr_cmd(CMD_RESET_ENABLE);
    csr_cmd(CMD_RESET);
    csr_cmd(CMD_ENTER_QPI);
    direct_mode_off();
    wr(M1_TIMING, timing);
    wr(M1_RFMT, RFMT);
    wr(M1_RCMD, CMD_QUAD_READ);
    wr(M1_WFMT, WFMT);
    wr(M1_WCMD, CMD_QUAD_WRITE);
    wr(XIP_CTRL + SET, XIP_CTRL_WRITABLE_M1);
    // Read back so every write above has landed before we return to flash.
    rd(M1_WCMD);
    rd(XIP_CTRL);
    // SAFETY: memory ordering barrier only.
    unsafe { asm!("fence iorw, iorw") };
}

/// Flash (M0) timing valid at both 150 and 348 MHz: COOLDOWN=1, RXDELAY=2,
/// MIN_DESELECT=6, CLKDIV=4 (87 MHz at 348; PicoMite's value above 288 MHz).
#[cfg(feature = "overclock")]
#[inline(never)]
#[link_section = ".data.ram_func"]
fn flash_slow_timing() {
    wr(M0_TIMING, 0x4000_6204);
    rd(M0_TIMING);
    for _ in 0..1000 {
        // SAFETY: no-op.
        unsafe { asm!("nop") };
    }
}

/// Port of the Pico SDK's vreg_set_voltage for RP2350 (POWMAN).
#[cfg(feature = "overclock")]
fn vreg_set_voltage(vsel: u32) {
    use powman::*;
    wr(VREG_CTRL + SET, PASSWORD | VREG_CTRL_UNLOCK);
    while rd(VREG) & VREG_UPDATE_IN_PROGRESS != 0 {}
    let cur = rd(VREG) & 0xffff & !VREG_VSEL_BITS;
    wr(VREG, PASSWORD | cur | vsel << VREG_VSEL_LSB);
    while rd(VREG) & VREG_UPDATE_IN_PROGRESS != 0 {}
}

/// Stock clocks: 150 MHz from the HAL's reference setup.
#[cfg(not(feature = "overclock"))]
fn init_clocks(
    xosc: hal::pac::XOSC,
    clocks: hal::pac::CLOCKS,
    pll_sys: hal::pac::PLL_SYS,
    pll_usb: hal::pac::PLL_USB,
    resets: &mut hal::pac::RESETS,
    watchdog: &mut hal::Watchdog,
) -> hal::clocks::ClocksManager {
    hal::clocks::init_clocks_and_plls(XTAL_HZ, xosc, clocks, pll_sys, pll_usb, resets, watchdog)
        .unwrap()
}

/// Overclock: voltage first, then flash timing that is safe at the new
/// clock, then the PLL (same order as the Arm C test and PicoMite).
#[cfg(feature = "overclock")]
fn init_clocks(
    xosc: hal::pac::XOSC,
    clocks: hal::pac::CLOCKS,
    pll_sys: hal::pac::PLL_SYS,
    pll_usb: hal::pac::PLL_USB,
    resets: &mut hal::pac::RESETS,
    watchdog: &mut hal::Watchdog,
) -> hal::clocks::ClocksManager {
    use fugit::RateExtU32;
    use hal::pll::{common_configs::PLL_USB_48MHZ, setup_pll_blocking};
    let xosc = hal::xosc::setup_xosc_blocking(xosc, XTAL_HZ.Hz()).unwrap();
    watchdog.enable_tick_generation((XTAL_HZ / 1_000_000) as u16);
    vreg_set_voltage(VREG_VSEL);
    flash_slow_timing();
    let mut clocks = hal::clocks::ClocksManager::new(clocks);
    let pll_sys =
        setup_pll_blocking(pll_sys, xosc.operating_frequency(), PLL_SYS_OC, &mut clocks, resets)
            .unwrap();
    let pll_usb =
        setup_pll_blocking(pll_usb, xosc.operating_frequency(), PLL_USB_48MHZ, &mut clocks, resets)
            .unwrap();
    clocks.init_default(&xosc, &pll_sys, &pll_usb).unwrap();
    flash_slow_timing();
    clocks
}

// ---- timing helpers --------------------------------------------------------

fn mcycle() -> u32 {
    let c: u32;
    // SAFETY: reading a counter CSR has no side effects.
    unsafe { asm!("csrr {0}, mcycle", out(reg) c) };
    c
}

struct Clock<'a>(&'a hal::Timer<hal::timer::CopyableTimer0>);

impl Clock<'_> {
    fn us(&self) -> u64 {
        self.0.get_counter().ticks()
    }
}

/// Core clock in kHz: mcycle against the 1 MHz timer.
fn measure_core_khz(t: &Clock) -> u32 {
    // SAFETY: clears the cycle-counter inhibit bit (mcountinhibit.CY).
    unsafe { asm!("csrci 0x320, 1") };
    let start = t.us();
    while t.us() == start {}
    let c0 = mcycle();
    let t0 = t.us();
    while t.us() - t0 < 100_000 {}
    mcycle().wrapping_sub(c0) / 100
}

// ---- tests -----------------------------------------------------------------

/// Smallest power of two at which the PSRAM address space wraps.
fn detect_size() -> u32 {
    let mut sz = 1u32 << 20;
    while sz < PSRAM_WINDOW {
        wr(PSRAM_UNCACHED, 0);
        wr(PSRAM_UNCACHED + sz as usize, 0xc0ff_ee42);
        if rd(PSRAM_UNCACHED) == 0xc0ff_ee42 {
            wr(PSRAM_UNCACHED, 0);
            return sz;
        }
        wr(PSRAM_UNCACHED + sz as usize, 0);
        sz <<= 1;
    }
    PSRAM_WINDOW
}

#[inline(always)]
fn pattern(i: u32, seed: u32) -> u32 {
    let mut x = i.wrapping_mul(0x9e37_79b9) ^ seed;
    x ^= x >> 15;
    x.wrapping_mul(0x2c1b_3c6d)
}

/// Write the pattern through `wbase`, verify through `rbase`.
fn memtest(t: &Clock, wbase: usize, rbase: usize, size: u32, seed: u32) {
    let n = size / 4;
    let t0 = t.us();
    for i in 0..n {
        wr(wbase + 4 * i as usize, pattern(i, seed));
    }
    let t1 = t.us();
    let res = result();
    let before = res.errors;
    for i in 0..n {
        let got = rd(rbase + 4 * i as usize);
        let want = pattern(i, seed);
        if got != want {
            if res.errors == 0 {
                res.first_bad_addr = (rbase + 4 * i as usize) as u32;
                res.first_bad_got = got;
                res.first_bad_want = want;
            }
            res.errors += 1;
        }
    }
    let t2 = t.us();
    let kbps = |us: u64| u64::from(size) * 1000 / (us + 1);
    rprintln!(
        "  w {:08x} {} ms ({} KB/s), r {:08x} {} ms ({} KB/s), errors {}",
        wbase,
        (t1 - t0) / 1000,
        kbps(t1 - t0),
        rbase,
        (t2 - t1) / 1000,
        kbps(t2 - t1),
        res.errors - before
    );
}

/// Average ns per random 32-bit load over `range` bytes.
fn random_read_ns(t: &Clock, base: usize, range: u32, count: u32) -> u64 {
    let mask = range / 4 - 1;
    let mut x: u32 = 12345;
    let t0 = t.us();
    for _ in 0..count {
        x = x.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        // Hazard3 is in-order with blocking loads, so these serialise.
        rd(base + 4 * ((x >> 8) & mask) as usize);
    }
    (t.us() - t0) * 1000 / u64::from(count)
}

#[hal::entry]
fn main() -> ! {
    // Block when the buffer is full so nothing is lost before the host attaches.
    rtt_init_print!(rtt_target::ChannelMode::BlockIfFull, 4096);
    let mut pac = hal::pac::Peripherals::take().unwrap();
    let mut watchdog = hal::Watchdog::new(pac.WATCHDOG);

    let clocks = init_clocks(
        pac.XOSC,
        pac.CLOCKS,
        pac.PLL_SYS,
        pac.PLL_USB,
        &mut pac.RESETS,
        &mut watchdog,
    );

    let timer = hal::Timer::new_timer0(pac.TIMER0, &mut pac.RESETS, &clocks);
    wr(TIMER0_DBGPAUSE, 0);
    let t = Clock(&timer);
    let sio = hal::Sio::new(pac.SIO);
    let pins = hal::gpio::Pins::new(pac.IO_BANK0, pac.PADS_BANK0, sio.gpio_bank0, &mut pac.RESETS);

    use hal::Clock as _;
    let sys_hz = clocks.system_clock.freq().to_Hz();
    rprintln!(
        "\n=== psramtest-rs: RP2350 Hazard3 (RISC-V), sys_clk {} Hz{}, PSRAM CS1 on GP0 ===",
        sys_hz,
        if cfg!(feature = "overclock") { " (overclock, vreg 1.20 V)" } else { "" }
    );
    rprintln!("measured core clock: {} kHz (mcycle vs 1 MHz timer)", measure_core_khz(&t));
    let misa: u32;
    // SAFETY: reading misa has no side effects.
    unsafe { asm!("csrr {0}, misa", out(reg) misa) };
    let ext: [u8; 26] = core::array::from_fn(|i| if misa >> i & 1 != 0 { b'A' + i as u8 } else { b'.' });
    rprintln!("misa {:08x}: {}", misa, core::str::from_utf8(&ext).unwrap());

    let res = result();
    res.stage = 1;
    let _cs = pins.gpio0.into_function::<hal::gpio::FunctionXipCs1>();
    let csr_clkdiv = sys_hz.div_ceil(PSRAM_MAX_CSR_CLK);
    let id = psram_read_id(csr_clkdiv);
    res.id = id;
    rprintln!(
        "ID: mfid {:02x} kgd {:02x} eid {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
        id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]
    );
    if id[1] != KGD_PASS {
        rprintln!("FAIL: no PSRAM (KGD {:02x}, want {:02x})", id[1], KGD_PASS);
        res.magic = 0x5452_5350;
        loop {
            hal::arch::wfi();
        }
    }

    // AP Memory APS6404L: 109 MHz max, tCEM 8 us, tCPH 18 ns.
    res.stage = 2;
    let period_fs = 1_000_000_000_000_000u64 / u64::from(sys_hz);
    let clkdiv = PSRAM_CLKDIV.unwrap_or(sys_hz.div_ceil(109_000_000));
    let max_select = ((8_000_000_000u64 >> 6) / period_fs) as u32;
    let min_deselect = 18_000_000u64.div_ceil(period_fs) as u32;
    let rxdelay = 1 + if sys_hz > 150_000_000 { clkdiv } else { 1 };
    let timing = 1 << 30 | 2 << 28 | 3 << 23 | max_select << 17 | min_deselect << 12 | rxdelay << 8 | clkdiv;
    psram_qmi_setup(csr_clkdiv, timing);
    rprintln!(
        "QMI M1: timing {:08x} (clkdiv {} = {} MHz SCK, max_select {}, min_deselect {}, rxdelay {})",
        rd(M1_TIMING),
        clkdiv,
        sys_hz / clkdiv / 1_000_000,
        max_select,
        min_deselect,
        rxdelay
    );

    res.stage = 3;
    let size = detect_size();
    res.size = size;
    rprintln!("detected size: {} KB", size >> 10);

    res.stage = 4;
    let soak_t0 = t.us();
    let mut round = 0u32;
    while t.us() - soak_t0 < SOAK_S * 1_000_000 {
        rprintln!(
            "-- round {}, t={} s, errors so far {}",
            round + 1,
            (t.us() - soak_t0) / 1_000_000,
            res.errors
        );
        memtest(&t, PSRAM_UNCACHED, PSRAM_UNCACHED, size, 0x0123_4567 + round);
        memtest(&t, PSRAM_UNCACHED, PSRAM_CACHED, size, 0x89ab_cdef - round);
        memtest(&t, PSRAM_CACHED, PSRAM_CACHED, size, 0x5a5a_5a5a ^ round);
        round += 1;
        res.rounds = round;
    }
    let elapsed_s = (t.us() - soak_t0) / 1_000_000;

    res.stage = 5;
    rprintln!("random 32-bit loads:");
    rprintln!("  uncached, 8 MB range: {} ns", random_read_ns(&t, PSRAM_UNCACHED, size, 200_000));
    rprintln!("  cached,   8 MB range: {} ns", random_read_ns(&t, PSRAM_CACHED, size, 200_000));
    rprintln!("  cached,   4 KB range: {} ns", random_read_ns(&t, PSRAM_CACHED, 4096, 200_000));

    rprintln!(
        "RESULT: {}, {} MB, {} errors, {} rounds ({} MB verified) in {} s",
        if res.errors == 0 { "PASS" } else { "FAIL" },
        size >> 20,
        res.errors,
        round,
        round * 3 * (size >> 20),
        elapsed_s
    );
    if res.errors != 0 {
        rprintln!(
            "  first bad @{:08x} got {:08x} want {:08x}",
            res.first_bad_addr,
            res.first_bad_got,
            res.first_bad_want
        );
    }
    rprintln!("measured core clock at end: {} kHz", measure_core_khz(&t));
    res.magic = 0x5452_5350;
    loop {
        hal::arch::wfi();
    }
}

#[link_section = ".bi_entries"]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_cargo_bin_name!(),
    hal::binary_info::rp_program_description!(c"PSRAM soak test (RISC-V)"),
];
