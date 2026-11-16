//! The SD card on SPI1 (GP8 MISO, pulled up; GP9 chip select, a GPIO; GP10
//! SCK; GP11 MOSI) as `rvcore::sdcard`'s bus. The HAL sets the controller
//! up and sets its clock; the transfers drive the PL022's registers
//! directly, keeping its 8-entry FIFOs full so the clock runs without gaps
//! between bytes (the HAL's transfer waits for each byte to come back before
//! it sends the next).

use embedded_hal::digital::OutputPin as _;
use hal::fugit::RateExtU32;
use rp235x_hal as hal;

use crate::pins::{Spi1, Spi1Cs};

/// SPI1's registers.
const SSPDR: *mut u32 = 0x4008_8008 as *mut u32;
const SSPSR: *const u32 = 0x4008_800C as *const u32;
const SR_TNF: u32 = 1 << 1;
const SR_RNE: u32 = 1 << 2;
const SR_BSY: u32 = 1 << 4;
/// The FIFOs' depth: bytes in flight at most, so the RX FIFO can't overrun.
const FIFO: usize = 8;
/// The SIO's shared RISC-V timer (system clock cycles; both cores see it).
const SIO_MTIME: *const u32 = 0xd000_01b0 as *const u32;
const SIO_MTIMEH: *const u32 = 0xd000_01b4 as *const u32;

pub struct BoardSd {
    _spi: Spi1,
    cs: Spi1Cs,
    peri_hz: u32,
    sys_mhz: u64,
}

impl BoardSd {
    pub fn new(spi: Spi1, mut cs: Spi1Cs, peri_hz: u32, sys_hz: u32) -> Self {
        let _ = cs.set_high();
        BoardSd { _spi: spi, cs, peri_hz, sys_mhz: u64::from(sys_hz / 1_000_000).max(1) }
    }

    /// Clock `n` bytes: the `i`th sent is `tx(i)`, and what comes back
    /// goes to `rx(i, byte)`.
    #[inline(always)]
    fn run(n: usize, tx: impl Fn(usize) -> u8, mut rx: impl FnMut(usize, u8)) {
        // SAFETY: SPI1's data and status registers; SPI1 is ours (`_spi`).
        unsafe {
            // anything left from before
            while SSPSR.read_volatile() & SR_RNE != 0 {
                let _ = SSPDR.read_volatile();
            }
            let (mut sent, mut got) = (0, 0);
            while got < n {
                let sr = SSPSR.read_volatile();
                if sent < n && sent - got < FIFO && sr & SR_TNF != 0 {
                    SSPDR.write_volatile(u32::from(tx(sent)));
                    sent += 1;
                }
                if sr & SR_RNE != 0 {
                    rx(got, SSPDR.read_volatile() as u8);
                    got += 1;
                }
            }
            while SSPSR.read_volatile() & SR_BSY != 0 {}
        }
    }
}

impl rvcore::sdcard::SdBus for BoardSd {
    fn set_hz(&mut self, hz: u32) {
        self._spi.set_baudrate(self.peri_hz.Hz(), hz.Hz());
    }
    fn select(&mut self, on: bool) {
        let _ = if on { self.cs.set_low() } else { self.cs.set_high() };
    }
    fn transfer(&mut self, buf: &mut [u8]) {
        let p = buf.as_mut_ptr();
        // SAFETY: `i` < buf.len(); a byte is read before it is overwritten
        // (sent runs ahead of got).
        Self::run(buf.len(), |i| unsafe { p.add(i).read() }, |i, b| unsafe { p.add(i).write(b) });
    }
    fn send(&mut self, tx: &[u8]) {
        Self::run(tx.len(), |i| tx[i], |_, _| {});
    }
    fn recv(&mut self, rx: &mut [u8]) {
        Self::run(rx.len(), |_| 0xFF, |i, b| rx[i] = b);
    }
    fn now_us(&mut self) -> u64 {
        let t = loop {
            // SAFETY: the SIO timer's registers.
            let (hi, lo, hi2) = unsafe { (SIO_MTIMEH.read_volatile(), SIO_MTIME.read_volatile(), SIO_MTIMEH.read_volatile()) };
            if hi == hi2 {
                break (u64::from(hi) << 32) | u64::from(lo);
            }
        };
        t / self.sys_mhz
    }
}
