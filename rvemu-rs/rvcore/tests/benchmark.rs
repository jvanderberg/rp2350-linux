use rvcore::{
    Bus, Cpu, Priv,
    benchmark::{EXT, Log},
};
const BASE: u32 = 0x80000000;
#[derive(Default)]
struct BenchBus {
    enabled: bool,
    log: Log,
}
impl Bus for BenchBus {
    fn mmio_load(&mut self, _: u32, _: u32) -> Result<u32, u32> {
        panic!("MMIO")
    }
    fn mmio_store(&mut self, _: u32, _: u32, _: u32) -> Result<(), u32> {
        panic!("MMIO")
    }
    fn external_irq(&self) -> bool {
        false
    }
    fn console_putc(&mut self, _: u8) {}
    fn console_getc(&mut self) -> Option<u8> {
        None
    }
    fn wfi(&mut self, _: &mut Cpu) {}
    fn benchmark_supported(&self) -> bool {
        self.enabled
    }
    fn benchmark_record(&mut self, cpu: &Cpu, tag: u32) -> bool {
        self.log.record(cpu, tag, 123, 456)
    }
    fn benchmark_report(&mut self, _: &Cpu) -> Option<u32> {
        let n = self.log.points().len() as u32;
        self.log.clear();
        Some(n)
    }
}
fn call(c: &mut Cpu, b: &mut BenchBus, eid: u32, fid: u32, arg: u32) -> (i32, u32) {
    c.pc = BASE;
    c.x[17] = eid;
    c.x[16] = fid;
    c.x[10] = arg;
    c.step(b);
    (c.x[10] as i32, c.x[11])
}
#[test]
fn benchmark_discovery_bounds_and_privilege() {
    let mut ram = vec![0u32; 1024];
    ram[0] = 0x73;
    let mut c = unsafe { Cpu::new(ram.as_mut_ptr().cast(), BASE, 4096, BASE, 0) };
    let mut b = BenchBus::default();
    assert_eq!(call(&mut c, &mut b, 0x10, 3, EXT), (0, 0));
    assert_eq!(call(&mut c, &mut b, EXT, 0, 0).0, -2);
    b.enabled = true;
    assert_eq!(call(&mut c, &mut b, 0x10, 3, EXT), (0, 1));
    assert_eq!(call(&mut c, &mut b, EXT, 2, 0).0, -2);
    assert_eq!(call(&mut c, &mut b, EXT, 0, 4).0, -3);
    assert!(b.log.points().is_empty());
    c.stats.native_buffer_calls[4] = 7;
    c.stats.idle_ticks = 99;
    let before = c.instret;
    assert_eq!(call(&mut c, &mut b, EXT, 0, 0), (0, 0));
    let p = &b.log.points()[0];
    assert_eq!(
        (p.guest, p.host, p.ns, p.idle, p.native[4]),
        (before, 123, 456, 99, 7)
    );
    for i in 1..32 {
        assert_eq!(call(&mut c, &mut b, EXT, 0, i % 4).0, 0);
    }
    assert_eq!(call(&mut c, &mut b, EXT, 0, 0).0, -3);
    assert_eq!(b.log.points().len(), 32);
    // report hands back the count and empties the log for the next run
    assert_eq!(call(&mut c, &mut b, EXT, 1, 0), (0, 32));
    assert!(b.log.points().is_empty());
    assert_eq!(call(&mut c, &mut b, EXT, 0, 0).0, 0);
    c.privilege = Priv::U;
    call(&mut c, &mut b, EXT, 0, 0);
    assert_eq!(c.scause, 8); // userspace ECALL traps to Linux, never the host service
    assert_eq!(b.log.points().len(), 1);
}
