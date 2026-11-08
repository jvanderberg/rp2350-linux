//! Experimental physical-buffer ABI; see README. Never accepts MMIO or
//! overlapping output/input/parameters. Errors may modify output.
use crate::Cpu;
pub(crate) const EXT: u32 = 0x0A00_0001;

impl Cpu {
    /// Transfer exclusive ownership of reserved native memory to this CPU.
    pub fn set_native_workspace(&mut self, workspace: &'static mut rvaccel::Workspace) {
        self.native_workspace = workspace;
    }
    pub fn native_stack_peaks(&self) -> Option<&[usize; rvaccel::OP_COUNT]> {
        unsafe { self.native_workspace.as_ref().map(|w| w.stack_peaks()) }
    }
    /// Whether the workspace's execution stack is erased (see
    /// `rvaccel::Workspace::stack_erased`); `None` without a workspace.
    pub fn native_stack_erased(&self) -> Option<bool> {
        unsafe { self.native_workspace.as_ref().map(|w| w.stack_erased()) }
    }
    pub(crate) fn native_buffers_supported(&self) -> bool {
        !self.native_workspace.is_null() || !cfg!(target_os = "none")
    }

    fn buffer_offset(&self, pa: u32, len: u32, write: bool) -> Option<usize> {
        let off = pa.checked_sub(self.ram_base)?;
        pa.checked_add(len)?;
        if off.checked_add(len)? > self.ram_size || (write && len != 0 && off < self.ro_size) {
            return None;
        }
        Some(off as usize)
    }

    pub(crate) fn pv_buffer(&mut self, fid: u32) -> (i32, u32) {
        if !self.native_buffers_supported() || fid >= rvaccel::OP_COUNT as u32 {
            return (-2, 0);
        }
        self.stats.native_buffer_calls[fid as usize] += 1;
        let [dst, cap, src, len, params, plen] = self.x[10..16].try_into().unwrap();
        if cap > rvaccel::MAX_BUFFER as u32
            || len > rvaccel::MAX_BUFFER as u32
            || plen > rvaccel::MAX_PARAMS as u32
        {
            return (-3, 0);
        }
        let Some(d) = self.buffer_offset(dst, cap, true) else {
            return (-3, 0);
        };
        let Some(s) = self.buffer_offset(src, len, false) else {
            return (-3, 0);
        };
        let p = if plen == 0 {
            0
        } else {
            let Some(p) = self.buffer_offset(params, plen, false) else {
                return (-3, 0);
            };
            p
        };
        let overlap =
            |a: usize, n: usize, b: usize, m: usize| n != 0 && m != 0 && a < b + m && b < a + n;
        if overlap(d, cap as usize, s, len as usize) || overlap(d, cap as usize, p, plen as usize) {
            return (-3, 0);
        }
        // Native libraries can partially write on failure. Invalidate the
        // whole validated output capacity on either outcome, before JIT reentry.
        if cap != 0 {
            self.native_write = Some((dst, dst + cap));
            self.res_valid = false;
        }
        // SAFETY: all ranges checked, output disjoint from both inputs,
        // and Cpu owns the backing RAM for the duration of this call.
        let result = unsafe {
            let src = core::slice::from_raw_parts(self.ram.add(s), len as usize);
            let dst = core::slice::from_raw_parts_mut(self.ram.add(d), cap as usize);
            let params = core::slice::from_raw_parts(self.ram.add(p), plen as usize);
            if let Some(workspace) = self.native_workspace.as_mut() {
                workspace.run(fid, src, dst, params)
            } else {
                #[cfg(not(target_os = "none"))]
                {
                    rvaccel::run(fid, src, dst, params)
                }
                #[cfg(target_os = "none")]
                {
                    Err(-2)
                }
            }
        };
        match result {
            Ok(n) => (0, n as u32),
            Err(e) => (e, 0),
        }
    }
}
