//! The swap region of the board's flash (0x10200000-0x103FEFFF) as a
//! `swapdev::Flash`: erase and program through the QMI's direct mode, read
//! through the uncached XIP alias.
//!
//! A stand-in until the storage thread's shared flash primitive lands; only
//! this region can be erased or written, never the Wi-Fi sector after it.
//!
//! - **Why direct mode works.** Every XIP read mode the bootrom sets up keeps
//!   an 8-bit serial command prefix, so the flash stays in a serial command
//!   state and accepts 06h/20h/02h/05h at any time.
//! - **Everything stalls.** While the QMI is in direct mode, or the flash is
//!   busy, nothing can fetch from flash or touch PSRAM: the firmware and the
//!   kernel text run from flash and guest RAM is PSRAM. The command code runs
//!   from SRAM, calls nothing, and polls the flash's status until it is done.
//! - **No cache maintenance.** Reads use the uncached alias, so the XIP cache
//!   never holds lines of this region. Invalidating the whole cache would
//!   drop dirty guest RAM lines.

use core::arch::asm;
use core::ptr::{read_volatile, write_volatile};

use rp2350_board as board;
use rvcore::swapdev::{CHUNK, Flash, PAGE};

/// Region, as offsets in flash.
pub const SWAP_START: u32 = 0x0020_0000;
pub const SWAP_END: u32 = 0x003F_F000;
/// Uncached, non-allocating XIP alias of flash.
const XIP_NOCACHE_NOALLOC: usize = 0x1400_0000;

const QMI: usize = 0x400d_0000;
const DIRECT_CSR: usize = QMI;
const DIRECT_TX: usize = QMI + 0x04;
const DIRECT_RX: usize = QMI + 0x08;
const SET: usize = 0x2000;
const CLR: usize = 0x3000;
const CSR_EN: u32 = 1 << 0;
const CSR_BUSY: u32 = 1 << 1;
const CSR_ASSERT_CS0N: u32 = 1 << 2;
const CSR_TXFULL: u32 = 1 << 10;
const CSR_TXEMPTY: u32 = 1 << 11;
const CSR_RXEMPTY: u32 = 1 << 16;
const CSR_CLKDIV_LSB: u32 = 22;
const TX_NOPUSH: u32 = 1 << 20;

const CMD_WRITE_ENABLE: u32 = 0x06;
const CMD_SECTOR_ERASE: u32 = 0x20;
const CMD_PAGE_PROGRAM: u32 = 0x02;
const CMD_READ_STATUS: u32 = 0x05;
const STATUS_BUSY: u32 = 1;

pub struct SwapFlash {
    /// Direct-mode clock divider: SCK at most 30 MHz.
    clkdiv: u32,
    /// Give up on an operation after this many cycles.
    timeout: u64,
    hz: u32,
}

impl SwapFlash {
    pub fn new(sys_hz: u32) -> Self {
        SwapFlash { clkdiv: sys_hz.div_ceil(30_000_000), timeout: u64::from(sys_hz), hz: sys_hz }
    }
}

#[inline(always)]
fn rd(addr: usize) -> u32 {
    // SAFETY: a QMI register.
    unsafe { read_volatile(addr as *const u32) }
}

#[inline(always)]
fn wr(addr: usize, v: u32) {
    // SAFETY: a QMI register.
    unsafe { write_volatile(addr as *mut u32, v) }
}

#[inline(always)]
fn mcycle() -> u32 {
    let c: u32;
    // SAFETY: reading the cycle counter.
    unsafe { asm!("csrr {0}, mcycle", out(reg) c) };
    c
}

// No arrays of constants below: those can live in .rodata, in flash.

#[inline(always)]
fn cs(low: bool) {
    wr(DIRECT_CSR + if low { SET } else { CLR }, CSR_ASSERT_CS0N);
}

/// Shift one byte out (and one in); returns the byte received.
#[inline(always)]
fn byte(b: u32) -> u32 {
    wr(DIRECT_TX, b);
    while rd(DIRECT_CSR) & CSR_TXEMPTY == 0 {}
    while rd(DIRECT_CSR) & CSR_BUSY != 0 {}
    rd(DIRECT_RX)
}

/// Queue one byte without keeping what comes back.
#[inline(always)]
fn put(b: u32) {
    while rd(DIRECT_CSR) & CSR_TXFULL != 0 {}
    wr(DIRECT_TX, b | TX_NOPUSH);
}

#[inline(always)]
fn put_done() {
    while rd(DIRECT_CSR) & CSR_TXEMPTY == 0 {}
    while rd(DIRECT_CSR) & CSR_BUSY != 0 {}
}

/// Wait for the flash to finish (status BUSY clear); false on timeout.
#[inline(always)]
fn wait_ready(timeout: u32) -> bool {
    let t0 = mcycle();
    loop {
        cs(true);
        byte(CMD_READ_STATUS);
        let st = byte(0);
        cs(false);
        if st & STATUS_BUSY == 0 {
            return true;
        }
        if mcycle().wrapping_sub(t0) > timeout {
            return false;
        }
    }
}

/// One erase (`data` null) or 256-byte program operation at flash offset
/// `addr`, in direct mode from SRAM. Calls nothing outside SRAM.
#[inline(never)]
#[unsafe(link_section = ".data.ram_func")]
fn flash_op(clkdiv: u32, addr: u32, data: *const u8, timeout: u32) -> bool {
    wr(DIRECT_CSR, clkdiv << CSR_CLKDIV_LSB | CSR_EN);
    while rd(DIRECT_CSR) & CSR_BUSY != 0 {}
    cs(true);
    put(CMD_WRITE_ENABLE);
    put_done();
    cs(false);
    cs(true);
    put(if data.is_null() { CMD_SECTOR_ERASE } else { CMD_PAGE_PROGRAM });
    put(addr >> 16 & 0xFF);
    put(addr >> 8 & 0xFF);
    put(addr & 0xFF);
    if !data.is_null() {
        let mut i = 0;
        while i < CHUNK {
            // SAFETY: the caller passes CHUNK readable bytes in SRAM; a
            // volatile read keeps this a plain loop (no memcpy into flash).
            put(u32::from(unsafe { read_volatile(data.add(i)) }));
            i += 1;
        }
    }
    put_done();
    cs(false);
    let ok = wait_ready(timeout);
    while rd(DIRECT_CSR) & CSR_RXEMPTY == 0 {
        rd(DIRECT_RX);
    }
    wr(DIRECT_CSR + CLR, CSR_EN | CSR_ASSERT_CS0N);
    while rd(DIRECT_CSR) & (CSR_EN | CSR_BUSY) != 0 {}
    // SAFETY: orders the register writes before the next flash fetch.
    unsafe { asm!("fence iorw, iorw") };
    ok
}

impl Flash for SwapFlash {
    fn sectors(&self) -> u32 {
        (SWAP_END - SWAP_START) / PAGE as u32
    }
    fn erase(&mut self, sector: u32) -> bool {
        let addr = SWAP_START + sector * PAGE as u32;
        if addr >= SWAP_END {
            return false;
        }
        // erases take up to 400 ms (datasheet maximum)
        flash_op(self.clkdiv, addr, core::ptr::null(), self.timeout.min(u64::from(u32::MAX)) as u32)
    }
    fn program(&mut self, off: u32, data: &[u8; CHUNK]) -> bool {
        let addr = SWAP_START + off;
        if off % CHUNK as u32 != 0 || addr + CHUNK as u32 > SWAP_END {
            return false;
        }
        // `data` may be in PSRAM: copy it to the stack (SRAM) first
        let buf = *data;
        flash_op(self.clkdiv, addr, buf.as_ptr(), (self.timeout / 100) as u32)
    }
    fn read(&self, off: u32, buf: &mut [u8]) {
        let addr = SWAP_START + off;
        if addr as usize + buf.len() > SWAP_END as usize {
            buf.fill(0);
            return;
        }
        let src = XIP_NOCACHE_NOALLOC + addr as usize;
        // SAFETY: mapped flash inside the swap region (checked above).
        unsafe { core::ptr::copy_nonoverlapping(src as *const u8, buf.as_mut_ptr(), buf.len()) };
    }
    fn cycles(&self) -> u64 {
        board::mcycle64()
    }
    fn hz(&self) -> u32 {
        self.hz
    }
}
