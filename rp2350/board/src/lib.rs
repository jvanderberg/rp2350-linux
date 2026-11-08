//! RP2350B board support shared by the bring-up tests and the emulator.
//!
//! - Clocks: stock 150 MHz, or (feature `overclock`, default) 348 MHz at
//!   1.20 V: voltage first, then flash timing that is safe at both clocks,
//!   then the PLL (same order as the Arm C test and PicoMite).
//! - PSRAM on QMI CS1 = GP0 (AP Memory APS6404L). The sequence follows
//!   PicoMite/FanPico psram.c (Timo Kokkonen): exit QPI, read ID, reset,
//!   enter QPI, program M1 timing/format.
//! - Hazard3 counters, and TIMER0.DBGPAUSE, which otherwise stops the timer
//!   whenever a debugger halts a core.
#![no_std]

use core::arch::asm;
use core::ptr::{read_volatile, write_volatile};

use rp235x_hal as hal;

pub const XTAL_HZ: u32 = 12_000_000;

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

pub const PSRAM_CACHED: usize = 0x1100_0000;
pub const PSRAM_UNCACHED: usize = 0x1500_0000;
const PSRAM_WINDOW: u32 = 16 << 20;
const PSRAM_MAX_CSR_CLK: u32 = 5_000_000;

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

/// Bring up the clocks (see the crate docs) and return the clock manager.
#[cfg(not(feature = "overclock"))]
pub fn init_clocks(
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

/// Bring up the clocks (see the crate docs) and return the clock manager.
#[cfg(feature = "overclock")]
pub fn init_clocks(
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

/// Human-readable clock configuration for banners.
pub const CLOCK_NOTE: &str = if cfg!(feature = "overclock") { " (overclock, vreg 1.20 V)" } else { "" };

/// Keep TIMER0 running while a debugger halts a core (DBGPAUSE resets to 7).
pub fn timer_ignore_debug_halt() {
    wr(TIMER0_DBGPAUSE, 0);
}

/// Let mcycle / minstret count (clear mcountinhibit.CY and .IR).
pub fn enable_counters() {
    // SAFETY: only clears counter-inhibit bits.
    unsafe { asm!("csrci 0x320, 5") };
}

/// Core cycle counter (64-bit).
pub fn mcycle64() -> u64 {
    loop {
        let (hi, lo, hi2): (u32, u32, u32);
        // SAFETY: reading counter CSRs has no side effects.
        unsafe { asm!("csrr {0}, mcycleh", "csrr {1}, mcycle", "csrr {2}, mcycleh", out(reg) hi, out(reg) lo, out(reg) hi2) };
        if hi == hi2 {
            return (u64::from(hi) << 32) | u64::from(lo);
        }
    }
}

/// Retired host instructions (64-bit).
pub fn minstret64() -> u64 {
    loop {
        let (hi, lo, hi2): (u32, u32, u32);
        // SAFETY: reading counter CSRs has no side effects.
        unsafe { asm!("csrr {0}, minstreth", "csrr {1}, minstret", "csrr {2}, minstreth", out(reg) hi, out(reg) lo, out(reg) hi2) };
        if hi == hi2 {
            return (u64::from(hi) << 32) | u64::from(lo);
        }
    }
}

pub type Gpio0Default = hal::gpio::Pin<
    hal::gpio::bank0::Gpio0,
    <hal::gpio::bank0::Gpio0 as hal::gpio::DefaultTypeState>::Function,
    <hal::gpio::bank0::Gpio0 as hal::gpio::DefaultTypeState>::PullType,
>;

#[derive(Clone, Copy, Debug)]
pub struct Psram {
    /// mfid, kgd, eid[6]
    pub id: [u8; 8],
    /// Detected size in bytes.
    pub size: u32,
    pub clkdiv: u32,
    pub sck_hz: u32,
    /// M1_TIMING as programmed.
    pub timing: u32,
}

#[derive(Clone, Copy, Debug)]
pub enum PsramError {
    /// No chip answered (known-good-die byte wrong); the raw ID is attached.
    NotFound([u8; 8]),
}

/// Bring up the PSRAM on QMI CS1 (GP0) and map it writable at
/// `PSRAM_CACHED` / `PSRAM_UNCACHED`.
pub fn psram_init(cs: Gpio0Default, sys_hz: u32) -> Result<Psram, PsramError> {
    let _cs = cs.into_function::<hal::gpio::FunctionXipCs1>();
    let csr_clkdiv = sys_hz.div_ceil(PSRAM_MAX_CSR_CLK);
    let id = psram_read_id(csr_clkdiv);
    if id[1] != KGD_PASS {
        return Err(PsramError::NotFound(id));
    }
    // AP Memory APS6404L: 109 MHz max, tCEM 8 us, tCPH 18 ns.
    let period_fs = 1_000_000_000_000_000u64 / u64::from(sys_hz);
    let clkdiv = PSRAM_CLKDIV.unwrap_or(sys_hz.div_ceil(109_000_000));
    let max_select = ((8_000_000_000u64 >> 6) / period_fs) as u32;
    let min_deselect = 18_000_000u64.div_ceil(period_fs) as u32;
    let rxdelay = 1 + if sys_hz > 150_000_000 { clkdiv } else { 1 };
    let timing = 1 << 30 | 2 << 28 | 3 << 23 | max_select << 17 | min_deselect << 12 | rxdelay << 8 | clkdiv;
    psram_qmi_setup(csr_clkdiv, timing);
    Ok(Psram { id, size: detect_size(), clkdiv, sck_hz: sys_hz / clkdiv, timing: rd(M1_TIMING) })
}

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

// ---- USB BOOTSEL -------------------------------------------------------------

/// Watchdog SCRATCH0 (kept across a debugger reset; the bootrom's reboot
/// handling uses SCRATCH4..7).
const WATCHDOG_SCRATCH0: usize = 0x400d_800c;
/// A debugger writes this to SCRATCH0 and resets the chip to ask the
/// firmware for USB BOOTSEL (see `bootsel_if_requested`).
pub const BOOTSEL_MAGIC: u32 = 0xb007_5e1c;

/// Reboot into the bootrom's USB BOOTSEL mode (picotool / mass storage).
pub fn reboot_to_bootsel() -> ! {
    hal::reboot::reboot(
        hal::reboot::RebootKind::BootSel { picoboot_disabled: false, msd_disabled: false },
        hal::reboot::RebootArch::Riscv,
    )
}

/// Go to BOOTSEL if a debugger asked for it via `BOOTSEL_MAGIC`.
pub fn bootsel_if_requested() {
    // SAFETY: watchdog scratch register.
    unsafe {
        if read_volatile(WATCHDOG_SCRATCH0 as *const u32) == BOOTSEL_MAGIC {
            write_volatile(WATCHDOG_SCRATCH0 as *mut u32, 0);
            reboot_to_bootsel();
        }
    }
}
