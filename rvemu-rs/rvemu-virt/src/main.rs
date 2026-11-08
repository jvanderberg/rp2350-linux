//! Bare-metal rvemu for QEMU `virt` (riscv32, M-mode, `-bios none`): the
//! development stand-in for the RP2350. The run script loads the guest
//! Image / initramfs / DTB straight into guest RAM and a boot-info block.
//!
//! Modes: 0 = interpreter, 1 = translator, 2 = lockstep (translator checked
//! against an interpreter on a separate copy of RAM after every step).
#![no_std]
#![no_main]

use core::fmt::Write;
use core::mem::MaybeUninit;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};

use rvcore::dev::{Console, Platform};
use rvcore::virtio::{GuestMem, MemDisk};
use rvcore::{Bus, Cpu, Halt};
use rvjit::{Event, JcEntry, Jit, JitMem};

core::arch::global_asm!(
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
    csrw mtvec, t0
    call main
3:  j 3b

.align 4
trap_entry:
    csrw mscratch, sp
    la sp, __trap_stack_top
    call trap_handler
4:  j 4b
"#
);

// ------------------------------------------------------------------ QEMU I/O

const UART: usize = 0x1000_0000;
const TEST_DEV: usize = 0x0010_0000;
const INFO_ADDR: usize = 0x80F0_0000;
const GUEST_RAM: usize = 0x8100_0000;
const REF_RAM: usize = 0x8500_0000;
/// Disk image for the virtio block device (virt.sh -d), `Info::disk_len` bytes.
const DISK: usize = 0x8A00_0000;

fn uart_putc(b: u8) {
    unsafe {
        while read_volatile((UART + 5) as *const u8) & 0x20 == 0 {}
        write_volatile(UART as *mut u8, b);
    }
}

fn uart_getc() -> Option<u8> {
    unsafe {
        if read_volatile((UART + 5) as *const u8) & 1 != 0 {
            Some(read_volatile(UART as *const u8))
        } else {
            None
        }
    }
}

fn qemu_exit(code: u32) -> ! {
    unsafe { write_volatile(TEST_DEV as *mut u32, if code == 0 { 0x5555 } else { (code << 16) | 0x3333 }) };
    loop {
        unsafe { core::arch::asm!("wfi") };
    }
}

struct Out;
impl Write for Out {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                uart_putc(b'\r');
            }
            uart_putc(b);
        }
        Ok(())
    }
}

macro_rules! println {
    ($($t:tt)*) => {{ let _ = writeln!(Out, $($t)*); }};
}

#[panic_handler]
fn panic(info: &core::panic::PanicInfo) -> ! {
    println!("\n[rvemu-virt] panic: {info}");
    qemu_exit(3);
}

#[unsafe(no_mangle)]
extern "C" fn trap_handler() -> ! {
    let (cause, epc, tval): (u32, u32, u32);
    unsafe {
        core::arch::asm!("csrr {0}, mcause", "csrr {1}, mepc", "csrr {2}, mtval",
            out(reg) cause, out(reg) epc, out(reg) tval);
    }
    println!("\n[rvemu-virt] host trap: mcause={cause:#x} mepc={epc:#010x} mtval={tval:#010x}");
    qemu_exit(4);
}

fn host_instret() -> u64 {
    let (lo, hi, hi2): (u32, u32, u32);
    unsafe {
        core::arch::asm!("csrr {1}, minstreth", "csrr {0}, minstret", "csrr {2}, minstreth",
            out(reg) lo, out(reg) hi, out(reg) hi2);
    }
    if hi != hi2 { (hi2 as u64) << 32 } else { ((hi as u64) << 32) | lo as u64 }
}

// ------------------------------------------------------------------ boot info

#[repr(C)]
struct Info {
    magic: u32,
    mode: u32,
    ram_size: u32,
    max_minsns: u32,
    batch: u32,
    until: [u8; 60],
    disk_len: u32,
}
const MAGIC: u32 = 0x4D45_5652; // "RVEM"

// ------------------------------------------------------------------ guest bus

struct VirtConsole {
    batch: bool,
    quiet: bool,
    rx: Option<u8>,
    until: &'static [u8],
    tail: [u8; 64],
    tail_len: usize,
    until_hit: bool,
}

impl VirtConsole {
    fn new(batch: bool, quiet: bool, until: &'static [u8]) -> Self {
        VirtConsole { batch, quiet, rx: None, until, tail: [0; 64], tail_len: 0, until_hit: false }
    }
    fn poll(&mut self) {
        if !self.batch && self.rx.is_none() {
            self.rx = uart_getc();
        }
    }
}

impl Console for VirtConsole {
    fn putc(&mut self, b: u8) {
        if !self.quiet {
            if b == b'\n' {
                uart_putc(b'\r');
            }
            uart_putc(b);
        }
        if !self.until.is_empty() && !self.until_hit {
            if self.tail_len == self.tail.len() {
                self.tail.copy_within(1.., 0);
                self.tail_len -= 1;
            }
            self.tail[self.tail_len] = b;
            self.tail_len += 1;
            if self.tail[..self.tail_len].ends_with(self.until) {
                self.until_hit = true;
            }
        }
    }
    fn rx_ready(&self) -> bool {
        self.rx.is_some()
    }
    fn getc(&mut self) -> Option<u8> {
        self.rx.take()
    }
}

struct VirtBus {
    plat: Platform<VirtConsole, MemDisk>,
}

impl VirtBus {
    /// With the disk at `DISK` when `disk_len` is non-zero, DMA into `ram`.
    fn new(con: VirtConsole, disk_len: u32, ram: usize, ram_size: u32) -> Self {
        let plat = if disk_len == 0 {
            Platform::new(con)
        } else {
            // SAFETY: virt.sh loaded `disk_len` bytes at DISK; nothing else uses them.
            let disk = unsafe { MemDisk::new(DISK as *const u8, disk_len as usize) };
            Platform::new(con).with_disk(disk, GuestMem { ptr: ram as *mut u8, base: 0, size: ram_size, ro: 0 })
        };
        VirtBus { plat }
    }
}

impl VirtBus {
    fn poll(&mut self) {
        self.plat.con.poll();
        self.plat.update_irqs();
    }
}

impl Bus for VirtBus {
    fn mmio_load(&mut self, pa: u32, size: u32) -> Result<u32, u32> {
        self.plat.mmio_load(pa, size)
    }
    fn mmio_store(&mut self, pa: u32, size: u32, val: u32) -> Result<(), u32> {
        self.plat.mmio_store(pa, size, val)
    }
    fn external_irq(&self) -> bool {
        self.plat.plic.irq
    }
    fn console_putc(&mut self, b: u8) {
        self.plat.con.putc(b)
    }
    fn console_getc(&mut self) -> Option<u8> {
        self.plat.con.getc()
    }
    fn take_dma_write(&mut self) -> Option<(u32, u32)> {
        self.plat.take_dma_write()
    }
    /// Same policy as the Mac host in batch mode: skip to the timer deadline.
    fn wfi(&mut self, cpu: &mut Cpu) {
        self.poll();
        if cpu.sip_value(self) & cpu.sie != 0 {
            return;
        }
        let now = cpu.time();
        if cpu.stimecmp <= now {
            return;
        }
        if cpu.stimecmp == u64::MAX {
            if self.plat.con.batch {
                println!("\n[rvemu-virt] guest idle with no timer and no input");
                cpu.halt = Some(Halt::Idle);
            }
            return;
        }
        let skip = cpu.stimecmp - now;
        cpu.time_skip += skip;
        cpu.stats.idle_ticks += skip;
    }
}

// ------------------------------------------------------------------ storage

/// Translated-code cache size in KiB: 1024 by default, or RVEMU_CODE_KB at
/// build time (e.g. 192 to match rvemu-pico's SRAM cache).
const CODE_KB: usize = match option_env!("RVEMU_CODE_KB") {
    Some(s) => parse_usize(s),
    None => 1024,
};
const CODE_WORDS: usize = CODE_KB * 256;
const NBUCKETS: usize = 16 * 1024;

const fn parse_usize(s: &str) -> usize {
    let b = s.as_bytes();
    let mut i = 0;
    let mut v = 0;
    while i < b.len() {
        assert!(b[i].is_ascii_digit(), "RVEMU_CODE_KB must be a number");
        v = v * 10 + (b[i] - b'0') as usize;
        i += 1;
    }
    v
}

static mut CODE: [u32; CODE_WORDS] = [0; CODE_WORDS];
static mut BUCKETS: [u32; NBUCKETS] = [0; NBUCKETS];
static mut CODE_PAGES: [u8; 2048] = [0; 2048];
/// Jump-cache entries: 4096, or RVEMU_NJC at build time (rvemu-pico uses 1024).
const NJC: usize = match option_env!("RVEMU_NJC") {
    Some(s) => parse_usize(s),
    None => 4096,
};
static mut JUMP_CACHE: [JcEntry; NJC] = [JcEntry::EMPTY; NJC];
/// Code-buffer regions: 1 (flush when full) by default, or RVEMU_REGIONS.
const REGIONS: usize = match option_env!("RVEMU_REGIONS") {
    Some(s) => parse_usize(s),
    None => 1,
};
static mut CHAIN_LOG: [u32; 16 * 256] = [0; 16 * 256];
/// Backing store for translated blocks: RVEMU_STORE_KB of QEMU RAM above the
/// lockstep copy (0 = none, the default).
const STORE_KB: usize = match option_env!("RVEMU_STORE_KB") {
    Some(s) => parse_usize(s),
    None => 0,
};
const STORE_ADDR: usize = 0x8900_0000;
static mut STORE_EPOCHS: [u8; 16384] = [0; 16384];
static mut CPU: MaybeUninit<Cpu> = MaybeUninit::uninit();
static mut REF_CPU: MaybeUninit<Cpu> = MaybeUninit::uninit();

fn until_check(cpu: &mut Cpu, bus: &VirtBus) {
    if bus.plat.con.until_hit && cpu.stats.until_instret == 0 {
        cpu.stats.until_instret = cpu.instret;
        cpu.halt = Some(Halt::Until);
    }
}

fn dump(tag: &str, c: &Cpu) {
    println!("{tag}: pc={:#010x} priv={:?} instret={} satp={:#x} sepc={:#x} scause={:#x} stval={:#x}",
        c.pc, c.privilege, c.instret, c.satp, c.sepc, c.scause, c.stval);
    for r in 0..32 {
        let _ = write!(Out, "x{r}={:08x}{}", c.x[r], if r % 8 == 7 { "\n" } else { " " });
    }
}

fn same_state(a: &Cpu, b: &Cpu) -> bool {
    a.x == b.x
        && a.pc == b.pc
        && a.privilege == b.privilege
        && a.instret == b.instret
        && a.satp == b.satp
        && a.sepc == b.sepc
        && a.scause == b.scause
        && a.stval == b.stval
        && a.sie_bit == b.sie_bit
        && a.sum == b.sum
}

#[unsafe(no_mangle)]
extern "C" fn main() -> ! {
    let info = unsafe { &*(INFO_ADDR as *const Info) };
    if info.magic != MAGIC {
        println!("[rvemu-virt] no boot info at {INFO_ADDR:#x}");
        qemu_exit(2);
    }
    let ulen = info.until.iter().position(|&b| b == 0).unwrap_or(info.until.len());
    let until: &'static [u8] = unsafe { core::slice::from_raw_parts(info.until.as_ptr(), ulen) };
    let ram_size = info.ram_size;
    let dtb = ram_size - 0x10000;
    let mode = info.mode;
    let max_insns = info.max_minsns as u64 * 1_000_000;
    println!("[rvemu-virt] mode={} ram={} MiB until={:?} code cache {} KiB in {} region(s), disk {} KiB", ["interp", "jit", "lockstep"][mode as usize % 3],
        ram_size >> 20, core::str::from_utf8(until).unwrap_or("?"), CODE_KB, REGIONS, info.disk_len >> 10);

    let cpu: &mut Cpu = unsafe {
        let p = (*addr_of_mut!(CPU)).as_mut_ptr();
        Cpu::init(p, GUEST_RAM as *mut u8, 0, ram_size, 0, dtb)
    };
    let mut bus = VirtBus::new(VirtConsole::new(info.batch != 0, false, until), info.disk_len, GUEST_RAM, ram_size);

    // calibrate the host counter: this loop retires 2,000,001 instructions
    let c0 = host_instret();
    unsafe {
        core::arch::asm!("li {0}, 1000000", "1: addi {0}, {0}, -1", "bnez {0}, 1b", out(reg) _);
    }
    let c1 = host_instret();
    println!("[rvemu-virt] host counter calibration: {} for 2000001 instructions", c1 - c0);

    let t0 = host_instret();
    let mut events: u64 = 0;
    match mode {
        0 => {
            while cpu.halt.is_none() {
                for _ in 0..4096 {
                    cpu.check_irq(&bus);
                    cpu.step(&mut bus);
                    until_check(cpu, &bus);
                    if cpu.halt.is_some() {
                        break;
                    }
                }
                bus.poll();
                if max_insns != 0 && cpu.instret >= max_insns && cpu.halt.is_none() {
                    cpu.halt = Some(Halt::MaxInsns);
                }
            }
        }
        _ => {
            let lockstep = mode == 2;
            let mut jit = unsafe {
                Jit::new(
                    cpu,
                    JitMem {
                        code: &mut *addr_of_mut!(CODE),
                        buckets: &mut *addr_of_mut!(BUCKETS),
                        code_pages: &mut *addr_of_mut!(CODE_PAGES),
                        jump_cache: &mut *addr_of_mut!(JUMP_CACHE),
                        regions: REGIONS,
                        chain_log: &mut (&mut *addr_of_mut!(CHAIN_LOG))[..REGIONS * 256],
                        store: core::slice::from_raw_parts_mut(STORE_ADDR as *mut u32, STORE_KB * 256),
                        store_epochs: &mut *addr_of_mut!(STORE_EPOCHS),
                    },
                )
            };
            let mut refbus = VirtBus::new(VirtConsole::new(true, true, b""), info.disk_len, REF_RAM, ram_size);
            let rcpu: Option<&mut Cpu> = if lockstep {
                unsafe {
                    core::ptr::copy_nonoverlapping(GUEST_RAM as *const u8, REF_RAM as *mut u8, ram_size as usize);
                    let p = (*addr_of_mut!(REF_CPU)).as_mut_ptr();
                    Some(Cpu::init(p, REF_RAM as *mut u8, 0, ram_size, 0, dtb))
                }
            } else {
                None
            };
            let mut rcpu = rcpu;
            while cpu.halt.is_none() {
                let before = cpu.instret;
                let ev = jit.run_one(cpu, &mut bus);
                events += 1;
                if let Some(r) = rcpu.as_deref_mut() {
                    let ok = match ev {
                        Event::Irq => r.check_irq(&refbus),
                        Event::Interp => {
                            while r.instret < before {
                                r.check_irq(&refbus);
                                r.step(&mut refbus);
                            }
                            r.check_irq(&refbus);
                            r.step(&mut refbus);
                            true
                        }
                        Event::Block { .. } => {
                            while r.instret < cpu.instret {
                                r.check_irq(&refbus);
                                r.step(&mut refbus);
                            }
                            true
                        }
                    };
                    if !ok || !same_state(cpu, r) {
                        println!("\n[rvemu-virt] LOCKSTEP MISMATCH after event #{events} {ev:?} (instret before {before})");
                        dump("jit", cpu);
                        dump("ref", r);
                        qemu_exit(5);
                    }
                }
                until_check(cpu, &bus);
                if events & 0xFFF == 0 {
                    bus.poll();
                    if max_insns != 0 && cpu.instret >= max_insns && cpu.halt.is_none() {
                        cpu.halt = Some(Halt::MaxInsns);
                    }
                }
            }
            let s = &jit.stats;
            println!("\n---- translator ----");
            println!("blocks {}  guest insns translated {}  code {} KiB ({:.1} words/insn)  live {} KiB",
                s.blocks, s.guest_insns_translated, s.code_words * 4 / 1024,
                s.code_words as f32 / s.guest_insns_translated.max(1) as f32, jit.code_words_used() * 4 / 1024);
            println!("entries {}  insns in translated code {} ({:.1}%)  interpreted {}",
                s.entries, s.jit_insns, 100.0 * s.jit_insns as f32 / cpu.instret.max(1) as f32, s.interp_steps);
            println!("exits: jump {}  interp {}  budget {}   chains {}  flushes {}  evictions {}  smc {}",
                s.exit_jump, s.exit_interp, s.exit_budget, s.chains, s.flushes, s.evictions, s.smc_invalidations);
            println!("store: {} KiB, restores {} ({} words), resets {}", STORE_KB, s.restores, s.restore_words, s.store_resets);
            let g = cpu.instret.max(1) as f32;
            println!("host insns per guest insn: total {:.2} = translated code {:.2} + interpreter {:.2} + translation {:.2} + dispatch {:.2}",
                s.h_total as f32 / g, s.h_code as f32 / g, s.h_interp as f32 / g, s.h_translate as f32 / g,
                (s.h_total - s.h_code - s.h_interp - s.h_translate) as f32 / g);
            let _ = write!(Out, "interpreted by opcode:");
            for (op, &c) in s.interp_op.iter().enumerate() {
                if c != 0 {
                    let _ = write!(Out, " {op:#04x}:{c}");
                }
            }
            println!();
            if lockstep {
                println!("lockstep: {events} events checked, all matched");
            }
        }
    }
    let host = host_instret() - t0;
    println!("\n[rvemu-virt] stopped: {:?} at pc={:#010x}", cpu.halt, cpu.pc);
    println!("guest instructions {}", cpu.instret);
    if cpu.stats.until_instret != 0 {
        println!("  to --until match {}", cpu.stats.until_instret);
    }
    println!("host instructions {} ({:.2} per guest instruction; meaningful with -icount)",
        host, host as f32 / cpu.instret.max(1) as f32);
    qemu_exit(0);
}
