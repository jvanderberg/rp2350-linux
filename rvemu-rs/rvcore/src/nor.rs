//! A NOR flash device for the guest (`compatible = "rvemu,flash"`): the
//! guest's MTD driver (guest/linux/rvemu-flash.c) runs jffs2 on it.
//!
//! The device copies between flash and guest RAM itself, so a command is
//! one MMIO store. It completes before the store returns (no interrupt):
//! the guest is paused for exactly as long as the flash is busy.
//!
//! Registers (32-bit):
//!
//! | Offset | | |
//! |---|---|---|
//! | 0x00 | MAGIC | "RVFL" |
//! | 0x04 | SIZE | bytes |
//! | 0x08 | ERASESIZE | 4096 |
//! | 0x0c | WRITESIZE | 1: bit-writable NOR, as jffs2 expects of plain NOR |
//! | 0x10 | OFFSET | flash offset of the next command |
//! | 0x14 | ADDR | guest physical address of the buffer |
//! | 0x18 | LEN | bytes |
//! | 0x1c | CMD | write 1 read, 2 program, 3 erase (OFFSET, LEN sector aligned) |
//! | 0x20 | STATUS | 0 done, 1 bad arguments, 2 flash error |
//! | 0x40.. | STATS | backend counters (`NorFlash::stat`): erases, erases skipped (already blank), pages programmed, longest stall (us), total stall (ms) |
//!
//! Programs have NOR semantics: bits only go from 1 to 0.

use crate::cpu::exc;
use crate::virtio::GuestMem;

pub const NOR_BASE: u32 = 0xF800_0400;
pub const NOR_SIZE: u32 = 0x200;

const MAGIC: u32 = 0x4C46_5652; // "RVFL"
pub const ERASESIZE: u32 = 4096;
pub const WRITESIZE: u32 = 1;

const CMD_READ: u32 = 1;
const CMD_PROGRAM: u32 = 2;
const CMD_ERASE: u32 = 3;
const ST_OK: u32 = 0;
const ST_ARGS: u32 = 1;
const ST_IO: u32 = 2;

/// The flash behind the device, addressed from 0.
pub trait NorFlash {
    fn size(&self) -> u32;
    fn read(&mut self, off: u32, buf: &mut [u8]) -> bool;
    /// AND `data` into the flash at `off`.
    fn program(&mut self, off: u32, data: &[u8]) -> bool;
    /// Set the `ERASESIZE` sector at `off` to 0xFF.
    fn erase(&mut self, off: u32) -> bool;
    /// Counter `i` of `STATS` (0 when the backend doesn't keep it).
    fn stat(&self, _i: u32) -> u32 {
        0
    }
}

/// Number of `STATS` registers.
pub const NSTATS: u32 = 5;

/// No flash (the device is left out of the platform).
pub struct NoNor;

impl NorFlash for NoNor {
    fn size(&self) -> u32 {
        0
    }
    fn read(&mut self, _off: u32, _buf: &mut [u8]) -> bool {
        false
    }
    fn program(&mut self, _off: u32, _data: &[u8]) -> bool {
        false
    }
    fn erase(&mut self, _off: u32) -> bool {
        false
    }
}

pub struct NorDev<F: NorFlash> {
    pub flash: F,
    mem: GuestMem,
    off: u32,
    addr: u32,
    len: u32,
    status: u32,
    /// Guest RAM written by the last read, for `take_dma_write`.
    dirty: Option<(u32, u32)>,
}

impl<F: NorFlash> NorDev<F> {
    pub fn new(flash: F, mem: GuestMem) -> Self {
        NorDev { flash, mem, off: 0, addr: 0, len: 0, status: ST_OK, dirty: None }
    }

    pub fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.dirty.take()
    }

    pub fn load(&mut self, off: u32, size: u32) -> Result<u32, u32> {
        if size != 4 {
            return Err(exc::LOAD_FAULT);
        }
        Ok(match off {
            0x00 => MAGIC,
            0x04 => self.flash.size(),
            0x08 => ERASESIZE,
            0x0c => WRITESIZE,
            0x10 => self.off,
            0x14 => self.addr,
            0x18 => self.len,
            0x20 => self.status,
            0x40..0x54 => self.flash.stat((off - 0x40) / 4),
            _ => 0,
        })
    }

    pub fn store(&mut self, off: u32, size: u32, v: u32) -> Result<(), u32> {
        if size != 4 {
            return Err(exc::STORE_FAULT);
        }
        match off {
            0x10 => self.off = v,
            0x14 => self.addr = v,
            0x18 => self.len = v,
            0x1c => self.status = self.command(v),
            _ => {}
        }
        Ok(())
    }

    fn command(&mut self, cmd: u32) -> u32 {
        let (off, len) = (self.off, self.len);
        if off.checked_add(len).is_none_or(|e| e > self.flash.size()) {
            return ST_ARGS;
        }
        let pa = u64::from(self.addr);
        let n = len as usize;
        let ok = match cmd {
            CMD_READ => {
                let Some(p) = self.mem.range_mut(pa, n) else { return ST_ARGS };
                // SAFETY: `range_mut` checked the bounds; guest RAM is ours.
                let buf = unsafe { core::slice::from_raw_parts_mut(p, n) };
                let ok = self.flash.read(off, buf);
                let (lo, hi) = (self.addr, self.addr + len);
                self.dirty = Some(self.dirty.map_or((lo, hi), |(a, b)| (a.min(lo), b.max(hi))));
                ok
            }
            CMD_PROGRAM => {
                let Some(p) = self.mem.range(pa, n) else { return ST_ARGS };
                // SAFETY: `range` checked the bounds.
                let data = unsafe { core::slice::from_raw_parts(p, n) };
                self.flash.program(off, data)
            }
            CMD_ERASE => {
                if off % ERASESIZE != 0 || len % ERASESIZE != 0 {
                    return ST_ARGS;
                }
                (off..off + len).step_by(ERASESIZE as usize).all(|s| self.flash.erase(s))
            }
            _ => return ST_ARGS,
        };
        if ok { ST_OK } else { ST_IO }
    }
}
