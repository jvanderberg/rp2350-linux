//! virtio-mmio (version 2) devices: a block device and a network device,
//! split virtqueues, no indirect descriptors.
//!
//! Requests complete synchronously: the block device when the driver
//! notifies its queue, the network device's transmit queue likewise, and its
//! receive queue whenever `VirtioNet::poll` finds a frame and a free buffer.
//! Buffers are guest physical addresses inside guest RAM (no IOMMU), reached
//! through `GuestMem`. Every guest RAM range a device writes is reported once
//! via `take_dma_write`, so a translator can drop code it translated from
//! those pages (DMA bypasses the CPU's store path).

use core::ptr;

/// Block device registers.
pub const VIRTIO_BASE: u32 = 0xF800_0000;
pub const VIRTIO_SIZE: u32 = 0x200;
pub const VIRTIO_IRQ: u32 = 2;
/// Network device registers.
pub const VIRTIO_NET_BASE: u32 = 0xF800_0200;
pub const VIRTIO_NET_IRQ: u32 = 3;
/// Swap device registers (a second, writable block device).
pub const VIRTIO_SWAP_BASE: u32 = 0xF800_0600;
pub const VIRTIO_SWAP_IRQ: u32 = 5;

const MAGIC: u32 = 0x7472_6976; // "virt"
const DEVICE_NET: u32 = 1;
const DEVICE_BLK: u32 = 2;
const VENDOR: u32 = 0x554D_4552; // "REMU"
/// Queue sizes. Linux stops a network transmit queue while fewer than
/// MAX_SKB_FRAGS + 2 (19) descriptors are free, so that one needs 32; the
/// others stay small (every posted receive buffer is guest RAM held).
const BLK_QUEUE: u32 = 16;
const NET_RX_QUEUE: u32 = 16;
const NET_TX_QUEUE: u32 = 32;
/// Longest descriptor chain followed (the largest queue).
const CHAIN_MAX: usize = 32;
const NQUEUES: usize = 2;

/// VIRTIO_F_VERSION_1 is feature bit 32: bit 0 of the high word.
const F_HI_VERSION_1: u32 = 1;

const DESC_F_NEXT: u16 = 1;
const DESC_F_WRITE: u16 = 2;

const BLK_F_RO: u32 = 1 << 5;
const BLK_F_BLK_SIZE: u32 = 1 << 6;
const BLK_T_IN: u32 = 0;
const BLK_T_OUT: u32 = 1;
const BLK_T_FLUSH: u32 = 4;
const BLK_T_GET_ID: u32 = 8;
const BLK_S_OK: u8 = 0;
const BLK_S_IOERR: u8 = 1;
const BLK_S_UNSUPP: u8 = 2;

const NET_F_MAC: u32 = 1 << 5;
const NET_F_STATUS: u32 = 1 << 16;
const NET_S_LINK_UP: u16 = 1;
/// virtio_net_hdr_v1 (VERSION_1 always has `num_buffers`).
const NET_HDR: usize = 12;
/// Largest frame passed either way: Ethernet header + 1500 + VLAN tag.
pub const FRAME_MAX: usize = 1518;

// ------------------------------------------------------------------ backends

/// Backing storage in 512-byte sectors.
pub trait Disk {
    fn sectors(&self) -> u64;
    fn read_only(&self) -> bool {
        true
    }
    /// Logical block size offered to the driver (a power of two, >= 512).
    fn block_size(&self) -> u32 {
        512
    }
    /// The device ID (`/sys/block/vdX/serial` in the guest), NUL padded.
    fn id(&self) -> [u8; 20] {
        *b"rvemu-disk\0\0\0\0\0\0\0\0\0\0"
    }
    /// Fill `buf` from byte offset `sector * 512`; false on error.
    fn read(&mut self, sector: u64, buf: &mut [u8]) -> bool;
    fn write(&mut self, _sector: u64, _buf: &[u8]) -> bool {
        false
    }
}

impl<D: Disk + ?Sized> Disk for &mut D {
    fn sectors(&self) -> u64 {
        (**self).sectors()
    }
    fn read_only(&self) -> bool {
        (**self).read_only()
    }
    fn block_size(&self) -> u32 {
        (**self).block_size()
    }
    fn id(&self) -> [u8; 20] {
        (**self).id()
    }
    fn read(&mut self, sector: u64, buf: &mut [u8]) -> bool {
        (**self).read(sector, buf)
    }
    fn write(&mut self, sector: u64, buf: &[u8]) -> bool {
        (**self).write(sector, buf)
    }
}

/// No disk (the device is left out of the platform).
pub struct NoDisk;

impl Disk for NoDisk {
    fn sectors(&self) -> u64 {
        0
    }
    fn read(&mut self, _sector: u64, _buf: &mut [u8]) -> bool {
        false
    }
}

/// Read-only disk image in memory the host can address (a file loaded on
/// the Mac, flash on the RP2350).
pub struct MemDisk {
    data: *const u8,
    len: usize,
}

impl MemDisk {
    /// # Safety
    /// `data..data + len` must stay readable for the disk's lifetime.
    pub unsafe fn new(data: *const u8, len: usize) -> Self {
        MemDisk { data, len }
    }
}

impl Disk for MemDisk {
    fn sectors(&self) -> u64 {
        (self.len / 512) as u64
    }
    fn read(&mut self, sector: u64, buf: &mut [u8]) -> bool {
        let Some(off) = usize::try_from(sector).ok().and_then(|s| s.checked_mul(512)) else { return false };
        if off.checked_add(buf.len()).is_none_or(|e| e > self.len) {
            return false;
        }
        // SAFETY: in bounds (checked above); the image outlives the disk.
        unsafe { ptr::copy_nonoverlapping(self.data.add(off), buf.as_mut_ptr(), buf.len()) };
        true
    }
}

/// An Ethernet link: whole frames, no FCS.
pub trait NetLink {
    fn mac(&self) -> [u8; 6];
    fn link_up(&self) -> bool {
        true
    }
    /// Transmit one frame (dropping it is allowed).
    fn send(&mut self, frame: &[u8]);
    /// Take the next received frame into `buf`; its length.
    fn recv(&mut self, buf: &mut [u8; FRAME_MAX]) -> Option<usize>;
}

/// No network (the device is left out of the platform).
pub struct NoNet;

impl NetLink for NoNet {
    fn mac(&self) -> [u8; 6] {
        [0; 6]
    }
    fn link_up(&self) -> bool {
        false
    }
    fn send(&mut self, _frame: &[u8]) {}
    fn recv(&mut self, _buf: &mut [u8; FRAME_MAX]) -> Option<usize> {
        None
    }
}

// ------------------------------------------------------------------ guest memory

/// Guest RAM as the device sees it (guest physical addresses).
#[derive(Clone, Copy)]
pub struct GuestMem {
    pub ptr: *mut u8,
    pub base: u32,
    pub size: u32,
    /// The first `ro` bytes are read-only (flash-backed kernel text): no
    /// device writes there.
    pub ro: u32,
}

impl GuestMem {
    pub(crate) fn range(&self, pa: u64, len: usize) -> Option<*mut u8> {
        let off = pa.checked_sub(self.base as u64)?;
        (off.checked_add(len as u64)? <= self.size as u64).then(|| {
            // SAFETY: in bounds of guest RAM (checked above).
            unsafe { self.ptr.add(off as usize) }
        })
    }
    /// As `range`, for a device write.
    pub(crate) fn range_mut(&self, pa: u64, len: usize) -> Option<*mut u8> {
        if pa < self.base as u64 + self.ro as u64 {
            return None;
        }
        self.range(pa, len)
    }
    fn read<const N: usize>(&self, pa: u64) -> Option<[u8; N]> {
        let p = self.range(pa, N)?;
        let mut b = [0u8; N];
        // SAFETY: `range` checked the bounds.
        unsafe { ptr::copy_nonoverlapping(p, b.as_mut_ptr(), N) };
        Some(b)
    }
    fn u16(&self, pa: u64) -> Option<u16> {
        self.read::<2>(pa).map(u16::from_le_bytes)
    }
    fn u32(&self, pa: u64) -> Option<u32> {
        self.read::<4>(pa).map(u32::from_le_bytes)
    }
    fn u64(&self, pa: u64) -> Option<u64> {
        self.read::<8>(pa).map(u64::from_le_bytes)
    }
    fn write(&self, pa: u64, data: &[u8]) -> bool {
        match self.range_mut(pa, data.len()) {
            Some(p) => {
                // SAFETY: `range` checked the bounds.
                unsafe { ptr::copy_nonoverlapping(data.as_ptr(), p, data.len()) };
                true
            }
            None => false,
        }
    }
}

// ------------------------------------------------------------------ transport

#[derive(Default, Clone, Copy)]
struct Queue {
    num: u32,
    ready: bool,
    desc: u64,
    avail: u64,
    used: u64,
    last_avail: u16,
}

#[derive(Clone, Copy)]
struct Desc {
    addr: u64,
    len: u32,
    flags: u16,
}

const NO_DESC: Desc = Desc { addr: 0, len: 0, flags: 0 };
const DIRTY_MAX: usize = 8;

/// What a register store asks of the device.
enum Kick {
    None,
    Notify(u32),
}

/// The virtio-mmio registers and virtqueues, shared by the devices.
struct Transport {
    mem: GuestMem,
    device_id: u32,
    nqueues: usize,
    /// QueueNumMax of each queue
    qmax: [u32; NQUEUES],
    /// device feature words 0 and 1
    features: [u32; 2],
    status: u32,
    dev_features_sel: u32,
    drv_features: [u32; 2],
    drv_features_sel: u32,
    queue_sel: u32,
    q: [Queue; NQUEUES],
    int_status: u32,
    config_generation: u32,
    /// Guest RAM written since the last `take_dma_write`, as [lo, hi)
    /// ranges. Kept apart, not merged into one span: a request's data and
    /// its status byte can be megabytes apart, and the translator drops the
    /// code of every page in a reported range.
    dirty: [(u32, u32); DIRTY_MAX],
    ndirty: usize,
}

impl Transport {
    fn new(mem: GuestMem, device_id: u32, sizes: &[u32], features_lo: u32) -> Self {
        let mut qmax = [0; NQUEUES];
        qmax[..sizes.len()].copy_from_slice(sizes);
        Transport {
            mem,
            device_id,
            nqueues: sizes.len(),
            qmax,
            features: [features_lo, F_HI_VERSION_1],
            status: 0,
            dev_features_sel: 0,
            drv_features: [0; 2],
            drv_features_sel: 0,
            queue_sel: 0,
            q: [Queue::default(); NQUEUES],
            int_status: 0,
            config_generation: 0,
            dirty: [(0, 0); DIRTY_MAX],
            ndirty: 0,
        }
    }

    fn reset(&mut self) {
        self.status = 0;
        self.drv_features = [0; 2];
        self.q = [Queue::default(); NQUEUES];
        self.int_status = 0;
    }

    fn driver_ok(&self) -> bool {
        self.status & 4 != 0
    }

    /// The selected queue, if it exists.
    fn sel(&mut self) -> Option<&mut Queue> {
        let n = self.nqueues;
        self.q.get_mut(self.queue_sel as usize).filter(|_| (self.queue_sel as usize) < n)
    }

    fn load(&self, off: u32) -> u32 {
        let q = self.q.get(self.queue_sel as usize).filter(|_| (self.queue_sel as usize) < self.nqueues);
        match off {
            0x000 => MAGIC,
            0x004 => 2,
            0x008 => self.device_id,
            0x00c => VENDOR,
            0x010 => self.features.get(self.dev_features_sel as usize).copied().unwrap_or(0),
            0x034 => q.map_or(0, |_| self.qmax[self.queue_sel as usize]),
            0x044 => q.is_some_and(|q| q.ready) as u32,
            0x060 => self.int_status,
            0x070 => self.status,
            0x0fc => self.config_generation,
            _ => 0,
        }
    }

    fn store(&mut self, off: u32, v: u32) -> Kick {
        let lo = |old: u64| (old & !0xFFFF_FFFF) | v as u64;
        let hi = |old: u64| (old & 0xFFFF_FFFF) | (v as u64) << 32;
        match off {
            0x014 => self.dev_features_sel = v,
            0x020 => {
                if let Some(f) = self.drv_features.get_mut(self.drv_features_sel as usize) {
                    *f = v;
                }
            }
            0x024 => self.drv_features_sel = v,
            0x030 => self.queue_sel = v,
            0x050 => return Kick::Notify(v),
            0x064 => self.int_status &= !v,
            0x070 => {
                if v == 0 {
                    self.reset();
                } else {
                    self.status = v;
                }
            }
            _ => {
                let max = self.qmax.get(self.queue_sel as usize).copied().unwrap_or(0);
                if let Some(q) = self.sel() {
                    match off {
                        0x038 => q.num = v.min(max),
                        0x044 => q.ready = v & 1 != 0,
                        0x080 => q.desc = lo(q.desc),
                        0x084 => q.desc = hi(q.desc),
                        0x090 => q.avail = lo(q.avail),
                        0x094 => q.avail = hi(q.avail),
                        0x0a0 => q.used = lo(q.used),
                        0x0a4 => q.used = hi(q.used),
                        _ => {}
                    }
                }
            }
        }
        Kick::None
    }

    fn queue_live(&self, qi: usize) -> bool {
        let q = &self.q[qi];
        q.ready && q.num != 0 && self.driver_ok()
    }

    /// Next available descriptor chain head on queue `qi`, without taking it.
    fn peek_avail(&self, qi: usize) -> Option<u16> {
        if !self.queue_live(qi) {
            return None;
        }
        let q = &self.q[qi];
        let avail_idx = self.mem.u16(q.avail + 2)?;
        if q.last_avail == avail_idx {
            return None;
        }
        self.mem.u16(q.avail + 4 + 2 * (q.last_avail as u64 % q.num as u64))
    }

    /// Return chain `head` on queue `qi` with `written` bytes, and interrupt.
    fn push_used(&mut self, qi: usize, head: u16, written: u32) {
        let q = self.q[qi];
        // used ring: flags, idx, then { id: u32, len: u32 }[num]
        if let Some(used_idx) = self.mem.u16(q.used + 2) {
            let e = q.used + 4 + 8 * (used_idx as u64 % q.num as u64);
            let mut ent = [0u8; 8];
            ent[..4].copy_from_slice(&(head as u32).to_le_bytes());
            ent[4..].copy_from_slice(&written.to_le_bytes());
            self.mem.write(e, &ent);
            self.mem.write(q.used + 2, &used_idx.wrapping_add(1).to_le_bytes());
        }
        self.q[qi].last_avail = q.last_avail.wrapping_add(1);
        self.int_status |= 1;
    }

    /// The descriptor chain at `head` on queue `qi` into `chain`; its length.
    fn chain(&self, qi: usize, head: u16, chain: &mut [Desc; CHAIN_MAX]) -> usize {
        let q = &self.q[qi];
        let mut n = 0;
        let mut i = head;
        loop {
            if i as u32 >= q.num {
                return 0;
            }
            let a = q.desc + 16 * i as u64;
            let (Some(addr), Some(len), Some(flags), Some(next)) =
                (self.mem.u64(a), self.mem.u32(a + 8), self.mem.u16(a + 12), self.mem.u16(a + 14))
            else {
                return 0;
            };
            chain[n] = Desc { addr, len, flags };
            n += 1;
            if flags & DESC_F_NEXT == 0 || n == chain.len() {
                return n;
            }
            i = next;
        }
    }

    fn note_dirty(&mut self, pa: u64, len: usize) {
        let (lo, hi) = (pa as u32, (pa + len as u64) as u32);
        let live = &mut self.dirty[..self.ndirty];
        // touching an existing range (or no room left): widen it
        let near = live.iter().position(|&(a, b)| lo <= b && a <= hi);
        match near.or(if self.ndirty == DIRTY_MAX { Some(DIRTY_MAX - 1) } else { None }) {
            Some(i) => live[i] = (live[i].0.min(lo), live[i].1.max(hi)),
            None => {
                self.dirty[self.ndirty] = (lo, hi);
                self.ndirty += 1;
            }
        }
    }

    /// Write `data` into guest RAM at `pa`, noting it for `take_dma_write`.
    fn dma_write(&mut self, pa: u64, data: &[u8]) -> bool {
        let ok = self.mem.write(pa, data);
        if ok {
            self.note_dirty(pa, data.len());
        }
        ok
    }

    fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.ndirty = self.ndirty.checked_sub(1)?;
        Some(self.dirty[self.ndirty])
    }
}

/// A config-space read of `size` bytes at `off` from the bytes `cfg`.
fn config_read(cfg: &[u8], off: u32, size: u32) -> u32 {
    (0..size).fold(0, |v, i| v | (cfg.get((off + i) as usize).copied().unwrap_or(0) as u32) << (8 * i))
}

// ------------------------------------------------------------------ block device

pub struct VirtioBlk<D: Disk> {
    pub disk: D,
    t: Transport,
}

impl<D: Disk> VirtioBlk<D> {
    pub fn new(disk: D, mem: GuestMem) -> Self {
        let ro = if disk.read_only() { BLK_F_RO } else { 0 };
        let bs = if disk.block_size() != 512 { BLK_F_BLK_SIZE } else { 0 };
        VirtioBlk { disk, t: Transport::new(mem, DEVICE_BLK, &[BLK_QUEUE], ro | bs) }
    }

    pub fn irq_line(&self) -> bool {
        self.t.int_status != 0
    }

    pub fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.t.take_dma_write()
    }

    pub fn load(&mut self, off: u32, size: u32) -> Result<u32, u32> {
        if off >= 0x100 {
            // virtio_blk_config: u64 capacity, size_max, seg_max, geometry,
            // blk_size; the rest isn't offered
            let mut cfg = [0u8; 24];
            cfg[..8].copy_from_slice(&self.disk.sectors().to_le_bytes());
            cfg[20..].copy_from_slice(&self.disk.block_size().to_le_bytes());
            return Ok(config_read(&cfg, off - 0x100, size));
        }
        if size != 4 {
            return Err(crate::cpu::exc::LOAD_FAULT);
        }
        Ok(self.t.load(off))
    }

    pub fn store(&mut self, off: u32, size: u32, v: u32) -> Result<(), u32> {
        if off >= 0x100 {
            return Ok(()); // config space is read-only here
        }
        if size != 4 {
            return Err(crate::cpu::exc::STORE_FAULT);
        }
        if let Kick::Notify(0) = self.t.store(off, v) {
            while let Some(head) = self.t.peek_avail(0) {
                let written = self.request(head);
                self.t.push_used(0, head, written);
            }
        }
        Ok(())
    }

    /// One request: header descriptor, data descriptors, status descriptor.
    /// Returns the bytes written into guest memory.
    fn request(&mut self, head: u16) -> u32 {
        let mut chain = [NO_DESC; CHAIN_MAX];
        let n = self.t.chain(0, head, &mut chain);
        if n < 2 || chain[0].len < 16 || chain[n - 1].flags & DESC_F_WRITE == 0 {
            return 0;
        }
        let mem = self.t.mem;
        let (Some(kind), Some(sector)) = (mem.u32(chain[0].addr), mem.u64(chain[0].addr + 8)) else {
            return 0;
        };
        let data = &chain[1..n - 1];
        let mut written = 0u32;
        let status = match kind {
            BLK_T_IN => {
                let mut s = sector;
                let mut ok = true;
                for d in data {
                    if d.flags & DESC_F_WRITE == 0 {
                        ok = false;
                        break;
                    }
                    let Some(p) = mem.range_mut(d.addr, d.len as usize) else {
                        ok = false;
                        break;
                    };
                    // SAFETY: `range_mut` checked the bounds; guest RAM is ours.
                    let buf = unsafe { core::slice::from_raw_parts_mut(p, d.len as usize) };
                    if !self.disk.read(s, buf) {
                        ok = false;
                        break;
                    }
                    self.t.note_dirty(d.addr, d.len as usize);
                    written += d.len;
                    s += d.len as u64 / 512;
                }
                if ok { BLK_S_OK } else { BLK_S_IOERR }
            }
            BLK_T_OUT => {
                let mut s = sector;
                let mut ok = !self.disk.read_only();
                for d in data {
                    if !ok {
                        break;
                    }
                    let Some(p) = mem.range(d.addr, d.len as usize) else {
                        ok = false;
                        break;
                    };
                    // SAFETY: `range` checked the bounds.
                    let buf = unsafe { core::slice::from_raw_parts(p, d.len as usize) };
                    ok = self.disk.write(s, buf);
                    s += d.len as u64 / 512;
                }
                if ok { BLK_S_OK } else { BLK_S_IOERR }
            }
            BLK_T_FLUSH => BLK_S_OK,
            BLK_T_GET_ID => {
                let id = self.disk.id();
                match data.first() {
                    Some(d) if d.flags & DESC_F_WRITE != 0 => {
                        let len = (d.len as usize).min(id.len());
                        self.t.dma_write(d.addr, &id[..len]);
                        written += len as u32;
                        BLK_S_OK
                    }
                    _ => BLK_S_IOERR,
                }
            }
            _ => BLK_S_UNSUPP,
        };
        self.t.dma_write(chain[n - 1].addr, &[status]);
        written + 1
    }
}

// ------------------------------------------------------------------ network device

pub struct VirtioNet<L: NetLink> {
    pub link: L,
    t: Transport,
    link_was_up: bool,
}

impl<L: NetLink> VirtioNet<L> {
    pub fn new(link: L, mem: GuestMem) -> Self {
        let up = link.link_up();
        VirtioNet { link, t: Transport::new(mem, DEVICE_NET, &[NET_RX_QUEUE, NET_TX_QUEUE], NET_F_MAC | NET_F_STATUS), link_was_up: up }
    }

    pub fn irq_line(&self) -> bool {
        self.t.int_status != 0
    }

    pub fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.t.take_dma_write()
    }

    /// virtio_net_config: mac[6], status u16
    fn config(&self) -> [u8; 8] {
        let mut c = [0u8; 8];
        c[..6].copy_from_slice(&self.link.mac());
        c[6..].copy_from_slice(&(if self.link.link_up() { NET_S_LINK_UP } else { 0 }).to_le_bytes());
        c
    }

    pub fn load(&mut self, off: u32, size: u32) -> Result<u32, u32> {
        if off >= 0x100 {
            return Ok(config_read(&self.config(), off - 0x100, size));
        }
        if size != 4 {
            return Err(crate::cpu::exc::LOAD_FAULT);
        }
        Ok(self.t.load(off))
    }

    pub fn store(&mut self, off: u32, size: u32, v: u32) -> Result<(), u32> {
        if off >= 0x100 {
            return Ok(()); // the MAC is the link's
        }
        if size != 4 {
            return Err(crate::cpu::exc::STORE_FAULT);
        }
        match self.t.store(off, v) {
            Kick::Notify(1) => self.transmit(),
            Kick::Notify(_) => self.poll(),
            Kick::None => {}
        }
        Ok(())
    }

    /// Send every frame on the transmit queue.
    fn transmit(&mut self) {
        let mut frame = [0u8; NET_HDR + FRAME_MAX];
        while let Some(head) = self.t.peek_avail(1) {
            let mut chain = [NO_DESC; CHAIN_MAX];
            let n = self.t.chain(1, head, &mut chain);
            let mut len = 0usize;
            let mut ok = n != 0;
            for d in &chain[..n] {
                let dl = d.len as usize;
                let Some(p) = self.t.mem.range(d.addr, dl).filter(|_| len + dl <= frame.len()) else {
                    ok = false;
                    break;
                };
                // SAFETY: `range` checked the bounds.
                unsafe { ptr::copy_nonoverlapping(p, frame.as_mut_ptr().add(len), dl) };
                len += dl;
            }
            if ok && len > NET_HDR {
                self.link.send(&frame[NET_HDR..len]);
            }
            self.t.push_used(1, head, 0);
        }
    }

    /// Deliver received frames while the guest has buffers posted, and
    /// signal link changes. Call it regularly (each device poll).
    pub fn poll(&mut self) {
        let up = self.link.link_up();
        if up != self.link_was_up {
            self.link_was_up = up;
            self.t.config_generation = self.t.config_generation.wrapping_add(1);
            self.t.int_status |= 2;
        }
        let mut frame = [0u8; FRAME_MAX];
        while let Some(head) = self.t.peek_avail(0) {
            let Some(len) = self.link.recv(&mut frame) else { break };
            let mut chain = [NO_DESC; CHAIN_MAX];
            let n = self.t.chain(0, head, &mut chain);
            // header (num_buffers = 1), then the frame, across the buffers
            let mut hdr = [0u8; NET_HDR];
            hdr[10] = 1;
            let (mut src_hdr, mut src) = (&hdr[..], &frame[..len]);
            let mut written = 0u32;
            for d in chain[..n].iter().filter(|d| d.flags & DESC_F_WRITE != 0) {
                let mut at = d.addr;
                let mut room = d.len as usize;
                for part in [&mut src_hdr, &mut src] {
                    let k = part.len().min(room);
                    if k != 0 && self.t.dma_write(at, &part[..k]) {
                        *part = &part[k..];
                        at += k as u64;
                        room -= k;
                        written += k as u32;
                    }
                }
            }
            // a frame that didn't fit is dropped, as a real NIC would
            self.t.push_used(0, head, if src.is_empty() { written } else { 0 });
        }
    }
}
