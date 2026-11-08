//! Counters, plus (with the `instrument` feature) unique-code tracking and a
//! model of the RP2350 XIP cache that fronts PSRAM.

#[derive(Clone)]
pub struct Stats {
    /// Retired instructions by major opcode.
    pub op: [u64; 128],
    pub tlb_miss: [u64; 3],
    pub tlb_flushes: u64,
    pub tlb_page_flushes: u64,
    pub exceptions: u64,
    pub interrupts: u64,
    pub sbi_calls: u64,
    pub native_buffer_calls: [u64; 13],
    pub wfi: u64,
    pub idle_ticks: u64,
    pub unique_insns: u64,
    pub unique_code_pages: u64,
    /// Snapshot at the first `sret` into U-mode.
    pub user_instret: u64,
    pub user_unique_insns: u64,
    /// Snapshot when the host's `--until` string was printed.
    pub until_instret: u64,
    pub until_unique_insns: u64,
    /// Register operand uses (approximate: decoded rd/rs1/rs2 fields by format).
    pub reg_use: [u64; 32],
}

impl Stats {
    pub const fn new() -> Self {
        Stats {
            op: [0; 128],
            tlb_miss: [0; 3],
            tlb_flushes: 0,
            tlb_page_flushes: 0,
            exceptions: 0,
            interrupts: 0,
            sbi_calls: 0,
            native_buffer_calls: [0; 13],
            wfi: 0,
            idle_ticks: 0,
            unique_insns: 0,
            unique_code_pages: 0,
            user_instret: 0,
            user_unique_insns: 0,
            until_instret: 0,
            until_unique_insns: 0,
            reg_use: [0; 32],
        }
    }
}

impl Default for Stats {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(feature = "instrument")]
pub use enabled::*;

#[cfg(feature = "instrument")]
mod enabled {
    use super::Stats;
    use alloc::vec;
    use alloc::vec::Vec;

    /// Set-associative cache model (1 or 2 ways, LRU) over guest physical
    /// addresses.
    pub struct XipCache {
        sets: u32,
        ways: u32,
        line_shift: u32,
        tags: Vec<u32>,
        mru: Vec<u8>,
        pub accesses: u64,
        pub misses: u64,
    }

    impl XipCache {
        pub fn new(size: u32, ways: u32, line: u32) -> Self {
            assert!(ways == 1 || ways == 2);
            let sets = size / (line * ways);
            assert!(sets.is_power_of_two());
            XipCache {
                sets,
                ways,
                line_shift: line.trailing_zeros(),
                tags: vec![u32::MAX; (sets * ways) as usize],
                mru: vec![0; sets as usize],
                accesses: 0,
                misses: 0,
            }
        }

        #[inline]
        pub fn access(&mut self, pa: u32) {
            let line = pa >> self.line_shift;
            let set = (line & (self.sets - 1)) as usize;
            let base = set * self.ways as usize;
            self.accesses += 1;
            for w in 0..self.ways as usize {
                if self.tags[base + w] == line {
                    self.mru[set] = w as u8;
                    return;
                }
            }
            self.misses += 1;
            let victim = if self.ways == 1 { 0 } else { (self.mru[set] ^ 1) as usize };
            self.tags[base + victim] = line;
            self.mru[set] = victim as u8;
        }
    }

    pub struct Model {
        /// Sees guest fetches and data (what an interpreter touches in PSRAM).
        pub interp: XipCache,
        /// Sees guest data only (translated code lives in SRAM).
        pub jit: XipCache,
        /// Also fed guest data; the simulator adds its own accesses (code
        /// fetched through XIP) to these.
        pub extra: Vec<XipCache>,
    }

    pub struct Instrument {
        code_bits: Vec<u8>,
        page_bits: Vec<u8>,
        pub model: Option<Model>,
        /// Per-step flags for the translator simulator (the driver clears them
        /// before each step): RAM page index of the last store, and whether a
        /// data access was misaligned (translated code exits for those).
        pub step_store_page: Option<u32>,
        pub step_misaligned: bool,
    }

    impl Instrument {
        pub fn new(ram_size: u32) -> Self {
            Instrument {
                code_bits: vec![0; (ram_size / 32) as usize],
                page_bits: vec![0; (ram_size / 4096 / 8 + 1) as usize],
                model: None,
                step_store_page: None,
                step_misaligned: false,
            }
        }

        pub fn enable_model(&mut self) {
            // RP2350 XIP cache: 16 KiB, 2-way, 8-byte lines (to be verified).
            self.model = Some(Model {
                interp: XipCache::new(16384, 2, 8),
                jit: XipCache::new(16384, 2, 8),
                extra: Vec::new(),
            });
        }

        #[inline]
        pub fn fetch(&mut self, stats: &mut Stats, off: u32) {
            let w = (off >> 2) as usize;
            let bit = 1u8 << (w & 7);
            if self.code_bits[w >> 3] & bit == 0 {
                self.code_bits[w >> 3] |= bit;
                stats.unique_insns += 1;
                let p = (off >> 12) as usize;
                let pbit = 1u8 << (p & 7);
                if self.page_bits[p >> 3] & pbit == 0 {
                    self.page_bits[p >> 3] |= pbit;
                    stats.unique_code_pages += 1;
                }
            }
            if let Some(m) = &mut self.model {
                m.interp.access(off);
            }
        }

        #[inline]
        pub fn data(&mut self, off: u32) {
            if let Some(m) = &mut self.model {
                m.interp.access(off);
                m.jit.access(off);
                for x in &mut m.extra {
                    x.access(off);
                }
            }
        }

        #[inline]
        pub fn load(&mut self, off: u32, size: u32) {
            self.data(off);
            if off & (size - 1) != 0 {
                self.step_misaligned = true;
            }
        }

        #[inline]
        pub fn store(&mut self, off: u32, size: u32) {
            self.data(off);
            if off & (size - 1) != 0 {
                self.step_misaligned = true;
            }
            self.step_store_page = Some(off >> 12);
        }

        /// A misaligned access that crossed a page and was split into bytes.
        #[inline]
        pub fn misaligned(&mut self) {
            self.step_misaligned = true;
        }
    }
}

/// Zero-cost stand-in when instrumentation is compiled out.
#[cfg(not(feature = "instrument"))]
pub struct Instrument;

#[cfg(not(feature = "instrument"))]
impl Instrument {
    pub fn new(_ram_size: u32) -> Self {
        Instrument
    }
    #[inline(always)]
    pub fn fetch(&mut self, _stats: &mut Stats, _off: u32) {}
    #[inline(always)]
    pub fn data(&mut self, _off: u32) {}
    #[inline(always)]
    pub fn load(&mut self, _off: u32, _size: u32) {}
    #[inline(always)]
    pub fn store(&mut self, _off: u32, _size: u32) {}
    #[inline(always)]
    pub fn misaligned(&mut self) {}
}
