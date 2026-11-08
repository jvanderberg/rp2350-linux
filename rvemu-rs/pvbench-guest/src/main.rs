#![no_std]
#![no_main]
use core::arch::{asm, global_asm};
global_asm!(
    r#"
.section .text.start,"ax"
.globl _start
_start:
 .option push
 .option norelax
 la sp, __stack_top
 .option pop
 call main
1: j 1b
"#
);
fn message(s: &str) {
    for b in s.bytes() {
        unsafe {
            asm!("ecall",in("a7")1,inout("a0") b as u32=>_,options(nostack));
        }
    }
}
fn stop(ok: bool) -> ! {
    message(if ok { "PV PASS\n" } else { "PV FAIL\n" });
    unsafe {
        asm!("ecall",in("a7")8,options(nostack));
    }
    loop {
        core::hint::spin_loop();
    }
}
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    stop(false)
}
#[unsafe(no_mangle)]
extern "C" fn main() -> ! {
    let config = unsafe { core::slice::from_raw_parts(0x100000 as *const u32, 8) };
    let (fid, len, cap, plen, repeats, native, expected_len, expected_err) = (
        config[0],
        config[1],
        config[2],
        config[3],
        config[4],
        config[5],
        config[6],
        config[7] as i32,
    );
    let src = unsafe { core::slice::from_raw_parts(0x200000 as *const u8, len as usize) };
    let params = unsafe { core::slice::from_raw_parts(0x400000 as *const u8, plen as usize) };
    // No Rust output reference spans the assembly call that writes guest RAM.
    for _ in 0..repeats {
        let (err, n) = if native != 0 {
            let mut a0 = 0x300000u32;
            let mut a1 = cap;
            unsafe {
                asm!("ecall",in("a7")0x0a000001u32,in("a6")fid,
                inout("a0")a0,inout("a1")a1,in("a2")0x200000u32,in("a3")len,
                in("a4")0x400000u32,in("a5")plen, options(nostack));
            }
            (a0 as i32, a1)
        } else {
            let dst = unsafe { core::slice::from_raw_parts_mut(0x300000 as *mut u8, cap as usize) };
            match rvaccel::run(core::hint::black_box(fid), src, dst, params) {
                Ok(n) => (0, n as u32),
                Err(e) => (e, 0),
            }
        };
        if err != expected_err || (err == 0 && n != expected_len) {
            stop(false);
        }
    }
    let out = unsafe { core::slice::from_raw_parts(0x300000 as *const u8, expected_len as usize) };
    let expected =
        unsafe { core::slice::from_raw_parts(0x500000 as *const u8, expected_len as usize) };
    stop(out == expected)
}
