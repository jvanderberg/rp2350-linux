//! Soft TLB, Sv32 page walk and guest memory access.
//!
//! The TLB is indexed `[mmu_idx][access][slot]`. A miss walks the guest page
//! tables, sets A/D bits, checks permissions and installs an entry; the
//! translator's slow path will call the same fill.

use crate::cpu::*;

const PTE_V: u32 = 0x01;
const PTE_R: u32 = 0x02;
const PTE_W: u32 = 0x04;
const PTE_X: u32 = 0x08;
const PTE_U: u32 = 0x10;
const PTE_A: u32 = 0x40;
const PTE_D: u32 = 0x80;
const PAGE_MASK: u32 = 0xFFF;

const PFAULT: [u32; ACC_N] = [exc::LOAD_PFAULT, exc::STORE_PFAULT, exc::INSN_PFAULT];
const AFAULT: [u32; ACC_N] = [exc::LOAD_FAULT, exc::STORE_FAULT, exc::INSN_FAULT];

impl Cpu {
    pub fn tlb_flush_all(&mut self) {
        for set in self.tlb.iter_mut() {
            for table in set.iter_mut() {
                for (i, e) in table.iter_mut().enumerate() {
                    // a page whose index bits point at a different slot
                    e.tag = (((i + 1) & (TLB_SIZE - 1)) as u32) << 12;
                    e.addend = 0;
                }
            }
        }
        self.stats.tlb_flushes += 1;
    }

    /// Drop the entries for one virtual page (all contexts and access types).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn tlb_flush_page(&mut self, va: u32) {
        let slot = ((va >> 12) as usize) & (TLB_SIZE - 1);
        for set in self.tlb.iter_mut() {
            for table in set.iter_mut() {
                let e = &mut table[slot];
                if (e.tag ^ va) >> 12 == 0 {
                    e.tag = (((slot + 1) & (TLB_SIZE - 1)) as u32) << 12;
                    e.addend = 0;
                }
            }
        }
        self.stats.tlb_page_flushes += 1;
    }

    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn tlb_lookup(&self, va: u32, acc: usize) -> Option<usize> {
        let slot = ((va >> 12) as usize) & (TLB_SIZE - 1);
        debug_assert!(self.mmu_idx < MMU_N && acc < ACC_N);
        // SAFETY: mmu_idx < MMU_N and acc < ACC_N by construction; slot is masked.
        let e = unsafe { self.tlb.get_unchecked(self.mmu_idx).get_unchecked(acc).get_unchecked(slot) };
        if (e.tag ^ va) >> 12 == 0 {
            Some((va as usize).wrapping_add(e.addend))
        } else {
            None
        }
    }

    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn is_code_page(&self, ppage: u32) -> bool {
        if self.code_pages.is_null() {
            return false;
        }
        let n = ((ppage - self.ram_base) >> 12) as usize;
        // SAFETY: the translator sizes the bitmap to cover all of RAM.
        unsafe { *self.code_pages.add(n >> 3) & (1 << (n & 7)) != 0 }
    }

    /// Drop write-TLB entries (all permission contexts) that map to the
    /// host page `host_page`, so stores to it take the slow path.
    pub fn tlb_evict_writes_to(&mut self, host_page: usize) {
        for set in self.tlb.iter_mut() {
            for (i, e) in set[ACC_W].iter_mut().enumerate() {
                if (e.tag as usize).wrapping_add(e.addend) == host_page {
                    e.tag = (((i + 1) & (TLB_SIZE - 1)) as u32) << 12;
                    e.addend = 0;
                }
            }
        }
    }

    /// Physical address `va` maps to for `acc` if the TLB already holds it
    /// (no walk, no fill).
    pub fn tlb_probe(&self, va: u32, acc: usize) -> Option<u32> {
        self.tlb_lookup(va, acc).map(|h| self.host_off(h) + self.ram_base)
    }

    /// Translate an instruction address: (physical address, host address).
    /// Fills the fetch TLB like a real fetch would.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn translate_pc(&mut self, pc: u32) -> Result<(u32, usize), u32> {
        let h = match self.tlb_lookup(pc, ACC_X) {
            Some(h) => h,
            None => match self.tlb_fill(pc, ACC_X)? {
                (_, Some(h)) => h,
                (_, None) => return Err(exc::INSN_FAULT),
            },
        };
        Ok((self.host_off(h) + self.ram_base, h))
    }

    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn host_off(&self, host: usize) -> u32 {
        (host - self.ram as usize) as u32
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn ram_word(&self, pa: u64) -> Option<*mut u32> {
        let base = self.ram_base as u64;
        if pa >= base && pa + 4 <= base + self.ram_size as u64 {
            // SAFETY: in bounds of the RAM block.
            Some(unsafe { self.ram.add((pa - base) as usize) } as *mut u32)
        } else {
            None
        }
    }

    /// Sv32 walk: physical address or exception cause.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    fn translate(&mut self, va: u32, acc: usize) -> Result<u32, u32> {
        if self.satp >> 31 == 0 {
            return Ok(va);
        }
        let mut table = ((self.satp & 0x3F_FFFF) as u64) << 12;
        for level in [1u32, 0] {
            let vpn = (va >> (12 + 10 * level)) & 0x3FF;
            let pte_p = self.ram_word(table + vpn as u64 * 4).ok_or(AFAULT[acc])?;
            // SAFETY: pte_p points into guest RAM (checked above).
            let pte = unsafe { pte_p.read_unaligned() };
            if pte & PTE_V == 0 || (pte & PTE_R == 0 && pte & PTE_W != 0) {
                return Err(PFAULT[acc]);
            }
            if pte & (PTE_R | PTE_X) == 0 {
                table = ((pte >> 10) as u64) << 12;
                continue;
            }

            let ppn = pte >> 10;
            if level == 1 && ppn & 0x3FF != 0 {
                return Err(PFAULT[acc]); // misaligned superpage
            }
            let ok = match acc {
                ACC_X => pte & PTE_X != 0,
                ACC_R => pte & PTE_R != 0 || (self.mxr && pte & PTE_X != 0),
                _ => pte & PTE_W != 0,
            };
            if !ok {
                return Err(PFAULT[acc]);
            }
            if self.privilege == Priv::U {
                if pte & PTE_U == 0 {
                    return Err(PFAULT[acc]);
                }
            } else if pte & PTE_U != 0 && (acc == ACC_X || !self.sum) {
                return Err(PFAULT[acc]);
            }

            let updated = pte | PTE_A | if acc == ACC_W { PTE_D } else { 0 };
            if updated != pte {
                unsafe { pte_p.write_unaligned(updated) };
            }

            let mut p = (ppn as u64) << 12;
            p |= if level == 1 { (va & 0x3F_FFFF) as u64 } else { (va & PAGE_MASK) as u64 };
            if p >> 32 != 0 {
                return Err(AFAULT[acc]);
            }
            return Ok(p as u32);
        }
        Err(PFAULT[acc])
    }

    /// A TLB miss in translated code: the host address of `va` once an entry
    /// for it is installed, or None (MMIO, a fault, a write to a code page)
    /// for the interpreter to do the access. Changes nothing the
    /// interpreter's own fill would not.
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub fn tlb_refill(&mut self, va: u32, acc: usize) -> Option<usize> {
        let smc = self.smc_page;
        match self.tlb_fill(va, acc) {
            Ok((_, Some(h))) if self.tlb_lookup(va, acc).is_some() => Some(h),
            _ => {
                self.smc_page = smc;
                None
            }
        }
    }

    /// TLB miss: translate and, if the target is RAM, install an entry.
    /// Returns the physical address and the host address (None for MMIO).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn tlb_fill(&mut self, va: u32, acc: usize) -> Result<(u32, Option<usize>), u32> {
        self.stats.tlb_miss[acc] += 1;
        let pa = self.translate(va, acc)?;
        let ppage = pa & !PAGE_MASK;
        if ppage >= self.ram_base && ppage - self.ram_base < self.ram_size {
            if acc == ACC_W && ppage - self.ram_base < self.ro_size {
                return Err(AFAULT[ACC_W]);
            }
            let hp = self.ram as usize + (ppage - self.ram_base) as usize;
            if acc == ACC_W && self.is_code_page(ppage) {
                // translated code lives here: no fast-path entry, and tell
                // the translator to drop it
                self.smc_page = Some(ppage);
                return Ok((pa, Some(hp + (va & PAGE_MASK) as usize)));
            }
            let vpage = va & !PAGE_MASK;
            let slot = ((va >> 12) as usize) & (TLB_SIZE - 1);
            let e = &mut self.tlb[self.mmu_idx][acc][slot];
            e.tag = vpage;
            e.addend = hp.wrapping_sub(vpage as usize);
            Ok((pa, Some(hp + (va & PAGE_MASK) as usize)))
        } else {
            Ok((pa, None))
        }
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn mem_load<B: Bus>(&mut self, bus: &mut B, va: u32, size: u32) -> Result<u32, u32> {
        if (va & PAGE_MASK) + size <= 0x1000 {
            let h = match self.tlb_lookup(va, ACC_R) {
                Some(h) => h,
                None => match self.tlb_fill(va, ACC_R)? {
                    (_, Some(h)) => h,
                    (pa, None) => return bus.mmio_load(pa, size),
                },
            };
            self.inst.load(self.host_off(h), size);
            // SAFETY: h is within a RAM page and the access does not cross it.
            return Ok(unsafe {
                match size {
                    1 => *(h as *const u8) as u32,
                    2 => (h as *const u16).read_unaligned() as u32,
                    _ => (h as *const u32).read_unaligned(),
                }
            });
        }
        // misaligned access crossing a page boundary
        self.inst.misaligned();
        let mut v = 0;
        for i in 0..size {
            v |= self.mem_load(bus, va.wrapping_add(i), 1)? << (8 * i);
        }
        Ok(v)
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn mem_store<B: Bus>(&mut self, bus: &mut B, va: u32, size: u32, val: u32) -> Result<(), u32> {
        if (va & PAGE_MASK) + size <= 0x1000 {
            let h = match self.tlb_lookup(va, ACC_W) {
                Some(h) => h,
                None => match self.tlb_fill(va, ACC_W)? {
                    (_, Some(h)) => h,
                    (pa, None) => return bus.mmio_store(pa, size, val),
                },
            };
            self.inst.store(self.host_off(h), size);
            // SAFETY: as in mem_load.
            unsafe {
                match size {
                    1 => *(h as *mut u8) = val as u8,
                    2 => (h as *mut u16).write_unaligned(val as u16),
                    _ => (h as *mut u32).write_unaligned(val),
                }
            }
            return Ok(());
        }
        for i in 0..size {
            self.mem_store(bus, va.wrapping_add(i), 1, (val >> (8 * i)) & 0xFF)?;
        }
        Ok(())
    }

    /// Host pointer for an AMO (needs write permission; MMIO unsupported).
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn amo_ptr(&mut self, va: u32) -> Result<usize, u32> {
        if let Some(h) = self.tlb_lookup(va, ACC_W) {
            return Ok(h);
        }
        match self.tlb_fill(va, ACC_W)? {
            (_, Some(h)) => Ok(h),
            (_, None) => Err(exc::STORE_FAULT),
        }
    }

    /// Fetch an instruction word; returns it and its offset in RAM.
    #[inline(always)]
    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn fetch(&mut self, pc: u32) -> Result<(u32, u32), u32> {
        let h = match self.tlb_lookup(pc, ACC_X) {
            Some(h) => h,
            None => match self.tlb_fill(pc, ACC_X)? {
                (_, Some(h)) => h,
                (_, None) => return Err(exc::INSN_FAULT),
            },
        };
        // SAFETY: h is within a RAM page; pc is 4-aligned (checked at jumps).
        let insn = unsafe { (h as *const u32).read_unaligned() };
        Ok((insn, self.host_off(h)))
    }
}
