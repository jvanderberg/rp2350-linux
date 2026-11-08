use rvcore::{Bus, Cpu};
const BASE: u32 = 0x80000000;
const EXT: u32 = 0x0a000001;
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
fn call(c: &mut Cpu, eid: u32, fid: u32, args: [u32; 6]) -> (i32, u32) {
    c.pc = BASE;
    c.x[17] = eid;
    c.x[16] = fid;
    c.x[10..16].copy_from_slice(&args);
    c.step(&mut NoBus);
    (c.x[10] as i32, c.x[11])
}
#[test]
fn discovery() {
    let mut ram = vec![0u32; 16384];
    ram[0] = 0x73;
    let mut c = unsafe { Cpu::new(ram.as_mut_ptr().cast(), BASE, 65536, BASE, 0) };
    #[cfg(feature = "pv-buffers")]
    assert_eq!(c.stats.native_buffer_calls.len(), rvaccel::OP_COUNT);
    assert_eq!(
        call(&mut c, 0x10, 3, [EXT, 0, 0, 0, 0, 0]),
        (0, cfg!(feature = "pv-buffers") as u32)
    );
    if !cfg!(feature = "pv-buffers") {
        assert_eq!(call(&mut c, EXT, 0, [0; 6]).0, -2);
    }
}
#[test]
#[cfg(feature = "pv-buffers")]
fn range_checks_and_failure_notification() {
    let mut ram = vec![0xa5a5a5a5u32; 16384];
    ram[0] = 0x73;
    // A raw literal LZ4 block starts at PA BASE+4096.
    ram[1024] = u32::from_le_bytes(*b"\x50hel");
    ram[1025] = u32::from_le_bytes(*b"lo!!");
    let mut c = unsafe { Cpu::new(ram.as_mut_ptr().cast(), BASE, 65536, BASE, 0) };
    c.set_readonly_prefix(4096);
    // Unaligned buffers and crossing a page boundary are legal.
    let args = [BASE + 12286, 16, BASE + 4096, 6, 0, 0];
    assert_eq!(call(&mut c, EXT, 0, args), (0, 5));
    let bytes = unsafe { core::slice::from_raw_parts(ram.as_ptr().cast::<u8>(), 65536) };
    assert_eq!(&bytes[12286..12291], b"hello");
    assert_eq!(bytes[12285], 0xa5);
    assert_eq!(bytes[12291], 0xa5);
    assert_eq!(c.take_native_write(), Some((BASE + 12286, BASE + 12302)));
    let before = ram.clone();
    for bad in [
        [BASE, 16, BASE + 4096, 6, 0, 0],
        [BASE + 4096, 16, BASE + 4096, 6, 0, 0],
        [BASE + 8192, 32769, BASE + 4096, 6, 0, 0],
        [BASE + 65530, 16, BASE + 4096, 6, 0, 0],
        [BASE + 8192, 16, BASE + 65535, 6, 0, 0],
        [BASE + 8192, 16, BASE - 1, 6, 0, 0],
        [BASE + 8192, 16, BASE + 4096, 6, BASE + 8192, 44],
        [BASE + 8192, 16, BASE + 4096, 6, BASE + 65530, 44],
        [0xfffffff0, 32, BASE + 4096, 6, 0, 0],
        [BASE + 8192, 16, BASE + 4096, 6, BASE + 16384, 4097],
    ] {
        assert_eq!(call(&mut c, EXT, 0, bad), (-3, 0));
        assert_eq!(ram, before);
        assert_eq!(c.take_native_write(), None);
    }
    // Bad input can modify output: dirty notification must also survive errors.
    ram[1024] = 0xf0;
    assert_eq!(
        call(&mut c, EXT, 0, [BASE + 8192, 8192, BASE + 4096, 1, 0, 0]),
        (-1, 0)
    );
    assert_eq!(c.take_native_write(), Some((BASE + 8192, BASE + 16384)));
    // Invalid op cannot mutate memory; U-mode is a syscall trap, not a native call.
    let before = ram.clone();
    assert_eq!(call(&mut c, EXT, 99, args), (-2, 0));
    assert_eq!(ram, before);
    c.privilege = rvcore::Priv::U;
    call(&mut c, EXT, 0, args);
    assert_eq!(c.scause, rvcore::exc::ECALL_U);
    assert_eq!(ram, before);
    assert_eq!(c.take_native_write(), None);
}
