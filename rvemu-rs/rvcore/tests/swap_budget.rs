//! The flash swap device's write budget: a burst the size of swap, then
//! one page per `REFILL_SECS`.

use std::cell::Cell;
use std::rc::Rc;

use rvcore::swapdev::{CHUNK, Flash, FlashSwap, PAGE, REFILL_SECS};
use rvcore::virtio::Disk;

const HZ: u32 = 1_000_000;
const SECTORS: u32 = 64;
const SPARE: u32 = 8;

/// Blank NOR flash whose clock the test sets.
struct TestFlash {
    data: Vec<u8>,
    now: Rc<Cell<u64>>,
}

impl Flash for TestFlash {
    fn sectors(&self) -> u32 {
        SECTORS
    }
    fn erase(&mut self, sector: u32) -> bool {
        let o = sector as usize * PAGE;
        self.data[o..o + PAGE].fill(0xFF);
        true
    }
    fn program(&mut self, off: u32, data: &[u8; CHUNK]) -> bool {
        for (f, &d) in self.data[off as usize..off as usize + CHUNK].iter_mut().zip(data) {
            *f &= d;
        }
        true
    }
    fn read(&self, off: u32, buf: &mut [u8]) {
        buf.copy_from_slice(&self.data[off as usize..off as usize + buf.len()]);
    }
    fn cycles(&self) -> u64 {
        self.now.get()
    }
    fn hz(&self) -> u32 {
        HZ
    }
}

fn swap() -> (FlashSwap<TestFlash>, Rc<Cell<u64>>) {
    let now = Rc::new(Cell::new(0));
    let flash = TestFlash { data: vec![0xFF; SECTORS as usize * PAGE], now: now.clone() };
    (FlashSwap::new(flash, SPARE), now)
}

fn page(fill: u8) -> Vec<u8> {
    vec![fill; PAGE]
}

fn write(s: &mut FlashSwap<TestFlash>, lp: u32, data: &[u8]) -> bool {
    s.write(u64::from(lp) * (PAGE as u64 / 512), data)
}

fn read(s: &mut FlashSwap<TestFlash>, lp: u32) -> Vec<u8> {
    let mut buf = page(0);
    assert!(s.read(u64::from(lp) * (PAGE as u64 / 512), &mut buf));
    buf
}

#[test]
fn a_burst_fills_swap_then_writes_wait_for_refill() {
    let (mut s, now) = swap();
    let pages = s.pages();
    assert_eq!(pages, SECTORS - SPARE);
    for lp in 0..pages {
        assert!(write(&mut s, lp, &page(lp as u8 + 1)), "burst page {lp}");
    }
    // the budget is spent: the write fails and the old contents stay
    assert!(!write(&mut s, 0, &page(0xAA)));
    assert_eq!(s.stats.pages_refused, 1);
    assert_eq!(read(&mut s, 0), page(1));
    // zero pages take no flash, so no budget
    assert!(write(&mut s, 1, &page(0)));
    assert_eq!(read(&mut s, 1), page(0));
    // one refill period buys exactly one page
    now.set(u64::from(HZ) * REFILL_SECS);
    assert!(write(&mut s, 0, &page(0xAA)));
    assert!(!write(&mut s, 2, &page(0xBB)));
    assert_eq!(read(&mut s, 0), page(0xAA));
    assert_eq!(s.stats.pages_refused, 2);
}

#[test]
fn an_idle_budget_refills_to_a_full_burst_and_no_more() {
    let (mut s, now) = swap();
    let pages = s.pages();
    for lp in 0..pages {
        assert!(write(&mut s, lp, &page(1)));
    }
    // a long idle spell refills the whole burst, never more than it
    now.set(u64::from(HZ) * REFILL_SECS * u64::from(pages) * 10);
    for lp in 0..pages {
        assert!(write(&mut s, lp, &page(2)), "refilled page {lp}");
    }
    assert!(!write(&mut s, 0, &page(3)));
}

#[test]
fn a_request_over_budget_writes_none_of_its_pages() {
    let (mut s, _now) = swap();
    let pages = s.pages();
    for lp in 0..pages - 1 {
        assert!(write(&mut s, lp, &page(1)));
    }
    // one page of budget left: a two-page request fails whole
    let mut two = page(5);
    two.extend(page(6));
    assert!(!write(&mut s, 0, &two));
    assert_eq!(read(&mut s, 0), page(1));
    assert_eq!(read(&mut s, 1), page(1));
    assert_eq!(s.stats.pages_refused, 2);
}
