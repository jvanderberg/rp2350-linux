//! rvemu on the RP2350B: the guest runs on the Hazard3 cores with its RAM in
//! the 8 MB PSRAM (cached XIP window). Guest console output goes to RTT.
//!
//! Runs the jittest guest (../jittest-guest, embedded and copied into guest
//! RAM before each run) three times: translator, lockstep (translator checked
//! against an interpreter on a second copy of RAM after every dispatcher
//! step), and interpreter. Every run must print the same output as the Mac
//! reference; scripts/pico.sh checks that.
#![no_std]
#![no_main]

use core::mem::MaybeUninit;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};

use panic_rtt_target as _;
use rp2350_board as board;
use rp235x_hal as hal;
use rtt_target::{rprint, rprintln, rtt_init_print, ChannelMode};
use rvcore::dev::{Console, Platform};
use rvcore::{Bus, Cpu, Halt};
use rvjit::{Event, JcEntry, Jit, JitMem};

#[unsafe(link_section = ".start_block")]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

static GUEST: &[u8] = include_bytes!("../../jittest-guest/jittest.bin");
/// Same RAM size as the Mac reference run (`rvemu -m 4`).
const RAM_SIZE: u32 = 4 << 20;
const DTB_ADDR: u32 = RAM_SIZE - 0x10000;
const GUEST_RAM: usize = board::PSRAM_CACHED;
/// Lockstep reference copy: the upper 4 MiB of the PSRAM.
const REF_RAM: usize = board::PSRAM_CACHED + RAM_SIZE as usize;

const XIP_CTR_HIT: usize = 0x400c_800c;
const XIP_CTR_ACC: usize = 0x400c_8010;

// ------------------------------------------------------------------ translator storage (SRAM)

const CODE_WORDS: usize = 64 * 1024; // 256 KiB of translated code and block headers
const NBUCKETS: usize = 1024;
const NJC: usize = 1024;

static mut CODE: [u32; CODE_WORDS] = [0; CODE_WORDS];
static mut BUCKETS: [u32; NBUCKETS] = [0; NBUCKETS];
static mut CODE_PAGES: [u8; (RAM_SIZE >> 15) as usize] = [0; (RAM_SIZE >> 15) as usize];
static mut JUMP_CACHE: [JcEntry; NJC] = [JcEntry::EMPTY; NJC];
static mut CPU: MaybeUninit<Cpu> = MaybeUninit::uninit();
static mut REF_CPU: MaybeUninit<Cpu> = MaybeUninit::uninit();

// ------------------------------------------------------------------ console

/// Guest console: line-buffered to RTT (unless quiet), plus an FNV-1a hash of
/// every byte so runs can be compared without diffing text.
struct RttConsole {
    quiet: bool,
    line: [u8; 160],
    len: usize,
    hash: u32,
    bytes: u32,
}

impl RttConsole {
    const fn new(quiet: bool) -> Self {
        RttConsole { quiet, line: [0; 160], len: 0, hash: 0x811c_9dc5, bytes: 0 }
    }
    fn flush(&mut self) {
        if self.len != 0 {
            rprint!("{}", core::str::from_utf8(&self.line[..self.len]).unwrap_or("<non-utf8>"));
            self.len = 0;
        }
    }
}

impl Console for RttConsole {
    fn putc(&mut self, b: u8) {
        self.hash = (self.hash ^ u32::from(b)).wrapping_mul(0x0100_0193);
        self.bytes += 1;
        if self.quiet {
            return;
        }
        self.line[self.len] = b;
        self.len += 1;
        if b == b'\n' || self.len == self.line.len() {
            self.flush();
        }
    }
    fn rx_ready(&self) -> bool {
        false
    }
    fn getc(&mut self) -> Option<u8> {
        None
    }
}

struct PicoBus {
    plat: Platform<RttConsole>,
}

impl PicoBus {
    fn new(quiet: bool) -> Self {
        PicoBus { plat: Platform::new(RttConsole::new(quiet)) }
    }
}

impl Bus for PicoBus {
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
    /// Batch policy, same as the Mac host and rvemu-virt: skip to the timer
    /// deadline; a guest idle with no timer is stopped.
    fn wfi(&mut self, cpu: &mut Cpu) {
        self.plat.update_irqs();
        if cpu.sip_value(self) & cpu.sie != 0 {
            return;
        }
        let now = cpu.time();
        if cpu.stimecmp <= now {
            return;
        }
        if cpu.stimecmp == u64::MAX {
            if !self.plat.con.quiet {
                rprintln!("\n[rvemu-pico] guest idle with no timer and no input");
            }
            cpu.halt = Some(Halt::Idle);
            return;
        }
        let skip = cpu.stimecmp - now;
        cpu.time_skip += skip;
        cpu.stats.idle_ticks += skip;
    }
}

// ------------------------------------------------------------------ host traps

#[unsafe(export_name = "ExceptionHandler")]
fn host_exception(_tf: &riscv_rt::TrapFrame) -> ! {
    let (cause, epc, tval): (u32, u32, u32);
    // SAFETY: reading trap CSRs.
    unsafe {
        core::arch::asm!("csrr {0}, mcause", "csrr {1}, mepc", "csrr {2}, mtval",
            out(reg) cause, out(reg) epc, out(reg) tval);
    }
    // rtt-target's rprintln! with no arguments does not format, so pass them
    rprintln!("\n[rvemu-pico] HOST TRAP mcause={:#x} mepc={:#010x} mtval={:#010x}", cause, epc, tval);
    loop {
        hal::arch::wfi();
    }
}

fn stop() -> ! {
    rprintln!("[rvemu-pico] done");
    loop {
        hal::arch::wfi();
    }
}

// ------------------------------------------------------------------ runs

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Interp,
    Jit,
    Lockstep,
}

impl Mode {
    fn name(self) -> &'static str {
        match self {
            Mode::Interp => "interpreter",
            Mode::Jit => "translator",
            Mode::Lockstep => "lockstep",
        }
    }
}

struct RunResult {
    halt: Option<Halt>,
    instret: u64,
    cycles: u64,
    host_insns: u64,
    con_hash: u32,
    con_bytes: u32,
}

/// Fresh guest RAM: zeroed, image at PA 0.
fn load_guest(base: usize) {
    let ram = base as *mut u8;
    // SAFETY: base..+RAM_SIZE is mapped, writable PSRAM (psram_init).
    unsafe {
        core::ptr::write_bytes(ram, 0, RAM_SIZE as usize);
        core::ptr::copy_nonoverlapping(GUEST.as_ptr(), ram, GUEST.len());
    }
}

/// # Safety
/// `slot` must not be in use; `base` must be guest RAM loaded by `load_guest`.
unsafe fn new_cpu(slot: *mut MaybeUninit<Cpu>, base: usize) -> &'static mut Cpu {
    // SAFETY: per the contract above.
    unsafe {
        let p = (*slot).as_mut_ptr();
        Cpu::init(p, base as *mut u8, 0, RAM_SIZE, 0, DTB_ADDR)
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

fn dump(tag: &str, c: &Cpu) {
    rprintln!(
        "{}: pc={:#010x} priv={:?} instret={} satp={:#x} sepc={:#x} scause={:#x} stval={:#x}",
        tag, c.pc, c.privilege, c.instret, c.satp, c.sepc, c.scause, c.stval
    );
    for r in (0..32).step_by(8) {
        rprintln!(
            "  x{:<2} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
            r, c.x[r], c.x[r + 1], c.x[r + 2], c.x[r + 3], c.x[r + 4], c.x[r + 5], c.x[r + 6], c.x[r + 7]
        );
    }
}

fn run(mode: Mode) -> RunResult {
    rprintln!("--- {} ---", mode.name());
    load_guest(GUEST_RAM);
    // SAFETY: one run at a time owns CPU / REF_CPU and the translator tables.
    let cpu = unsafe { new_cpu(addr_of_mut!(CPU), GUEST_RAM) };
    let mut bus = PicoBus::new(false);
    // SAFETY: XIP counter registers; any write clears them.
    unsafe {
        write_volatile(XIP_CTR_HIT as *mut u32, 0);
        write_volatile(XIP_CTR_ACC as *mut u32, 0);
    }
    let c0 = board::mcycle64();
    let i0 = board::minstret64();

    if mode == Mode::Interp {
        interp_loop(cpu, &mut bus);
    } else {
        // SAFETY: the translator tables are only used by this run.
        let mut jit = unsafe {
            Jit::new(
                cpu,
                JitMem {
                    code: &mut *addr_of_mut!(CODE),
                    buckets: &mut *addr_of_mut!(BUCKETS),
                    code_pages: &mut *addr_of_mut!(CODE_PAGES),
                    jump_cache: &mut *addr_of_mut!(JUMP_CACHE),
                    regions: 1,
                    chain_log: &mut [],
                    store: &mut [],
                    store_epochs: &mut [],
                },
            )
        };
        // Guest RAM is PSRAM, where Hazard3 AMOs fault (no exclusives outside SRAM).
        jit.native_amo = false;
        let mut rcpu = if mode == Mode::Lockstep {
            load_guest(REF_RAM);
            // SAFETY: REF_CPU is only used by this run.
            Some(unsafe { new_cpu(addr_of_mut!(REF_CPU), REF_RAM) })
        } else {
            None
        };
        let mut refbus = PicoBus::new(true);
        let events = jit_loop(&mut jit, cpu, &mut bus, rcpu.as_deref_mut(), &mut refbus);
        if let Some(r) = rcpu.as_deref() {
            if refbus.plat.con.hash != bus.plat.con.hash {
                rprintln!("[rvemu-pico] LOCKSTEP console mismatch: ref {:08x} jit {:08x}",
                    refbus.plat.con.hash, bus.plat.con.hash);
            }
            rprintln!("lockstep: {} events checked, all matched (ref instret {})", events, r.instret);
        }
        let s = &jit.stats;
        rprintln!(
            "blocks {}  guest insns translated {}  code {} KiB  live {} KiB",
            s.blocks, s.guest_insns_translated, s.code_words * 4 / 1024, jit.code_words_used() * 4 / 1024
        );
        rprintln!(
            "entries {}  insns in translated code {}  interpreted {}",
            s.entries, s.jit_insns, s.interp_steps
        );
        rprintln!(
            "exits: jump {}  interp {}  budget {}   chains {}  flushes {}  smc {}",
            s.exit_jump, s.exit_interp, s.exit_budget, s.chains, s.flushes, s.smc_invalidations
        );
        let g = cpu.instret.max(1);
        rprintln!(
            "host insns per guest insn: total {} = translated code {} + interpreter {} + translation {} (x100)",
            s.h_total * 100 / g, s.h_code * 100 / g, s.h_interp * 100 / g, s.h_translate * 100 / g
        );
    }

    let cycles = board::mcycle64() - c0;
    let host_insns = board::minstret64() - i0;
    // SAFETY: XIP counter registers.
    let (hit, acc) = unsafe { (read_volatile(XIP_CTR_HIT as *const u32), read_volatile(XIP_CTR_ACC as *const u32)) };
    bus.plat.con.flush();
    let g = cpu.instret.max(1);
    rprintln!(
        "{}: {:?}, {} guest insns, {}.{:02} host cycles and {}.{:02} host insns per guest insn",
        mode.name(), cpu.halt, cpu.instret,
        cycles / g, cycles * 100 / g % 100, host_insns / g, host_insns * 100 / g % 100
    );
    rprintln!(
        "XIP cache (flash code + guest RAM): {} accesses, {}% hits; console {} bytes hash {:08x}",
        acc, u64::from(hit) * 100 / u64::from(acc.max(1)), bus.plat.con.bytes, bus.plat.con.hash
    );
    RunResult {
        halt: cpu.halt,
        instret: cpu.instret,
        cycles,
        host_insns,
        con_hash: bus.plat.con.hash,
        con_bytes: bus.plat.con.bytes,
    }
}

// The run loops live in SRAM (as do the rvcore / rvjit hot paths with the
// `ram-code` feature). They must be functions of their own: whatever the
// compiler inlines into a caller runs from the caller's section, and inlined
// into `main` it would run from flash through the XIP cache.

#[inline(never)]
#[unsafe(link_section = ".data.rvemu_hot")]
fn interp_loop(cpu: &mut Cpu, bus: &mut PicoBus) {
    while cpu.halt.is_none() {
        for _ in 0..4096 {
            cpu.check_irq(bus);
            cpu.step(bus);
            if cpu.halt.is_some() {
                break;
            }
        }
        bus.plat.update_irqs();
    }
}

/// Translator dispatcher loop, optionally checked step by step against an
/// interpreter (`rcpu`) on its own copy of guest RAM. Returns the event count.
#[inline(never)]
#[unsafe(link_section = ".data.rvemu_hot")]
fn jit_loop(
    jit: &mut Jit,
    cpu: &mut Cpu,
    bus: &mut PicoBus,
    mut rcpu: Option<&mut Cpu>,
    refbus: &mut PicoBus,
) -> u64 {
    let mut events: u64 = 0;
    while cpu.halt.is_none() {
        let before = cpu.instret;
        let ev = jit.run_one(cpu, bus);
        events += 1;
        if let Some(r) = rcpu.as_deref_mut() {
            let ok = match ev {
                Event::Irq => r.check_irq(refbus),
                Event::Interp => {
                    while r.instret < before {
                        r.check_irq(refbus);
                        r.step(refbus);
                    }
                    r.check_irq(refbus);
                    r.step(refbus);
                    true
                }
                Event::Block { .. } => {
                    while r.instret < cpu.instret {
                        r.check_irq(refbus);
                        r.step(refbus);
                    }
                    true
                }
            };
            if !ok || !same_state(cpu, r) {
                bus.plat.con.flush();
                rprintln!(
                    "\n[rvemu-pico] LOCKSTEP MISMATCH after event #{} {:?} (instret before {})",
                    events, ev, before
                );
                dump("jit", cpu);
                dump("ref", r);
                stop();
            }
        }
        if events & 0xFFF == 0 {
            bus.plat.update_irqs();
        }
    }
    events
}

// ------------------------------------------------------------------ main

#[hal::entry]
fn main() -> ! {
    rtt_init_print!(ChannelMode::BlockIfFull, 4096);
    let mut pac = hal::pac::Peripherals::take().unwrap();
    let mut watchdog = hal::Watchdog::new(pac.WATCHDOG);
    let clocks = board::init_clocks(
        pac.XOSC,
        pac.CLOCKS,
        pac.PLL_SYS,
        pac.PLL_USB,
        &mut pac.RESETS,
        &mut watchdog,
    );
    board::timer_ignore_debug_halt();
    board::enable_counters();
    let sio = hal::Sio::new(pac.SIO);
    let pins = hal::gpio::Pins::new(pac.IO_BANK0, pac.PADS_BANK0, sio.gpio_bank0, &mut pac.RESETS);
    use hal::Clock as _;
    let sys_hz = clocks.system_clock.freq().to_Hz();

    rprintln!("\n=== rvemu-pico: Hazard3 @ {} MHz{} ===", sys_hz / 1_000_000, board::CLOCK_NOTE);
    let psram = match board::psram_init(pins.gpio0, sys_hz) {
        Ok(p) => p,
        Err(e) => {
            rprintln!("PSRAM init failed: {:?}", e);
            stop();
        }
    };
    rprintln!(
        "PSRAM {} KB at {} MHz (timing {:08x}); guest RAM {} KB at {:#010x}; guest image {} bytes",
        psram.size >> 10, psram.sck_hz / 1_000_000, psram.timing, RAM_SIZE >> 10, GUEST_RAM, GUEST.len()
    );
    rprintln!("translator: {} KiB code cache, {} jump-cache entries", CODE_WORDS * 4 / 1024, NJC);

    let modes = [Mode::Jit, Mode::Lockstep, Mode::Interp];
    let mut results: [Option<RunResult>; 3] = [None, None, None];
    for (i, &m) in modes.iter().enumerate() {
        results[i] = Some(run(m));
    }

    rprintln!("\n=== summary ===");
    let base = results[0].as_ref().unwrap();
    let mut agree = true;
    for (m, r) in modes.iter().zip(results.iter()) {
        let r = r.as_ref().unwrap();
        let g = r.instret.max(1);
        rprintln!(
            "{:<12} {:?}  instret {}  console {:08x}  {} ms  {} kIPS  {}.{:02} cycles/insn",
            m.name(), r.halt, r.instret, r.con_hash,
            r.cycles * 1000 / u64::from(sys_hz),
            r.instret * u64::from(sys_hz) / r.cycles.max(1) / 1000,
            r.cycles / g, r.cycles * 100 / g % 100
        );
        let _ = r.host_insns;
        agree &= r.halt == base.halt && r.instret == base.instret && r.con_hash == base.con_hash
            && r.con_bytes == base.con_bytes;
    }
    let interp = results[2].as_ref().unwrap();
    rprintln!(
        "translator speedup over interpreter: {}.{:02}x",
        interp.cycles / base.cycles.max(1),
        interp.cycles * 100 / base.cycles.max(1) % 100
    );
    rprintln!("[rvemu-pico] all runs {}", if agree { "AGREE" } else { "DISAGREE" });
    stop();
}

#[unsafe(link_section = ".bi_entries")]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_cargo_bin_name!(),
    hal::binary_info::rp_program_description!(c"rvemu on Hazard3"),
];
