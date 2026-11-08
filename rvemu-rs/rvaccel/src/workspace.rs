//! Caller-owned native execution memory. No guest pointers or global lock.
use crate::{OP_COUNT, run_inner};

// Kept separate from the emulator's small control stack. Sized by RV32 tests.
pub const STACK_BYTES: usize = 24 * 1024;
#[cfg(target_arch = "riscv32")]
const GUARD_WORDS: usize = 64;
#[cfg(target_arch = "riscv32")]
const PAINT: u32 = 0x51ac51ac;

#[repr(C, align(16))]
/// One exclusively borrowed workspace per executing CPU. The RV32 callback
/// uses its own 24 KiB stack; a low-water guard detects budget regressions.
/// The guard is diagnostic, not hardware memory protection. No unwinding
/// across the assembly stack switch is supported (firmware uses panic=abort).
///
/// Between calls the whole stack holds the paint pattern. After a call the
/// first word that differs from it, counted from the bottom, is the call's
/// exact high-water mark; repainting from there to the top erases what the
/// call left on the stack. The cost is a read of the untouched part and a
/// write of the used part, not three passes over all 24 KiB (on the board the
/// workspace is in PSRAM, behind the 16 KB XIP cache).
pub struct Workspace {
    stack: [u32; STACK_BYTES / 4],
    peaks: [usize; OP_COUNT],
    /// The stack is painted (false after `new`/`init_at`: zeroed).
    painted: bool,
    kem: sntrup761::Workspace,
}
impl Workspace {
    pub const fn new() -> Self {
        Self {
            stack: [0; STACK_BYTES / 4],
            peaks: [0; OP_COUNT],
            painted: false,
            kem: sntrup761::Workspace::new(),
        }
    }
    /// Initialize directly in reserved firmware memory, without a large return temporary.
    /// # Safety
    /// `place` must be aligned, writable for Self, exclusively owned for the returned
    /// lifetime, and disjoint from guest RAM and all live stacks.
    pub unsafe fn init_at<'a>(place: *mut Self) -> &'a mut Self {
        // All fields, including vendored KEM scratch, are integer arrays or
        // a bool. Zero is valid for the entire representation.
        unsafe {
            core::ptr::write_bytes(place, 0, 1);
            &mut *place
        }
    }
    pub fn stack_peaks(&self) -> &[usize; OP_COUNT] {
        &self.peaks
    }
    /// Between calls: whether the execution stack holds nothing an operation
    /// left there (all paint, or all zero before the first call).
    pub fn stack_erased(&self) -> bool {
        #[cfg(target_arch = "riscv32")]
        {
            let fill = if self.painted { PAINT } else { 0 };
            self.stack.iter().all(|&w| w == fill)
        }
        #[cfg(not(target_arch = "riscv32"))]
        true
    }
    pub fn run(
        &mut self,
        fid: u32,
        src: &[u8],
        dst: &mut [u8],
        params: &[u8],
    ) -> Result<usize, i32> {
        if fid >= OP_COUNT as u32 {
            return Err(-2);
        }
        #[cfg(target_arch = "riscv32")]
        {
            if !self.painted {
                paint(&mut self.stack);
                self.painted = true;
            }
            let mut call = Call {
                fid,
                src,
                dst,
                params,
                work: &mut self.kem,
                result: Err(-1),
            };
            // SAFETY: stack has 16-byte alignment, is exclusively borrowed, and the
            // callback returns synchronously. Call and its slices stay on the caller's
            // stack/RAM. Rust code preserves s0, which holds the original stack pointer.
            unsafe {
                rvaccel_on_stack(
                    self.stack.as_mut_ptr().add(self.stack.len()),
                    (&mut call as *mut Call<'_>).cast(),
                );
            }
            let untouched = self.stack.iter().take_while(|&&x| x == PAINT).count();
            assert!(
                untouched >= GUARD_WORDS,
                "native buffer stack guard exhausted"
            );
            self.peaks[fid as usize] = self.peaks[fid as usize].max(STACK_BYTES - 4 * untouched);
            // Erase library temporaries outside the explicit arithmetic
            // workspace: every word below `untouched` still holds the paint.
            paint(&mut self.stack[untouched..]);
            call.result
        }
        #[cfg(not(target_arch = "riscv32"))]
        run_inner(fid, src, dst, params, &mut self.kem)
    }
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

#[cfg(target_arch = "riscv32")]
struct Call<'a> {
    fid: u32,
    src: &'a [u8],
    dst: &'a mut [u8],
    params: &'a [u8],
    work: &'a mut sntrup761::Workspace,
    result: Result<usize, i32>,
}
#[cfg(target_arch = "riscv32")]
unsafe extern "C" {
    fn rvaccel_on_stack(top: *mut u32, call: *mut core::ffi::c_void);
}
#[cfg(target_arch = "riscv32")]
extern "C" fn execute(call: &mut Call<'_>) {
    call.result = run_inner(call.fid, call.src, call.dst, call.params, call.work);
}
#[cfg(target_arch = "riscv32")]
core::arch::global_asm!(r#"
    .section .text.rv_on_stack,"ax"
    .balign 4
    .global rvaccel_on_stack
rvaccel_on_stack:
    addi sp, sp, -16
    sw ra, 12(sp)
    sw s0, 8(sp)
    mv s0, sp
    mv sp, a0
    mv a0, a1
    call {execute}
    mv sp, s0
    lw s0, 8(sp)
    lw ra, 12(sp)
    addi sp, sp, 16
    ret
"#, execute = sym execute);
