//! Swap space on NOR flash, as a `Disk` for a writable virtio-blk device.
//!
//! A guest page is 4 KiB, the size of one flash erase sector, so each
//! logical page lives in a sector of its own. A write goes to an erased
//! sector, and the sector it replaces becomes dirty. Nothing is ever copied
//! (no garbage collection, write amplification 1), and erasing can wait: the
//! host calls `idle_erase` while the guest is idle, and a write erases
//! synchronously only when no erased sector is left.
//!
//! - **Placement.** Writes take the next erased sector after a moving head,
//!   so erases rotate through the region (wear levelling). The host starts
//!   the head somewhere different on every boot (`set_head`).
//! - **Static wear levelling.** A page that stays in swap pins its sector,
//!   and writes would wear only the rest. So every `MOVE_EVERY`th write also
//!   moves the live page nearest ahead of the head to a fresh sector: every
//!   sector keeps taking its turn, for 1/`MOVE_EVERY` more writes.
//! - **Nothing persists.** Swap is empty at every boot, so the map lives in
//!   host RAM only and there is no on-flash metadata to keep consistent.
//!   Sectors are of unknown state at boot: blank ones are found by reading.
//! - **Zero pages** take no flash at all (as zram's same-filled pages).
//! - **Stored inverted.** Data is written as its complement, so runs of zero
//!   bytes are erased flash (0xFF) and their 256-byte program operations are
//!   skipped.
//! - **Statistics.** The page after the swap area reads back as text:
//!   counters and flash timings (`stats`).

use core::fmt::Write as _;

/// Flash erase sector and guest page.
pub const PAGE: usize = 4096;
/// Flash program page.
pub const CHUNK: usize = 256;
const CHUNKS: usize = PAGE / CHUNK;
/// Largest region handled: the map tables are fixed arrays.
pub const MAX_SECTORS: usize = 512;
/// One wear-levelling move per this many page writes.
const MOVE_EVERY: u32 = 8;

/// A region of NOR flash, addressed from its start.
pub trait Flash {
    /// Erase sectors (4 KiB) in the region.
    fn sectors(&self) -> u32;
    /// Erase one sector; false if the flash didn't finish.
    fn erase(&mut self, sector: u32) -> bool;
    /// Program one 256-byte page at byte offset `off`; false on failure.
    fn program(&mut self, off: u32, data: &[u8; CHUNK]) -> bool;
    /// Read `buf.len()` bytes at byte offset `off`.
    fn read(&self, off: u32, buf: &mut [u8]);
    /// A free-running cycle counter, for the timings in the statistics.
    fn cycles(&self) -> u64;
    /// Cycles per second of `cycles`.
    fn hz(&self) -> u32;
}

const UNMAPPED: u16 = 0xFFFF;
const ZERO: u16 = 0xFFFE;
/// Sector states other than "holds logical page N".
const ERASED: u16 = 0xFFFF;
const DIRTY: u16 = 0xFFFE;
/// State not known yet (boot): blank-check it before use.
const UNKNOWN: u16 = 0xFFFD;

#[derive(Default, Clone, Copy)]
pub struct Stats {
    pub pages_written: u64,
    /// Pages moved for wear levelling (not counted in `pages_written`).
    pub pages_moved: u64,
    pub zero_pages: u64,
    pub pages_read: u64,
    pub programs: u64,
    pub programs_skipped: u64,
    pub erases_sync: u64,
    pub erases_idle: u64,
    pub blank_found: u64,
    pub errors: u64,
    pub erase_cycles: u64,
    pub erase_max: u64,
    pub program_cycles: u64,
    pub program_max: u64,
    /// Longest single `write` call (what one request stalls the guest).
    pub write_max: u64,
    pub write_cycles: u64,
}

pub struct FlashSwap<F: Flash> {
    pub flash: F,
    /// Logical pages offered for swap (the stats page follows them).
    pages: u32,
    nsect: u32,
    map: [u16; MAX_SECTORS],
    state: [u16; MAX_SECTORS],
    head: u32,
    /// Sectors that are dirty or unknown (worth an idle erase).
    to_erase: u32,
    /// Page writes since the last wear-levelling move.
    since_move: u32,
    pub stats: Stats,
}

impl<F: Flash> FlashSwap<F> {
    /// Swap of `flash.sectors() - spare` pages. `spare` sectors are always
    /// free to take a burst of writes without waiting for an erase.
    pub fn new(flash: F, spare: u32) -> Self {
        let nsect = flash.sectors().min(MAX_SECTORS as u32);
        let pages = nsect.saturating_sub(spare.max(1));
        let mut state = [ERASED; MAX_SECTORS];
        for s in &mut state[..nsect as usize] {
            *s = UNKNOWN;
        }
        FlashSwap {
            flash,
            pages,
            nsect,
            map: [UNMAPPED; MAX_SECTORS],
            state,
            head: 0,
            to_erase: nsect,
            since_move: 0,
            stats: Stats::default(),
        }
    }

    /// Where the first write goes; pick a different place each boot.
    pub fn set_head(&mut self, sector: u32) {
        self.head = sector % self.nsect.max(1);
    }

    /// Logical pages of swap.
    pub fn pages(&self) -> u32 {
        self.pages
    }

    /// Whether `idle_erase` has work to do. Not before the guest's first
    /// write (mkswap): a guest that doesn't use swap never erases flash.
    pub fn wants_erase(&self) -> bool {
        self.to_erase != 0 && self.stats.pages_written + self.stats.zero_pages != 0
    }

    fn timed_erase(&mut self, s: u32) -> bool {
        let t0 = self.flash.cycles();
        let ok = self.flash.erase(s);
        let dt = self.flash.cycles() - t0;
        self.stats.erase_cycles += dt;
        self.stats.erase_max = self.stats.erase_max.max(dt);
        if !ok {
            self.stats.errors += 1;
        }
        ok
    }

    fn is_blank(&self, s: u32) -> bool {
        let mut buf = [0u8; CHUNK];
        (0..CHUNKS as u32).all(|c| {
            self.flash.read(s * PAGE as u32 + c * CHUNK as u32, &mut buf);
            buf.iter().all(|&b| b == 0xFF)
        })
    }

    /// Make sector `s` (dirty or unknown) erased. `idle` only for statistics.
    fn clean(&mut self, s: u32, idle: bool) -> bool {
        if self.state[s as usize] == UNKNOWN && self.is_blank(s) {
            self.state[s as usize] = ERASED;
            self.stats.blank_found += 1;
            self.to_erase -= 1;
            return true;
        }
        if !self.timed_erase(s) {
            return false;
        }
        if idle {
            self.stats.erases_idle += 1;
        } else {
            self.stats.erases_sync += 1;
        }
        self.state[s as usize] = ERASED;
        self.to_erase -= 1;
        true
    }

    /// Erase one dirty sector, the next one after the head. Call it while the
    /// guest is idle; it stalls for one erase (about 45 ms).
    pub fn idle_erase(&mut self) {
        if !self.wants_erase() {
            return;
        }
        for i in 0..self.nsect {
            let s = (self.head + i) % self.nsect;
            if matches!(self.state[s as usize], DIRTY | UNKNOWN) {
                self.clean(s, true);
                return;
            }
        }
    }

    /// An erased sector for a write: the first one from the head, else erase
    /// the first dirty one.
    fn alloc(&mut self) -> Option<u32> {
        let n = self.nsect;
        let found = (0..n).map(|i| (self.head + i) % n).find(|&s| self.state[s as usize] == ERASED);
        let s = match found {
            Some(s) => s,
            None => {
                let s = (0..n).map(|i| (self.head + i) % n).find(|&s| matches!(self.state[s as usize], DIRTY | UNKNOWN))?;
                if !self.clean(s, false) {
                    return None;
                }
                s
            }
        };
        self.head = (s + 1) % n;
        Some(s)
    }

    fn release(&mut self, lp: u32) {
        let old = self.map[lp as usize];
        if old != UNMAPPED && old != ZERO {
            self.state[old as usize] = DIRTY;
            self.to_erase += 1;
        }
        self.map[lp as usize] = UNMAPPED;
    }

    /// Write logical page `lp`. Chunk `c` (256 bytes) comes from `new(c)`
    /// when that returns data, else from the page's current contents.
    fn write_page(&mut self, lp: u32, new: &dyn Fn(usize) -> Option<*const u8>) -> bool {
        let old = self.map[lp as usize];
        let all_new = (0..CHUNKS).all(|c| new(c).is_some());
        if all_new
            && (0..CHUNKS).all(|c| {
                // SAFETY: `new` yields pointers to CHUNK readable bytes.
                let d = unsafe { core::slice::from_raw_parts(new(c).unwrap(), CHUNK) };
                d.iter().all(|&b| b == 0)
            })
        {
            self.release(lp);
            self.map[lp as usize] = ZERO;
            self.stats.zero_pages += 1;
            return true;
        }
        let Some(s) = self.alloc() else {
            self.stats.errors += 1;
            return false;
        };
        let mut buf = [0u8; CHUNK];
        let mut ok = true;
        for c in 0..CHUNKS {
            match new(c) {
                Some(p) => {
                    // SAFETY: as above.
                    let d = unsafe { core::slice::from_raw_parts(p, CHUNK) };
                    for (o, &b) in buf.iter_mut().zip(d) {
                        *o = !b;
                    }
                }
                None if old == UNMAPPED || old == ZERO => buf.fill(0xFF),
                None => self.flash.read(old as u32 * PAGE as u32 + (c * CHUNK) as u32, &mut buf),
            }
            if buf.iter().all(|&b| b == 0xFF) {
                self.stats.programs_skipped += 1;
                continue;
            }
            let t0 = self.flash.cycles();
            ok &= self.flash.program(s * PAGE as u32 + (c * CHUNK) as u32, &buf);
            let dt = self.flash.cycles() - t0;
            self.stats.programs += 1;
            self.stats.program_cycles += dt;
            self.stats.program_max = self.stats.program_max.max(dt);
        }
        self.release(lp);
        // even a failed program leaves the sector to be erased again
        self.map[lp as usize] = s as u16;
        self.state[s as usize] = lp as u16;
        if all_new {
            self.stats.pages_written += 1;
        } else {
            self.stats.pages_moved += 1;
        }
        if !ok {
            self.stats.errors += 1;
        }
        ok
    }

    /// Move the live page nearest ahead of the head to a fresh sector.
    fn move_one(&mut self) {
        let n = self.nsect;
        let live = (0..n).map(|i| (self.head + i) % n).find(|&s| self.state[s as usize] < UNKNOWN);
        if let Some(s) = live {
            let lp = u32::from(self.state[s as usize]);
            self.write_page(lp, &|_| None);
        }
    }

    /// The statistics page, as text.
    fn stats_page(&self, buf: &mut [u8]) {
        struct W<'a>(&'a mut [u8], usize);
        impl core::fmt::Write for W<'_> {
            fn write_str(&mut self, s: &str) -> core::fmt::Result {
                let n = s.len().min(self.0.len() - self.1);
                self.0[self.1..self.1 + n].copy_from_slice(&s.as_bytes()[..n]);
                self.1 += n;
                Ok(())
            }
        }
        buf.fill(0);
        let s = &self.stats;
        let us = |c: u64| c * 1_000_000 / u64::from(self.flash.hz().max(1));
        let (mut erased, mut dirty, mut live) = (0, 0, 0);
        for &st in &self.state[..self.nsect as usize] {
            match st {
                ERASED => erased += 1,
                DIRTY | UNKNOWN => dirty += 1,
                _ => live += 1,
            }
        }
        let mut w = W(buf, 0);
        let _ = write!(
            w,
            "pages {}\nsectors {}\nlive {}\nerased {}\ndirty {}\nzero_mapped {}\n\
             pages_written {}\npages_moved {}\nzero_pages {}\npages_read {}\nprograms {}\nprograms_skipped {}\n\
             erases_sync {}\nerases_idle {}\nblank_found {}\nerrors {}\n\
             erase_us_total {}\nerase_us_max {}\nprogram_us_total {}\nprogram_us_max {}\n\
             write_us_total {}\nwrite_us_max {}\n",
            self.pages,
            self.nsect,
            live,
            erased,
            dirty,
            self.map[..self.pages as usize].iter().filter(|&&m| m == ZERO).count(),
            s.pages_written,
            s.pages_moved,
            s.zero_pages,
            s.pages_read,
            s.programs,
            s.programs_skipped,
            s.erases_sync,
            s.erases_idle,
            s.blank_found,
            s.errors,
            us(s.erase_cycles),
            us(s.erase_max),
            us(s.program_cycles),
            us(s.program_max),
            us(s.write_cycles),
            us(s.write_max),
        );
    }
}

impl<F: Flash> crate::virtio::Disk for FlashSwap<F> {
    /// The swap pages, then one page of statistics.
    fn sectors(&self) -> u64 {
        u64::from(self.pages + 1) * (PAGE as u64 / 512)
    }
    fn read_only(&self) -> bool {
        false
    }
    fn block_size(&self) -> u32 {
        PAGE as u32
    }
    fn id(&self) -> [u8; 20] {
        *b"rvemu-flash-swap\0\0\0\0"
    }
    fn read(&mut self, sector: u64, buf: &mut [u8]) -> bool {
        let Ok(off) = usize::try_from(sector * 512) else { return false };
        if !buf.len().is_multiple_of(512) || off + buf.len() > (self.pages as usize + 1) * PAGE {
            return false;
        }
        let mut done = 0;
        while done < buf.len() {
            let at = off + done;
            let (lp, in_page) = (at / PAGE, at % PAGE);
            let n = (PAGE - in_page).min(buf.len() - done);
            let out = &mut buf[done..done + n];
            if lp == self.pages as usize {
                // whole-page reads only (the block size is a page)
                if in_page == 0 && n == PAGE {
                    self.stats_page(out);
                } else {
                    out.fill(0);
                }
            } else {
                match self.map[lp] {
                    UNMAPPED | ZERO => out.fill(0),
                    s => {
                        self.flash.read(u32::from(s) * PAGE as u32 + in_page as u32, out);
                        for b in out.iter_mut() {
                            *b = !*b;
                        }
                    }
                }
                if in_page == 0 {
                    self.stats.pages_read += 1;
                }
            }
            done += n;
        }
        true
    }
    fn write(&mut self, sector: u64, buf: &[u8]) -> bool {
        let Ok(off) = usize::try_from(sector * 512) else { return false };
        if !buf.len().is_multiple_of(512) || off + buf.len() > self.pages as usize * PAGE {
            return false;
        }
        let t0 = self.flash.cycles();
        let mut ok = true;
        let mut lp = off / PAGE;
        while lp * PAGE < off + buf.len() {
            let base = lp * PAGE;
            let new = |c: usize| {
                let at = base + c * CHUNK;
                (at >= off && at + CHUNK <= off + buf.len()).then(|| buf[at - off..].as_ptr())
            };
            ok &= self.write_page(lp as u32, &new);
            self.since_move += 1;
            if self.since_move >= MOVE_EVERY {
                self.since_move = 0;
                self.move_one();
            }
            lp += 1;
        }
        let dt = self.flash.cycles() - t0;
        self.stats.write_cycles += dt;
        self.stats.write_max = self.stats.write_max.max(dt);
        ok
    }
}
