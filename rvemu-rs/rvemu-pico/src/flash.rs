//! Flash erase and program at run time, for everything in the firmware that
//! writes flash: the guest's /home storage (`flashdev`) and swap.
//!
//! The firmware and the guest's kernel text run from flash (XIP), and the
//! PSRAM sits on the same QMI. While the flash is erasing or programming, no
//! XIP access to either chip select may happen, so every operation:
//! - refuses any range outside `REGIONS`: never the firmware, the Wi-Fi
//!   sector at 0x103FF000, the guest blob, the squashfs or the kernel text;
//! - parks core 1 in SRAM (`park_core1`, where core 1's guard plugs in);
//! - masks interrupts and runs from SRAM: it saves the QMI window settings
//!   (the board's 348 MHz flash timing, the PSRAM's QPI setup on CS1), the
//!   QSPI pads and XIP_CTRL, calls the bootrom's `connect_internal_flash`,
//!   `flash_exit_xip` and erase/program, then restores all of them;
//! - invalidates only the XIP cache lines of the range it changed. The rest
//!   of the cache, including guest RAM's dirty PSRAM lines, stays as it was
//!   (the bootrom's `flash_flush_cache` would discard those).
//!
//! One critical section erases one 4 KB sector (45 ms typical, 400 ms at
//! most) or programs one 256-byte page (about 0.7 ms), so the longest stall
//! is one sector erase. Callers batch work and keep erases rare.
//!
//! Addresses are XIP addresses (0x10xxxxxx). Sources may be anywhere,
//! PSRAM included: each page is copied into an SRAM buffer before XIP stops.

use core::arch::asm;
use core::ptr::{read_volatile, write_volatile};

use rp2350_board as board;
use rp235x_hal::rom_data;

pub const SECTOR: u32 = 4096;
pub const PAGE: u32 = 256;
pub const XIP_BASE: u32 = 0x1000_0000;
/// Flash's uncached, non-allocating XIP alias: reads through it neither hit
/// nor fill the XIP cache that guest RAM uses.
pub const XIP_NOCACHE_NOALLOC: u32 = 0x1400_0000;

/// A writable flash range, [start, end), as XIP addresses.
#[derive(Clone, Copy)]
pub struct Region {
    pub start: u32,
    pub end: u32,
}

/// The guest's /home (the storage thread's flashdev): 1 MB.
pub const STORAGE: Region = Region { start: 0x1010_0000, end: 0x1020_0000 };
/// Swap: up to the Wi-Fi settings sector at 0x103FF000, which stays untouched.
pub const SWAP: Region = Region { start: 0x1020_0000, end: 0x103F_F000 };
/// Everything this module will ever erase or program.
const REGIONS: [Region; 2] = [STORAGE, SWAP];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// Outside `REGIONS`, or crossing out of one.
    Range,
    /// An erase not on a sector boundary.
    Align,
    /// Core 1 could not be parked.
    Core1,
    /// XIP is in a mode the restore can't handle (continuous read, no
    /// command prefix); set up by something other than the bootrom.
    XipMode,
}

/// Counters for measuring stalls (cycles at the system clock).
#[derive(Clone, Copy, Default)]
pub struct Stats {
    pub erases: u32,
    pub pages: u32,
    /// Erases skipped because the sector was already blank.
    pub erases_skipped: u32,
    pub stall_cycles_max: u32,
    pub stall_cycles_total: u64,
}

static mut STATS: Stats =
    Stats { erases: 0, pages: 0, erases_skipped: 0, stall_cycles_max: 0, stall_cycles_total: 0 };

pub fn stats() -> Stats {
    // SAFETY: single-core use from core 0; a plain copy.
    unsafe { core::ptr::addr_of!(STATS).read() }
}

fn in_regions(addr: u32, len: u32) -> bool {
    let Some(end) = addr.checked_add(len) else { return false };
    REGIONS.iter().any(|r| addr >= r.start && end <= r.end)
}

/// Erase the 4 KB sector at `addr`. A sector that is already blank is left
/// alone (no erase cycle spent, no stall).
pub fn erase_sector(addr: u32) -> Result<(), Error> {
    if !addr.is_multiple_of(SECTOR) {
        return Err(Error::Align);
    }
    if !in_regions(addr, SECTOR) {
        return Err(Error::Range);
    }
    if is_erased(addr, SECTOR) {
        // SAFETY: core 0 only.
        unsafe { (*core::ptr::addr_of_mut!(STATS)).erases_skipped += 1 };
        return Ok(());
    }
    run(Op::Erase, addr, core::ptr::null())?;
    // SAFETY: core 0 only.
    unsafe { (*core::ptr::addr_of_mut!(STATS)).erases += 1 };
    Ok(())
}

/// Program `data` at `addr`, any alignment and length inside one region.
/// NOR semantics: bits only go from 1 to 0, so the result is the AND of the
/// old contents and `data`. Pages whose bytes are all 0xFF are skipped.
pub fn program(addr: u32, data: &[u8]) -> Result<(), Error> {
    let len = u32::try_from(data.len()).map_err(|_| Error::Range)?;
    if !in_regions(addr, len) {
        return Err(Error::Range);
    }
    let mut done = 0u32;
    while done < len {
        let a = addr + done;
        let page = a & !(PAGE - 1);
        let off = a - page;
        let n = (PAGE - off).min(len - done);
        let mut buf = [0xFFu8; PAGE as usize];
        let src = &data[done as usize..(done + n) as usize];
        buf[off as usize..(off + n) as usize].copy_from_slice(src);
        if src.iter().any(|&b| b != 0xFF) {
            program_page(page, &buf)?;
        }
        done += n;
    }
    Ok(())
}

/// Program one whole 256-byte page at `addr` (page aligned) from `page`,
/// which must be in SRAM.
pub fn program_page(addr: u32, page: &[u8; PAGE as usize]) -> Result<(), Error> {
    if !addr.is_multiple_of(PAGE) {
        return Err(Error::Align);
    }
    if !in_regions(addr, PAGE) {
        return Err(Error::Range);
    }
    run(Op::Program, addr, page.as_ptr())?;
    // SAFETY: core 0 only.
    unsafe { (*core::ptr::addr_of_mut!(STATS)).pages += 1 };
    Ok(())
}

/// Read flash at `addr` (anywhere in flash) through the uncached alias.
pub fn read(addr: u32, buf: &mut [u8]) {
    let src = (addr - XIP_BASE + XIP_NOCACHE_NOALLOC) as *const u8;
    // SAFETY: flash is mapped read-only at both aliases; callers pass flash addresses.
    unsafe { core::ptr::copy_nonoverlapping(src, buf.as_mut_ptr(), buf.len()) };
}

/// Whether `len` bytes at `addr` (word aligned) are all 0xFF.
pub fn is_erased(addr: u32, len: u32) -> bool {
    let p = (addr - XIP_BASE + XIP_NOCACHE_NOALLOC) as *const u32;
    // SAFETY: as in `read`.
    (0..len as usize / 4).all(|i| unsafe { p.add(i).read_volatile() } == u32::MAX)
}

// ------------------------------------------------------------------ core 1

/// Keep core 1 off XIP for one operation: true once it spins in SRAM.
/// Core 1 doesn't run flash code yet; the core 1 service's `core1::park()`
/// guard goes here (and its release in `unpark_core1`).
fn park_core1() -> bool {
    true
}

fn unpark_core1() {}

/// System clock, for converting `Stats` cycles; set by the firmware at start.
static mut SYS_HZ: u32 = 150_000_000;

pub fn set_sys_hz(hz: u32) {
    // SAFETY: core 0, at start-up.
    unsafe { *core::ptr::addr_of_mut!(SYS_HZ) = hz };
}

pub fn sys_hz() -> u32 {
    // SAFETY: a plain read; written once at start.
    unsafe { core::ptr::addr_of!(SYS_HZ).read() }
}

// ------------------------------------------------------------------ the critical section

#[derive(Clone, Copy, PartialEq, Eq)]
enum Op {
    Erase,
    Program,
}

const QMI: u32 = 0x400d_0000;
const QMI_DIRECT_CSR: u32 = QMI;
/// M0_TIMING .. M1_WCMD: five registers per window.
const QMI_M0: u32 = QMI + 0x0c;
const QMI_M0_RFMT: u32 = QMI + 0x10;
const QMI_WINDOW_REGS: usize = 10;
const RFMT_PREFIX_LEN: u32 = 1 << 12;
const DIRECT_EN: u32 = 1 << 0;
const DIRECT_BUSY: u32 = 1 << 1;
const DIRECT_ASSERT_CS0N: u32 = 1 << 2;
const DIRECT_ASSERT_CS1N: u32 = 1 << 3;
const XIP_CTRL: u32 = 0x400c_8000;
/// PADS_QSPI GPIO_QSPI_SCLK .. GPIO_QSPI_SS.
const PADS_QSPI_IO: u32 = 0x4004_0004;
const PADS_QSPI_N: usize = 6;
const XIP_MAINTENANCE: u32 = 0x1800_0000;
const XIP_INVALIDATE_BY_ADDRESS: u32 = 2;
const XIP_LINE: u32 = 8;
const REG_CLR: u32 = 0x3000;
/// As the Pico SDK passes them; a one-sector count always gets the 4 KB
/// sector erase (20h).
const BLOCK64: u32 = 1 << 16;
const CMD_BLOCK_ERASE: u8 = 0xd8;

/// Bootrom entry points, looked up before XIP stops (the lookup runs from flash).
struct Rom {
    connect: unsafe extern "C" fn(),
    exit_xip: unsafe extern "C" fn(),
    erase: unsafe extern "C" fn(u32, usize, u32, u8),
    program: unsafe extern "C" fn(u32, *const u8, usize),
}

#[inline(always)]
fn rd(a: u32) -> u32 {
    // SAFETY: a fixed RP2350 register address.
    unsafe { read_volatile(a as *const u32) }
}

#[inline(always)]
fn wr(a: u32, v: u32) {
    // SAFETY: a fixed RP2350 register address.
    unsafe { write_volatile(a as *mut u32, v) }
}

/// Mask machine interrupts; returns whether they were enabled.
#[inline(always)]
fn irq_off() -> bool {
    let m: u32;
    // SAFETY: clears mstatus.MIE only.
    unsafe { asm!("csrrci {0}, mstatus, 8", out(reg) m) };
    m & 8 != 0
}

#[inline(always)]
fn irq_restore(was_on: bool) {
    if was_on {
        // SAFETY: sets mstatus.MIE only.
        unsafe { asm!("csrsi mstatus, 8") };
    }
}

fn run(op: Op, addr: u32, page: *const u8) -> Result<(), Error> {
    if rd(QMI_M0_RFMT) & (3 * RFMT_PREFIX_LEN) == 0 {
        return Err(Error::XipMode);
    }
    let rom = Rom {
        connect: rom_data::connect_internal_flash::ptr(),
        exit_xip: rom_data::flash_exit_xip::ptr(),
        erase: rom_data::flash_range_erase::ptr(),
        program: rom_data::flash_range_program::ptr(),
    };
    if !park_core1() {
        return Err(Error::Core1);
    }
    let mie = irq_off();
    let t0 = board::mcycle64();
    // SAFETY: `in_ram` touches flash only through the bootrom; `page` is in SRAM.
    unsafe { in_ram(&rom, op, addr - XIP_BASE, page) };
    let dt = u32::try_from(board::mcycle64() - t0).unwrap_or(u32::MAX);
    irq_restore(mie);
    unpark_core1();
    // SAFETY: core 0 only.
    unsafe {
        let s = &mut *core::ptr::addr_of_mut!(STATS);
        s.stall_cycles_max = s.stall_cycles_max.max(dt);
        s.stall_cycles_total += u64::from(dt);
    }
    Ok(())
}

/// The part that runs with XIP stopped: SRAM code calling only the bootrom.
/// # Safety
/// Interrupts masked, core 1 parked, `page` (for a program) in SRAM.
#[inline(never)]
#[unsafe(link_section = ".data.ram_func")]
unsafe fn in_ram(rom: &Rom, op: Op, offs: u32, page: *const u8) {
    let mut qmi = [0u32; QMI_WINDOW_REGS];
    let mut pads = [0u32; PADS_QSPI_N];
    let mut i = 0;
    while i < QMI_WINDOW_REGS {
        qmi[i] = rd(QMI_M0 + 4 * i as u32);
        i += 1;
    }
    i = 0;
    while i < PADS_QSPI_N {
        pads[i] = rd(PADS_QSPI_IO + 4 * i as u32);
        i += 1;
    }
    let xip_ctrl = rd(XIP_CTRL);

    // SAFETY: bootrom code (not XIP); the caller's guarantees.
    unsafe {
        (rom.connect)();
        (rom.exit_xip)();
        match op {
            Op::Erase => (rom.erase)(offs, SECTOR as usize, BLOCK64, CMD_BLOCK_ERASE),
            Op::Program => (rom.program)(offs, page, PAGE as usize),
        }
    }

    // out of direct mode, then the saved windows, pads and XIP_CTRL
    wr(QMI_DIRECT_CSR + REG_CLR, DIRECT_EN | DIRECT_ASSERT_CS0N | DIRECT_ASSERT_CS1N);
    while rd(QMI_DIRECT_CSR) & (DIRECT_EN | DIRECT_BUSY) != 0 {}
    i = 0;
    while i < QMI_WINDOW_REGS {
        wr(QMI_M0 + 4 * i as u32, qmi[i]);
        i += 1;
    }
    i = 0;
    while i < PADS_QSPI_N {
        wr(PADS_QSPI_IO + 4 * i as u32, pads[i]);
        i += 1;
    }
    wr(XIP_CTRL, xip_ctrl);
    // drop stale cached copies of the changed range (cached flash alias)
    let len = if op == Op::Erase { SECTOR } else { PAGE };
    let mut a = offs;
    while a < offs + len {
        // SAFETY: XIP cache maintenance space; a write per line.
        unsafe { write_volatile((XIP_MAINTENANCE + a + XIP_INVALIDATE_BY_ADDRESS) as *mut u8, 0) };
        a += XIP_LINE;
    }
    // read back so every write has landed before we fetch from flash again
    rd(QMI_M0 + 4 * (QMI_WINDOW_REGS as u32 - 1));
    rd(XIP_CTRL);
    // SAFETY: ordering barrier only.
    unsafe { asm!("fence iorw, iorw") };
}
