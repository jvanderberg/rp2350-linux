//! Platform devices: 16550-style UART, a single-context SiFive PLIC (hart 0,
//! S-mode), optional virtio-mmio block and network devices, and optionally
//! the board's pins (`periph`). The host supplies the console, the disk,
//! the network link and the pins.

use crate::cpu::exc;
use crate::periph::{GPIO_IRQ, IO_BASE, IO_SIZE, Io, NoPins, Pins};
use crate::virtio::{
    Disk, GuestMem, NetLink, NoDisk, NoNet, VIRTIO_BASE, VIRTIO_IRQ, VIRTIO_NET_BASE, VIRTIO_NET_IRQ, VIRTIO_SIZE,
    VirtioBlk, VirtioNet,
};

pub const PLIC_BASE: u32 = 0xF000_0000;
pub const PLIC_SIZE: u32 = 0x0400_0000;
pub const UART_BASE: u32 = 0xF400_0000;
pub const UART_SIZE: u32 = 0x0000_0100;
pub const UART_IRQ: u32 = 1;

/// Host side of the serial port.
pub trait Console {
    fn putc(&mut self, b: u8);
    fn rx_ready(&self) -> bool;
    fn getc(&mut self) -> Option<u8>;
}

#[derive(Default)]
pub struct Plic {
    prio: [u32; 32],
    pending: u32,
    enable: u32,
    threshold: u32,
    /// Claimed and not yet completed.
    claimed: u32,
    /// Device interrupt lines.
    level: u32,
    /// Output: S-mode external interrupt to hart 0.
    pub irq: bool,
}

impl Plic {
    fn update(&mut self) {
        self.pending |= self.level & !self.claimed;
        let cand = self.pending & self.enable;
        self.irq = (1..32).any(|i| cand & (1 << i) != 0 && self.prio[i] > self.threshold);
    }

    fn claim(&mut self) -> u32 {
        let cand = self.pending & self.enable;
        let (mut best, mut best_prio) = (0, 0);
        for i in 1..32 {
            if cand & (1 << i) != 0 && self.prio[i] > self.threshold && self.prio[i] > best_prio {
                best = i as u32;
                best_prio = self.prio[i];
            }
        }
        if best != 0 {
            self.pending &= !(1 << best);
            self.claimed |= 1 << best;
        }
        best
    }

    pub fn set_level(&mut self, irq: u32, high: bool) {
        if high {
            self.level |= 1 << irq;
        } else {
            self.level &= !(1 << irq);
        }
        self.update();
    }

    pub fn load(&mut self, off: u32, size: u32) -> Result<u32, u32> {
        if size != 4 {
            return Err(exc::LOAD_FAULT);
        }
        Ok(match off {
            0..=0x7F => self.prio[(off >> 2) as usize],
            0x1000 => self.pending,
            0x2000 => self.enable,
            0x20_0000 => self.threshold,
            0x20_0004 => {
                let v = self.claim();
                self.update();
                v
            }
            _ => 0,
        })
    }

    pub fn store(&mut self, off: u32, size: u32, v: u32) -> Result<(), u32> {
        if size != 4 {
            return Err(exc::STORE_FAULT);
        }
        match off {
            0..=0x7F => {
                if off != 0 {
                    self.prio[(off >> 2) as usize] = v & 7;
                }
            }
            0x2000 => self.enable = v & !1,
            0x20_0000 => self.threshold = v & 7,
            0x20_0004 if v < 32 => self.claimed &= !(1 << v),
            _ => {}
        }
        self.update();
        Ok(())
    }
}

#[derive(Default)]
pub struct Uart {
    ier: u8,
    lcr: u8,
    mcr: u8,
    dll: u8,
    dlh: u8,
    scr: u8,
    thre_pending: bool,
}

impl Uart {
    fn iir(&self, rx: bool) -> u8 {
        if self.ier & 1 != 0 && rx {
            0x04 // received data available
        } else if self.ier & 2 != 0 && self.thre_pending {
            0x02 // transmitter holding register empty
        } else {
            0x01 // none
        }
    }

    pub fn irq_line(&self, con: &impl Console) -> bool {
        self.iir(con.rx_ready()) != 0x01
    }

    pub fn load(&mut self, off: u32, size: u32, con: &mut impl Console) -> Result<u32, u32> {
        if size != 1 {
            return Err(exc::LOAD_FAULT);
        }
        let dlab = self.lcr & 0x80 != 0;
        Ok(match off {
            0 if dlab => self.dll as u32,
            0 => con.getc().unwrap_or(0) as u32,
            1 if dlab => self.dlh as u32,
            1 => self.ier as u32,
            2 => {
                let iir = self.iir(con.rx_ready());
                if iir == 0x02 {
                    self.thre_pending = false;
                }
                iir as u32
            }
            3 => self.lcr as u32,
            4 => self.mcr as u32,
            5 => 0x60 | con.rx_ready() as u32, // THRE | TEMT | DR
            6 => 0xB0,                         // DCD | DSR | CTS
            7 => self.scr as u32,
            _ => 0,
        })
    }

    pub fn store(&mut self, off: u32, size: u32, v: u32, con: &mut impl Console) -> Result<(), u32> {
        if size != 1 {
            return Err(exc::STORE_FAULT);
        }
        let dlab = self.lcr & 0x80 != 0;
        let b = v as u8;
        match off {
            0 if dlab => self.dll = b,
            0 => {
                con.putc(b);
                self.thre_pending = true; // transmits instantly
            }
            1 if dlab => self.dlh = b,
            1 => {
                // enabling the THRE interrupt with an empty THR raises it
                if self.ier & 2 == 0 && b & 2 != 0 {
                    self.thre_pending = true;
                }
                self.ier = b & 0x0F;
            }
            3 => self.lcr = b,
            4 => self.mcr = b,
            7 => self.scr = b,
            _ => {}
        }
        Ok(())
    }
}

/// UART + PLIC (+ disk, + network, + pins) wired together, generic over the
/// host console, disk, network link and pins.
pub struct Platform<C: Console, D: Disk = NoDisk, N: NetLink = NoNet, P: Pins = NoPins> {
    pub uart: Uart,
    pub plic: Plic,
    pub con: C,
    pub blk: Option<VirtioBlk<D>>,
    pub net: Option<VirtioNet<N>>,
    pub io: Option<Io<P>>,
}

impl<C: Console, D: Disk, N: NetLink, P: Pins> Platform<C, D, N, P> {
    /// UART and PLIC only.
    pub fn new(con: C) -> Self {
        Platform { uart: Uart::default(), plic: Plic::default(), con, blk: None, net: None, io: None }
    }

    /// Add the GPIO, I2C, SPI, PWM and ADC devices at `IO_BASE` on `pins`.
    pub fn with_io(mut self, pins: P, mem: GuestMem) -> Self {
        self.io = Some(Io::new(pins, mem));
        self
    }

    /// Add a virtio block device at `VIRTIO_BASE` reading and writing `mem`.
    pub fn with_disk(mut self, disk: D, mem: GuestMem) -> Self {
        self.blk = Some(VirtioBlk::new(disk, mem));
        self
    }

    /// Add a virtio network device at `VIRTIO_NET_BASE` on `link`.
    pub fn with_net(mut self, link: N, mem: GuestMem) -> Self {
        self.net = Some(VirtioNet::new(link, mem));
        self
    }

    /// Refresh the interrupt lines; also delivers received frames, so call
    /// it regularly.
    pub fn update_irqs(&mut self) {
        let line = self.uart.irq_line(&self.con);
        self.plic.set_level(UART_IRQ, line);
        if let Some(b) = &self.blk {
            self.plic.set_level(VIRTIO_IRQ, b.irq_line());
        }
        if let Some(n) = &mut self.net {
            n.poll();
            self.plic.set_level(VIRTIO_NET_IRQ, n.irq_line());
        }
        if let Some(io) = &mut self.io {
            io.poll();
            self.plic.set_level(GPIO_IRQ, io.irq_line());
        }
    }

    pub fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        if let Some(r) = self.blk.as_mut().and_then(|b| b.take_dma_write()) {
            return Some(r);
        }
        if let Some(r) = self.net.as_mut().and_then(|n| n.take_dma_write()) {
            return Some(r);
        }
        self.io.as_mut().and_then(|io| io.take_dma_write())
    }

    pub fn mmio_load(&mut self, pa: u32, size: u32) -> Result<u32, u32> {
        if pa.wrapping_sub(UART_BASE) < UART_SIZE {
            let r = self.uart.load(pa - UART_BASE, size, &mut self.con);
            if r.is_ok() {
                self.update_irqs();
            }
            return r;
        }
        if pa.wrapping_sub(PLIC_BASE) < PLIC_SIZE {
            return self.plic.load(pa - PLIC_BASE, size);
        }
        if let Some(b) = &mut self.blk
            && pa.wrapping_sub(VIRTIO_BASE) < VIRTIO_SIZE
        {
            return b.load(pa - VIRTIO_BASE, size);
        }
        if let Some(n) = &mut self.net
            && pa.wrapping_sub(VIRTIO_NET_BASE) < VIRTIO_SIZE
        {
            return n.load(pa - VIRTIO_NET_BASE, size);
        }
        if let Some(io) = &mut self.io
            && pa.wrapping_sub(IO_BASE) < IO_SIZE
        {
            let r = io.load(pa, size);
            self.update_irqs();
            return r;
        }
        Err(exc::LOAD_FAULT)
    }

    pub fn mmio_store(&mut self, pa: u32, size: u32, v: u32) -> Result<(), u32> {
        if pa.wrapping_sub(UART_BASE) < UART_SIZE {
            let r = self.uart.store(pa - UART_BASE, size, v, &mut self.con);
            if r.is_ok() {
                self.update_irqs();
            }
            return r;
        }
        if pa.wrapping_sub(PLIC_BASE) < PLIC_SIZE {
            return self.plic.store(pa - PLIC_BASE, size, v);
        }
        if let Some(b) = &mut self.blk
            && pa.wrapping_sub(VIRTIO_BASE) < VIRTIO_SIZE
        {
            let r = b.store(pa - VIRTIO_BASE, size, v);
            self.update_irqs();
            return r;
        }
        if let Some(n) = &mut self.net
            && pa.wrapping_sub(VIRTIO_NET_BASE) < VIRTIO_SIZE
        {
            let r = n.store(pa - VIRTIO_NET_BASE, size, v);
            self.update_irqs();
            return r;
        }
        if let Some(io) = &mut self.io
            && pa.wrapping_sub(IO_BASE) < IO_SIZE
        {
            let r = io.store(pa, size, v);
            self.update_irqs();
            return r;
        }
        Err(exc::STORE_FAULT)
    }
}
