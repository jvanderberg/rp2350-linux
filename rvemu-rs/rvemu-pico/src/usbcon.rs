//! The guest's console on the target's USB (CDC-ACM), served by core1.
//!
//! core0's UART model pushes output into `CON_OUT` and pops input from
//! `CON_IN`. This service moves the bytes between those rings and USB:
//! - Output waits in `CON_OUT` (16 KB) while no terminal is open (DTR low),
//!   keeping the newest bytes, so a late `screen` shows the end of the boot
//!   log. With a terminal open nothing is dropped: core0 waits for room when
//!   the ring is full, so the guest runs at the speed of the terminal
//!   (unless the terminal takes nothing for `STALL_MS`).
//! - Input is read from USB only while `CON_IN` has room for a packet, so
//!   the host waits rather than bytes being lost.
//! - Opening the port at 1200 baud and closing it reboots into BOOTSEL.

use rp2350_board as board;
use rp235x_hal as hal;
use usb_device::prelude::*;
use usbd_serial::SerialPort;

use crate::core1::{self, ByteRing};

pub const CON_OUT_LEN: usize = 16384;
/// Guest console output (core0 pushes, core1 pops).
pub static CON_OUT: ByteRing<CON_OUT_LEN> = ByteRing::new();
/// Guest console input (core1 pushes, core0 pops).
#[unsafe(link_section = ".sram89.con_in")]
pub static CON_IN: ByteRing<256> = ByteRing::new();

/// With no terminal, `CON_OUT` keeps at least this much room, dropping the
/// oldest output.
const KEEP_FREE: usize = 1024;
/// A terminal that takes nothing for this long is treated as gone.
const STALL_MS: u32 = 500;

pub struct UsbConsole {
    usb: UsbDevice<'static, hal::usb::UsbBus>,
    serial: SerialPort<'static, hal::usb::UsbBus>,
    stall_cycles: u64,
    /// When USB last took output (or there was none to send).
    progress: u64,
}

impl UsbConsole {
    pub fn new(usb: UsbDevice<'static, hal::usb::UsbBus>, serial: SerialPort<'static, hal::usb::UsbBus>, sys_hz: u32) -> Self {
        UsbConsole { usb, serial, stall_cycles: u64::from(sys_hz / 1000 * STALL_MS), progress: 0 }
    }
}

impl core1::Service for UsbConsole {
    fn poll(&mut self) {
        let now = core1::now();
        self.usb.poll(&mut [&mut self.serial]);
        if CON_IN.free() >= 64 {
            let mut buf = [0u8; 64];
            if let Ok(n) = self.serial.read(&mut buf)
                && n != 0
            {
                CON_IN.push_slice(&buf[..n]);
                core1::notify_core0();
            }
        }
        let dtr = self.serial.dtr();
        if self.serial.line_coding().data_rate() == 1200 && !dtr {
            board::reboot_to_bootsel();
        }
        let mut live = false;
        if dtr {
            loop {
                let f = CON_OUT.front();
                if f.is_empty() {
                    self.progress = now;
                    break;
                }
                match self.serial.write(f) {
                    Ok(n) if n > 0 => {
                        CON_OUT.consume(n);
                        self.progress = now;
                    }
                    _ => break,
                }
            }
            live = now - self.progress < self.stall_cycles;
        }
        if !live {
            let len = CON_OUT.len();
            if len > CON_OUT_LEN - KEEP_FREE {
                CON_OUT.consume(len - (CON_OUT_LEN - KEEP_FREE));
            }
        }
    }
}
