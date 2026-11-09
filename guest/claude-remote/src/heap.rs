//! The allocator: blocks of 2 KiB and up are their own anonymous mappings,
//! returned to the kernel when freed; smaller ones come from a bump arena
//! that is emptied after each request (nothing small outlives a request).

use crate::sys;
use core::alloc::{GlobalAlloc, Layout};
use core::cell::UnsafeCell;

const ARENA: usize = 256 * 1024;
const BIG: usize = 2048;
const PAGE: usize = 4096;

pub struct Heap {
    arena: UnsafeCell<*mut u8>,
    used: UnsafeCell<usize>,
    high: UnsafeCell<usize>,
}

unsafe impl Sync for Heap {}

#[global_allocator]
pub static HEAP: Heap = Heap {
    arena: UnsafeCell::new(core::ptr::null_mut()),
    used: UnsafeCell::new(0),
    high: UnsafeCell::new(0),
};

fn pages(n: usize) -> usize {
    (n + PAGE - 1) & !(PAGE - 1)
}

impl Heap {
    pub fn init() {
        unsafe {
            *HEAP.arena.get() = sys::mmap(ARENA).unwrap_or(core::ptr::null_mut());
        }
    }

    /// Between requests: forget the small blocks and hand their pages back.
    pub fn reset() {
        unsafe {
            let arena = *HEAP.arena.get();
            let high = *HEAP.high.get();
            if !arena.is_null() && high > PAGE {
                sys::release(arena.add(PAGE), pages(high) - PAGE);
            }
            *HEAP.used.get() = 0;
            *HEAP.high.get() = 0;
        }
    }

    fn in_arena(&self, p: *mut u8) -> bool {
        let arena = unsafe { *self.arena.get() } as usize;
        arena != 0 && (arena..arena + ARENA).contains(&(p as usize))
    }
}

unsafe impl GlobalAlloc for Heap {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let size = layout.size().max(1);
        if size < BIG {
            unsafe {
                let arena = *self.arena.get();
                let used = *self.used.get();
                let start = (used + layout.align() - 1) & !(layout.align() - 1);
                if !arena.is_null() && start + size <= ARENA {
                    *self.used.get() = start + size;
                    if start + size > *self.high.get() {
                        *self.high.get() = start + size;
                    }
                    return arena.add(start);
                }
            }
        }
        // page-aligned, so any alignment up to a page is met
        sys::mmap(pages(size)).unwrap_or(core::ptr::null_mut())
    }

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        if !self.in_arena(p) {
            sys::munmap(p, pages(layout.size().max(1)));
        }
    }
}
