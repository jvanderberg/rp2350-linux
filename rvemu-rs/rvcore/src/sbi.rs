//! Built-in SBI (the kernel's ecall interface to "firmware").

use crate::cpu::*;

const ERR_NOT_SUPPORTED: i32 = -2;
const ERR_INVALID_PARAM: i32 = -3;
const ERR_ALREADY_AVAILABLE: i32 = -6;

const EXT_BASE: u32 = 0x10;
const EXT_TIME: u32 = 0x5449_4D45;
const EXT_IPI: u32 = 0x0073_5049;
const EXT_RFENCE: u32 = 0x5246_4E43;
const EXT_HSM: u32 = 0x0048_534D;
const EXT_SRST: u32 = 0x5352_5354;

fn hart0_in_mask(mask: u32, base: u32) -> bool {
    base == u32::MAX || (base == 0 && mask & 1 != 0)
}

impl Cpu {
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn sbi_call<B: Bus>(&mut self, bus: &mut B) {
        let (eid, fid) = (self.x[17], self.x[16]);
        let (a0, a1) = (self.x[10], self.x[11]);
        self.stats.sbi_calls += 1;

        // legacy v0.1 calls return a single value in a0
        match eid {
            0x00 => {
                self.stimecmp = ((a1 as u64) << 32) | a0 as u64;
                self.x[10] = 0;
                return;
            }
            0x01 => {
                bus.console_putc(a0 as u8);
                self.x[10] = 0;
                return;
            }
            0x02 => {
                self.x[10] = bus.console_getc().map_or(u32::MAX, |b| b as u32);
                return;
            }
            0x08 => {
                self.halt = Some(Halt::Shutdown);
                self.x[10] = 0;
                return;
            }
            _ => {}
        }

        let mut err = 0i32;
        let mut val = 0u32;
        match eid {
            EXT_BASE => match fid {
                0 => val = 0x0200_0000, // spec v2.0
                1 => val = 0x5250_3233, // impl id "RP23"
                2 => val = 1,
                3 => {
                    val = matches!(a0, EXT_BASE | EXT_TIME | EXT_IPI | EXT_RFENCE | EXT_HSM | EXT_SRST)
                        as u32
                }
                4..=6 => val = 0,
                _ => err = ERR_NOT_SUPPORTED,
            },
            EXT_TIME => {
                if fid == 0 {
                    self.stimecmp = ((a1 as u64) << 32) | a0 as u64;
                } else {
                    err = ERR_NOT_SUPPORTED;
                }
            }
            EXT_IPI => {
                if fid == 0 {
                    if hart0_in_mask(a0, a1) {
                        self.sip |= MIP_SSIP;
                    }
                } else {
                    err = ERR_NOT_SUPPORTED;
                }
            }
            EXT_RFENCE => match fid {
                0 => {} // remote fence.i: nothing cached yet
                1 | 2 => self.tlb_flush_all(),
                _ => err = ERR_NOT_SUPPORTED,
            },
            EXT_HSM => match fid {
                0 => err = if a0 == 0 { ERR_ALREADY_AVAILABLE } else { ERR_INVALID_PARAM },
                2 => {
                    if a0 != 0 {
                        err = ERR_INVALID_PARAM; // hart 0 is STARTED (0)
                    }
                }
                _ => err = ERR_NOT_SUPPORTED,
            },
            EXT_SRST => {
                if fid == 0 {
                    self.halt = Some(Halt::Shutdown);
                } else {
                    err = ERR_NOT_SUPPORTED;
                }
            }
            _ => err = ERR_NOT_SUPPORTED,
        }
        self.x[10] = err as u32;
        self.x[11] = val;
    }
}
