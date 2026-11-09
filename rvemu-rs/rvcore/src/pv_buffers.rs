//! Experimental physical-buffer ABI; see README. Never accepts MMIO or
//! overlapping output/input/parameters. Errors may modify output.
use crate::{Bus, Cpu};
pub(crate) const EXT: u32 = 0x0A00_0001;
/// FID flag: the operation may return `ERR_PENDING` (see `pv_buffer`).
pub const MAY_PEND: u32 = 0x100;
/// Continue the paused operation for another slice.
pub const CONTINUE: u32 = 0x200;
/// Drop the paused operation.
pub const ABORT: u32 = 0x201;
/// The operation was paused: call `CONTINUE` (or `ABORT`).
pub const ERR_PENDING: i32 = -100;

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

    /// FIDs 0..13 run an operation to completion. With `MAY_PEND` set the
    /// operation may return `ERR_PENDING` after a time slice (only on a bus
    /// that preempts, `Bus::native_preemptible`); `CONTINUE` runs it for
    /// another slice, `ABORT` drops it. One operation is in progress at a
    /// time: a new one while one is paused fails with `ERR_BUSY`.
    /// Out of line: rare, and kept out of the SRAM-resident SBI dispatch.
    #[inline(never)]
    pub(crate) fn pv_buffer<B: Bus>(&mut self, fid: u32, bus: &mut B) -> (i32, u32) {
        if !self.native_buffers_supported() {
            return (-2, 0);
        }
        if fid == CONTINUE {
            // SAFETY: a paused job's buffers were validated by `start` and
            // lie in guest RAM, which the Cpu owns.
            let Some(w) = (unsafe { self.native_workspace.as_mut() }) else {
                return (-3, 0);
            };
            if !w.pending() {
                return (-3, 0);
            }
            bus.native_slice_begin();
            // SAFETY: as above.
            let step = unsafe { w.resume() };
            bus.native_slice_end(self);
            return self.native_step(step);
        }
        if fid == ABORT {
            // SAFETY: the workspace pointer is exclusively ours.
            if let Some(w) = unsafe { self.native_workspace.as_mut() }
                && w.pending()
            {
                w.abort();
                self.stats.native_aborts += 1;
            }
            self.native_job_dst = None;
            return (0, 0);
        }
        let may_pend = fid & MAY_PEND != 0;
        let fid = fid & !MAY_PEND;
        if fid >= rvaccel::OP_COUNT as u32 {
            return (-2, 0);
        }
        // SAFETY: as above.
        if unsafe { self.native_workspace.as_ref() }.is_some_and(|w| w.pending()) {
            return (rvaccel::ERR_BUSY, 0);
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
        // whole validated output capacity after every slice, before JIT reentry.
        self.native_job_dst = (cap != 0).then_some((dst, dst + cap));
        if let Some(w) = unsafe { self.native_workspace.as_mut() } {
            let preemptible = may_pend && bus.native_preemptible();
            bus.native_slice_begin();
            // SAFETY: all ranges checked, output disjoint from both inputs;
            // the Cpu owns the backing RAM, and the guest (the kernel's
            // bounce buffers) keeps them until the job is done or aborted.
            let step = unsafe {
                w.start(
                    fid,
                    self.ram.add(s),
                    len as usize,
                    self.ram.add(d),
                    cap as usize,
                    self.ram.add(p),
                    plen as usize,
                    preemptible,
                )
            };
            bus.native_slice_end(self);
            return self.native_step(step);
        }
        #[cfg(not(target_os = "none"))]
        {
            // SAFETY: as above; this call finishes here.
            let result = unsafe {
                let src = core::slice::from_raw_parts(self.ram.add(s), len as usize);
                let dst = core::slice::from_raw_parts_mut(self.ram.add(d), cap as usize);
                let params = core::slice::from_raw_parts(self.ram.add(p), plen as usize);
                rvaccel::run(fid, src, dst, params)
            };
            self.native_step(rvaccel::Step::Done(result))
        }
        #[cfg(target_os = "none")]
        (-2, 0)
    }

    fn native_step(&mut self, step: rvaccel::Step) -> (i32, u32) {
        if let Some(range) = self.native_job_dst {
            self.native_write = Some(range);
            self.res_valid = false;
        }
        match step {
            rvaccel::Step::Pending => {
                self.stats.native_preemptions += 1;
                (ERR_PENDING, 0)
            }
            rvaccel::Step::Done(r) => {
                self.native_job_dst = None;
                match r {
                    Ok(n) => (0, n as u32),
                    Err(e) => (e, 0),
                }
            }
        }
    }
}
