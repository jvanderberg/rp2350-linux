//! Hart state and the RV32IMA interpreter.
//!
//! There is no M-mode: the kernel runs in S-mode and its ecalls are handled
//! in `sbi.rs`. Every memory access goes through the soft TLB in `mmu.rs`.

use crate::instrument::{Instrument, Stats};

/// Guest timebase. Time is 1 tick per retired instruction plus ticks skipped
/// while idle, so runs are deterministic.
pub const TIMEBASE_HZ: u32 = 25_000_000;

pub const TLB_BITS: u32 = 8;
pub const TLB_SIZE: usize = 1 << TLB_BITS;

pub const ACC_R: usize = 0;
pub const ACC_W: usize = 1;
pub const ACC_X: usize = 2;
pub const ACC_N: usize = 3;

/// Permission contexts with separate TLB sets.
pub const MMU_U: usize = 0;
pub const MMU_S: usize = 1;
pub const MMU_S_SUM: usize = 2;
pub const MMU_N: usize = 3;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Priv {
    U = 0,
    S = 1,
}

pub mod exc {
    pub const INSN_MISALIGN: u32 = 0;
    pub const INSN_FAULT: u32 = 1;
    pub const ILLEGAL: u32 = 2;
    pub const BREAKPOINT: u32 = 3;
    pub const LOAD_MISALIGN: u32 = 4;
    pub const LOAD_FAULT: u32 = 5;
    pub const STORE_MISALIGN: u32 = 6;
    pub const STORE_FAULT: u32 = 7;
    pub const ECALL_U: u32 = 8;
    pub const INSN_PFAULT: u32 = 12;
    pub const LOAD_PFAULT: u32 = 13;
    pub const STORE_PFAULT: u32 = 15;
}

pub const CAUSE_IRQ: u32 = 0x8000_0000;
pub const IRQ_SSI: u32 = 1;
pub const IRQ_STI: u32 = 5;
pub const IRQ_SEI: u32 = 9;
pub const MIP_SSIP: u32 = 1 << IRQ_SSI;
pub const MIP_STIP: u32 = 1 << IRQ_STI;
pub const MIP_SEIP: u32 = 1 << IRQ_SEI;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Halt {
    Shutdown,
    Until,
    MaxInsns,
    Idle,
    User,
    /// The guest kernel panicked (the host saw its message).
    Panic,
}

/// One soft-TLB slot: `host = va + addend` for the page `tag`. An empty slot
/// holds a page number that cannot index it, so a hit is one xor + shift.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct TlbEntry {
    pub tag: u32,
    pub addend: usize,
}

/// Everything outside the hart: MMIO devices, console, idle policy.
pub trait Bus {
    fn mmio_load(&mut self, pa: u32, size: u32) -> Result<u32, u32>;
    fn mmio_store(&mut self, pa: u32, size: u32, val: u32) -> Result<(), u32>;
    /// Level of the PLIC's S-mode external interrupt output.
    fn external_irq(&self) -> bool;
    fn console_putc(&mut self, b: u8);
    fn console_getc(&mut self) -> Option<u8>;
    /// Guest executed WFI. May advance `cpu.time_skip` or set `cpu.halt`.
    fn wfi(&mut self, cpu: &mut Cpu);
    /// Guest RAM a device wrote since the last call (guest physical [lo, hi)),
    /// so a translator can drop code translated from it.
    fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        None
    }
}

/// Fields the block translator's generated code reads and writes. Kept
/// right after `x`/`pc` so they sit at small offsets from the state pointer.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct JitRegs {
    /// Instructions the current entry into translated code may still retire.
    pub budget: i32,
    /// Guest pc and exit reason written by the exit stub.
    pub exit_pc: u32,
    pub exit_info: u32,
    /// Host stack pointer saved by the entry trampoline.
    pub host_sp: usize,
    /// `&tlb[mmu_idx][acc][0]` for the current permission context.
    pub tlb_cur: [usize; ACC_N],
    /// Translator's jump cache (owned by the translator; 0 if none).
    pub jc_base: usize,
}

#[repr(C)]
pub struct Cpu {
    pub x: [u32; 32],
    pub pc: u32,
    pub jit: JitRegs,
    pub privilege: Priv,
    pub(crate) mmu_idx: usize,

    pub sie_bit: bool,
    pub spie: bool,
    pub spp: bool,
    pub sum: bool,
    pub mxr: bool,
    pub sie: u32,
    /// Software-held pending bits (SSIP). STIP is derived from the timer and
    /// SEIP from the bus when read.
    pub sip: u32,
    pub stvec: u32,
    pub sscratch: u32,
    pub sepc: u32,
    pub scause: u32,
    pub stval: u32,
    pub satp: u32,
    pub scounteren: u32,
    pub stimecmp: u64,
    pub instret: u64,
    pub time_skip: u64,
    pub(crate) res_valid: bool,
    pub(crate) res_addr: u32,

    pub(crate) ram: *mut u8,
    pub(crate) ram_base: u32,
    pub(crate) ram_size: u32,
    /// The first `ro_size` bytes of RAM are read-only (the RP2350 backs the
    /// kernel's text with flash there): stores to them fault.
    pub(crate) ro_size: u32,

    /// Translator support: one bit per RAM page that holds translated code.
    /// Null when no translator is attached. Writes to such pages never get
    /// a write-TLB entry; the first one sets `smc_page`.
    pub code_pages: *const u8,
    pub smc_page: Option<u32>,

    pub halt: Option<Halt>,
    pub stats: Stats,
    pub inst: Instrument,

    pub(crate) tlb: [[[TlbEntry; TLB_SIZE]; ACC_N]; MMU_N],
}

#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_i(insn: u32) -> u32 {
    ((insn as i32) >> 20) as u32
}
#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_s(insn: u32) -> u32 {
    ((((insn as i32) >> 25) << 5) as u32) | ((insn >> 7) & 0x1F)
}
#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_b(insn: u32) -> u32 {
    ((((insn as i32) >> 31) << 12) as u32)
        | ((insn << 4) & 0x800)
        | ((insn >> 20) & 0x7E0)
        | ((insn >> 7) & 0x1E)
}
#[inline(always)]
#[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
fn imm_j(insn: u32) -> u32 {
    ((((insn as i32) >> 31) << 20) as u32)
        | (insn & 0xF_F000)
        | ((insn >> 9) & 0x800)
        | ((insn >> 20) & 0x7FE)
}

impl Cpu {
    /// # Safety
    /// `ram` must point to `ram_size` writable bytes that outlive the `Cpu`
    /// and are not accessed through other references while it runs.
    pub unsafe fn new(ram: *mut u8, ram_base: u32, ram_size: u32, entry: u32, dtb: u32) -> Self {
        let mut c = Self::fresh(ram, ram_base, ram_size, entry);
        c.boot_regs(dtb);
        c
    }

    /// `new`, written straight into `place` and returned from there. The
    /// struct is ~20 KB (mostly TLB): built in place it needs no stack
    /// frame or return slot of that size (the RP2350 stack is ~48 KB).
    ///
    /// # Safety
    /// As `new`; `place` must be valid for writes and not in use.
    pub unsafe fn init<'a>(place: *mut Cpu, ram: *mut u8, ram_base: u32, ram_size: u32, entry: u32, dtb: u32) -> &'a mut Cpu {
        // SAFETY: per the contract.
        unsafe {
            place.write(Self::fresh(ram, ram_base, ram_size, entry));
            let c = &mut *place;
            c.boot_regs(dtb);
            c
        }
    }

    #[inline(always)]
    fn fresh(ram: *mut u8, ram_base: u32, ram_size: u32, entry: u32) -> Self {
        Cpu {
            x: [0; 32],
            pc: entry,
            jit: JitRegs { budget: 0, exit_pc: 0, exit_info: 0, host_sp: 0, tlb_cur: [0; ACC_N], jc_base: 0 },
            privilege: Priv::S,
            mmu_idx: MMU_S,
            sie_bit: false,
            spie: false,
            spp: false,
            sum: false,
            mxr: false,
            sie: 0,
            sip: 0,
            stvec: 0,
            sscratch: 0,
            sepc: 0,
            scause: 0,
            stval: 0,
            satp: 0,
            scounteren: 0,
            stimecmp: u64::MAX,
            instret: 0,
            time_skip: 0,
            res_valid: false,
            res_addr: 0,
            ram,
            ram_base,
            ram_size,
            ro_size: 0,
            code_pages: core::ptr::null(),
            smc_page: None,
            halt: None,
            stats: Stats::new(),
            inst: Instrument::new(ram_size),
            tlb: [[[TlbEntry { tag: 0, addend: 0 }; TLB_SIZE]; ACC_N]; MMU_N],
        }
    }

    fn boot_regs(&mut self, dtb: u32) {
        self.x[10] = 0; // hart id
        self.x[11] = dtb;
        self.tlb_flush_all();
        self.stats.tlb_flushes = 0;
    }

    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn time(&self) -> u64 {
        self.instret + self.time_skip
    }

    /// Pending interrupt bits as software sees them in `sip`.
    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn sip_value<B: Bus>(&self, bus: &B) -> u32 {
        let mut p = self.sip;
        if self.time() >= self.stimecmp {
            p |= MIP_STIP;
        }
        if bus.external_irq() {
            p |= MIP_SEIP;
        }
        p
    }

    pub(crate) fn update_mmu_idx(&mut self) {
        self.mmu_idx = match (self.privilege, self.sum) {
            (Priv::U, _) => MMU_U,
            (Priv::S, false) => MMU_S,
            (Priv::S, true) => MMU_S_SUM,
        };
        self.sync_tlb_ptrs();
    }

    /// Refresh `jit.tlb_cur`. Needed after the `Cpu` is moved.
    pub fn sync_tlb_ptrs(&mut self) {
        for acc in 0..ACC_N {
            self.jit.tlb_cur[acc] = self.tlb[self.mmu_idx][acc].as_ptr() as usize;
        }
    }

    /// Instructions that can retire before a timer interrupt could become
    /// pending (u64::MAX if none can, or one already is).
    pub fn timer_deadline(&self) -> u64 {
        let t = self.time();
        if self.stimecmp > t { self.stimecmp - t } else { u64::MAX }
    }

    pub fn ram_ptr(&self) -> *mut u8 {
        self.ram
    }
    pub fn ram_base(&self) -> u32 {
        self.ram_base
    }
    pub fn ram_size(&self) -> u32 {
        self.ram_size
    }
    /// Make the first `bytes` of RAM (page aligned) read-only to the guest.
    pub fn set_readonly_prefix(&mut self, bytes: u32) {
        self.ro_size = bytes & !0xFFF;
        self.tlb_flush_all();
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn trap(&mut self, cause: u32, tval: u32) {
        if cause & CAUSE_IRQ != 0 {
            self.stats.interrupts += 1;
        } else {
            self.stats.exceptions += 1;
        }
        self.scause = cause;
        self.stval = tval;
        self.sepc = self.pc;
        self.spp = self.privilege == Priv::S;
        self.spie = self.sie_bit;
        self.sie_bit = false;
        self.privilege = Priv::S;
        self.update_mmu_idx();
        self.res_valid = false;
        let base = self.stvec & !3;
        self.pc = if self.stvec & 1 != 0 && cause & CAUSE_IRQ != 0 {
            base.wrapping_add(4 * (cause & !CAUSE_IRQ))
        } else {
            base
        };
    }

    /// Take a pending, enabled interrupt if there is one.
    #[inline]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn check_irq<B: Bus>(&mut self, bus: &B) -> bool {
        let p = self.sip_value(bus) & self.sie;
        if p == 0 || (self.privilege == Priv::S && !self.sie_bit) {
            return false;
        }
        let irq = if p & MIP_SEIP != 0 {
            IRQ_SEI
        } else if p & MIP_SSIP != 0 {
            IRQ_SSI
        } else {
            IRQ_STI
        };
        self.trap(CAUSE_IRQ | irq, 0);
        true
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn sret(&mut self) {
        self.privilege = if self.spp { Priv::S } else { Priv::U };
        self.sie_bit = self.spie;
        self.spie = true;
        self.spp = false;
        self.update_mmu_idx();
        if self.privilege == Priv::U && self.stats.user_instret == 0 {
            self.stats.user_instret = self.instret + 1;
            self.stats.user_unique_insns = self.stats.unique_insns;
        }
    }

    // ------------------------------------------------------------ CSRs

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn csr_read<B: Bus>(&self, bus: &B, csr: u32) -> Result<u32, u32> {
        match csr {
            0xC00 | 0xC02 => return Ok(self.instret as u32),
            0xC80 | 0xC82 => return Ok((self.instret >> 32) as u32),
            0xC01 => return Ok(self.time() as u32),
            0xC81 => return Ok((self.time() >> 32) as u32),
            _ => {}
        }
        if self.privilege != Priv::S {
            return Err(exc::ILLEGAL);
        }
        Ok(match csr {
            0x100 => {
                (self.sie_bit as u32) << 1
                    | (self.spie as u32) << 5
                    | (self.spp as u32) << 8
                    | (self.sum as u32) << 18
                    | (self.mxr as u32) << 19
            }
            0x104 => self.sie,
            0x105 => self.stvec,
            0x106 => self.scounteren,
            0x140 => self.sscratch,
            0x141 => self.sepc,
            0x142 => self.scause,
            0x143 => self.stval,
            0x144 => self.sip_value(bus),
            0x180 => self.satp,
            _ => return Err(exc::ILLEGAL),
        })
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn csr_write(&mut self, csr: u32, v: u32) -> Result<(), u32> {
        if self.privilege != Priv::S || (csr >> 10) == 3 {
            return Err(exc::ILLEGAL);
        }
        match csr {
            0x100 => {
                let old_mxr = self.mxr;
                self.sie_bit = v & (1 << 1) != 0;
                self.spie = v & (1 << 5) != 0;
                self.spp = v & (1 << 8) != 0;
                self.sum = v & (1 << 18) != 0;
                self.mxr = v & (1 << 19) != 0;
                if self.mxr != old_mxr {
                    self.tlb_flush_all();
                }
                self.update_mmu_idx();
            }
            0x104 => self.sie = v & (MIP_SSIP | MIP_STIP | MIP_SEIP),
            0x105 => self.stvec = v & !2,
            0x106 => self.scounteren = v,
            0x140 => self.sscratch = v,
            0x141 => self.sepc = v & !3,
            0x142 => self.scause = v,
            0x143 => self.stval = v,
            0x144 => self.sip = (self.sip & !MIP_SSIP) | (v & MIP_SSIP),
            0x180 => {
                // Sv32 or bare; ASID bits not implemented (read back as 0)
                self.satp = if v >> 31 != 0 { v & !(0x1FF << 22) } else { 0 };
                self.tlb_flush_all();
            }
            _ => return Err(exc::ILLEGAL),
        }
        Ok(())
    }

    // ------------------------------------------------------------ execute

    /// Execute one instruction (or take the exception it raises).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn step<B: Bus>(&mut self, bus: &mut B) {
        let (insn, off) = match self.fetch(self.pc) {
            Ok(v) => v,
            Err(e) => return self.trap(e, self.pc),
        };
        self.inst.fetch(&mut self.stats, off);
        if let Err(e) = self.exec(bus, insn) {
            let (cause, tval) = e;
            self.trap(cause, tval);
        }
    }

    /// Ok(()) retires the instruction; Err((cause, tval)) traps.
    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn exec<B: Bus>(&mut self, bus: &mut B, insn: u32) -> Result<(), (u32, u32)> {
        let opcode = insn & 0x7F;
        let rd = ((insn >> 7) & 31) as usize;
        let rs1 = ((insn >> 15) & 31) as usize;
        let rs2 = ((insn >> 20) & 31) as usize;
        let f3 = (insn >> 12) & 7;
        let f7 = insn >> 25;
        let a = self.x[rs1];
        let b = self.x[rs2];
        #[cfg(feature = "instrument")]
        {
            let (r, s1, s2) = match opcode {
                0x37 | 0x17 | 0x6F => (true, false, false),
                0x67 | 0x03 | 0x13 => (true, true, false),
                0x63 | 0x23 => (false, true, true),
                0x33 | 0x2F => (true, true, true),
                _ => (false, false, false),
            };
            if r { self.stats.reg_use[rd] += 1; }
            if s1 { self.stats.reg_use[rs1] += 1; }
            if s2 { self.stats.reg_use[rs2] += 1; }
        }
        let pc = self.pc;
        let mut next = pc.wrapping_add(4);
        let illegal = Err((exc::ILLEGAL, insn));

        let val: Option<u32> = match opcode {
            0x37 => Some(insn & 0xFFFF_F000),
            0x17 => Some(pc.wrapping_add(insn & 0xFFFF_F000)),
            0x6F => {
                let link = next;
                next = pc.wrapping_add(imm_j(insn));
                Some(link)
            }
            0x67 => {
                if f3 != 0 {
                    return illegal;
                }
                let link = next;
                next = a.wrapping_add(imm_i(insn)) & !1;
                Some(link)
            }
            0x63 => {
                let t = match f3 {
                    0 => a == b,
                    1 => a != b,
                    4 => (a as i32) < (b as i32),
                    5 => (a as i32) >= (b as i32),
                    6 => a < b,
                    7 => a >= b,
                    _ => return illegal,
                };
                if t {
                    next = pc.wrapping_add(imm_b(insn));
                }
                None
            }
            0x03 => {
                let va = a.wrapping_add(imm_i(insn));
                let size = match f3 {
                    0 | 4 => 1,
                    1 | 5 => 2,
                    2 => 4,
                    _ => return illegal,
                };
                let v = self.mem_load(bus, va, size).map_err(|e| (e, va))?;
                Some(match f3 {
                    0 => v as u8 as i8 as i32 as u32,
                    1 => v as u16 as i16 as i32 as u32,
                    _ => v,
                })
            }
            0x23 => {
                let va = a.wrapping_add(imm_s(insn));
                if f3 > 2 {
                    return illegal;
                }
                self.mem_store(bus, va, 1 << f3, b).map_err(|e| (e, va))?;
                None
            }
            0x13 => {
                let imm = imm_i(insn);
                let sh = rs2 as u32;
                Some(match f3 {
                    0 => a.wrapping_add(imm),
                    1 => {
                        if f7 != 0 {
                            return illegal;
                        }
                        a << sh
                    }
                    2 => ((a as i32) < (imm as i32)) as u32,
                    3 => (a < imm) as u32,
                    4 => a ^ imm,
                    5 => match f7 {
                        0 => a >> sh,
                        0x20 => ((a as i32) >> sh) as u32,
                        _ => return illegal,
                    },
                    6 => a | imm,
                    _ => a & imm,
                })
            }
            0x33 => {
                if f7 == 1 {
                    Some(match f3 {
                        0 => a.wrapping_mul(b),
                        1 => (((a as i32 as i64) * (b as i32 as i64)) >> 32) as u32,
                        2 => (((a as i32 as i64) * (b as u64 as i64)) >> 32) as u32,
                        3 => (((a as u64) * (b as u64)) >> 32) as u32,
                        4 => {
                            if b == 0 {
                                u32::MAX
                            } else if a == 0x8000_0000 && b == u32::MAX {
                                a
                            } else {
                                ((a as i32) / (b as i32)) as u32
                            }
                        }
                        5 => a.checked_div(b).unwrap_or(u32::MAX),
                        6 => {
                            if b == 0 {
                                a
                            } else if a == 0x8000_0000 && b == u32::MAX {
                                0
                            } else {
                                ((a as i32) % (b as i32)) as u32
                            }
                        }
                        _ => a.checked_rem(b).unwrap_or(a),
                    })
                } else {
                    if f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5)) {
                        return illegal;
                    }
                    Some(match f3 {
                        0 => {
                            if f7 != 0 {
                                a.wrapping_sub(b)
                            } else {
                                a.wrapping_add(b)
                            }
                        }
                        1 => a << (b & 31),
                        2 => ((a as i32) < (b as i32)) as u32,
                        3 => (a < b) as u32,
                        4 => a ^ b,
                        5 => {
                            if f7 != 0 {
                                ((a as i32) >> (b & 31)) as u32
                            } else {
                                a >> (b & 31)
                            }
                        }
                        6 => a | b,
                        _ => a & b,
                    })
                }
            }
            0x0F => {
                // FENCE / FENCE.I: no-ops for a single in-order hart
                if f3 > 1 {
                    return illegal;
                }
                None
            }
            0x73 => {
                if f3 == 0 {
                    match insn {
                        0x0000_0073 => {
                            if self.privilege == Priv::U {
                                return Err((exc::ECALL_U, 0));
                            }
                            self.sbi_call(bus);
                        }
                        0x0010_0073 => return Err((exc::BREAKPOINT, pc)),
                        0x1020_0073 => {
                            if self.privilege != Priv::S {
                                return illegal;
                            }
                            next = self.sepc;
                            self.sret();
                        }
                        0x1050_0073 => {
                            if self.privilege != Priv::S {
                                return illegal;
                            }
                            self.stats.wfi += 1;
                            bus.wfi(self);
                        }
                        _ if f7 == 0x09 && rd == 0 => {
                            // SFENCE.VMA: rs1 = 0 flushes everything, otherwise one page
                            if self.privilege != Priv::S {
                                return illegal;
                            }
                            if rs1 == 0 {
                                self.tlb_flush_all();
                            } else {
                                self.tlb_flush_page(a);
                            }
                        }
                        _ => return illegal,
                    }
                    None
                } else if f3 != 4 {
                    let csr = insn >> 20;
                    let src = if f3 & 4 != 0 { rs1 as u32 } else { a };
                    let op = f3 & 3;
                    let mut old = 0;
                    if !(op == 1 && rd == 0) {
                        old = self.csr_read(bus, csr).map_err(|_| (exc::ILLEGAL, insn))?;
                    }
                    if op == 1 || rs1 != 0 {
                        let nv = match op {
                            1 => src,
                            2 => old | src,
                            _ => old & !src,
                        };
                        self.csr_write(csr, nv).map_err(|_| (exc::ILLEGAL, insn))?;
                    }
                    Some(old)
                } else {
                    return illegal;
                }
            }
            0x2F => {
                let f5 = insn >> 27;
                if f3 != 2 {
                    return illegal;
                }
                if a & 3 != 0 {
                    let cause = if f5 == 2 { exc::LOAD_MISALIGN } else { exc::STORE_MISALIGN };
                    return Err((cause, a));
                }
                match f5 {
                    0x02 => {
                        if rs2 != 0 {
                            return illegal;
                        }
                        let v = self.mem_load(bus, a, 4).map_err(|e| (e, a))?;
                        self.res_valid = true;
                        self.res_addr = a;
                        Some(v)
                    }
                    0x03 => {
                        let r = if self.res_valid && self.res_addr == a {
                            self.mem_store(bus, a, 4, b).map_err(|e| (e, a))?;
                            0
                        } else {
                            1
                        };
                        self.res_valid = false;
                        Some(r)
                    }
                    _ => {
                        let h = self.amo_ptr(a).map_err(|e| (e, a))?;
                        self.inst.store(self.host_off(h), 4);
                        // SAFETY: amo_ptr returned a pointer into guest RAM,
                        // 4-byte aligned within one page.
                        let old = unsafe { core::ptr::read_unaligned(h as *const u32) };
                        let nv = match f5 {
                            0x00 => old.wrapping_add(b),
                            0x01 => b,
                            0x04 => old ^ b,
                            0x08 => old | b,
                            0x0C => old & b,
                            0x10 => (old as i32).min(b as i32) as u32,
                            0x14 => (old as i32).max(b as i32) as u32,
                            0x18 => old.min(b),
                            0x1C => old.max(b),
                            _ => return illegal,
                        };
                        unsafe { core::ptr::write_unaligned(h as *mut u32, nv) };
                        Some(old)
                    }
                }
            }
            _ => return illegal,
        };

        if next & 3 != 0 {
            return Err((exc::INSN_MISALIGN, next));
        }
        if let Some(v) = val
            && rd != 0
        {
            self.x[rd] = v;
        }
        self.pc = next;
        self.instret += 1;
        self.stats.op[opcode as usize] += 1;
        Ok(())
    }
}
