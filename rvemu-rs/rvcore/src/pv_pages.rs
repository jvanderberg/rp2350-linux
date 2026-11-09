//! Experimental ABI: EID 0x0A000000, FID 0 zero / 1 copy one 4 KiB page.
//! a0 = destination PA, a1 = source PA for copy; SBI error/value return.
//! Physical RAM only, no virtual translation or MMIO. Disabled by default.
use crate::Cpu;

pub(crate) const EXT: u32 = 0x0A00_0000;
const PAGE: u32 = 4096;

impl Cpu {
    #[inline(always)]
    fn pv_page_offset(&self, pa: u32, write: bool) -> Option<u32> {
        let off = pa.checked_sub(self.ram_base)?;
        if pa & (PAGE - 1) != 0
            || off.checked_add(PAGE)? > self.ram_size
            || (write && off < self.ro_size)
            || (self.ram as usize + off as usize) & 3 != 0
        {
            return None;
        }
        Some(off)
    }

    #[cfg_attr(feature = "ram-code", unsafe(link_section = ".data.rvemu_hot"))]
    pub(crate) fn pv_page(&mut self, fid: u32, dst: u32, src: u32) -> i32 {
        if fid > 1 {
            return -2;
        }
        let Some(d) = self.pv_page_offset(dst, true) else {
            return -3;
        };
        let s = if fid == 1 {
            let Some(s) = self.pv_page_offset(src, false) else {
                return -3;
            };
            s
        } else {
            0
        };
        // Complete validation before any writes. Aligned pages can only
        // overlap if identical, which is supported. Word accesses avoid
        // Hazard3's unsafe misaligned PSRAM accesses.
        for off in (0..PAGE).step_by(4) {
            let value = if fid == 1 {
                self.inst.load(s + off, 4);
                unsafe { self.ram.add((s + off) as usize).cast::<u32>().read() }
            } else {
                0
            };
            self.inst.store(d + off, 4);
            unsafe { self.ram.add((d + off) as usize).cast::<u32>().write(value) };
        }
        // One destination page fits the existing single-page SMC channel.
        // The JIT consumes it after the interpreted ecall, before re-entry.
        if self.is_code_page(dst) {
            self.smc_page = Some(dst);
        }
        self.res_valid = false;
        0
    }
}
