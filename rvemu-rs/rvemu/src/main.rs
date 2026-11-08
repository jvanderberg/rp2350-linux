//! rvemu host for macOS/Linux: loads a kernel Image + initramfs, generates a
//! device tree, runs the guest on the terminal and reports statistics plus an
//! RP2350 timing model.

use std::io::Write;
use std::process::{Command, ExitCode};
use std::time::Instant;

use rvcore::dev::{Console, PLIC_BASE, PLIC_SIZE, Platform, UART_BASE, UART_IRQ, UART_SIZE};
use rvcore::swapdev::{CHUNK, Flash, FlashSwap, PAGE};
use rvcore::virtio::{
    GuestMem, MemDisk, VIRTIO_BASE, VIRTIO_IRQ, VIRTIO_NET_BASE, VIRTIO_NET_IRQ, VIRTIO_SIZE, VIRTIO_SWAP_BASE,
    VIRTIO_SWAP_IRQ,
};
use stubnet::StubNet;
use rvcore::{Bus, Cpu, Halt, Priv, TIMEBASE_HZ};

mod sim;
mod stubnet;

const DTB_RESERVE: u32 = 0x10000; // top 64 KiB of RAM holds the DTB

// ------------------------------------------------------------------ console

struct TermConsole {
    stdin_open: bool,
    rx: std::collections::VecDeque<u8>,
    out: Vec<u8>,
    until: Vec<u8>,
    tail: Vec<u8>,
    until_hit: bool,
    /// The kernel printed its panic trailer.
    panicked: bool,
    ctrl_a: bool,
    user_quit: bool,
    saved_tty: Option<libc::termios>,
}

impl TermConsole {
    fn new(interactive: bool, until: Option<&str>) -> Self {
        let mut c = TermConsole {
            stdin_open: interactive,
            rx: Default::default(),
            out: Vec::with_capacity(4096),
            until: until.map(|s| s.as_bytes().to_vec()).unwrap_or_default(),
            tail: Vec::with_capacity(256),
            until_hit: false,
            panicked: false,
            ctrl_a: false,
            user_quit: false,
            saved_tty: None,
        };
        c.until.truncate(256);
        if interactive && unsafe { libc::isatty(0) } == 1 {
            unsafe {
                let mut t: libc::termios = std::mem::zeroed();
                if libc::tcgetattr(0, &mut t) == 0 {
                    c.saved_tty = Some(t);
                    t.c_lflag &= !(libc::ICANON | libc::ECHO | libc::ISIG);
                    t.c_iflag &= !(libc::ICRNL | libc::IXON);
                    t.c_cc[libc::VMIN] = 1;
                    t.c_cc[libc::VTIME] = 0;
                    libc::tcsetattr(0, libc::TCSANOW, &t);
                }
            }
        }
        c
    }

    fn flush(&mut self) {
        let mut out = std::io::stdout().lock();
        let _ = out.write_all(&self.out);
        let _ = out.flush();
        self.out.clear();
    }

    fn restore(&mut self) {
        self.flush();
        if let Some(t) = self.saved_tty.take() {
            unsafe { libc::tcsetattr(0, libc::TCSANOW, &t) };
        }
    }

    fn stdin_readable(timeout_ms: i32) -> bool {
        let mut p = libc::pollfd { fd: 0, events: libc::POLLIN, revents: 0 };
        unsafe { libc::poll(&mut p, 1, timeout_ms) > 0 && p.revents & (libc::POLLIN | libc::POLLHUP) != 0 }
    }

    fn read_input(&mut self) {
        while self.stdin_open && self.rx.len() < 255 && Self::stdin_readable(0) {
            let mut ch = 0u8;
            let n = unsafe { libc::read(0, &mut ch as *mut u8 as *mut libc::c_void, 1) };
            if n <= 0 {
                self.stdin_open = false;
                return;
            }
            if self.ctrl_a {
                self.ctrl_a = false;
                if ch == b'x' {
                    self.user_quit = true;
                    return;
                }
            } else if ch == 1 {
                self.ctrl_a = true;
                continue;
            }
            self.rx.push_back(ch);
        }
    }
}

impl Console for TermConsole {
    fn putc(&mut self, b: u8) {
        self.out.push(b);
        if b == b'\n' || self.out.len() == 4096 {
            self.flush();
        }
        if self.tail.len() == 256 {
            self.tail.remove(0);
        }
        self.tail.push(b);
        if !self.until.is_empty() && !self.until_hit && self.tail.ends_with(&self.until) {
            self.until_hit = true;
        }
        if b == b'c' && self.tail.ends_with(b"---[ end Kernel panic") {
            self.panicked = true;
        }
    }
    fn rx_ready(&self) -> bool {
        !self.rx.is_empty()
    }
    fn getc(&mut self) -> Option<u8> {
        self.rx.pop_front()
    }
}

// ------------------------------------------------------------------ swap

/// The board's swap region (0x10200000-0x103FEFFF) as host memory, with
/// W25Q128 datasheet-typical erase and program times on a 348 MHz cycle
/// count, so the swap statistics estimate what the board would stall.
struct ModelFlash {
    data: Vec<u8>,
    cycles: u64,
}

const SWAP_SECTORS: u32 = 511;
/// Sectors kept free for bursts of writes (the board's choice too).
const SWAP_SPARE: u32 = 63;
const MODEL_HZ: u32 = 348_000_000;

impl ModelFlash {
    fn new() -> Self {
        // old contents: every sector needs an erase before its first use
        ModelFlash { data: vec![0; SWAP_SECTORS as usize * PAGE], cycles: 0 }
    }
}

impl Flash for ModelFlash {
    fn sectors(&self) -> u32 {
        SWAP_SECTORS
    }
    fn erase(&mut self, sector: u32) -> bool {
        let o = sector as usize * PAGE;
        self.data[o..o + PAGE].fill(0xFF);
        self.cycles += u64::from(MODEL_HZ) * 45 / 1000;
        true
    }
    fn program(&mut self, off: u32, data: &[u8; CHUNK]) -> bool {
        for (f, &d) in self.data[off as usize..off as usize + CHUNK].iter_mut().zip(data) {
            *f &= d; // NOR: programming only clears bits
        }
        self.cycles += u64::from(MODEL_HZ) * 7 / 10_000;
        true
    }
    fn read(&self, off: u32, buf: &mut [u8]) {
        buf.copy_from_slice(&self.data[off as usize..off as usize + buf.len()]);
    }
    fn cycles(&self) -> u64 {
        self.cycles
    }
    fn hz(&self) -> u32 {
        MODEL_HZ
    }
}

// ------------------------------------------------------------------ bus

struct HostBus {
    plat: Platform<TermConsole, MemDisk, StubNet, FlashSwap<ModelFlash>>,
    interactive: bool,
}

impl HostBus {
    fn poll(&mut self) {
        self.plat.con.read_input();
        self.plat.update_irqs();
    }
}

impl Bus for HostBus {
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

    /// Guest time is instruction-driven, so idle time skips to the next timer
    /// deadline. Interactive mode first waits (bounded, real time) for input.
    fn wfi(&mut self, cpu: &mut Cpu) {
        self.poll();
        if cpu.sip_value(self) & cpu.sie != 0 {
            return;
        }
        // as the board does: erase a dirty swap sector while the guest idles
        if let Some(w) = &mut self.plat.swap {
            w.disk.idle_erase();
        }
        let now = cpu.time();
        if cpu.stimecmp <= now {
            return;
        }
        if self.interactive && self.plat.con.stdin_open {
            let ms = ((cpu.stimecmp - now) / (TIMEBASE_HZ as u64 / 1000)).min(100) as i32;
            self.plat.con.flush();
            if TermConsole::stdin_readable(ms) {
                self.poll();
                if cpu.sip_value(self) & cpu.sie != 0 {
                    return;
                }
            }
            if cpu.stimecmp == u64::MAX {
                return; // no timer armed: keep waiting for input
            }
        } else if cpu.stimecmp == u64::MAX {
            eprintln!("\n[rvemu] guest idle with no timer and no input");
            cpu.halt = Some(Halt::Idle);
            return;
        }
        let skip = cpu.stimecmp - now;
        cpu.time_skip += skip;
        cpu.stats.idle_ticks += skip;
    }
}

// ------------------------------------------------------------------ setup

fn make_dts(ram_base: u32, ram_size: u32, bootargs: &str, initrd: Option<(u32, u32)>, disk: bool, net: bool, swap: bool) -> String {
    let initrd = match initrd {
        Some((s, e)) => format!("    linux,initrd-start = <0x{s:x}>;\n    linux,initrd-end = <0x{e:x}>;\n"),
        None => String::new(),
    };
    let node = |base: u32, irq: u32| {
        format!(
            r#"    virtio@{base:x} {{
      compatible = "virtio,mmio";
      reg = <0x{base:x} 0x{VIRTIO_SIZE:x}>;
      interrupts = <{irq}>;
      interrupt-parent = <&plic0>;
    }};
"#
        )
    };
    let mut virtio = String::new();
    if disk {
        virtio += &node(VIRTIO_BASE, VIRTIO_IRQ);
    }
    if net {
        virtio += &node(VIRTIO_NET_BASE, VIRTIO_NET_IRQ);
    }
    if swap {
        virtio += &node(VIRTIO_SWAP_BASE, VIRTIO_SWAP_IRQ);
    }
    format!(
        r#"/dts-v1/;
/ {{
  #address-cells = <1>;
  #size-cells = <1>;
  model = "rvemu";
  compatible = "rvemu";
  chosen {{
    bootargs = "{bootargs}";
    stdout-path = "/soc/serial@f4000000";
{initrd}  }};
  cpus {{
    #address-cells = <1>;
    #size-cells = <0>;
    timebase-frequency = <{TIMEBASE_HZ}>;
    cpu0: cpu@0 {{
      device_type = "cpu";
      compatible = "riscv";
      reg = <0>;
      status = "okay";
      riscv,isa = "rv32ima";
      mmu-type = "riscv,sv32";
      cpu0_intc: interrupt-controller {{
        #interrupt-cells = <1>;
        #address-cells = <0>;
        interrupt-controller;
        compatible = "riscv,cpu-intc";
      }};
    }};
  }};
  memory@{ram_base:x} {{
    device_type = "memory";
    reg = <0x{ram_base:x} 0x{ram_size:x}>;
  }};
  soc {{
    #address-cells = <1>;
    #size-cells = <1>;
    compatible = "simple-bus";
    ranges;
    plic0: interrupt-controller@f0000000 {{
      compatible = "sifive,plic-1.0.0";
      reg = <0x{PLIC_BASE:x} 0x{PLIC_SIZE:x}>;
      #interrupt-cells = <1>;
      #address-cells = <0>;
      interrupt-controller;
      interrupts-extended = <&cpu0_intc 9>;
      riscv,ndev = <31>;
    }};
    serial@f4000000 {{
      compatible = "ns16550";
      reg = <0x{UART_BASE:x} 0x{UART_SIZE:x}>;
      interrupts = <{UART_IRQ}>;
      interrupt-parent = <&plic0>;
      clock-frequency = <5000000>;
      no-loopback-test;
    }};
{virtio}  }};
}};
"#
    )
}

fn compile_dts(dts: &str) -> Result<Vec<u8>, String> {
    let tmp = std::env::temp_dir().join(format!("rvemu-{}", std::process::id()));
    let (src, out) = (tmp.with_extension("dts"), tmp.with_extension("dtb"));
    std::fs::write(&src, dts).map_err(|e| e.to_string())?;
    let status = Command::new("dtc").args(["-q", "-I", "dts", "-O", "dtb", "-o"]).arg(&out).arg(&src).status();
    let _ = std::fs::remove_file(&src);
    match status {
        Ok(s) if s.success() => {}
        _ => return Err("dtc failed (is dtc installed?)".into()),
    }
    let blob = std::fs::read(&out).map_err(|e| e.to_string())?;
    let _ = std::fs::remove_file(&out);
    Ok(blob)
}

// ------------------------------------------------------------------ report

/// Timing model for RP2350 Hazard3 at 300 MHz. Estimates, not measurements.
mod model {
    pub const HOST_HZ: f64 = 300e6;
    pub const PSRAM_MISS_CYCLES: f64 = 100.0;
    pub const INTERP_CYCLES_PER_INSN: f64 = 55.0;
    pub const JIT_ALU: f64 = 1.4;
    pub const JIT_MEM: f64 = 14.0;
    pub const JIT_BRANCH: f64 = 4.0;
    pub const JIT_JAL: f64 = 4.0;
    pub const JIT_JALR: f64 = 20.0;
    pub const JIT_AMO: f64 = 40.0;
    pub const JIT_SYSTEM: f64 = 150.0;
    pub const JIT_TRAP: f64 = 200.0;
    pub const JIT_TRANSLATE_PER_INSN: f64 = 150.0;
    pub const JIT_RETRANSLATE_FACTOR: f64 = 2.0;
}

fn report(c: &Cpu, host_s: f64) {
    let s = &c.stats;
    let op = &s.op;
    let n = c.instret;
    let alu = op[0x13] + op[0x33] + op[0x37] + op[0x17];
    let (ld, st, br, jal, jalr, amo, sys) = (op[0x03], op[0x23], op[0x63], op[0x6F], op[0x67], op[0x2F], op[0x73] + op[0x0F]);
    let pct = |v: u64| if n > 0 { 100.0 * v as f64 / n as f64 } else { 0.0 };

    eprintln!("\n==== rvemu stats ====");
    eprintln!("host time          {:.2} s  ({:.1} MIPS)", host_s, if host_s > 0.0 { n as f64 / host_s / 1e6 } else { 0.0 });
    eprintln!("guest instructions {n}");
    if s.user_instret != 0 {
        eprintln!("  to first U-mode  {}  (unique insns {})", s.user_instret, s.user_unique_insns);
    }
    if s.until_instret != 0 {
        eprintln!("  to --until match {}  (unique insns {})", s.until_instret, s.until_unique_insns);
    }
    eprintln!(
        "unique insns run   {} ({:.0} KiB of code, {} pages)",
        s.unique_insns,
        s.unique_insns as f64 * 4.0 / 1024.0,
        s.unique_code_pages
    );
    eprintln!(
        "mix  alu {:.1}%  load {:.1}%  store {:.1}%  branch {:.1}%  jal {:.1}%  jalr {:.1}%  amo {:.2}%  system {:.2}%",
        pct(alu), pct(ld), pct(st), pct(br), pct(jal), pct(jalr), pct(amo), pct(sys)
    );
    eprintln!(
        "tlb misses  R {}  W {}  X {}   flushes {}  page flushes {}",
        s.tlb_miss[0], s.tlb_miss[1], s.tlb_miss[2], s.tlb_flushes, s.tlb_page_flushes
    );
    eprintln!(
        "traps  exceptions {}  interrupts {}  sbi {}  wfi {}  idle ticks {} ({:.2} s guest)",
        s.exceptions,
        s.interrupts,
        s.sbi_calls,
        s.wfi,
        s.idle_ticks,
        s.idle_ticks as f64 / TIMEBASE_HZ as f64
    );

    if std::env::var_os("RVEMU_REGS").is_some() {
        let total: u64 = s.reg_use[1..].iter().sum();
        let mut v: Vec<(usize, u64)> = s.reg_use.iter().copied().enumerate().skip(1).collect();
        v.sort_by_key(|&(_, n)| n);
        eprint!("register uses (least first):");
        for (r, n) in v {
            eprint!(" x{r}:{:.2}%", 100.0 * n as f64 / total as f64);
        }
        eprintln!();
    }
    let Some(m) = &c.inst.model else { return };
    use model::*;
    let interp = n as f64 * INTERP_CYCLES_PER_INSN + m.interp.misses as f64 * PSRAM_MISS_CYCLES;
    let jit = alu as f64 * JIT_ALU
        + (ld + st) as f64 * JIT_MEM
        + br as f64 * JIT_BRANCH
        + jal as f64 * JIT_JAL
        + jalr as f64 * JIT_JALR
        + amo as f64 * JIT_AMO
        + sys as f64 * JIT_SYSTEM
        + (s.exceptions + s.interrupts) as f64 * JIT_TRAP
        + s.unique_insns as f64 * JIT_TRANSLATE_PER_INSN * JIT_RETRANSLATE_FACTOR
        + m.jit.misses as f64 * PSRAM_MISS_CYCLES;
    let rate = |x: &rvcore::instrument::XipCache| {
        if x.accesses > 0 { 100.0 * x.misses as f64 / x.accesses as f64 } else { 0.0 }
    };
    eprintln!("\n---- RP2350 @300 MHz timing model (estimate) ----");
    eprintln!("XIP cache, interpreter (fetch+data): {} misses / {} ({:.2}%)", m.interp.misses, m.interp.accesses, rate(&m.interp));
    eprintln!("XIP cache, translator (data only):   {} misses / {} ({:.2}%)", m.jit.misses, m.jit.accesses, rate(&m.jit));
    eprintln!("interpreter  {:.1} cycles/insn  -> {:.1} s", interp / n.max(1) as f64, interp / HOST_HZ);
    eprintln!("translator   {:.1} cycles/insn  -> {:.1} s", jit / n.max(1) as f64, jit / HOST_HZ);
    eprintln!("(guest time excludes {:.2} s of idle WFI)", s.idle_ticks as f64 / TIMEBASE_HZ as f64);
}

// ------------------------------------------------------------------ main

const USAGE: &str = "usage: rvemu -k Image [-i initrd] [options]
  -k FILE        kernel Image (loaded at start of RAM)
  -i FILE        initramfs (cpio)
  -d FILE        disk image for the virtio block device (read-only); without
                 -i the default command line roots on it (squashfs, see
                 guest/post-build-disk.sh)
  -N             virtio network device on a stub LAN: gateway 10.0.2.2
                 answers ARP and ping (the guest is 52:54:00:12:34:56)
  -W             swap device (/dev/vdb in the disk guest): the board's flash
                 swap on a modelled 2 MB flash, statistics in its last page
  -b FILE        use this DTB instead of generating one
  -m MB          guest RAM size (default 64)
  -X BYTES       the first BYTES of the kernel (its text) are extra, read-only
                 guest RAM, as the RP2350 maps them from flash: RAM = BYTES + MB
  -a ARGS        kernel command line (default \"earlycon console=ttyS0\")
  -u STRING      stop when the console prints STRING (e.g. \"login:\")
  -n COUNT       stop after COUNT guest instructions
  -M             enable the RP2350 timing model (XIP cache sim)
  -B             batch mode: no keyboard input, idle time skipped
  -S FILE        simulate the translator cache policies in FILE (see sim.rs);
                 results to -J FILE (default sim.json), block corpus to -C FILE
  -D             print the generated device tree and exit
Ctrl-A x exits.";

struct Opts {
    kernel: Option<String>,
    initrd: Option<String>,
    dtb: Option<String>,
    ram_mb: u32,
    /// -X: kernel text bytes kept outside the -m budget (the RP2350 reads
    /// them from flash): guest RAM is text + ram_mb, the text read-only.
    text: u32,
    bootargs: String,
    until: Option<String>,
    max_insns: u64,
    model: bool,
    batch: bool,
    dump_dts: bool,
    sim: Option<String>,
    sim_out: String,
    corpus: Option<String>,
    disk: Option<String>,
    bootargs_set: bool,
    net: bool,
    swap: bool,
}

fn parse_args() -> Result<Opts, String> {
    let mut o = Opts {
        kernel: None,
        initrd: None,
        dtb: None,
        ram_mb: 64,
        text: 0,
        bootargs: "earlycon console=ttyS0".into(),
        until: None,
        max_insns: 0,
        model: false,
        batch: false,
        dump_dts: false,
        sim: None,
        sim_out: "sim.json".into(),
        corpus: None,
        disk: None,
        bootargs_set: false,
        net: false,
        swap: false,
    };
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let mut val = || args.next().ok_or_else(|| format!("{a} needs a value"));
        match a.as_str() {
            "-k" => o.kernel = Some(val()?),
            "-i" => o.initrd = Some(val()?),
            "-b" => o.dtb = Some(val()?),
            "-m" => o.ram_mb = val()?.parse().map_err(|_| "bad -m")?,
            "-X" => {
                let v = val()?;
                let n = match v.strip_prefix("0x") {
                    Some(h) => u32::from_str_radix(h, 16),
                    None => v.parse(),
                };
                o.text = n.map_err(|_| "bad -X")?;
                if o.text & 0xFFF != 0 || o.text > 16 << 20 {
                    return Err("-X must be a page multiple, at most 16 MiB".into());
                }
            }
            "-a" => {
                o.bootargs = val()?;
                o.bootargs_set = true;
            }
            "-d" => o.disk = Some(val()?),
            "-N" => o.net = true,
            "-W" => o.swap = true,
            "-u" => o.until = Some(val()?),
            "-n" => o.max_insns = val()?.parse().map_err(|_| "bad -n")?,
            "-M" => o.model = true,
            "-B" => o.batch = true,
            "-D" => o.dump_dts = true,
            "-S" => o.sim = Some(val()?),
            "-J" => o.sim_out = val()?,
            "-C" => o.corpus = Some(val()?),
            _ => return Err(USAGE.into()),
        }
    }
    if o.kernel.is_none() && !o.dump_dts {
        return Err(USAGE.into());
    }
    if !(4..=1024).contains(&o.ram_mb) {
        return Err("RAM size must be 4..1024 MB".into());
    }
    if o.disk.is_some() && o.initrd.is_none() && !o.bootargs_set {
        o.bootargs = ROOT_ON_DISK.into();
    }
    Ok(o)
}

/// Kernel command line for the squashfs root on the virtio disk; the
/// guest's /sbin/overlay-init puts a tmpfs overlay on it.
const ROOT_ON_DISK: &str = "earlycon console=ttyS0 root=/dev/vda ro rootfstype=squashfs init=/sbin/overlay-init";

fn read(path: &str) -> Result<Vec<u8>, String> {
    std::fs::read(path).map_err(|e| format!("cannot read {path}: {e}"))
}

fn run() -> Result<(), String> {
    let o = parse_args()?;
    let ram_base = 0u32;
    let ram_size = (o.ram_mb << 20) + o.text;
    let dtb_addr = ram_size - DTB_RESERVE;

    let initrd = o.initrd.as_deref().map(read).transpose()?;
    let initrd_addr = match &initrd {
        Some(d) => {
            if d.len() as u64 + DTB_RESERVE as u64 > ram_size as u64 / 2 {
                return Err("initrd too large for RAM size".into());
            }
            (dtb_addr - d.len() as u32) & !0xFFF
        }
        None => 0,
    };
    let dts = make_dts(
        ram_base,
        ram_size,
        &o.bootargs,
        initrd.as_ref().map(|d| (ram_base + initrd_addr, ram_base + initrd_addr + d.len() as u32)),
        o.disk.is_some(),
        o.net,
        o.swap,
    );
    if o.dump_dts {
        print!("{dts}");
        return Ok(());
    }

    let mut ram = vec![0u8; ram_size as usize].into_boxed_slice();
    let kimg = read(o.kernel.as_deref().unwrap())?;
    let limit = if initrd.is_some() { initrd_addr } else { dtb_addr };
    if kimg.len() > limit as usize {
        return Err("kernel does not fit in RAM".into());
    }
    ram[..kimg.len()].copy_from_slice(&kimg);
    if let Some(d) = &initrd {
        ram[initrd_addr as usize..initrd_addr as usize + d.len()].copy_from_slice(d);
    }
    let dtb = match &o.dtb {
        Some(p) => read(p)?,
        None => compile_dts(&dts)?,
    };
    if dtb.len() > DTB_RESERVE as usize {
        return Err("DTB too large".into());
    }
    ram[dtb_addr as usize..dtb_addr as usize + dtb.len()].copy_from_slice(&dtb);

    // SAFETY: `ram` outlives `cpu` and is only accessed through it from here on.
    let mut cpu = Box::new(unsafe { Cpu::new(ram.as_mut_ptr(), ram_base, ram_size, ram_base, ram_base + dtb_addr) });
    if o.model || o.sim.is_some() {
        cpu.inst.enable_model();
    }
    let mut sim = match &o.sim {
        Some(spec) => Some(sim::Sim::new(&mut cpu, spec, &o.sim_out, o.corpus.as_deref())?),
        None => None,
    };
    let disk_img = o.disk.as_deref().map(read).transpose()?.map(Vec::into_boxed_slice);
    let con = TermConsole::new(!o.batch, o.until.as_deref());
    cpu.set_readonly_prefix(o.text);
    let gmem = GuestMem { ptr: ram.as_mut_ptr(), base: ram_base, size: ram_size, ro: o.text };
    let mut plat = Platform::new(con);
    if let Some(img) = &disk_img {
        // SAFETY: `disk_img` outlives the bus.
        plat = plat.with_disk(unsafe { MemDisk::new(img.as_ptr(), img.len()) }, gmem);
    }
    if o.net {
        plat = plat.with_net(StubNet::default(), gmem);
    }
    if o.swap {
        plat = plat.with_swap(FlashSwap::new(ModelFlash::new(), SWAP_SPARE), gmem);
    }
    let mut bus = HostBus { plat, interactive: !o.batch };

    eprintln!(
        "[rvemu] RAM {} MiB, kernel {} bytes, initrd {} bytes @0x{:x}, dtb @0x{:x}",
        o.ram_mb,
        kimg.len(),
        initrd.as_ref().map_or(0, |d| d.len()),
        ram_base + initrd_addr,
        ram_base + dtb_addr
    );
    drop(initrd);

    let t0 = Instant::now();
    while cpu.halt.is_none() {
        for _ in 0..4096 {
            let irq = cpu.check_irq(&bus);
            if let Some(s) = &mut sim {
                s.pre(&mut cpu, irq);
            }
            cpu.step(&mut bus);
            if let Some(s) = &mut sim {
                s.post(&mut cpu);
            }
            if bus.plat.con.until_hit && cpu.stats.until_instret == 0 {
                cpu.stats.until_instret = cpu.instret;
                cpu.stats.until_unique_insns = cpu.stats.unique_insns;
                cpu.halt = Some(Halt::Until);
            }
            if cpu.halt.is_some() {
                break;
            }
        }
        bus.poll();
        if bus.plat.con.user_quit {
            cpu.halt = Some(Halt::User);
        }
        // a panicked guest spins forever; batch runs stop
        if o.batch && bus.plat.con.panicked && cpu.halt.is_none() {
            cpu.halt = Some(Halt::Panic);
        }
        if o.max_insns != 0 && cpu.instret >= o.max_insns && cpu.halt.is_none() {
            cpu.halt = Some(Halt::MaxInsns);
        }
    }
    let host_s = t0.elapsed().as_secs_f64();
    bus.plat.con.restore();

    let why = match cpu.halt {
        Some(Halt::Shutdown) => "guest shutdown",
        Some(Halt::Until) => "--until matched",
        Some(Halt::MaxInsns) => "instruction limit",
        Some(Halt::Idle) => "guest idle",
        Some(Halt::User) => "user exit",
        Some(Halt::Panic) => "guest kernel panic",
        None => "",
    };
    eprintln!(
        "\n[rvemu] stopped: {why} at pc=0x{:08x} priv={}",
        cpu.pc,
        if cpu.privilege == Priv::S { 'S' } else { 'U' }
    );
    report(&cpu, host_s);
    if let Some(s) = &mut sim {
        s.finish(&cpu)?;
    }
    drop(cpu);
    drop(ram);
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("rvemu: {e}");
            ExitCode::from(2)
        }
    }
}
