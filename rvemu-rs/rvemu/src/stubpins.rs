//! A simulated board behind the guest's GPIO, I2C, SPI, PWM and ADC (`-G`),
//! so the guest side can be developed on the Mac:
//! - GPIO: an output reads back its value, an input its pull (0 without
//!   one), or the line it is jumpered to (`RVEMU_JUMPERS=14-15,...`). Level
//!   changes latch edge interrupts as the RP2350's pads do.
//! - I2C: bus 0 has a 256-byte EEPROM at 0x50 (24C02 style: the first byte
//!   written sets the address); every other address NACKs.
//! - SPI: MISO is wired to MOSI (loopback).
//! - PWM: the RP2350's slice arithmetic at 150 MHz.
//! - ADC: the temperature sensor reads 27 degC; GP40+N reads 400 * N + 100.
//! - PIO: two software PIO blocks (`piosim.rs`) on the free pins, and the
//!   DMA channels between guest RAM and their FIFOs, moving a word whenever
//!   the FIFO has room or data.

use crate::piosim::PioSim;
use rvcore::periph::{DMA_CHANNELS, DmaState, DmaXfer, FREE_PINS, I2cError, I2cOp, Line, NGPIO, Pins, Pwm, pwm_timing};

/// A DMA channel at work.
struct Dma {
    x: DmaXfer,
    /// Bytes past `x.mem` the memory side is at, and transfers left.
    pos: usize,
    left: u32,
}

const CLK_HZ: u32 = 150_000_000;
const EEPROM_ADDR: u8 = 0x50;

pub struct StubPins {
    line: [Line; NGPIO as usize],
    jumper: [Option<u32>; NGPIO as usize],
    levels: u64,
    edges: u64,
    eeprom: [u8; 256],
    eeprom_ptr: u8,
    pio: PioSim,
    /// The PIO block each pin is given to.
    pio_func: [Option<usize>; NGPIO as usize],
    dma: [Option<Dma>; DMA_CHANNELS],
    dma_done: u32,
}

impl StubPins {
    pub fn new() -> Self {
        let mut jumper = [None; NGPIO as usize];
        for pair in std::env::var("RVEMU_JUMPERS").unwrap_or_default().split(',') {
            let mut it = pair.split('-').filter_map(|s| s.trim().parse::<u32>().ok());
            if let (Some(a), Some(b)) = (it.next(), it.next())
                && a < NGPIO
                && b < NGPIO
            {
                jumper[a as usize] = Some(b);
                jumper[b as usize] = Some(a);
            }
        }
        let mut p = StubPins {
            line: [Line { pull_down: true, ..Line::default() }; NGPIO as usize],
            jumper,
            levels: 0,
            edges: 0,
            eeprom: std::array::from_fn(|i| i as u8),
            eeprom_ptr: 0,
            pio: PioSim::new(),
            pio_func: [None; NGPIO as usize],
            dma: [const { None }; DMA_CHANNELS],
            dma_done: 0,
        };
        p.levels = p.compute();
        p
    }

    /// What drives pin `n`: its PIO block or its GPIO output, if either.
    fn driven(&self, n: usize) -> Option<bool> {
        match self.pio_func[n] {
            Some(b) => self.pio.drives(b, n as u32),
            None => self.line[n].oe.then_some(self.line[n].out),
        }
    }

    fn level(&self, n: usize) -> bool {
        self.driven(n).or_else(|| self.jumper[n].and_then(|j| self.driven(j as usize))).unwrap_or(self.line[n].pull_up)
    }

    fn compute(&self) -> u64 {
        (0..NGPIO as usize).filter(|&n| self.level(n)).fold(0, |m, n| m | 1 << n)
    }

    /// Take the new levels, latching edges on the lines that want them.
    fn relevel(&mut self) {
        let new = self.compute();
        let (rise, fall) = (new & !self.levels, !new & self.levels);
        for k in 0..NGPIO as usize {
            let l = self.line[k];
            if (l.rise && rise >> k & 1 != 0) || (l.fall && fall >> k & 1 != 0) {
                self.edges |= 1 << k;
            }
        }
        self.levels = new;
    }

    /// One word for each DMA channel whose FIFO is ready; whether any moved.
    fn dma_step(&mut self) -> bool {
        let mut moved = false;
        for k in 0..DMA_CHANNELS {
            let Some(d) = &mut self.dma[k] else { continue };
            let x = d.x;
            let ready = if x.from_fifo { self.pio.rx_ready(x.blk, x.sm) } else { self.pio.tx_free(x.blk, x.sm) };
            if !ready || d.left == 0 {
                continue;
            }
            let size = 1usize << x.size_log2;
            // SAFETY: rvcore checked the buffer (count transfers, or the
            // ring) is guest RAM; `pos` stays inside it.
            unsafe {
                let p = x.mem.add(d.pos);
                if x.from_fifo {
                    let v = self.pio.pop_rx(x.blk, x.sm);
                    match size {
                        1 => *p = v as u8,
                        2 => (p as *mut u16).write_unaligned(v as u16),
                        _ => (p as *mut u32).write_unaligned(v),
                    }
                } else {
                    // narrow writes reach the FIFO copied across the word
                    let v = match size {
                        1 => u32::from(*p) * 0x0101_0101,
                        2 => u32::from((p as *const u16).read_unaligned()) * 0x0001_0001,
                        _ => (p as *const u32).read_unaligned(),
                    };
                    self.pio.push_tx(x.blk, x.sm, v);
                }
            }
            d.pos += size;
            if x.ring_log2 != 0 {
                d.pos &= (1 << x.ring_log2) - 1;
            }
            if !x.endless {
                d.left -= 1;
                if d.left == 0 {
                    self.dma_done |= 1 << k;
                }
            }
            moved = true;
        }
        moved
    }

    /// Run the PIO blocks up to now.
    fn pio_run(&mut self) {
        let n = self.pio.due();
        let mut levels = self.compute();
        let mut changed = false;
        for _ in 0..n {
            let dma = self.dma_step();
            let (moved, ch) = self.pio.cycle(levels);
            if ch {
                levels = self.compute();
                changed = true;
            }
            if !moved && !dma {
                break;
            }
        }
        if changed {
            self.relevel();
        }
    }
}

impl Pins for StubPins {
    fn gpio_valid(&self) -> u64 {
        FREE_PINS
    }

    fn gpio_set(&mut self, n: u32, line: Line) {
        self.line[n as usize] = line;
        self.relevel();
    }

    fn gpio_levels(&mut self) -> u64 {
        self.levels
    }

    fn gpio_edges(&mut self) -> u64 {
        std::mem::take(&mut self.edges)
    }

    fn i2c(&mut self, bus: usize, addr: u8, ops: &mut [I2cOp<'_>]) -> Result<(), I2cError> {
        if bus != 0 || addr != EEPROM_ADDR {
            return Err(I2cError::Nack);
        }
        for op in ops.iter_mut() {
            match op {
                I2cOp::Write(b) => {
                    if let Some((&a, data)) = b.split_first() {
                        self.eeprom_ptr = a;
                        for &d in data {
                            self.eeprom[self.eeprom_ptr as usize] = d;
                            self.eeprom_ptr = self.eeprom_ptr.wrapping_add(1);
                        }
                    }
                }
                I2cOp::Read(b) => {
                    for d in b.iter_mut() {
                        *d = self.eeprom[self.eeprom_ptr as usize];
                        self.eeprom_ptr = self.eeprom_ptr.wrapping_add(1);
                    }
                }
            }
        }
        Ok(())
    }

    fn spi_transfer(&mut self, _bus: usize, _hz: u32, _mode: u8, _buf: &mut [u8]) {}

    fn spi_cs(&mut self, _bus: usize, _active: bool) {}

    fn pwm_set(&mut self, _n: u32, pwm: Pwm) -> Pwm {
        match pwm_timing(CLK_HZ, pwm.period_ns, pwm.duty_ns) {
            Some(t) => Pwm { period_ns: t.period_ns, duty_ns: t.duty_ns, ..pwm },
            None => Pwm { period_ns: 0, duty_ns: 0, ..pwm },
        }
    }

    fn adc(&mut self, ch: u32) -> Option<u16> {
        // 0.706 V at 27 degC
        Some(if ch == 8 { 876 } else { 400 * ch as u16 + 100 })
    }

    fn pio_blocks(&self) -> u32 {
        2
    }

    fn pio_clk_hz(&self) -> u32 {
        CLK_HZ
    }

    fn pio_read(&mut self, blk: usize, reg: u32) -> u32 {
        self.pio_run();
        self.pio.load(blk, reg)
    }

    fn pio_write(&mut self, blk: usize, alias: u32, reg: u32, v: u32) {
        self.pio_run();
        self.pio.store(blk, alias, reg, v, self.levels);
        self.relevel();
    }

    fn pio_pin(&mut self, n: u32, blk: Option<usize>) {
        self.pio_func[n as usize] = blk;
        self.relevel();
    }

    fn pio_irqs(&mut self) -> u32 {
        self.pio_run();
        self.pio.irqs()
    }

    fn dma_channels(&self) -> u32 {
        DMA_CHANNELS as u32
    }

    fn dma_start(&mut self, ch: usize, x: DmaXfer) {
        self.pio_run();
        self.dma[ch] = Some(Dma { x, pos: 0, left: x.count });
        self.dma_done &= !(1 << ch);
    }

    fn dma_stop(&mut self, ch: usize) {
        self.dma[ch] = None;
    }

    fn dma_state(&mut self, ch: usize) -> DmaState {
        self.pio_run();
        match &self.dma[ch] {
            Some(d) => DmaState {
                busy: d.left != 0,
                error: false,
                remaining: d.left,
                mem: d.x.mem as usize + d.pos,
            },
            None => DmaState::default(),
        }
    }

    fn dma_done(&mut self) -> u32 {
        std::mem::take(&mut self.dma_done)
    }
}
