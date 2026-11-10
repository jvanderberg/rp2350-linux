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

use rvcore::periph::{FREE_PINS, I2cError, I2cOp, Line, NGPIO, Pins, Pwm, pwm_timing};

const CLK_HZ: u32 = 150_000_000;
const EEPROM_ADDR: u8 = 0x50;

pub struct StubPins {
    line: [Line; NGPIO as usize],
    jumper: [Option<u32>; NGPIO as usize],
    levels: u64,
    edges: u64,
    eeprom: [u8; 256],
    eeprom_ptr: u8,
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
        };
        p.levels = p.compute();
        p
    }

    fn level(&self, n: usize) -> bool {
        let l = self.line[n];
        if l.oe {
            return l.out;
        }
        if let Some(j) = self.jumper[n]
            && self.line[j as usize].oe
        {
            return self.line[j as usize].out;
        }
        l.pull_up
    }

    fn compute(&self) -> u64 {
        (0..NGPIO as usize).filter(|&n| self.level(n)).fold(0, |m, n| m | 1 << n)
    }
}

impl Pins for StubPins {
    fn gpio_valid(&self) -> u64 {
        FREE_PINS
    }

    fn gpio_set(&mut self, n: u32, line: Line) {
        self.line[n as usize] = line;
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
}
