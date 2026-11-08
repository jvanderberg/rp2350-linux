//! The swap region of the board's flash (`flash::SWAP`, 0x10200000-0x103FEFFF)
//! as a `swapdev::Flash`, on the shared flash primitive in `flash.rs`: erase
//! and program run from SRAM with core 1 parked and only the touched XIP
//! lines invalidated; reads go through the uncached XIP alias. Only the
//! storage and swap regions can be written, never the Wi-Fi sector after
//! them.

use rp2350_board as board;
use rvcore::swapdev::{CHUNK, Flash, PAGE};

use crate::flash;

pub struct SwapFlash {
    hz: u32,
}

impl SwapFlash {
    pub fn new(sys_hz: u32) -> Self {
        SwapFlash { hz: sys_hz }
    }
}

impl Flash for SwapFlash {
    fn sectors(&self) -> u32 {
        (flash::SWAP.end - flash::SWAP.start) / PAGE as u32
    }
    fn erase(&mut self, sector: u32) -> bool {
        flash::erase_sector(flash::SWAP.start + sector * PAGE as u32).is_ok()
    }
    fn program(&mut self, off: u32, data: &[u8; CHUNK]) -> bool {
        // `data` may be in PSRAM, which the flash operation can't touch:
        // copy it to the stack (SRAM) first
        let b = *data;
        flash::program_page(flash::SWAP.start + off, &b).is_ok()
    }
    fn read(&self, off: u32, buf: &mut [u8]) {
        if off as usize + buf.len() > (flash::SWAP.end - flash::SWAP.start) as usize {
            buf.fill(0);
            return;
        }
        flash::read(flash::SWAP.start + off, buf);
    }
    fn cycles(&self) -> u64 {
        board::mcycle64()
    }
    fn hz(&self) -> u32 {
        self.hz
    }
}
