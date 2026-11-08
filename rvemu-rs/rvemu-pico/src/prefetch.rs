//! Blocks translated ahead of time on core1 (`rvjit::Prefetch`).
//!
//! Most translations re-translate a block the code buffer evicted, and the
//! blocks evicted together from one guest page tend to come back together.
//! So core0 notes every evicted block in a ghost table (by page), and every
//! translation miss in a request ring; core1 then translates the page's
//! other ghosts into a store of ready blocks, which core0 asks first on its
//! next misses.
//!
//! - Ghost table (core0 writes, core1 reads): a ring of `GN` (va, page |
//!   link) entries, each linked to the page's previous entry; `HEADS`
//!   holds each page's newest. core1 checks every entry's page, so a
//!   half-written or overwritten one at worst costs a useless translation:
//!   a block is a function of (va, pa, privilege) and the code bytes alone.
//! - Store (core1 writes, core0 reads): a log of `SW` words, entries
//!   `[va, pa | sup, words | epoch << 16, code...]`, and an index of
//!   `NIDX` slots holding 1 + an entry's absolute position. core1 only ever
//!   writes within `WIN` words of the published write position `WABS`, so
//!   an entry is intact if it sits more than `WIN` words behind it: core0
//!   checks that before and after copying (a seqlock).
//! - The epoch is the page's epoch when core0 asked (`missed`); core0 has
//!   write-protected the page by then, so any later store to it changes
//!   the epoch and `find` refuses the stale blocks.

use core::cell::UnsafeCell;
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering, fence};
use rvjit::{Gen, Prefetch};

use crate::core1;

/// Ghost entries (power of two, below 4095: links are 12 bits).
const GN: usize = 2048;
/// Page heads (power of two).
const GH: usize = 256;
const NO_LINK: u32 = 0xFFF;
/// Requests queued on core1 (power of two).
const NQ: usize = 64;
/// Store log words (power of two).
const SW: usize = 4096;
const HDR: usize = 3;
/// Largest block kept (words; the average is ~20).
const CAP: usize = 256;
const WIN: u32 = (CAP + HDR) as u32;
const NIDX: usize = 512;
const PROBES: usize = 4;
/// Ghosts translated per pass before the other services get a turn.
const BURST: u32 = 8;

/// Words in SRAM for all of this (for `CODE_KIB`).
pub const SRAM_WORDS: usize = 2 * GN + GH + GH + 2 * NQ + SW + NIDX;

struct Words<const N: usize>(UnsafeCell<[u32; N]>);
// SAFETY: see the module docs for who writes what, and when.
unsafe impl<const N: usize> Sync for Words<N> {}
impl<const N: usize> Words<N> {
    const fn new() -> Self {
        Words(UnsafeCell::new([0; N]))
    }
    fn ptr(&self) -> *mut u32 {
        self.0.get().cast()
    }
    #[inline(always)]
    fn get(&self, i: usize) -> u32 {
        // SAFETY: `i % N` is in bounds; racing writes are allowed for.
        unsafe { self.ptr().add(i % N).read_volatile() }
    }
    #[inline(always)]
    fn set(&self, i: usize, v: u32) {
        // SAFETY: as above.
        unsafe { self.ptr().add(i % N).write_volatile(v) }
    }
}

static GHOST: Words<{ 2 * GN }> = Words::new();
static HEADS: Words<GH> = Words::new();
/// ghost entries written so far
static GW: AtomicU32 = AtomicU32::new(0);

/// requests: (va, page | sup << 8 | epoch)
static REQ: Words<{ 2 * NQ }> = Words::new();
static REQ_HEAD: AtomicU32 = AtomicU32::new(0);
static REQ_TAIL: AtomicU32 = AtomicU32::new(0);

static STORE: Words<SW> = Words::new();
static INDEX: Words<NIDX> = Words::new();
/// store words written so far (core1's write position)
static WABS: AtomicU32 = AtomicU32::new(0);
/// (core1) ghost position each head was last worked through to
static DONE: Words<GH> = Words::new();

/// Counters for the status line: requests and blocks translated (core1),
/// blocks core0 took and lost.
pub static REQUESTS: AtomicU32 = AtomicU32::new(0);
pub static TRANSLATED: AtomicU32 = AtomicU32::new(0);
pub static TAKEN: AtomicU32 = AtomicU32::new(0);
pub static LOST: AtomicU32 = AtomicU32::new(0);

#[inline(always)]
fn bump(c: &AtomicU32) {
    // one writer each
    c.store(c.load(Ordering::Relaxed).wrapping_add(1), Ordering::Relaxed);
}

struct GenSlot(UnsafeCell<MaybeUninit<Gen>>);
// SAFETY: written once by core0 before READY.
unsafe impl Sync for GenSlot {}
static GEN: GenSlot = GenSlot(UnsafeCell::new(MaybeUninit::uninit()));
static READY: AtomicBool = AtomicBool::new(false);

#[inline(always)]
fn head_slot(page: u32) -> usize {
    (page.wrapping_mul(0x9E37_79B1) >> (32 - GH.trailing_zeros())) as usize
}

#[inline(always)]
fn index_slot(va: u32, key: u32, i: usize) -> usize {
    let h = ((va ^ key.rotate_left(16)) >> 2).wrapping_mul(0x9E37_79B1) >> (32 - NIDX.trailing_zeros());
    (h as usize + i) % NIDX
}

/// Store entry at absolute position `abs` is still intact.
#[inline(always)]
fn intact(abs: u32) -> bool {
    WABS.load(Ordering::Acquire).wrapping_add(WIN).wrapping_sub(abs) <= SW as u32
}

// ------------------------------------------------------------------ core0

/// core0's side: what `rvjit::Jit` calls.
pub struct Client {
    /// the entry `find` named: log offset of its code, words, position
    off: usize,
    words: usize,
    abs: u32,
}

impl Client {
    /// Start: `bg` is core0's translator's (`Jit::block_gen`).
    pub fn new(bg: Gen) -> Self {
        // SAFETY: core1 reads GEN only after READY.
        unsafe { (*GEN.0.get()).write(bg) };
        READY.store(true, Ordering::Release);
        Client { off: 0, words: 0, abs: 0 }
    }
}

impl Prefetch for Client {
    #[unsafe(link_section = ".data.rvemu_hot")]
    fn evicted(&mut self, va: u32, pa: u32) {
        let g = GW.load(Ordering::Relaxed);
        let page = pa >> 12;
        let h = head_slot(page);
        let prev = HEADS.get(h);
        let link = if prev != 0 && g.wrapping_sub(prev - 1) < GN as u32 { (prev - 1) % GN as u32 } else { NO_LINK };
        let i = g as usize % GN;
        GHOST.set(2 * i, va);
        GHOST.set(2 * i + 1, page << 12 | link);
        HEADS.set(h, g.wrapping_add(1));
        GW.store(g.wrapping_add(1), Ordering::Release);
    }

    #[unsafe(link_section = ".data.rvemu_hot")]
    fn missed(&mut self, va: u32, pa: u32, sup: bool, epoch: u8) {
        let h = REQ_HEAD.load(Ordering::Relaxed);
        if h.wrapping_sub(REQ_TAIL.load(Ordering::Acquire)) < NQ as u32 {
            let i = h as usize % NQ;
            REQ.set(2 * i, va);
            REQ.set(2 * i + 1, (pa & !0xFFF) | u32::from(sup) << 8 | u32::from(epoch));
            REQ_HEAD.store(h.wrapping_add(1), Ordering::Release);
            core1::notify_core1();
        }
    }

    #[unsafe(link_section = ".data.rvemu_hot")]
    fn find(&mut self, va: u32, pa: u32, sup: bool, epoch: u8) -> Option<usize> {
        let key = pa | u32::from(sup);
        for i in 0..PROBES {
            let v = INDEX.get(index_slot(va, key, i));
            if v == 0 {
                continue;
            }
            let abs = v - 1;
            let e = abs as usize % SW;
            if STORE.get(e) != va || STORE.get(e + 1) != key {
                continue;
            }
            let w = STORE.get(e + 2);
            let words = (w & 0xFFFF) as usize;
            if w >> 16 != u32::from(epoch) || e + HDR + words > SW || !intact(abs) {
                return None;
            }
            (self.off, self.words, self.abs) = (e + HDR, words, abs);
            return Some(words);
        }
        None
    }

    #[unsafe(link_section = ".data.rvemu_hot")]
    fn copy(&mut self, dst: &mut [u32]) -> bool {
        let n = self.words.min(dst.len());
        // SAFETY: off + words <= SW (checked in `find`); racing writes are
        // caught by the check after.
        unsafe { core::ptr::copy_nonoverlapping(STORE.ptr().add(self.off), dst.as_mut_ptr(), n) };
        fence(Ordering::Acquire);
        let ok = intact(self.abs);
        bump(if ok { &TAKEN } else { &LOST });
        ok
    }
}

// ------------------------------------------------------------------ core1

/// core1's side: a `core1::Service`.
pub struct Prefetcher {
    /// the request being worked through: its va, page | sup << 8 | epoch,
    /// and the next ghost to look at (absolute), with how far back to go
    va: u32,
    req: u32,
    next: Option<u32>,
    floor_age: u32,
    gw: u32,
}

impl Prefetcher {
    pub const fn new() -> Self {
        Prefetcher { va: 0, req: 0, next: None, floor_age: 0, gw: 0 }
    }

    /// Start on the next request, if any.
    #[unsafe(link_section = ".data.core1")]
    fn take_request(&mut self) -> bool {
        let t = REQ_TAIL.load(Ordering::Relaxed);
        if REQ_HEAD.load(Ordering::Acquire) == t {
            return false;
        }
        let i = t as usize % NQ;
        (self.va, self.req) = (REQ.get(2 * i), REQ.get(2 * i + 1));
        REQ_TAIL.store(t.wrapping_add(1), Ordering::Release);
        bump(&REQUESTS);
        let page = self.req >> 12;
        let h = head_slot(page);
        let gw = GW.load(Ordering::Acquire);
        // ghosts older than the last walk of this head were seen then;
        // keep clear of the oldest quarter, which core0 may be overwriting
        let floor_age = gw.wrapping_sub(DONE.get(h)).min((GN - GN / 4) as u32);
        DONE.set(h, gw);
        let head = HEADS.get(h);
        self.gw = gw;
        self.floor_age = floor_age;
        self.next = (head != 0).then(|| head - 1);
        true
    }

    /// Translate block (va, pa) into the store.
    #[unsafe(link_section = ".data.core1")]
    fn put(&mut self, bg: &Gen, va: u32, pa: u32, sup: bool, epoch: u32) {
        let mut w = WABS.load(Ordering::Relaxed);
        let mut off = w as usize % SW;
        // SAFETY: only core1 writes the store; core0's reads are checked.
        let store = unsafe { &mut *STORE.0.get() };
        let mut n = if SW - off > HDR { bg.translate(va, pa, sup, &mut store[off + HDR..(off + HDR + CAP).min(SW)]) } else { 0 };
        if n == 0 && SW - off < CAP + HDR {
            // didn't fit in the lap's tail: start the next lap
            w = w.wrapping_add((SW - off) as u32);
            WABS.store(w, Ordering::Release);
            off = 0;
            n = bg.translate(va, pa, sup, &mut store[HDR..HDR + CAP]);
        }
        if n == 0 {
            return;
        }
        let key = pa | u32::from(sup);
        store[off] = va;
        store[off + 1] = key;
        store[off + 2] = n as u32 | epoch << 16;
        let abs = w;
        WABS.store(w.wrapping_add((HDR + n) as u32), Ordering::Release);
        // a free or stale slot, or the same key's; else the first probe's
        let mut slot = index_slot(va, key, 0);
        for i in 0..PROBES {
            let s = index_slot(va, key, i);
            let v = INDEX.get(s);
            if v == 0 || !intact(v - 1) {
                slot = s;
                break;
            }
            let o = (v - 1) as usize % SW;
            if store[o] == va && store[o + 1] == key {
                slot = s;
                break;
            }
        }
        INDEX.set(slot, abs.wrapping_add(1));
        bump(&TRANSLATED);
    }

    #[unsafe(link_section = ".data.core1")]
    pub fn poll(&mut self) -> bool {
        if !READY.load(Ordering::Acquire) {
            return false;
        }
        // SAFETY: written before READY.
        let bg = unsafe { (*GEN.0.get()).assume_init_ref() };
        let mut budget = BURST;
        loop {
            let Some(abs) = self.next else {
                if !self.take_request() {
                    return false;
                }
                continue;
            };
            self.next = None;
            let age = self.gw.wrapping_sub(abs);
            if age == 0 || age > self.floor_age {
                continue;
            }
            let i = abs as usize % GN;
            let (va, w) = (GHOST.get(2 * i), GHOST.get(2 * i + 1));
            let page = self.req >> 12;
            if w >> 12 != page {
                continue;
            }
            let link = w & 0xFFF;
            if link != NO_LINK {
                let back = (i as u32).wrapping_sub(link) % GN as u32;
                if back != 0 {
                    self.next = Some(abs.wrapping_sub(back));
                }
            }
            if va == self.va {
                continue;
            }
            let pa = page << 12 | (va & 0xFFF);
            self.put(bg, va, pa, self.req & 0x100 != 0, self.req & 0xFF);
            budget -= 1;
            if budget == 0 {
                return true;
            }
        }
    }
}
