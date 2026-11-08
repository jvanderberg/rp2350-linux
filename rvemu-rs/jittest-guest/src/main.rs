//! jittest guest: a bare-metal S-mode program for the rvemu emulator that
//! exercises everything the block translator handles specially. Each test
//! prints a hash and its exact instruction count; a run is only correct if
//! every line (and the final signature) matches across the Mac interpreter,
//! the board interpreter, the board translator and lockstep mode. Tests also
//! check themselves where an absolute answer exists ("SELFCHECK FAIL").
//!
//! Runs at guest PA 0 with 1 MiB of RAM, MMU off until the paging test.
#![no_std]
#![no_main]

use core::arch::{asm, global_asm};
use core::cell::UnsafeCell;
use core::fmt::Write;
use core::hint::black_box as bb;
use core::ptr::{addr_of, read_volatile as rv, write_volatile as wv};

global_asm!(
    r#"
.section .text.start, "ax"
.globl _start
_start:
    la sp, __stack_top
    la t0, __bss_start
    la t1, __bss_end
1:  bgeu t0, t1, 2f
    sw zero, 0(t0)
    addi t0, t0, 4
    j 1b
2:  la t0, trap_entry
    csrw stvec, t0
    call main
3:  j 3b

.text
.align 4
.globl trap_entry
trap_entry:
    addi sp, sp, -128
    sw x1, 4(sp)
    sw x3, 12(sp)
    sw x4, 16(sp)
    sw x5, 20(sp)
    sw x6, 24(sp)
    sw x7, 28(sp)
    sw x8, 32(sp)
    sw x9, 36(sp)
    sw x10, 40(sp)
    sw x11, 44(sp)
    sw x12, 48(sp)
    sw x13, 52(sp)
    sw x14, 56(sp)
    sw x15, 60(sp)
    sw x16, 64(sp)
    sw x17, 68(sp)
    sw x18, 72(sp)
    sw x19, 76(sp)
    sw x20, 80(sp)
    sw x21, 84(sp)
    sw x22, 88(sp)
    sw x23, 92(sp)
    sw x24, 96(sp)
    sw x25, 100(sp)
    sw x26, 104(sp)
    sw x27, 108(sp)
    sw x28, 112(sp)
    sw x29, 116(sp)
    sw x30, 120(sp)
    sw x31, 124(sp)
    addi t0, sp, 128
    sw t0, 8(sp)
    mv a0, sp
    call trap_rust
    lw x1, 4(sp)
    lw x3, 12(sp)
    lw x4, 16(sp)
    lw x5, 20(sp)
    lw x6, 24(sp)
    lw x7, 28(sp)
    lw x8, 32(sp)
    lw x9, 36(sp)
    lw x10, 40(sp)
    lw x11, 44(sp)
    lw x12, 48(sp)
    lw x13, 52(sp)
    lw x14, 56(sp)
    lw x15, 60(sp)
    lw x16, 64(sp)
    lw x17, 68(sp)
    lw x18, 72(sp)
    lw x19, 76(sp)
    lw x20, 80(sp)
    lw x21, 84(sp)
    lw x22, 88(sp)
    lw x23, 92(sp)
    lw x24, 96(sp)
    lw x25, 100(sp)
    lw x26, 104(sp)
    lw x27, 108(sp)
    lw x28, 112(sp)
    lw x29, 116(sp)
    lw x30, 120(sp)
    lw x31, 124(sp)
    addi sp, sp, 128
    sret

# enter_user(pc) -> a0 of the user code's ecall
.globl enter_user
enter_user:
    la t0, USER_CTX
    sw ra, 0(t0)
    sw sp, 4(t0)
    sw s0, 8(t0)
    sw s1, 12(t0)
    sw s2, 16(t0)
    sw s3, 20(t0)
    sw s4, 24(t0)
    sw s5, 28(t0)
    sw s6, 32(t0)
    sw s7, 36(t0)
    sw s8, 40(t0)
    sw s9, 44(t0)
    sw s10, 48(t0)
    sw s11, 52(t0)
    csrw sepc, a0
    li t1, 0x100
    csrc sstatus, t1
    sret
.globl user_return
user_return:
    la t0, USER_CTX
    lw ra, 0(t0)
    lw sp, 4(t0)
    lw s0, 8(t0)
    lw s1, 12(t0)
    lw s2, 16(t0)
    lw s3, 20(t0)
    lw s4, 24(t0)
    lw s5, 28(t0)
    lw s6, 32(t0)
    lw s7, 36(t0)
    lw s8, 40(t0)
    lw s9, 44(t0)
    lw s10, 48(t0)
    lw s11, 52(t0)
    ret

# U-mode code, copied to a user page: sum 16 words at VA 0x40004000, try a
# privileged CSR read (illegal from U), then ecall with the sum in a0.
.globl user_code_start, user_code_end
user_code_start:
    lui t0, 0x40004
    li a0, 0
    li t1, 16
1:  lw t2, 0(t0)
    add a0, a0, t2
    addi t0, t0, 4
    addi t1, t1, -1
    bnez t1, 1b
    csrr t3, sstatus
    ecall
user_code_end:

# Leaf function on its own page, also mapped at a second virtual address.
.section .alias_text, "ax"
.globl alias_fn
alias_fn:
    li t0, 100
1:  slli t1, a0, 5
    add a0, a0, t1
    xori a0, a0, 0x5a5
    addi t0, t0, -1
    bnez t0, 1b
    ret
"#
);

unsafe extern "C" {
    fn enter_user(pc: u32) -> u32;
    fn user_return();
    static user_code_start: u32;
    static user_code_end: u32;
    fn alias_fn(x: u32) -> u32;
    static __alias_page: u8;
}

// ------------------------------------------------------------------ globals

struct G<T>(UnsafeCell<T>);
// SAFETY: one hart; the trap handler and main never hold references at once
// (everything shared with the handler goes through volatile accesses).
unsafe impl<T> Sync for G<T> {}
impl<T> G<T> {
    const fn new(v: T) -> Self {
        G(UnsafeCell::new(v))
    }
    fn p(&self) -> *mut T {
        self.0.get()
    }
}

#[repr(C, align(4096))]
struct Page([u32; 1024]);
const PAGE0: Page = Page([0; 1024]);

#[unsafe(no_mangle)]
static USER_CTX: G<[u32; 14]> = G::new([0; 14]);
static ROOT: G<Page> = G::new(PAGE0);
static L2: G<Page> = G::new(PAGE0);
static PAGES: G<[Page; 6]> = G::new([PAGE0, PAGE0, PAGE0, PAGE0, PAGE0, PAGE0]);
static BUF: G<Page> = G::new(PAGE0);
static SMC: G<Page> = G::new(PAGE0);
static SIEVE: G<[u8; 32768]> = G::new([0; 32768]);
static QS: G<[u32; 2000]> = G::new([0; 2000]);
static DATA: G<[u8; 16384]> = G::new([0; 16384]);
static AMO: G<[u32; 16]> = G::new([0; 16]);

// trap handler state (volatile only)
static TRAPS: G<u32> = G::new(0);
static TRAP_HASH: G<u32> = G::new(0);
static TIMER_COUNT: G<u32> = G::new(0);
static TIMER_HASH: G<u32> = G::new(0);
static PF_COUNT: G<u32> = G::new(0);
static DEMAND_NEXT: G<u32> = G::new(0);
static SELF_FAIL: G<u32> = G::new(0);

fn vget(g: &G<u32>) -> u32 {
    // SAFETY: valid static.
    unsafe { rv(g.p()) }
}
fn vset(g: &G<u32>, v: u32) {
    // SAFETY: valid static.
    unsafe { wv(g.p(), v) }
}

// ------------------------------------------------------------------ SBI / CSR

fn sbi(eid: u32, a0: u32, a1: u32) -> u32 {
    let r: u32;
    // SAFETY: SBI call; the emulator implements it.
    unsafe { asm!("ecall", inlateout("a0") a0 => r, inlateout("a1") a1 => _, in("a7") eid) };
    r
}
fn putc(c: u8) {
    sbi(1, c as u32, 0);
}
fn set_timer(t: u64) {
    sbi(0, t as u32, (t >> 32) as u32);
}
fn shutdown() -> ! {
    sbi(8, 0, 0);
    loop {
        // SAFETY: idle forever if the emulator did not stop.
        unsafe { asm!("wfi") };
    }
}

macro_rules! csrr {
    ($csr:literal) => {{
        let v: u32;
        // SAFETY: CSR read.
        unsafe { asm!(concat!("csrr {0}, ", $csr), out(reg) v) };
        v
    }};
}
macro_rules! csrw {
    ($csr:literal, $v:expr) => {{
        // A local fn keeps the unsafe block un-nested when used inside one.
        #[inline(always)]
        fn op(v: u32) {
            // SAFETY: CSR access chosen by the test.
            unsafe { asm!(concat!("csrw ", $csr, ", {0}"), in(reg) v) }
        }
        op($v)
    }};
}
macro_rules! csrs {
    ($csr:literal, $v:expr) => {{
        // A local fn keeps the unsafe block un-nested when used inside one.
        #[inline(always)]
        fn op(v: u32) {
            // SAFETY: CSR access chosen by the test.
            unsafe { asm!(concat!("csrs ", $csr, ", {0}"), in(reg) v) }
        }
        op($v)
    }};
}
macro_rules! csrc {
    ($csr:literal, $v:expr) => {{
        // A local fn keeps the unsafe block un-nested when used inside one.
        #[inline(always)]
        fn op(v: u32) {
            // SAFETY: CSR access chosen by the test.
            unsafe { asm!(concat!("csrc ", $csr, ", {0}"), in(reg) v) }
        }
        op($v)
    }};
}

fn instret() -> u32 {
    csrr!("instret")
}
fn time64() -> u64 {
    loop {
        let hi = csrr!("timeh");
        let lo = csrr!("time");
        if csrr!("timeh") == hi {
            return ((hi as u64) << 32) | lo as u64;
        }
    }
}
fn sfence_all() {
    // SAFETY: TLB flush.
    unsafe { asm!("sfence.vma") };
}
fn sfence_va(va: u32) {
    // SAFETY: TLB flush of one page.
    unsafe { asm!("sfence.vma {0}, zero", in(reg) va) };
}

struct Out;
impl Write for Out {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            putc(b);
        }
        Ok(())
    }
}
macro_rules! println {
    ($($t:tt)*) => {{ let _ = writeln!(Out, $($t)*); }};
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    println!("PANIC: {info}");
    shutdown();
}

fn mix(h: u32, v: u32) -> u32 {
    let x = (h ^ v).wrapping_mul(0x0100_0193);
    x ^ (x >> 15)
}

fn check(name: &str, ok: bool) {
    if !ok {
        println!("SELFCHECK FAIL {name}");
        vset(&SELF_FAIL, vget(&SELF_FAIL) + 1);
    }
}

// ------------------------------------------------------------------ traps

const SSTATUS_SIE: u32 = 1 << 1;
const SSTATUS_SPP: u32 = 1 << 8;
const SSTATUS_SUM: u32 = 1 << 18;
const SIE_STIE: u32 = 1 << 5;
const TIMER_IRQS: u32 = 20;

const PTE_V: u32 = 1;
const PTE_R: u32 = 2;
const PTE_W: u32 = 4;
const PTE_X: u32 = 8;
const PTE_U: u32 = 16;

fn pte(pa: usize, flags: u32) -> u32 {
    ((pa as u32 >> 12) << 10) | flags
}
fn page_pa(i: usize) -> usize {
    // SAFETY: index < 6 by construction.
    unsafe { addr_of!((*PAGES.p())[i]) as usize }
}
fn l2(i: usize) -> *mut u32 {
    // SAFETY: index < 1024.
    unsafe { (L2.p() as *mut u32).add(i) }
}

#[unsafe(no_mangle)]
extern "C" fn trap_rust(f: &mut [u32; 32]) {
    let cause = csrr!("scause");
    let epc = csrr!("sepc");
    let tval = csrr!("stval");
    vset(&TRAPS, vget(&TRAPS) + 1);
    if cause & 0x8000_0000 != 0 {
        if cause & 0xff == 5 {
            let n = vget(&TIMER_COUNT) + 1;
            vset(&TIMER_COUNT, n);
            vset(&TIMER_HASH, mix(vget(&TIMER_HASH), instret()));
            if n < TIMER_IRQS {
                set_timer(time64() + 3000 + 37 * n as u64);
            } else {
                set_timer(u64::MAX);
            }
            return;
        }
        println!("unexpected interrupt {cause:#x}");
        shutdown();
    }
    vset(&TRAP_HASH, mix(mix(vget(&TRAP_HASH), cause), tval));
    match cause {
        // instruction access fault: return to the caller
        1 => csrw!("sepc", f[1]),
        // illegal, breakpoint, misaligned / access-faulting loads and stores: skip
        2..=7 => csrw!("sepc", epc + 4),
        // ecall from U: back to enter_user's caller in S-mode
        8 => {
            csrs!("sstatus", SSTATUS_SPP);
            csrw!("sepc", user_return as *const () as usize as u32);
        }
        12 | 13 | 15 => {
            vset(&PF_COUNT, vget(&PF_COUNT) + 1);
            let page = tval & !0xfff;
            match page {
                // demand-mapped pages: map and retry
                0x4000_3000 | 0x4000_5000 => {
                    let (slot, target) = if page == 0x4000_5000 {
                        (5, 5)
                    } else if vget(&DEMAND_NEXT) == 0 {
                        (3, 3)
                    } else {
                        (3, 4)
                    };
                    vset(&DEMAND_NEXT, 1);
                    // SAFETY: L2 slot inside the table.
                    unsafe { wv(l2(slot), pte(page_pa(target), PTE_V | PTE_R | PTE_W)) };
                    sfence_va(page);
                }
                // S-mode touching the U data page with SUM clear: skip
                0x4000_4000 => csrw!("sepc", epc + 4),
                _ => {
                    println!("unexpected page fault cause={cause} epc={epc:#x} tval={tval:#x}");
                    shutdown();
                }
            }
        }
        _ => {
            println!("unexpected trap cause={cause} epc={epc:#x} tval={tval:#x}");
            shutdown();
        }
    }
}

// ------------------------------------------------------------------ tests

fn t_alu() -> u32 {
    let (mut x, mut y, mut h) = (bb(0x1234_5678u32), bb(0x9abc_def0u32), 0u32);
    for i in 0..bb(20_000u32) {
        x = x.wrapping_add(y ^ i);
        y = y.rotate_left(i & 31) ^ (x >> 3);
        x ^= ((x as i32) >> (i % 13)) as u32;
        y = y.wrapping_sub(x << (i & 7));
        if (x as i32) < (y as i32) {
            x |= 1;
        } else {
            y &= !0x100;
        }
        if x < y {
            h = h.wrapping_add(0x7ff);
        }
        h = mix(h, x.wrapping_add(y));
    }
    h
}

macro_rules! op2 {
    ($ins:literal, $a:expr, $b:expr) => {{
        let r: u32;
        // SAFETY: pure register arithmetic.
        unsafe { asm!(concat!($ins, " {0}, {1}, {2}"), out(reg) r, in(reg) $a, in(reg) $b) };
        r
    }};
}
macro_rules! opi {
    ($ins:literal, $a:expr, $imm:literal) => {{
        let r: u32;
        // SAFETY: pure register arithmetic.
        unsafe { asm!(concat!($ins, " {0}, {1}, ", $imm), out(reg) r, in(reg) $a) };
        r
    }};
}

const EDGE: [u32; 12] = [
    0, 1, 2, 3, 7, 0xffff_ffff, 0xffff_fffe, 0x7fff_ffff, 0x8000_0000, 0x1234_5678, 0xdead_beef, 0xffff,
];

fn t_ops() -> u32 {
    let mut h = 0;
    for a in bb(EDGE) {
        for b in bb(EDGE) {
            for v in [
                op2!("add", a, b), op2!("sub", a, b), op2!("sll", a, b), op2!("srl", a, b),
                op2!("sra", a, b), op2!("slt", a, b), op2!("sltu", a, b), op2!("xor", a, b),
                op2!("or", a, b), op2!("and", a, b),
                op2!("mul", a, b), op2!("mulh", a, b), op2!("mulhsu", a, b), op2!("mulhu", a, b),
                op2!("div", a, b), op2!("divu", a, b), op2!("rem", a, b), op2!("remu", a, b),
            ] {
                h = mix(h, v);
            }
        }
        for v in [
            opi!("slti", a, "-1"), opi!("sltiu", a, "-1"), opi!("sltiu", a, "1"), opi!("srai", a, "31"),
            opi!("srli", a, "31"), opi!("slli", a, "31"), opi!("xori", a, "-1"), opi!("ori", a, "0x7ff"),
            opi!("andi", a, "-2048"), opi!("addi", a, "-2048"),
        ] {
            h = mix(h, v);
        }
    }
    let (lui, pc): (u32, u32);
    // SAFETY: register-only.
    unsafe { asm!("lui {0}, 0xfffff", "auipc {1}, 0", out(reg) lui, out(reg) pc) };
    mix(mix(h, lui), pc)
}

fn t_mem() -> u32 {
    let p = BUF.p() as *mut u8;
    let mut x = bb(1u32);
    let mut h = 0;
    // SAFETY: all offsets are inside the 4 KiB buffer.
    unsafe {
        for i in 0..4096 {
            x = x.wrapping_mul(1_103_515_245).wrapping_add(12345);
            wv(p.add(i), (x >> 16) as u8);
        }
        for i in (0..1024).step_by(2) {
            wv(p.add(i) as *mut u16, (i as u16).wrapping_mul(0x9e37));
        }
        for i in (1024..2048).step_by(4) {
            wv(p.add(i) as *mut u32, (i as u32).wrapping_mul(0x9e37_79b9));
        }
        for i in 0..4096 {
            h = mix(h, rv(p.add(i) as *const i8) as u32);
            h = mix(h, rv(p.add(i)) as u32);
            if i % 2 == 0 {
                h = mix(h, rv(p.add(i) as *const i16) as u32);
                h = mix(h, rv(p.add(i) as *const u16) as u32);
            }
            if i % 4 == 0 {
                h = mix(h, rv(p.add(i) as *const u32));
            }
        }
    }
    h
}

fn t_misaligned() -> u32 {
    let p = BUF.p() as usize;
    let (a, b, c): (u32, u32, u32);
    // SAFETY: each access either works or traps and is skipped by the handler.
    unsafe {
        asm!("li {0}, 0x5a5a5a5a", "lw {0}, 1({1})", out(reg) a, in(reg) p);
        asm!("li {0}, 0x1111", "lh {0}, 3({1})", out(reg) b, in(reg) p);
        asm!("li {0}, 0x77", "sw {0}, 2({1})", "sh {0}, 5({1})", "lw {0}, 0({1})", out(reg) c, in(reg) p);
    }
    mix(mix(mix(vget(&TRAP_HASH), a), b), c)
}

fn t_sieve() -> u32 {
    let s = SIEVE.p() as *mut u8;
    let n = 32768;
    let mut count = 0;
    let mut h = 0;
    // SAFETY: indices < n = array length.
    unsafe {
        for i in 0..n {
            wv(s.add(i), 1);
        }
        let mut i = 2;
        while i * i < n {
            if rv(s.add(i)) != 0 {
                let mut j = i * i;
                while j < n {
                    wv(s.add(j), 0);
                    j += i;
                }
            }
            i += 1;
        }
        for i in 2..n {
            if rv(s.add(i)) != 0 {
                count += 1;
                h = mix(h, i as u32);
            }
        }
    }
    check("sieve count", count == 3512);
    mix(h, count)
}

fn qsort(a: &mut [u32]) {
    if a.len() < 2 {
        return;
    }
    let p = a[(a.len() - 1) / 2];
    let (mut i, mut j) = (0usize, a.len() - 1);
    loop {
        while a[i] < p {
            i += 1;
        }
        while a[j] > p {
            j -= 1;
        }
        if i >= j {
            break;
        }
        a.swap(i, j);
        i += 1;
        j -= 1;
    }
    let (l, r) = a.split_at_mut(j + 1);
    qsort(l);
    qsort(r);
}

fn t_qsort() -> u32 {
    // SAFETY: only reference to QS while the test runs.
    let a = unsafe { &mut *QS.p() };
    let mut x = bb(42u32);
    for v in a.iter_mut() {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *v = x % 100_000;
    }
    qsort(a);
    check("qsort sorted", a.windows(2).all(|w| w[0] <= w[1]));
    a.iter().fold(0, |h, &v| mix(h, v))
}

#[inline(never)]
fn f0(x: u32) -> u32 { x.wrapping_mul(3).wrapping_add(1) }
#[inline(never)]
fn f1(x: u32) -> u32 { x ^ (x >> 5) }
#[inline(never)]
fn f2(x: u32) -> u32 { x.rotate_left(7) }
#[inline(never)]
fn f3(x: u32) -> u32 { x.wrapping_sub(0x1234) }
#[inline(never)]
fn f4(x: u32) -> u32 { if x & 1 != 0 { x >> 1 } else { x.wrapping_mul(5) } }
#[inline(never)]
fn f5(x: u32) -> u32 { !x }
#[inline(never)]
fn f6(x: u32) -> u32 { x.wrapping_add(x << 9) }
#[inline(never)]
fn f7(x: u32) -> u32 { (x as i32 >> 3) as u32 ^ 0x5555 }
static TABLE: [fn(u32) -> u32; 8] = [f0, f1, f2, f3, f4, f5, f6, f7];

fn t_indirect() -> u32 {
    let t = bb(&TABLE);
    let (mut x, mut h) = (bb(7u32), 0);
    for i in 0..bb(5000u32) {
        x = t[(x >> 7) as usize & 7](x ^ i);
        h = mix(h, x);
    }
    h
}

fn t_crc() -> u32 {
    let mut table = [0u32; 256];
    for (i, t) in table.iter_mut().enumerate() {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xedb8_8320 ^ (c >> 1) } else { c >> 1 };
        }
        *t = c;
    }
    // SAFETY: only reference to DATA while the test runs.
    let d = unsafe { &mut *DATA.p() };
    let mut x = bb(0xace1u32);
    for b in d.iter_mut() {
        x = x.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        *b = (x >> 24) as u8;
    }
    let mut crc = !0u32;
    for &b in d.iter() {
        crc = table[((crc ^ b as u32) & 0xff) as usize] ^ (crc >> 8);
    }
    let check_crc = {
        // CRC32 of "123456789" is cbf43926
        let mut c = !0u32;
        for &b in b"123456789" {
            c = table[((c ^ b as u32) & 0xff) as usize] ^ (c >> 8);
        }
        !c
    };
    check("crc32 check value", check_crc == 0xcbf4_3926);
    !crc
}

macro_rules! amo {
    ($ins:literal, $p:expr, $v:expr) => {{
        // Only used inside t_amo's unsafe block; $p points into the AMO array.
        let r: u32;
        asm!(concat!($ins, " {0}, {1}, ({2})"), out(reg) r, in(reg) $v, in(reg) $p);
        r
    }};
}

fn t_amo() -> u32 {
    let p = AMO.p() as *mut u32;
    let mut h = 0;
    // SAFETY: indices < 16.
    unsafe {
        for i in 0..16 {
            wv(p.add(i), EDGE[i % 12].wrapping_add(i as u32));
        }
        for v in bb(EDGE) {
            for r in [
                amo!("amoadd.w", p.add(0), v), amo!("amoswap.w", p.add(1), v), amo!("amoxor.w", p.add(2), v),
                amo!("amoor.w", p.add(3), v), amo!("amoand.w", p.add(4), v), amo!("amomin.w", p.add(5), v),
                amo!("amomax.w", p.add(6), v), amo!("amominu.w", p.add(7), v), amo!("amomaxu.w", p.add(8), v),
                amo!("amoadd.w.aqrl", p.add(9), v),
            ] {
                h = mix(h, r);
            }
            // rd == rs2, and rd = x0
            let mut w = v;
            asm!("amoswap.w {0}, {0}, ({1})", inout(reg) w, in(reg) p.add(10));
            asm!("amoadd.w zero, {0}, ({1})", in(reg) v, in(reg) p.add(11));
            h = mix(h, w);
        }
        // LR/SC: 100 increments, an unpaired SC, and an SC to another address
        for _ in 0..100 {
            asm!("1: lr.w {t}, ({p})", "addi {t}, {t}, 1", "sc.w {f}, {t}, ({p})", "bnez {f}, 1b",
                t = out(reg) _, f = out(reg) _, p = in(reg) p.add(12));
        }
        let (f1, f2): (u32, u32);
        asm!("sc.w {0}, {1}, ({2})", out(reg) f1, in(reg) 5, in(reg) p.add(13));
        asm!("lr.w {t}, ({a})", "sc.w {0}, {t}, ({b})", out(reg) f2, t = out(reg) _, a = in(reg) p.add(13), b = in(reg) p.add(14));
        check("sc without reservation fails", f1 != 0);
        check("sc to another address fails", f2 != 0);
        check("lr/sc increments", rv(p.add(12)) == EDGE[0].wrapping_add(12) + 100);
        for i in 0..16 {
            h = mix(h, rv(p.add(i)));
        }
        mix(mix(h, f1), f2)
    }
}

fn t_csr() -> u32 {
    csrw!("sscratch", bb(0xdead_beefu32));
    let s = csrr!("sscratch");
    check("sscratch", s == 0xdead_beef);
    let (i0, i1, t0, t1, c0, c1): (u32, u32, u32, u32, u32, u32);
    // SAFETY: register-only loop. Between the two instret reads: the first
    // read, li, and 1000 x (addi, bnez) = 2002 instructions; time and cycle
    // are each read one instruction later.
    unsafe {
        asm!("csrr {i0}, instret", "csrr {t0}, time", "csrr {c0}, cycle",
            "li {n}, 1000", "1: addi {n}, {n}, -1", "bnez {n}, 1b",
            "csrr {i1}, instret", "csrr {t1}, time", "csrr {c1}, cycle",
            i0 = out(reg) i0, t0 = out(reg) t0, c0 = out(reg) c0,
            i1 = out(reg) i1, t1 = out(reg) t1, c1 = out(reg) c1, n = out(reg) _);
    }
    check("instret delta", i1.wrapping_sub(i0) == 2004);
    check("time delta", t1.wrapping_sub(t0) == 2004);
    check("cycle delta", c1.wrapping_sub(c0) == 2004);
    let mut h = mix(mix(mix(s, i1 - i0), t1 - t0), c1 - c0);
    h = mix(h, csrr!("instreth"));
    h
}

fn addi(rd: u32, rs1: u32, imm: i32) -> u32 {
    ((imm as u32 & 0xfff) << 20) | (rs1 << 15) | (rd << 7) | 0x13
}
const RET: u32 = 0x0000_8067;

fn t_smc() -> u32 {
    let code = SMC.p() as *mut u32;
    let mut h = 0;
    for k in 0..40i32 {
        // SAFETY: the SMC page is ours; fence.i orders the stores before fetch.
        let f: extern "C" fn(u32) -> u32 = unsafe {
            wv(code, addi(10, 10, k));
            wv(code.add(1), addi(10, 10, 3 * k));
            wv(code.add(2), RET);
            asm!("fence.i");
            core::mem::transmute(code)
        };
        let a = f(h);
        let b = f(1);
        check("smc result", a == h.wrapping_add(4 * k as u32) && b == 1 + 4 * k as u32);
        h = mix(mix(h, a), b);
    }
    h
}

fn t_traps() -> u32 {
    let before = vget(&TRAPS);
    let (a, b): (u32, u32);
    // SAFETY: each instruction traps and the handler skips it or returns.
    unsafe {
        asm!(".word 0x00000000"); // illegal
        asm!("ebreak");
        asm!(".word 0xc0001073"); // csrw cycle, zero: read-only CSR
        asm!("li {0}, 0x77", "lw {0}, 0({1})", out(reg) a, in(reg) 0x3000_0000u32);
        asm!("sw {0}, 0({0})", in(reg) 0x3000_0004u32);
        let bad: extern "C" fn(u32) -> u32 = core::mem::transmute(0x3000_0000usize);
        b = bad(0x1234); // instruction access fault, handler returns to ra
    }
    let n = vget(&TRAPS) - before;
    check("trap count", n == 6);
    check("faulting load skipped", a == 0x77);
    mix(mix(mix(vget(&TRAP_HASH), n), a), b)
}

fn t_uart() -> u32 {
    let uart = 0xF400_0000usize as *mut u8;
    // SAFETY: the emulator's 16550 THR / LSR.
    unsafe {
        for &c in b"uart: mmio ok\n" {
            while rv(uart.add(5)) & 0x20 == 0 {}
            wv(uart, c);
        }
        rv(uart.add(5)) as u32
    }
}

fn t_mmu() -> u32 {
    let root = ROOT.p() as *mut u32;
    let alias_page = addr_of!(__alias_page) as usize;
    let mut h = 0;
    // SAFETY: page tables and pages are ours; mappings are torn down at the end.
    unsafe {
        wv(root, pte(0, PTE_V | PTE_R | PTE_W | PTE_X)); // 4 MiB identity, S only
        wv(root.add(0x3C0), pte(0xF000_0000, PTE_V | PTE_R | PTE_W)); // PLIC
        wv(root.add(0x3D0), pte(0xF400_0000, PTE_V | PTE_R | PTE_W)); // UART
        wv(root.add(0x100), pte(L2.p() as usize, PTE_V)); // 0x40000000 -> L2
        wv(l2(0), pte(page_pa(0), PTE_V | PTE_R | PTE_W));
        wv(l2(1), pte(page_pa(1), PTE_V | PTE_R | PTE_X | PTE_U));
        wv(l2(2), pte(alias_page, PTE_V | PTE_R | PTE_X));
        wv(l2(4), pte(page_pa(2), PTE_V | PTE_R | PTE_W | PTE_U));
        for i in 0..1024 {
            wv((page_pa(3) as *mut u32).add(i), 0x3333_3333);
            wv((page_pa(4) as *mut u32).add(i), 0x4444_4444);
        }
        for i in 0..16 {
            wv((page_pa(2) as *mut u32).add(i), i as u32 + 1);
        }
        let uc = addr_of!(user_code_start);
        let n = (addr_of!(user_code_end) as usize - uc as usize) / 4;
        for i in 0..n {
            wv((page_pa(1) as *mut u32).add(i), rv(uc.add(i)));
        }
        asm!("fence.i");
        csrw!("satp", 0x8000_0000 | (root as u32 >> 12));
        sfence_all();

        // alias write, identity read
        wv(0x4000_0010 as *mut u32, 0xabcd_0123);
        let v = rv((page_pa(0) + 0x10) as *const u32);
        check("alias store visible through identity map", v == 0xabcd_0123);
        h = mix(h, v);

        // demand fault, then remap the same va to another page
        let v1 = rv(0x4000_3000 as *const u32);
        wv(l2(3), 0);
        sfence_va(0x4000_3000);
        let v2 = rv(0x4000_3000 as *const u32);
        check("demand map", v1 == 0x3333_3333);
        check("remap after sfence.vma", v2 == 0x4444_4444);
        wv(0x4000_5008 as *mut u32, 0x5555_aaaa); // store page fault
        let v3 = rv((page_pa(5) + 8) as *const u32);
        check("store after demand map", v3 == 0x5555_aaaa);
        h = mix(mix(mix(h, v1), v2), v3);
        h = mix(h, vget(&PF_COUNT));
        // A/D bits the emulator set
        for i in [0, 1, 2, 3, 4, 5] {
            h = mix(h, rv(l2(i)) & 0xff);
        }

        // SUM: S-mode load from a U page faults unless SUM is set
        let (s0, s1): (u32, u32);
        asm!("li {0}, 0x99", "lw {0}, 0({1})", out(reg) s0, in(reg) 0x4000_4000u32);
        csrs!("sstatus", SSTATUS_SUM);
        asm!("lw {0}, 0({1})", out(reg) s1, in(reg) 0x4000_4000u32);
        csrc!("sstatus", SSTATUS_SUM);
        check("SUM=0 blocks S access to U page", s0 == 0x99);
        check("SUM=1 allows it", s1 == 1);
        h = mix(mix(h, s0), s1);

        // U-mode code
        let traps0 = vget(&TRAPS);
        let sum = enter_user(0x4000_1000);
        check("user-mode sum", sum == 136);
        check("user traps (illegal csr + ecall)", vget(&TRAPS) - traps0 == 2);
        h = mix(h, sum);

        // the same physical code through two virtual addresses
        let off = alias_fn as *const () as usize - alias_page;
        let via_alias: extern "C" fn(u32) -> u32 = core::mem::transmute(0x4000_2000 + off);
        let r1 = alias_fn(12345);
        let r2 = via_alias(12345);
        check("aliased code", r1 == r2);
        h = mix(mix(h, r1), r2);

        csrw!("satp", 0u32);
        sfence_all();
    }
    mix(h, vget(&TRAP_HASH))
}

fn t_timer() -> u32 {
    vset(&TIMER_COUNT, 0);
    set_timer(time64() + 3000);
    csrs!("sie", SIE_STIE);
    csrs!("sstatus", SSTATUS_SIE);
    let mut x = bb(1u32);
    while vget(&TIMER_COUNT) < TIMER_IRQS {
        x = x.wrapping_mul(1_103_515_245).wrapping_add(12345);
        x ^= x >> 7;
    }
    csrc!("sstatus", SSTATUS_SIE);
    csrc!("sie", SIE_STIE);
    mix(vget(&TIMER_HASH), x)
}

fn t_wfi() -> u32 {
    let t0 = time64();
    set_timer(t0 + 100_000);
    csrs!("sie", SIE_STIE); // pending wakes wfi even with SIE clear
    // SAFETY: waits for the timer.
    unsafe { asm!("wfi") };
    let t1 = time64();
    set_timer(u64::MAX);
    csrc!("sie", SIE_STIE);
    check("wfi slept to the deadline", t1 - t0 >= 100_000);
    (t1 - t0) as u32
}

// ------------------------------------------------------------------ main

#[unsafe(no_mangle)]
extern "C" fn main() -> ! {
    println!("jittest: start");
    type Test = (&'static str, fn() -> u32);
    let tests: [Test; 16] = [
        ("alu", t_alu),
        ("ops", t_ops),
        ("mem", t_mem),
        ("misalign", t_misaligned),
        ("sieve", t_sieve),
        ("qsort", t_qsort),
        ("indirect", t_indirect),
        ("crc", t_crc),
        ("amo", t_amo),
        ("csr", t_csr),
        ("smc", t_smc),
        ("traps", t_traps),
        ("uart", t_uart),
        ("mmu", t_mmu),
        ("timer", t_timer),
        ("wfi", t_wfi),
    ];
    let mut sig = 0;
    for (name, t) in tests {
        let i0 = instret();
        let h = t();
        let n = instret() - i0;
        println!("{name:9} {h:08x}  {n:8} insns");
        sig = mix(mix(sig, h), n);
    }
    // a heavier mix of the compute tests, for timing
    let i0 = instret();
    let mut b = 0;
    for _ in 0..4 {
        b = mix(b, t_alu());
        b = mix(b, t_sieve());
        b = mix(b, t_qsort());
        b = mix(b, t_crc());
    }
    let n = instret() - i0;
    println!("{:9} {b:08x}  {n:8} insns", "bench");
    sig = mix(mix(sig, b), n);
    let fails = vget(&SELF_FAIL);
    println!(
        "jittest: signature {sig:08x}, {} instructions, {}",
        instret(),
        if fails == 0 { "selfchecks ok" } else { "SELFCHECK FAILURES" }
    );
    shutdown();
}
