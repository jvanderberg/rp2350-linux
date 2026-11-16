//! Two software PIO blocks for the Mac's simulated board (`stubpins.rs`):
//! the RP2350 PIO's registers (as `rvcore::periph` hands them over) and its
//! state machines, so the guest's PIO driver and programs can be tried
//! without the board. They run in step with wall time at `RVEMU_PIO_HZ`
//! cycles a second (default 10 MHz, not the board's clk_sys), so a program
//! behaves as on the board, only slower, and only while the guest touches
//! the blocks or polls its devices.
//!
//! Left out: `irq`/`wait irq` on the neighbour blocks (PREV/NEXT), CTRL's
//! NEXTPREV operations, and the TX/RX FIFO status bits beyond FSTAT,
//! FLEVEL and FDEBUG's TXOVER/RXUNDER.

use std::time::Instant;

/// Cycles one run may catch up on (time past that is dropped).
const MAX_CYCLES: u64 = 200_000;

#[derive(Clone, Copy, Default)]
struct Fifo {
    d: [u32; 8],
    r: usize,
    n: usize,
}

impl Fifo {
    fn push(&mut self, v: u32) {
        self.d[(self.r + self.n) % 8] = v;
        self.n += 1;
    }
    fn pop(&mut self) -> u32 {
        let v = self.d[self.r];
        self.r = (self.r + 1) % 8;
        self.n -= 1;
        v
    }
}

#[derive(Clone, Copy)]
struct Sm {
    clkdiv: u32,
    execctrl: u32,
    shiftctrl: u32,
    pinctrl: u32,
    pc: u32,
    x: u32,
    y: u32,
    isr: u32,
    osr: u32,
    isr_n: u32,
    osr_n: u32,
    tx: Fifo,
    rx: Fifo,
    rxreg: [u32; 4],
    delay: u32,
    /// An instruction to run next instead of the one at `pc` (OUT EXEC,
    /// MOV EXEC, a write to SMn_INSTR).
    exec: Option<u16>,
    /// The instruction in progress, and whether it stalled (it runs again)
    /// and came from `exec` (it doesn't move `pc` on).
    instr: u16,
    stalled: bool,
    from_exec: bool,
    /// An `irq wait` set its flag and waits for it to clear.
    irq_waiting: bool,
    /// The clock divider's accumulator, in 1/256 cycles.
    frac: u32,
}

impl Sm {
    fn new() -> Self {
        Sm {
            clkdiv: 0x0001_0000,
            execctrl: 0x1F << 12,
            shiftctrl: 0x000C_0000,
            pinctrl: 0x1400_0000,
            pc: 0,
            x: 0,
            y: 0,
            isr: 0,
            osr: 0,
            isr_n: 0,
            osr_n: 32,
            tx: Fifo::default(),
            rx: Fifo::default(),
            rxreg: [0; 4],
            delay: 0,
            exec: None,
            instr: 0,
            stalled: false,
            from_exec: false,
            irq_waiting: false,
            frac: 0,
        }
    }

    fn restart(&mut self) {
        self.isr_n = 0;
        self.osr_n = 32;
        self.delay = 0;
        self.stalled = false;
        self.irq_waiting = false;
        self.exec = None;
    }

    fn tx_depth(&self) -> usize {
        match self.shiftctrl >> 30 & 3 {
            1 => 8, // FJOIN_TX
            2 => 0, // FJOIN_RX
            _ => 4,
        }
    }
    fn rx_depth(&self) -> usize {
        match self.shiftctrl >> 30 & 3 {
            1 => 0,
            2 => 8,
            _ => 4,
        }
    }
    fn push_thresh(&self) -> u32 {
        match self.shiftctrl >> 20 & 0x1F {
            0 => 32,
            n => n,
        }
    }
    fn pull_thresh(&self) -> u32 {
        match self.shiftctrl >> 25 & 0x1F {
            0 => 32,
            n => n,
        }
    }
    fn in_mask(&self) -> u32 {
        match self.shiftctrl & 0x1F {
            0 => !0,
            n => (1 << n) - 1,
        }
    }
    fn wrap_top(&self) -> u32 {
        self.execctrl >> 12 & 0x1F
    }
    fn wrap_bottom(&self) -> u32 {
        self.execctrl >> 7 & 0x1F
    }
    fn jmp_pin(&self) -> u32 {
        self.execctrl >> 24 & 0x1F
    }
    fn in_base(&self) -> u32 {
        self.pinctrl >> 15 & 0x1F
    }
}

/// What one instruction did.
enum Step {
    Stall,
    Next,
    Jump(u32),
}

struct Block {
    enable: u32,
    imem: [u16; 32],
    sm: [Sm; 4],
    irq: u32,
    inte: [u32; 2],
    intf: [u32; 2],
    gpiobase: u32,
    sync_bypass: u32,
    fdebug: u32,
    /// Pin outputs and directions, bit N = GPIO gpiobase + N.
    out: u32,
    oe: u32,
}

fn mask(n: u32) -> u32 {
    if n >= 32 { !0 } else { (1 << n) - 1 }
}

impl Block {
    fn new() -> Self {
        Block {
            enable: 0,
            imem: [0; 32],
            sm: [Sm::new(); 4],
            irq: 0,
            inte: [0; 2],
            intf: [0; 2],
            gpiobase: 0,
            sync_bypass: 0,
            fdebug: 0,
            out: 0,
            oe: 0,
        }
    }

    fn intr(&self) -> u32 {
        let mut v = (self.irq & 0xFF) << 8;
        for (k, s) in self.sm.iter().enumerate() {
            if s.rx.n != 0 {
                v |= 1 << k;
            }
            if s.tx.n < s.tx_depth() {
                v |= 1 << (4 + k);
            }
        }
        v
    }

    fn ints(&self, line: usize) -> u32 {
        (self.intr() & self.inte[line]) | self.intf[line]
    }

    fn write_pins(&mut self, base: u32, count: u32, v: u32, dirs: bool) {
        for i in 0..count.min(32) {
            let p = (base + i) % 32;
            let r = if dirs { &mut self.oe } else { &mut self.out };
            *r = (*r & !(1 << p)) | ((v >> i & 1) << p);
        }
    }

    /// The flag an IRQ / WAIT IRQ index names for state machine `k`; None
    /// for another block's.
    fn irq_flag(k: usize, idx: u32) -> Option<u32> {
        let n = idx & 7;
        match idx >> 3 & 3 {
            0 => Some(n),
            2 => Some((n & 4) | ((n + k as u32) & 3)),
            _ => None,
        }
    }

    fn run_sm(&mut self, k: usize, pins: u32, ins: u16) -> Step {
        let s = &mut self.sm[k];
        let op = ins >> 13;
        let a = u32::from(ins >> 5 & 7);
        let b = u32::from(ins & 0x1F);
        let in_pins = pins.rotate_right(s.in_base()) & s.in_mask();
        match op {
            // JMP
            0 => {
                let take = match a {
                    0 => true,
                    1 => s.x == 0,
                    2 => {
                        let t = s.x != 0;
                        s.x = s.x.wrapping_sub(1);
                        t
                    }
                    3 => s.y == 0,
                    4 => {
                        let t = s.y != 0;
                        s.y = s.y.wrapping_sub(1);
                        t
                    }
                    5 => s.x != s.y,
                    6 => pins >> s.jmp_pin() & 1 != 0,
                    _ => s.osr_n < s.pull_thresh(),
                };
                if take { Step::Jump(b) } else { Step::Next }
            }
            // WAIT
            1 => {
                let pol = a >> 2 & 1;
                let level = match a & 3 {
                    0 => pins >> b & 1,
                    1 => in_pins >> b & 1,
                    2 => {
                        let Some(f) = Self::irq_flag(k, b) else { return Step::Stall };
                        let set = self.irq >> f & 1;
                        if pol == 1 && set == 1 {
                            self.irq &= !(1 << f);
                            return Step::Next;
                        }
                        set
                    }
                    _ => pins >> ((s.jmp_pin() + b) % 32) & 1,
                };
                if level == pol { Step::Next } else { Step::Stall }
            }
            // IN
            2 => {
                let n = if b == 0 { 32 } else { b };
                let auto = s.shiftctrl >> 16 & 1 != 0;
                if auto && (s.isr_n + n).min(32) >= s.push_thresh() && s.rx.n >= s.rx_depth() {
                    return Step::Stall;
                }
                let data = match a {
                    0 => in_pins,
                    1 => s.x,
                    2 => s.y,
                    6 => s.isr,
                    7 => s.osr,
                    _ => 0,
                } & mask(n);
                s.isr = if n == 32 {
                    data
                } else if s.shiftctrl >> 18 & 1 != 0 {
                    (s.isr >> n) | (data << (32 - n))
                } else {
                    (s.isr << n) | data
                };
                s.isr_n = (s.isr_n + n).min(32);
                if auto && s.isr_n >= s.push_thresh() {
                    s.rx.push(s.isr);
                    s.isr = 0;
                    s.isr_n = 0;
                }
                Step::Next
            }
            // OUT
            3 => {
                let n = if b == 0 { 32 } else { b };
                if s.shiftctrl >> 17 & 1 != 0 && s.osr_n >= s.pull_thresh() {
                    if s.tx.n == 0 {
                        return Step::Stall;
                    }
                    s.osr = s.tx.pop();
                    s.osr_n = 0;
                }
                let data;
                if s.shiftctrl >> 19 & 1 != 0 {
                    data = s.osr & mask(n);
                    s.osr = if n == 32 { 0 } else { s.osr >> n };
                } else {
                    data = if n == 32 { s.osr } else { s.osr >> (32 - n) };
                    s.osr = if n == 32 { 0 } else { s.osr << n };
                }
                s.osr_n = (s.osr_n + n).min(32);
                let (out_base, out_count) = (s.pinctrl & 0x1F, s.pinctrl >> 20 & 0x3F);
                match a {
                    0 => self.write_pins(out_base, out_count, data, false),
                    1 => s.x = data,
                    2 => s.y = data,
                    4 => self.write_pins(out_base, out_count, data, true),
                    5 => return Step::Jump(data & 0x1F),
                    6 => {
                        s.isr = data;
                        s.isr_n = n;
                    }
                    7 => s.exec = Some(data as u16),
                    _ => {}
                }
                Step::Next
            }
            // PUSH, PULL, MOV to / from the RX FIFO's registers
            4 => {
                let pull = ins >> 7 & 1 != 0;
                if ins >> 4 & 1 != 0 {
                    let i = if ins >> 3 & 1 != 0 { (ins & 3) as usize } else { (s.y & 3) as usize };
                    if pull {
                        s.osr = s.rxreg[i];
                        s.osr_n = 0;
                    } else {
                        s.rxreg[i] = s.isr;
                    }
                    return Step::Next;
                }
                let (cond, block) = (ins >> 6 & 1 != 0, ins >> 5 & 1 != 0);
                if pull {
                    if cond && s.osr_n < s.pull_thresh() {
                        return Step::Next;
                    }
                    if s.tx.n == 0 {
                        if block {
                            return Step::Stall;
                        }
                        s.osr = s.x;
                    } else {
                        s.osr = s.tx.pop();
                    }
                    s.osr_n = 0;
                } else {
                    if cond && s.isr_n < s.push_thresh() {
                        return Step::Next;
                    }
                    if s.rx.n >= s.rx_depth() {
                        if block {
                            return Step::Stall;
                        }
                    } else {
                        s.rx.push(s.isr);
                    }
                    s.isr = 0;
                    s.isr_n = 0;
                }
                Step::Next
            }
            // MOV
            5 => {
                let src = match b & 7 {
                    0 => in_pins,
                    1 => s.x,
                    2 => s.y,
                    5 => {
                        let n = s.execctrl & 0x1F;
                        let st = match s.execctrl >> 5 & 3 {
                            0 => (s.tx.n as u32) < n,
                            1 => (s.rx.n as u32) < n,
                            _ => self.irq >> (n & 7) & 1 != 0,
                        };
                        if st { !0 } else { 0 }
                    }
                    6 => s.isr,
                    7 => s.osr,
                    _ => 0,
                };
                let v = match b >> 3 {
                    1 => !src,
                    2 => src.reverse_bits(),
                    _ => src,
                };
                let (out_base, out_count) = (s.pinctrl & 0x1F, s.pinctrl >> 20 & 0x3F);
                match a {
                    0 => self.write_pins(out_base, out_count, v, false),
                    1 => s.x = v,
                    2 => s.y = v,
                    3 => self.write_pins(out_base, out_count, v, true),
                    4 => s.exec = Some(v as u16),
                    5 => return Step::Jump(v & 0x1F),
                    6 => {
                        s.isr = v;
                        s.isr_n = 0;
                    }
                    _ => {
                        s.osr = v;
                        s.osr_n = 0;
                    }
                }
                Step::Next
            }
            // IRQ
            6 => {
                let Some(f) = Self::irq_flag(k, b) else { return Step::Next };
                if a >> 1 & 1 != 0 {
                    self.irq &= !(1 << f);
                    return Step::Next;
                }
                let s = &mut self.sm[k];
                if s.irq_waiting {
                    if self.irq >> f & 1 == 0 {
                        s.irq_waiting = false;
                        return Step::Next;
                    }
                    return Step::Stall;
                }
                self.irq |= 1 << f;
                if a & 1 != 0 {
                    s.irq_waiting = true;
                    return Step::Stall;
                }
                Step::Next
            }
            // SET
            _ => {
                let (set_base, set_count) = (s.pinctrl >> 5 & 0x1F, s.pinctrl >> 26 & 7);
                match a {
                    0 => self.write_pins(set_base, set_count, b, false),
                    1 => s.x = b,
                    2 => s.y = b,
                    4 => self.write_pins(set_base, set_count, b, true),
                    _ => {}
                }
                Step::Next
            }
        }
    }

    /// Side-set and delay of `ins` for state machine `k`: apply the side-set
    /// and return the delay.
    fn side_set(&mut self, k: usize, ins: u16) -> u32 {
        let s = &self.sm[k];
        let count = s.pinctrl >> 29 & 7;
        let field = u32::from(ins >> 8 & 0x1F);
        let delay_bits = 5u32.saturating_sub(count);
        let delay = field & mask(delay_bits);
        if count == 0 {
            return delay;
        }
        let mut side = field >> delay_bits;
        let mut bits = count;
        if s.execctrl >> 30 & 1 != 0 {
            bits -= 1;
            if side >> bits & 1 == 0 {
                return delay;
            }
            side &= mask(bits);
        }
        let (base, dirs) = (s.pinctrl >> 10 & 0x1F, s.execctrl >> 29 & 1 != 0);
        self.write_pins(base, bits, side, dirs);
        delay
    }

    /// One clk_sys cycle of state machine `k` (`pins`: the block's 32 GPIO
    /// inputs); whether anything moved.
    fn cycle_sm(&mut self, k: usize, pins: u32) -> bool {
        {
            let s = &mut self.sm[k];
            let div = match s.clkdiv >> 8 {
                d if d >> 8 == 0 => (1 << 16) << 8 | (d & 0xFF),
                d => d,
            };
            s.frac += 256;
            if s.frac < div {
                return true;
            }
            s.frac -= div;
            if s.delay > 0 {
                s.delay -= 1;
                return true;
            }
        }
        let s = &mut self.sm[k];
        let ins = if s.stalled {
            s.instr
        } else if let Some(e) = s.exec.take() {
            s.from_exec = true;
            e
        } else {
            s.from_exec = false;
            self.imem[s.pc as usize]
        };
        self.sm[k].instr = ins;
        let delay = self.side_set(k, ins);
        let step = self.run_sm(k, pins, ins);
        let s = &mut self.sm[k];
        match step {
            Step::Stall => {
                let first = !s.stalled;
                s.stalled = true;
                first
            }
            Step::Next => {
                s.stalled = false;
                if !s.from_exec {
                    s.pc = if s.pc == s.wrap_top() { s.wrap_bottom() } else { (s.pc + 1) & 0x1F };
                }
                s.delay = delay;
                true
            }
            Step::Jump(t) => {
                s.stalled = false;
                s.pc = t;
                s.delay = delay;
                true
            }
        }
    }

    fn load(&mut self, reg: u32) -> u32 {
        match reg {
            0x000 => self.enable,
            0x004 => {
                let mut v = 0;
                for (k, s) in self.sm.iter().enumerate() {
                    v |= u32::from(s.rx.n >= s.rx_depth()) << k
                        | u32::from(s.rx.n == 0) << (8 + k)
                        | u32::from(s.tx.n >= s.tx_depth()) << (16 + k)
                        | u32::from(s.tx.n == 0) << (24 + k);
                }
                v
            }
            0x008 => self.fdebug,
            0x00C => self.sm.iter().enumerate().fold(0, |v, (k, s)| v | ((s.tx.n as u32) | (s.rx.n as u32) << 4) << (8 * k)),
            0x020..0x030 => {
                let s = &mut self.sm[((reg - 0x20) / 4) as usize];
                if s.rx.n == 0 {
                    self.fdebug |= 1 << (8 + (reg - 0x20) / 4);
                    0
                } else {
                    s.rx.pop()
                }
            }
            0x030 => self.irq,
            0x038 => self.sync_bypass,
            0x03C => self.out,
            0x040 => self.oe,
            0x044 => 1 << 28 | 32 << 16 | 4 << 8 | 4,
            0x0C8..0x128 => {
                let (k, r) = (((reg - 0xC8) / 0x18) as usize, (reg - 0xC8) % 0x18);
                let s = &self.sm[k];
                match r {
                    0x00 => s.clkdiv,
                    0x04 => s.execctrl | u32::from(s.stalled) << 31,
                    0x08 => s.shiftctrl,
                    0x0C => s.pc,
                    0x10 => u32::from(s.instr),
                    _ => s.pinctrl,
                }
            }
            0x128..0x168 => {
                let i = ((reg - 0x128) / 4) as usize;
                self.sm[i / 4].rxreg[i % 4]
            }
            0x168 => self.gpiobase,
            0x16C => self.intr(),
            0x170 => self.inte[0],
            0x174 => self.intf[0],
            0x178 => self.ints(0),
            0x17C => self.inte[1],
            0x180 => self.intf[1],
            0x184 => self.ints(1),
            _ => 0,
        }
    }

    /// A store of `v` through `alias`; `cur` is the register's value.
    fn aliased(alias: u32, cur: u32, v: u32) -> u32 {
        match alias {
            1 => cur ^ v,
            2 => cur | v,
            3 => cur & !v,
            _ => v,
        }
    }

    fn store(&mut self, alias: u32, reg: u32, v: u32, pins: u32) {
        match reg {
            0x000 => {
                let v = Self::aliased(alias, self.enable, v);
                self.enable = v & 0xF;
                for k in 0..4 {
                    if v >> (4 + k) & 1 != 0 {
                        self.sm[k].restart();
                    }
                    if v >> (8 + k) & 1 != 0 {
                        self.sm[k].frac = 0;
                    }
                }
            }
            0x008 => self.fdebug &= !v,
            0x010..0x020 => {
                let k = ((reg - 0x10) / 4) as usize;
                let s = &mut self.sm[k];
                if s.tx.n >= s.tx_depth() {
                    self.fdebug |= 1 << (16 + k);
                } else {
                    s.tx.push(v);
                }
            }
            0x030 => self.irq &= !v,
            0x034 => self.irq |= v & 0xFF,
            0x038 => self.sync_bypass = Self::aliased(alias, self.sync_bypass, v),
            0x048..0x0C8 => self.imem[((reg - 0x48) / 4) as usize] = v as u16,
            0x0C8..0x128 => {
                let (k, r) = (((reg - 0xC8) / 0x18) as usize, (reg - 0xC8) % 0x18);
                let s = &mut self.sm[k];
                match r {
                    0x00 => s.clkdiv = Self::aliased(alias, s.clkdiv, v) & 0xFFFF_FF00,
                    0x04 => s.execctrl = Self::aliased(alias, s.execctrl, v) & 0x7FFF_FFFF,
                    0x08 => {
                        let n = Self::aliased(alias, s.shiftctrl, v);
                        if (n ^ s.shiftctrl) >> 30 != 0 {
                            s.tx = Fifo::default();
                            s.rx = Fifo::default();
                        }
                        s.shiftctrl = n;
                    }
                    0x10 => {
                        if self.enable >> k & 1 != 0 {
                            s.exec = Some(v as u16);
                        } else {
                            // a stopped machine runs it at once
                            let saved = (s.stalled, s.from_exec, s.delay);
                            let delay = self.side_set(k, v as u16);
                            let _ = delay;
                            let step = self.run_sm(k, pins, v as u16);
                            let s = &mut self.sm[k];
                            s.instr = v as u16;
                            match step {
                                Step::Jump(t) => s.pc = t,
                                Step::Stall => s.exec = Some(v as u16),
                                Step::Next => {}
                            }
                            (s.stalled, s.from_exec, s.delay) = saved;
                        }
                    }
                    0x14 => s.pinctrl = Self::aliased(alias, s.pinctrl, v),
                    _ => {}
                }
            }
            0x128..0x168 => {
                let i = ((reg - 0x128) / 4) as usize;
                self.sm[i / 4].rxreg[i % 4] = v;
            }
            0x168 => self.gpiobase = Self::aliased(alias, self.gpiobase, v) & 0x10,
            0x170 => self.inte[0] = Self::aliased(alias, self.inte[0], v) & 0xFFFF,
            0x174 => self.intf[0] = Self::aliased(alias, self.intf[0], v) & 0xFFFF,
            0x17C => self.inte[1] = Self::aliased(alias, self.inte[1], v) & 0xFFFF,
            0x180 => self.intf[1] = Self::aliased(alias, self.intf[1], v) & 0xFFFF,
            _ => {}
        }
    }
}

pub struct PioSim {
    blk: [Block; 2],
    hz: u64,
    last: Instant,
}

impl PioSim {
    pub fn new() -> Self {
        let hz = std::env::var("RVEMU_PIO_HZ").ok().and_then(|s| s.parse().ok()).unwrap_or(10_000_000);
        PioSim { blk: [Block::new(), Block::new()], hz, last: Instant::now() }
    }

    /// Cycles due since the last run.
    pub fn due(&mut self) -> u64 {
        let now = Instant::now();
        let ns = now.duration_since(self.last).as_nanos() as u64;
        let n = ns * self.hz / 1_000_000_000;
        if n > 0 {
            self.last = now;
        }
        n.min(MAX_CYCLES)
    }

    /// The pin block `blk` drives at GPIO `n`: Some(level) when it is an
    /// output there.
    pub fn drives(&self, blk: usize, n: u32) -> Option<bool> {
        let b = &self.blk[blk];
        let rel = n.checked_sub(b.gpiobase).filter(|&r| r < 32)?;
        (b.oe >> rel & 1 != 0).then_some(b.out >> rel & 1 != 0)
    }

    /// One cycle of every enabled state machine; (anything moved, the pins
    /// changed). `levels`: every GPIO's level.
    pub fn cycle(&mut self, levels: u64) -> (bool, bool) {
        let (mut moved, mut changed) = (false, false);
        for b in &mut self.blk {
            if b.enable == 0 {
                continue;
            }
            let pins = (levels >> b.gpiobase) as u32;
            let before = (b.out, b.oe);
            for k in 0..4 {
                if b.enable >> k & 1 != 0 {
                    moved |= b.cycle_sm(k, pins);
                }
            }
            changed |= (b.out, b.oe) != before;
        }
        (moved, changed)
    }

    pub fn load(&mut self, blk: usize, reg: u32) -> u32 {
        self.blk[blk].load(reg)
    }

    pub fn store(&mut self, blk: usize, alias: u32, reg: u32, v: u32, levels: u64) {
        let b = &mut self.blk[blk];
        let pins = (levels >> b.gpiobase) as u32;
        b.store(alias, reg, v, pins);
    }

    /// Room in state machine `sm`'s TX FIFO (for DMA).
    pub fn tx_free(&self, blk: usize, sm: usize) -> bool {
        let s = &self.blk[blk].sm[sm];
        s.tx.n < s.tx_depth()
    }
    pub fn push_tx(&mut self, blk: usize, sm: usize, v: u32) {
        self.blk[blk].sm[sm].tx.push(v);
    }
    /// A word in its RX FIFO.
    pub fn rx_ready(&self, blk: usize, sm: usize) -> bool {
        self.blk[blk].sm[sm].rx.n != 0
    }
    pub fn pop_rx(&mut self, blk: usize, sm: usize) -> u32 {
        self.blk[blk].sm[sm].rx.pop()
    }

    pub fn irqs(&self) -> u32 {
        (0..2).filter(|&b| self.blk[b].ints(0) != 0).fold(0, |m, b| m | 1 << b)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Load `src` into block 0 at 0 and set state machine 0 up as the SDK's
    /// default config with the program's wrap and side-set, plus `pinctrl`
    /// and `shiftctrl` bits.
    fn load(p: &mut PioSim, src: &str, pinctrl: u32, shiftctrl: u32) {
        let prog = pio_parser::Parser::<32>::parse_program(&format!("{src}\n")).unwrap().program;
        for (i, &w) in prog.code.iter().enumerate() {
            p.store(0, 0, 0x48 + 4 * i as u32, u32::from(w), 0);
        }
        let ss = prog.side_set;
        let exec = u32::from(prog.wrap.source) << 12
            | u32::from(prog.wrap.target) << 7
            | u32::from(ss.optional()) << 30
            | u32::from(ss.pindirs()) << 29;
        p.store(0, 0, 0xCC, exec, 0);
        p.store(0, 0, 0xD0, 0x000C_0000 | shiftctrl, 0);
        p.store(0, 0, 0xDC, u32::from(ss.bits()) << 29 | pinctrl, 0);
        p.store(0, 0, 0x000, 1, 0);
    }

    fn run(p: &mut PioSim, n: usize, levels: u64) {
        for _ in 0..n {
            p.cycle(levels);
        }
    }

    #[test]
    fn square_wave() {
        let mut p = PioSim::new();
        // SET pins at GP3, one pin
        load(&mut p, "set pindirs, 1\n.wrap_target\nset pins, 1 [3]\nset pins, 0 [3]\n.wrap", 3 << 5 | 1 << 26, 0);
        let mut seen = Vec::new();
        for _ in 0..24 {
            p.cycle(0);
            seen.push(p.drives(0, 3));
        }
        // after the pindirs: 4 cycles high, 4 low
        assert_eq!(seen[0], Some(false));
        assert_eq!(&seen[1..9], &[Some(true); 4].iter().chain(&[Some(false); 4]).copied().collect::<Vec<_>>()[..]);
        assert_eq!(seen[1..9], seen[9..17]);
    }

    #[test]
    fn fifo_echo_and_autopush() {
        let mut p = PioSim::new();
        load(&mut p, ".wrap_target\npull\nmov isr, ~osr\npush\n.wrap", 0, 0);
        p.store(0, 0, 0x10, 0x1234_5678, 0);
        p.store(0, 0, 0x10, 0, 0);
        run(&mut p, 20, 0);
        assert_eq!(p.load(0, 0x0C) >> 4 & 0xF, 2, "two words in RX");
        assert_eq!(p.load(0, 0x20), !0x1234_5678);
        assert_eq!(p.load(0, 0x20), !0);
        // empty: RXUNDER
        assert_eq!(p.load(0, 0x20), 0);
        assert_eq!(p.load(0, 0x08) >> 8 & 1, 1);
        // the machine waits on PULL: stalled
        assert_eq!(p.load(0, 0xCC) >> 31, 1);

        // IN pins, autopush at 8 bits, shifting left: GP0-GP3 = 0b1010
        let mut p = PioSim::new();
        load(&mut p, ".wrap_target\nin pins, 4\n.wrap", 0, 1 << 16 | 8 << 20);
        let sc = p.load(0, 0xD0) & !(1 << 18);
        p.store(0, 0, 0xD0, sc, 0);
        run(&mut p, 2, 0b1010);
        assert_eq!(p.load(0, 0x20), 0xAA);
    }

    #[test]
    fn side_set_and_out() {
        let mut p = PioSim::new();
        // out one bit at a time to GP5 with GP6 as a clock in side-set,
        // autopull at 4 bits, shifting right
        let src = ".side_set 1\n.wrap_target\nout pins, 1 side 0\nnop side 1\n.wrap";
        load(&mut p, src, 5 | 1 << 20 | 6 << 10, 1 << 17 | 4 << 25 | 1 << 19);
        p.store(0, 0, 0x10, 0b0110, 0);
        // the pins are outputs
        p.blk[0].oe = 1 << 5 | 1 << 6;
        let mut bits = Vec::new();
        for _ in 0..4 {
            p.cycle(0);
            p.cycle(0);
            bits.push(p.drives(0, 5).unwrap() as u32);
            assert_eq!(p.drives(0, 6), Some(true));
        }
        assert_eq!(bits, [0, 1, 1, 0]);
        // the FIFO is empty: OUT stalls, its side-set (0) still applies
        p.cycle(0);
        assert_eq!((p.drives(0, 6), p.load(0, 0xCC) >> 31), (Some(false), 1));
    }

    #[test]
    fn exec_and_irq() {
        let mut p = PioSim::new();
        // stopped machine: SMn_INSTR runs at once (set x, 21)
        let set_x = pio_parser::Parser::<32>::parse_program("set x, 21\n").unwrap().program.code[0];
        p.store(0, 0, 0xD8, u32::from(set_x), 0);
        assert_eq!(p.blk[0].sm[0].x, 21);
        // irq 2 rel on sm1 sets flag 3; INTR has it, IRQ0_INTE passes it
        load(&mut p, "irq 2 rel\nwait 0 irq 0", 0, 0);
        p.store(0, 0, 0, 2, 0); // only sm1
        p.blk[0].sm[1] = p.blk[0].sm[0];
        p.store(0, 0, 0x170, 1 << 11, 0);
        run(&mut p, 2, 0);
        assert_eq!(p.load(0, 0x30), 1 << 3);
        assert_eq!(p.irqs(), 1);
        p.store(0, 0, 0x30, 1 << 3, 0);
        assert_eq!(p.irqs(), 0);
    }
}
