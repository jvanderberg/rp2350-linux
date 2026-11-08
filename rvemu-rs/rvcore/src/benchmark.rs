//! Bounded benchmark snapshots. Hosts record at SBI boundaries and print only
//! after execution, so serial output is outside the measured region.
use crate::Cpu;
pub const EXT: u32 = 0x0a00_0002;

#[derive(Clone, Copy, Default)]
pub struct Point {
    pub tag: u32,
    pub guest: u64,
    pub host: u64,
    pub ns: u64,
    pub idle: u64,
    pub native: [u64; 13],
}

impl core::fmt::Display for Point {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(
            f,
            "benchmark point tag={} guest={} host={} ns={} idle={} native={:?}",
            self.tag, self.guest, self.host, self.ns, self.idle, self.native
        )
    }
}

#[derive(Default)]
pub struct Log {
    points: [Point; 32],
    len: usize,
}

impl Log {
    pub fn record(&mut self, cpu: &Cpu, tag: u32, host: u64, ns: u64) -> bool {
        if self.len == self.points.len() || tag > 3 {
            return false;
        }
        self.points[self.len] = Point {
            tag,
            guest: cpu.instret,
            host,
            ns,
            idle: cpu.stats.idle_ticks,
            native: cpu.stats.native_buffer_calls,
        };
        self.len += 1;
        true
    }
    pub fn clear(&mut self) {
        self.len = 0;
    }
    pub fn points(&self) -> &[Point] {
        &self.points[..self.len]
    }
}
