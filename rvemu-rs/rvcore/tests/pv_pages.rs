use rvcore::{Bus, Cpu};
#[cfg(feature = "pv-pages")]
use rvcore::{Priv, exc};
const BASE: u32 = 0x8000_0000;
const EXT: u32 = 0x0a00_0000;
struct NoBus;
impl Bus for NoBus {
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
}
fn call(c: &mut Cpu, eid: u32, fid: u32, dst: u32, src: u32) -> i32 {
    c.pc = BASE;
    c.x[17] = eid;
    c.x[16] = fid;
    c.x[10] = dst;
    c.x[11] = src;
    c.step(&mut NoBus);
    c.x[10] as i32
}
#[test]
fn probe_matches_feature() {
    let mut ram = [0u32; 4096];
    ram[0] = 0x73;
    let mut c = unsafe { Cpu::new(ram.as_mut_ptr().cast(), BASE, 16384, BASE, 0) };
    assert_eq!(call(&mut c, 0x10, 3, EXT, 0), 0);
    assert_eq!(c.x[11], cfg!(feature = "pv-pages") as u32);
    if !cfg!(feature = "pv-pages") {
        assert_eq!(call(&mut c, EXT, 0, BASE + 4096, 0), -2);
    }
}
#[test]
#[cfg(feature = "pv-pages")]
fn pages_errors_and_privilege() {
    let mut ram = [0xfeedbeefu32; 4096];
    ram[0] = 0x73;
    for (i, w) in ram[2048..3072].iter_mut().enumerate() {
        *w = i as u32 ^ 0xdeadbeef;
    }
    let mut c = unsafe { Cpu::new(ram.as_mut_ptr().cast(), BASE, 16384, BASE, 0) };
    c.set_readonly_prefix(4096);
    let code_pages = [0b10u8];
    c.code_pages = code_pages.as_ptr();
    c.res_valid = true;
    let before = ram;
    assert_eq!(call(&mut c, EXT, 0, BASE + 4096, 0), 0);
    assert!(ram[1024..2048].iter().all(|&v| v == 0));
    assert_eq!(&ram[..1024], &before[..1024]);
    assert_eq!(&ram[2048..], &before[2048..]);
    assert!(!c.res_valid);
    assert_eq!(c.smc_page.take(), Some(BASE + 4096));
    assert_eq!(call(&mut c, EXT, 1, BASE + 4096, BASE + 8192), 0);
    assert_eq!(&ram[1024..2048], &ram[2048..3072]);
    let before = ram;
    assert_eq!(call(&mut c, EXT, 1, BASE + 4096, BASE + 4096), 0);
    assert_eq!(ram, before);
    for (fid, dst, src, err) in [
        (2, BASE + 4096, 0, -2),
        (0, BASE, 0, -3),
        (0, BASE + 4097, 0, -3),
        (0, BASE - 4096, 0, -3),
        (0, BASE + 16384, 0, -3),
        (0, 0xfffff000, 0, -3),
        (1, BASE + 4096, BASE + 16384, -3),
        (1, BASE + 4096, BASE + 8193, -3),
    ] {
        c.smc_page = None;
        assert_eq!(call(&mut c, EXT, fid, dst, src), err);
        assert_eq!(ram, before);
        assert_eq!(c.smc_page, None);
    }
    // The last full page is legal; readonly text can be a source.
    assert_eq!(call(&mut c, EXT, 1, BASE + 12288, BASE), 0);
    assert_eq!(&ram[3072..], &ram[..1024]);
    let before = ram;
    c.privilege = Priv::U;
    call(&mut c, EXT, 0, BASE + 4096, 0);
    assert_eq!(c.scause, exc::ECALL_U);
    assert_eq!(ram, before);
}
