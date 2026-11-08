//! Boot the buildroot Linux guest on the RP2350B: kernel, initramfs and DTB
//! come from a blob in flash (scripts/pico-linux.sh writes it at
//! `BLOB_ADDR`), guest RAM is the whole 8 MB PSRAM (cached XIP window), the
//! console goes to RTT. Runs the translator (and, with `RUN_INTERPRETER`, the
//! interpreter) up to the console printing `login:`, and reports the
//! counters the cache-policy simulator predicts (rvemu -S, scripts/sim.py).
//! The translator is configured like rvemu-pico-shell's.
//!
//! The host loads this firmware and the blob over USB (picotool) and reads
//! RTT by attaching a debugger. The firmware waits for the host to drain its
//! banner before timing anything, and ends by rebooting into USB BOOTSEL so
//! the next load needs no button.
#![no_std]
#![no_main]

use core::mem::MaybeUninit;
use core::ptr::{addr_of_mut, read_volatile, write_volatile};

use panic_rtt_target as _;
use rp2350_board as board;
use rp235x_hal as hal;
use rtt_target::{ChannelMode, rprint, rprintln, rtt_init_print};
use rvcore::dev::{Console, Platform};
use rvcore::{Bus, Cpu, Halt};
use rvjit::{JcEntry, Jit, JitMem};

#[path = "../guest_blob.rs"]
mod guest_blob;
use guest_blob::{BLOB_ADDR, BlobHeader};

#[unsafe(link_section = ".start_block")]
#[used]
pub static IMAGE_DEF: hal::block::ImageDef = hal::block::ImageDef::secure_exe();

/// PSRAM: guest RAM, then the translator's backing store. The store is off:
/// on the board its traffic shares the 16 KB XIP cache with guest RAM, and a
/// 1 MiB store made the boot slower (9.9 s against 8.0 s; XIP misses 3.7 M
/// -> 12.7 M). With `STORE_WORDS = (1 << 20) / 4` the guest gets 7 MiB.
const STORE_WORDS: usize = 0;
const RAM_MAX: u32 = (8 << 20) - (STORE_WORDS * 4) as u32;
const STORE: usize = board::PSRAM_CACHED + RAM_MAX as usize;
const UNTIL: &[u8] = b"login:";

const XIP_CTR_HIT: usize = 0x400c_800c;
const XIP_CTR_ACC: usize = 0x400c_8010;

// ------------------------------------------------------------------ translator storage (SRAM)

/// 384 KiB in a ring of 8 regions, plus the PSRAM backing store. (The shell
/// has 326 KiB: core1's code and buffers take the rest of its SRAM.)
const CODE_WORDS: usize = 96 * 1024;
const NBUCKETS: usize = 1024;
const NJC: usize = 1024;
const REGIONS: usize = 8;
/// The interpreter baseline (36.5 s to login: at 348 MHz) only needs
/// measuring once.
const RUN_INTERPRETER: bool = false;

static mut CODE: [u32; CODE_WORDS] = [0; CODE_WORDS];
static mut BUCKETS: [u32; NBUCKETS] = [0; NBUCKETS];
/// Guest RAM as Linux sees it, at most: flash-backed kernel text + PSRAM.
const GUEST_MAX: u32 = RAM_MAX + guest_blob::TEXT_MAX;
static mut CODE_PAGES: [u8; (GUEST_MAX >> 15) as usize] = [0; (GUEST_MAX >> 15) as usize];
static mut JUMP_CACHE: [JcEntry; NJC] = [JcEntry::EMPTY; NJC];
static mut CHAIN_LOG: [u32; REGIONS * 128] = [0; REGIONS * 128];
static mut STORE_EPOCHS: [u8; (GUEST_MAX >> 12) as usize] = [0; (GUEST_MAX >> 12) as usize];
static mut CPU: MaybeUninit<Cpu> = MaybeUninit::uninit();

// ------------------------------------------------------------------ console

/// Line-buffered to RTT, an FNV-1a hash of every byte, and the `login:` stop.
struct RttConsole {
    line: [u8; 160],
    len: usize,
    hash: u32,
    bytes: u32,
    tail: [u8; 16],
    until_hit: bool,
}

impl RttConsole {
    const fn new() -> Self {
        RttConsole { line: [0; 160], len: 0, hash: 0x811c_9dc5, bytes: 0, tail: [0; 16], until_hit: false }
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
        self.tail.copy_within(1.., 0);
        let last = self.tail.len() - 1;
        self.tail[last] = b;
        if self.tail.ends_with(UNTIL) {
            self.until_hit = true;
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
    /// Batch policy, same as the Mac host: skip to the timer deadline.
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
            rprintln!("\n[rvemu-pico] guest idle with no timer and no input");
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
    rprintln!("\n[rvemu-pico] HOST TRAP mcause={:#x} mepc={:#010x} mtval={:#010x}", cause, epc, tval);
    loop {
        hal::arch::wfi();
    }
}

unsafe extern "C" {
    /// rtt-target's control block: id[16], max_up, max_down, then up
    /// channel 0 = name, buffer, size, write, read, flags.
    static _SEGGER_RTT: [u32; 12];
}

/// Wait (up to `secs`) until the host has read everything in RTT up channel 0.
fn wait_rtt_drained(secs: u64, sys_hz: u32) -> bool {
    let p = core::ptr::addr_of!(_SEGGER_RTT) as *const u32;
    let t0 = board::mcycle64();
    // SAFETY: the control block is live for the whole program.
    while unsafe { read_volatile(p.add(9)) != read_volatile(p.add(10)) } {
        if board::mcycle64() - t0 > secs * u64::from(sys_hz) {
            return false;
        }
    }
    true
}

fn stop(sys_hz: u32) -> ! {
    rprintln!("[rvemu-pico] done; rebooting to USB BOOTSEL");
    wait_rtt_drained(5, sys_hz);
    board::reboot_to_bootsel();
}

// ------------------------------------------------------------------ runs

fn run(jit_mode: bool, h: &BlobHeader, sys_hz: u32) {
    rprintln!("--- {} ---", if jit_mode { "translator" } else { "interpreter" });
    // SAFETY: guest RAM is the mapped, writable PSRAM; the header was checked.
    let dtb = unsafe { h.load() };
    // SAFETY: one run at a time owns CPU and the translator tables.
    let cpu = unsafe {
        let p = (*addr_of_mut!(CPU)).as_mut_ptr();
        let c = Cpu::init(p, h.ram_host() as *mut u8, 0, h.ram_size, 0, dtb);
        c.set_readonly_prefix(h.text_len);
        c
    };
    let mut bus = PicoBus { plat: Platform::new(RttConsole::new()) };
    // SAFETY: XIP counter registers; any write clears them.
    unsafe {
        write_volatile(XIP_CTR_HIT as *mut u32, 0);
        write_volatile(XIP_CTR_ACC as *mut u32, 0);
    }
    let c0 = board::mcycle64();
    let i0 = board::minstret64();
    let mut jit_stats = None;
    if jit_mode {
        // SAFETY: the translator tables are only used by this run.
        let mut jit = unsafe {
            Jit::new(
                cpu,
                JitMem {
                    code: &mut *addr_of_mut!(CODE),
                    buckets: &mut *addr_of_mut!(BUCKETS),
                    code_pages: &mut *addr_of_mut!(CODE_PAGES),
                    jump_cache: &mut *addr_of_mut!(JUMP_CACHE),
                    regions: REGIONS,
                    chain_log: &mut *addr_of_mut!(CHAIN_LOG),
                    // SAFETY: the top MiB of the PSRAM, above guest RAM (psram_init
                    // found 8 MB; the blob's RAM size is at most RAM_MAX).
                    store: core::slice::from_raw_parts_mut(STORE as *mut u32, STORE_WORDS),
                    store_epochs: &mut *addr_of_mut!(STORE_EPOCHS),
                },
            )
        };
        // Guest RAM is PSRAM, where Hazard3 AMOs fault.
        jit.native_amo = false;
        jit_loop(&mut jit, cpu, &mut bus);
        jit_stats = Some(jit.stats);
    } else {
        interp_loop(cpu, &mut bus);
    }
    let cycles = board::mcycle64() - c0;
    let host = board::minstret64() - i0;
    // SAFETY: XIP counter registers.
    let (hit, acc) = unsafe { (read_volatile(XIP_CTR_HIT as *const u32), read_volatile(XIP_CTR_ACC as *const u32)) };
    bus.plat.con.flush();
    let g = cpu.instret.max(1);
    rprintln!("\n[rvemu-pico] stopped: {:?} at pc={:#010x}", cpu.halt, cpu.pc);
    if let Some(s) = jit_stats {
        rprintln!(
            "blocks {}  guest insns translated {}  code words {}  entries {}  jit insns {}  interpreted {}",
            s.blocks, s.guest_insns_translated, s.code_words, s.entries, s.jit_insns, s.interp_steps
        );
        rprintln!(
            "exits: jump {}  interp {}  budget {}   chains {}  flushes {}  evictions {}  smc {}",
            s.exit_jump, s.exit_interp, s.exit_budget, s.chains, s.flushes, s.evictions, s.smc_invalidations
        );
        rprintln!(
            "store: restores {} ({} words)  resets {}",
            s.restores, s.restore_words, s.store_resets
        );
        rprintln!(
            "host insns (x100 per guest insn): total {}  code {}  interp {}  translate {}",
            s.h_total * 100 / g, s.h_code * 100 / g, s.h_interp * 100 / g, s.h_translate * 100 / g
        );
    }
    let ms = cycles * 1000 / u64::from(sys_hz);
    rprintln!(
        "RESULT {} instret {} cycles {} host_insns {} ms {} kIPS {} xip_acc {} xip_miss {} console {} bytes hash {:08x}",
        if jit_mode { "translator" } else { "interpreter" },
        cpu.instret, cycles, host, ms, cpu.instret * u64::from(sys_hz) / cycles.max(1) / 1000,
        acc, acc - hit, bus.plat.con.bytes, bus.plat.con.hash
    );
}

#[inline(never)]
#[unsafe(link_section = ".data.rvemu_hot")]
fn interp_loop(cpu: &mut Cpu, bus: &mut PicoBus) {
    while cpu.halt.is_none() {
        for _ in 0..4096 {
            cpu.check_irq(bus);
            cpu.step(bus);
            if bus.plat.con.until_hit {
                cpu.halt = Some(Halt::Until);
            }
            if cpu.halt.is_some() {
                break;
            }
        }
        bus.plat.update_irqs();
    }
}

#[inline(never)]
#[unsafe(link_section = ".data.rvemu_hot")]
fn jit_loop(jit: &mut Jit, cpu: &mut Cpu, bus: &mut PicoBus) {
    let mut events: u32 = 0;
    while cpu.halt.is_none() {
        jit.run_one(cpu, bus);
        if bus.plat.con.until_hit {
            cpu.halt = Some(Halt::Until);
        }
        events = events.wrapping_add(1);
        if events & 0xFFF == 0 {
            bus.plat.update_irqs();
        }
    }
}

// ------------------------------------------------------------------ main

#[hal::entry]
fn main() -> ! {
    board::bootsel_if_requested();
    rtt_init_print!(ChannelMode::BlockIfFull, 16384);
    let mut pac = hal::pac::Peripherals::take().unwrap();
    let mut watchdog = hal::Watchdog::new(pac.WATCHDOG);
    let clocks =
        board::init_clocks(pac.XOSC, pac.CLOCKS, pac.PLL_SYS, pac.PLL_USB, &mut pac.RESETS, &mut watchdog);
    board::timer_ignore_debug_halt();
    board::enable_counters();
    let sio = hal::Sio::new(pac.SIO);
    let pins = hal::gpio::Pins::new(pac.IO_BANK0, pac.PADS_BANK0, sio.gpio_bank0, &mut pac.RESETS);
    use hal::Clock as _;
    let sys_hz = clocks.system_clock.freq().to_Hz();

    rprintln!("\n=== rvemu-pico linux: Hazard3 @ {} MHz{} ===", sys_hz / 1_000_000, board::CLOCK_NOTE);
    let psram = match board::psram_init(pins.gpio0, sys_hz) {
        Ok(p) => p,
        Err(e) => {
            rprintln!("PSRAM init failed: {:?}", e);
            stop(sys_hz);
        }
    };
    let h = BlobHeader::read();
    if psram.size < 8 << 20 || !h.valid(RAM_MAX) {
        rprintln!("no valid guest blob at {:#010x} (magic {:#010x}); run scripts/pico-linux.sh", BLOB_ADDR, h.magic);
        stop(sys_hz);
    }
    rprintln!(
        "PSRAM {} KB at {} MHz; guest RAM {} KiB; kernel {} initrd {} dtb {} bytes",
        psram.size >> 10, psram.sck_hz / 1_000_000, h.ram_size >> 10, h.kernel_len, h.initrd_len, h.dtb_len
    );
    rprintln!(
        "translator: {} KiB code cache in {} regions, {} jump-cache entries, {} KiB PSRAM store",
        CODE_WORDS * 4 / 1024, REGIONS, NJC, STORE_WORDS * 4 / 1024
    );
    // don't time a run while RTT may block on a host that hasn't attached
    if !wait_rtt_drained(30, sys_hz) {
        rprintln!("[rvemu-pico] no debugger draining RTT after 30 s");
        stop(sys_hz);
    }
    run(true, &h, sys_hz);
    if RUN_INTERPRETER {
        run(false, &h, sys_hz);
    }
    stop(sys_hz);
}

#[unsafe(link_section = ".bi_entries")]
#[used]
pub static PICOTOOL_ENTRIES: [hal::binary_info::EntryAddr; 2] = [
    hal::binary_info::rp_cargo_bin_name!(),
    hal::binary_info::rp_program_description!(c"rvemu: Linux guest on Hazard3"),
];
