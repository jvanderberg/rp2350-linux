//! The Linux guest in flash (rp2350/tools/pack-guest.py writes it, picotool
//! loads it): a blob at `BLOB_ADDR` (header, the kernel Image after its
//! text, initramfs, DTB) and the kernel's text at the end of flash.
//!
//! Flash ends at 0x11000000, exactly where the PSRAM's window begins. With
//! the text in the last `text_len` bytes of flash, guest RAM is one block of
//! host addresses: guest PA 0.. is the kernel text, read in place from flash
//! (read-only to the guest), and the PSRAM follows. The text then costs no
//! PSRAM: the guest gets all of it for everything else.

use core::ptr::read_volatile;

pub const BLOB_ADDR: usize = 0x1040_0000;
const BLOB_MAGIC: u32 = 0x594C_5652; // "RVLY": header with the text fields
/// Blob contents must end below this offset (the disk follows).
const BLOB_MAX: u32 = 2 << 20;
/// End of flash = start of the PSRAM window.
pub const TEXT_END: usize = 0x1100_0000;
/// Largest kernel text kept in flash (it starts at TEXT_END - text_len).
pub const TEXT_MAX: u32 = 3 << 20;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct BlobHeader {
    pub magic: u32,
    /// Guest RAM as Linux sees it: text_len + PSRAM part.
    pub ram_size: u32,
    /// The Image after its first `text_len` bytes.
    pub kernel_off: u32,
    pub kernel_len: u32,
    pub initrd_off: u32,
    pub initrd_len: u32,
    pub dtb_off: u32,
    pub dtb_len: u32,
    /// The Image's first bytes, in flash at TEXT_END - text_len (page multiple).
    pub text_len: u32,
    /// FNV-1a of that text, to catch a stale copy.
    pub text_fnv: u32,
}

/// FNV-1a over `len` bytes at `p`.
///
/// # Safety
/// `p..p + len` must be readable.
unsafe fn fnv1a(p: *const u8, len: usize) -> u32 {
    let mut h: u32 = 0x811c_9dc5;
    for i in 0..len {
        // SAFETY: per the contract.
        h = (h ^ u32::from(unsafe { *p.add(i) })).wrapping_mul(0x0100_0193);
    }
    h
}

impl BlobHeader {
    pub fn read() -> Self {
        // SAFETY: flash is mapped at BLOB_ADDR (XIP).
        unsafe { read_volatile(BLOB_ADDR as *const BlobHeader) }
    }

    /// Host address of guest PA 0.
    pub fn ram_host(&self) -> usize {
        TEXT_END - self.text_len as usize
    }

    /// Bytes of guest RAM in PSRAM.
    pub fn psram_len(&self) -> u32 {
        self.ram_size - self.text_len
    }

    /// Well formed, at most `psram_max` bytes of PSRAM, and the text in
    /// flash matches.
    pub fn valid(&self, psram_max: u32) -> bool {
        let fits = |off: u32, len: u32| off.checked_add(len).is_some_and(|e| e <= BLOB_MAX);
        let ok = self.magic == BLOB_MAGIC
            && self.text_len <= TEXT_MAX
            && self.text_len & 0xFFF == 0
            && self.ram_size > self.text_len
            && self.psram_len() <= psram_max
            && self.psram_len() >= 1 << 20
            && fits(self.kernel_off, self.kernel_len)
            && fits(self.initrd_off, self.initrd_len)
            && fits(self.dtb_off, self.dtb_len)
            && self.dtb_len <= 0x10000
            && self.initrd_len < self.psram_len() / 2
            && self.text_len + self.kernel_len <= (self.ram_size - 0x10000 - self.initrd_len) & !0xFFF;
        // SAFETY: the text range is mapped flash (checked against TEXT_MAX).
        ok && unsafe { fnv1a(self.ram_host() as *const u8, self.text_len as usize) } == self.text_fnv
    }

    /// Fresh guest RAM, laid out as the Mac host does it; returns the DTB's
    /// guest physical address. Guest PA 0 is `ram_host()`.
    ///
    /// # Safety
    /// The PSRAM after TEXT_END must be `psram_len()` writable bytes, and the
    /// header valid.
    pub unsafe fn load(&self) -> u32 {
        let ram = self.ram_host() as *mut u8;
        let dtb_addr = self.ram_size - 0x10000;
        let initrd_addr = (dtb_addr - self.initrd_len) & !0xFFF;
        let src = |off: u32| (BLOB_ADDR + off as usize) as *const u8;
        let t = self.text_len as usize;
        // SAFETY: per the contract; everything written is at or above guest
        // PA text_len, i.e. in PSRAM.
        unsafe {
            core::ptr::write_bytes(ram.add(t), 0, self.psram_len() as usize);
            core::ptr::copy_nonoverlapping(src(self.kernel_off), ram.add(t), self.kernel_len as usize);
            core::ptr::copy_nonoverlapping(src(self.initrd_off), ram.add(initrd_addr as usize), self.initrd_len as usize);
            core::ptr::copy_nonoverlapping(src(self.dtb_off), ram.add(dtb_addr as usize), self.dtb_len as usize);
        }
        dtb_addr
    }
}
