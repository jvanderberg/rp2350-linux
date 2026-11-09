//! Caller-owned native execution memory. No guest pointers or global lock.
use crate::{OP_COUNT, run_inner};

// Kept separate from the emulator's small control stack. Sized by RV32 tests.
pub const STACK_BYTES: usize = 24 * 1024;
#[cfg(target_arch = "riscv32")]
const GUARD_WORDS: usize = 64;
#[cfg(target_arch = "riscv32")]
const PAINT: u32 = 0x51ac51ac;

/// What one slice of a native call came to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// The operation finished (its output length) or failed.
    Done(Result<usize, i32>),
    /// The operation was preempted (`Workspace::preempt`); `resume` continues
    /// it, `abort` drops it.
    Pending,
}

/// Error for a new operation while another one is paused.
pub const ERR_BUSY: i32 = -7;

const IDLE: u32 = 0;
const RUNNING: u32 = 1;
#[cfg(target_arch = "riscv32")]
const FINISHED: u32 = 2;

/// The operation in progress: what `start` was given, kept here (not on the
/// caller's stack) so it survives between slices.
#[derive(Clone, Copy)]
#[repr(C)]
struct Job {
    fid: u32,
    src: *const u8,
    src_len: usize,
    dst: *mut u8,
    dst_len: usize,
    params: *const u8,
    params_len: usize,
    /// Output length, or the (negative) error.
    result: isize,
}

impl Job {
    const NONE: Self = Self {
        fid: 0,
        src: core::ptr::null(),
        src_len: 0,
        dst: core::ptr::null_mut(),
        dst_len: 0,
        params: core::ptr::null(),
        params_len: 0,
        result: -1,
    };
}

#[repr(C, align(16))]
/// One exclusively borrowed workspace per executing CPU. The RV32 operation
/// runs on its own 24 KiB stack, as a coroutine of the caller: `start` runs
/// it until it finishes or until a timer interrupt the firmware arms calls
/// `preempt` (only when `start` was asked to allow it), and `resume`
/// continues a preempted one where it stopped. One operation at a time.
/// A low-water guard detects budget regressions; it is diagnostic, not
/// hardware memory protection. No unwinding across the stack switch is
/// supported (firmware uses panic=abort).
///
/// Between operations the whole stack holds the paint pattern. When one
/// finishes, the first word that differs from it, counted from the bottom,
/// is its exact high-water mark; repainting from there to the top erases
/// what it left on the stack. The cost is a read of the untouched part and a
/// write of the used part, not three passes over all 24 KiB (on the board the
/// workspace is in PSRAM, behind the 16 KB XIP cache).
pub struct Workspace {
    stack: [u32; STACK_BYTES / 4],
    peaks: [usize; OP_COUNT],
    /// The stack is painted (false after `new`/`init_at`: zeroed).
    painted: bool,
    /// The running operation enables interrupts (it may be preempted).
    preemptible: bool,
    /// The caller is suspended in `slice` and the operation is running.
    in_slice: bool,
    state: u32,
    /// Saved stack pointers of the caller and of the operation.
    caller_sp: usize,
    op_sp: usize,
    job: Job,
    kem: sntrup761::Workspace,
}

impl Workspace {
    pub const fn new() -> Self {
        Self {
            stack: [0; STACK_BYTES / 4],
            peaks: [0; OP_COUNT],
            painted: false,
            preemptible: false,
            in_slice: false,
            state: IDLE,
            caller_sp: 0,
            op_sp: 0,
            job: Job::NONE,
            kem: sntrup761::Workspace::new(),
        }
    }
    /// Initialize directly in reserved firmware memory, without a large return temporary.
    /// # Safety
    /// `place` must be aligned, writable for Self, exclusively owned for the returned
    /// lifetime, and disjoint from guest RAM and all live stacks.
    pub unsafe fn init_at<'a>(place: *mut Self) -> &'a mut Self {
        // All fields, including vendored KEM scratch, are integers, bools or
        // raw pointers. Zero is valid for the entire representation.
        unsafe {
            core::ptr::write_bytes(place, 0, 1);
            &mut *place
        }
    }
    pub fn stack_peaks(&self) -> &[usize; OP_COUNT] {
        &self.peaks
    }
    /// Between operations: whether the execution stack holds nothing an
    /// operation left there (all paint, or all zero before the first one).
    pub fn stack_erased(&self) -> bool {
        #[cfg(target_arch = "riscv32")]
        {
            let fill = if self.painted { PAINT } else { 0 };
            self.stack.iter().all(|&w| w == fill)
        }
        #[cfg(not(target_arch = "riscv32"))]
        true
    }
    /// An operation is paused (`start` or `resume` returned `Pending`).
    pub fn pending(&self) -> bool {
        self.state == RUNNING
    }
    /// Run one operation to completion.
    pub fn run(&mut self, fid: u32, src: &[u8], dst: &mut [u8], params: &[u8]) -> Result<usize, i32> {
        if self.pending() {
            return Err(ERR_BUSY);
        }
        // SAFETY: the slices outlive the call, which finishes before
        // returning: without `preemptible` nothing can preempt it.
        let mut step = unsafe {
            self.start(
                fid,
                src.as_ptr(),
                src.len(),
                dst.as_mut_ptr(),
                dst.len(),
                params.as_ptr(),
                params.len(),
                false,
            )
        };
        while step == Step::Pending {
            // SAFETY: as above.
            step = unsafe { self.resume() };
        }
        match step {
            Step::Done(r) => r,
            Step::Pending => unreachable!(),
        }
    }
    /// Start an operation. With `preemptible`, it runs with machine
    /// interrupts enabled, and a timer interrupt whose handler calls
    /// `preempt` pauses it (`Pending`).
    /// # Safety
    /// The three buffers must stay valid, and `dst` must not be accessed by
    /// anyone else, until the operation is `Done` or aborted. With
    /// `preemptible`, the only interrupt that may be enabled is one whose
    /// handler calls `preempt` (or returns without touching this workspace).
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn start(
        &mut self,
        fid: u32,
        src: *const u8,
        src_len: usize,
        dst: *mut u8,
        dst_len: usize,
        params: *const u8,
        params_len: usize,
        preemptible: bool,
    ) -> Step {
        if self.pending() {
            return Step::Done(Err(ERR_BUSY));
        }
        if fid >= OP_COUNT as u32 {
            return Step::Done(Err(-2));
        }
        self.job = Job { fid, src, src_len, dst, dst_len, params, params_len, result: -1 };
        #[cfg(target_arch = "riscv32")]
        {
            if !self.painted {
                paint(&mut self.stack);
                self.painted = true;
            }
            // The operation's first frame: `rvaccel_switch` pops it and
            // returns into `rvaccel_op_entry` with s0 = this workspace.
            let me = self as *mut Self as usize as u32;
            let top = self.stack.len();
            let frame = &mut self.stack[top - SWITCH_WORDS..];
            frame.fill(0);
            frame[0] = rvaccel_op_entry as *const () as usize as u32;
            frame[1] = me;
            self.op_sp = frame.as_ptr() as usize;
            self.preemptible = preemptible;
            self.state = RUNNING;
            self.slice()
        }
        #[cfg(not(target_arch = "riscv32"))]
        {
            let _ = preemptible;
            // SAFETY: the caller's contract; the call finishes here.
            let r = unsafe { self.job.call(&mut self.kem) };
            Step::Done(r)
        }
    }
    /// Continue the paused operation.
    /// # Safety
    /// As for `start`.
    pub unsafe fn resume(&mut self) -> Step {
        if !self.pending() {
            return Step::Done(Err(-3));
        }
        #[cfg(target_arch = "riscv32")]
        {
            self.slice()
        }
        #[cfg(not(target_arch = "riscv32"))]
        Step::Done(Err(-3))
    }
    /// Drop the paused operation (its output is incomplete) and erase what
    /// it left: its stack and the KEM scratch.
    pub fn abort(&mut self) {
        if !self.pending() {
            return;
        }
        self.state = IDLE;
        self.job = Job::NONE;
        #[cfg(target_arch = "riscv32")]
        {
            let untouched = self.stack.iter().take_while(|&&x| x == PAINT).count();
            paint(&mut self.stack[untouched..]);
            let kem = (&mut self.kem as *mut sntrup761::Workspace).cast::<u8>();
            for i in 0..core::mem::size_of::<sntrup761::Workspace>() {
                // SAFETY: bytes of the exclusively borrowed scratch (integer arrays).
                unsafe { core::ptr::write_volatile(kem.add(i), 0) };
            }
        }
    }
    /// From a machine interrupt handler: pause the running operation and
    /// return to the caller of `start`/`resume` (`Pending`). Returns when the
    /// operation is resumed. Does nothing unless an operation is running.
    /// # Safety
    /// `ws` is the workspace whose operation the interrupt may have
    /// interrupted; call only from that hart's interrupt handler.
    pub unsafe fn preempt(ws: *mut Self) {
        #[cfg(target_arch = "riscv32")]
        unsafe {
            if !core::ptr::read_volatile(core::ptr::addr_of!((*ws).in_slice)) {
                return;
            }
            // The interrupted state the trap entry did not save; nothing else
            // traps on this hart while the caller runs, but keep them exact.
            let (epc, status): (usize, usize);
            core::arch::asm!("csrr {0}, mepc", "csrr {1}, mstatus", out(reg) epc, out(reg) status);
            let caller = core::ptr::read_volatile(core::ptr::addr_of!((*ws).caller_sp));
            rvaccel_switch(core::ptr::addr_of_mut!((*ws).op_sp), caller);
            core::arch::asm!("csrw mepc, {0}", "csrw mstatus, {1}", in(reg) epc, in(reg) status);
        }
        #[cfg(not(target_arch = "riscv32"))]
        let _ = ws;
    }

    /// Switch to the operation until it finishes or is preempted.
    #[cfg(target_arch = "riscv32")]
    fn slice(&mut self) -> Step {
        let ws = self as *mut Self;
        // SAFETY: the operation runs on its own stack and touches only its
        // job, the KEM scratch and its stack; this frame is suspended
        // meanwhile and reads the state only through `ws` afterwards.
        unsafe {
            core::ptr::write_volatile(core::ptr::addr_of_mut!((*ws).in_slice), true);
            let op = core::ptr::read_volatile(core::ptr::addr_of!((*ws).op_sp));
            rvaccel_switch(core::ptr::addr_of_mut!((*ws).caller_sp), op);
            core::ptr::write_volatile(core::ptr::addr_of_mut!((*ws).in_slice), false);
            let state = core::ptr::read_volatile(core::ptr::addr_of!((*ws).state));
            let ws = &mut *ws;
            if state == RUNNING {
                assert!(
                    ws.stack[..GUARD_WORDS].iter().all(|&w| w == PAINT),
                    "native buffer stack guard exhausted"
                );
                return Step::Pending;
            }
            let untouched = ws.stack.iter().take_while(|&&x| x == PAINT).count();
            assert!(untouched >= GUARD_WORDS, "native buffer stack guard exhausted");
            let fid = ws.job.fid as usize;
            ws.peaks[fid] = ws.peaks[fid].max(STACK_BYTES - 4 * untouched);
            // Erase library temporaries outside the explicit arithmetic
            // workspace: every word below `untouched` still holds the paint.
            paint(&mut ws.stack[untouched..]);
            let r = ws.job.result;
            ws.job = Job::NONE;
            ws.state = IDLE;
            Step::Done(if r < 0 { Err(r as i32) } else { Ok(r as usize) })
        }
    }
}

impl Job {
    /// # Safety
    /// The buffers are valid and `dst` is exclusively ours.
    unsafe fn call(&self, kem: &mut sntrup761::Workspace) -> Result<usize, i32> {
        unsafe {
            let src = slice_or_empty(self.src, self.src_len);
            let params = slice_or_empty(self.params, self.params_len);
            let dst: &mut [u8] = if self.dst_len == 0 {
                &mut []
            } else {
                core::slice::from_raw_parts_mut(self.dst, self.dst_len)
            };
            run_inner(self.fid, src, dst, params, kem)
        }
    }
}

unsafe fn slice_or_empty<'a>(p: *const u8, n: usize) -> &'a [u8] {
    if n == 0 { &[] } else { unsafe { core::slice::from_raw_parts(p, n) } }
}

impl Default for Workspace {
    fn default() -> Self {
        Self::new()
    }
}

/// Overwrite `words` with the paint pattern; volatile, so the erasure of
/// dead stack contents is never optimized away.
#[cfg(target_arch = "riscv32")]
fn paint(words: &mut [u32]) {
    for w in words.iter_mut() {
        // SAFETY: a valid, exclusively borrowed word.
        unsafe { core::ptr::write_volatile(w, PAINT) };
    }
    core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
}

/// Words `rvaccel_switch` keeps on a suspended stack: ra, s0-s11, padding.
#[cfg(target_arch = "riscv32")]
const SWITCH_WORDS: usize = 16;

#[cfg(target_arch = "riscv32")]
unsafe extern "C" {
    /// Save ra and s0-s11 on this stack, store sp in `*save`, switch to the
    /// stack `to` (saved by an earlier switch, or `start`'s first frame) and
    /// return into it.
    fn rvaccel_switch(save: *mut usize, to: usize);
    fn rvaccel_op_entry();
}

/// The operation's whole life on its own stack: run it (interrupts on when
/// preemptible), record the result, and switch back to the caller for good.
#[cfg(target_arch = "riscv32")]
extern "C" fn rvaccel_op_main(ws: *mut Workspace) -> ! {
    // SAFETY: `start` set up the job; the caller is suspended in `slice`.
    unsafe {
        let job = core::ptr::read(core::ptr::addr_of!((*ws).job));
        let preemptible = core::ptr::read_volatile(core::ptr::addr_of!((*ws).preemptible));
        if preemptible {
            core::arch::asm!("csrsi mstatus, 8");
        }
        let r = job.call(&mut *core::ptr::addr_of_mut!((*ws).kem));
        // never preempted from here on: the job is done
        core::arch::asm!("csrci mstatus, 8");
        let r = match r {
            Ok(n) => n as isize,
            Err(e) => e as isize,
        };
        core::ptr::write(core::ptr::addr_of_mut!((*ws).job.result), r);
        core::ptr::write_volatile(core::ptr::addr_of_mut!((*ws).state), FINISHED);
        let caller = core::ptr::read_volatile(core::ptr::addr_of!((*ws).caller_sp));
        rvaccel_switch(core::ptr::addr_of_mut!((*ws).op_sp), caller);
    }
    unreachable!("a finished native operation was resumed")
}

#[cfg(target_arch = "riscv32")]
core::arch::global_asm!(r#"
    .section .text.rvaccel_switch,"ax"
    .balign 4
    .global rvaccel_switch
rvaccel_switch:
    addi sp, sp, -64
    sw ra, 0(sp)
    sw s0, 4(sp)
    sw s1, 8(sp)
    sw s2, 12(sp)
    sw s3, 16(sp)
    sw s4, 20(sp)
    sw s5, 24(sp)
    sw s6, 28(sp)
    sw s7, 32(sp)
    sw s8, 36(sp)
    sw s9, 40(sp)
    sw s10, 44(sp)
    sw s11, 48(sp)
    sw sp, 0(a0)
    mv sp, a1
    lw ra, 0(sp)
    lw s0, 4(sp)
    lw s1, 8(sp)
    lw s2, 12(sp)
    lw s3, 16(sp)
    lw s4, 20(sp)
    lw s5, 24(sp)
    lw s6, 28(sp)
    lw s7, 32(sp)
    lw s8, 36(sp)
    lw s9, 40(sp)
    lw s10, 44(sp)
    lw s11, 48(sp)
    addi sp, sp, 64
    ret

    .global rvaccel_op_entry
rvaccel_op_entry:
    mv a0, s0
    call {main}
"#, main = sym rvaccel_op_main);
