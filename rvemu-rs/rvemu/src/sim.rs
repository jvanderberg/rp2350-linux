//! Translator cache-policy simulator (`rvemu -S POLICIES`).
//!
//! The interpreter runs the guest once. Each policy follows the same
//! instruction stream and mirrors what rvjit's dispatcher (`run_one`) would do
//! with its own code cache: lookups, translations, flushes, chain slots, the
//! jump cache, the budget prologue and every exit. Block shapes and sizes come
//! from rvjit itself (`Jit::block_code`), so a policy with the board's
//! parameters reproduces the real translator's event counts. The counts go to
//! a JSON file; scripts/sim.py turns them into board time.
//!
//! Policy spec, one per line: `name key=value ...`
//!   kb=192         SRAM code buffer in KiB (includes the prelude, like CODE_WORDS)
//!   blocks=4096    block descriptors
//!   evict=flush    flush (flush-all when full, as rvjit does) | fifo (ring buffer)
//!   hot=1          translate a block on its Nth dispatch; before that the
//!                  dispatcher interprets it (1 = translate at once)
//!   store_kb=0     PSRAM backing store for translations (0 = none)
//!   xip=0          1: run store blocks in place through the XIP cache instead
//!                  of copying them into SRAM
//!   promote=0      with xip=1: copy a block into SRAM after N runs (0 = never)
//!   aot=0          1: kernel blocks come prebuilt from flash, 2: all blocks
//!   store_ring=0   1: a full store overwrites its oldest entries instead of
//!                  starting over
//!   jc=1024        jump cache entries
//!   wscale=100     translated size in percent of rvjit's (a denser encoding)
//!   regions=0      with evict=fifo: evict whole regions (the buffer split in
//!                  N), 0 = block by block
//!   pf=none        a second core (core1) translating ahead, with a time
//!                  budget of pf_avail% of core0's modelled cycles:
//!                  ghost   evicted blocks noted by page; a translation miss
//!                          has core1 translate the page's other ghosts into
//!                          a side store of pf_store_kb, which core0 copies
//!                          from (the reverted plan C, prefetch.rs)
//!                  direct  the same, but core1 puts them in the code buffer
//!                  oracle  upper bound: a block evicted before costs core0
//!                          nothing to bring back while core1 has the time
//!                  async   a miss queues the block for core1 and core0
//!                          interprets it (cold steps) until core1 has put
//!                          it in the code buffer; with more than 1 ms
//!                          queued, core0 translates it itself
//!                  succ    core0 translates a miss itself, and sends its
//!                          same-page successors (fall-through, branch and
//!                          jal targets) to core1, which puts them in the
//!                          code buffer and follows theirs to pf_depth; a
//!                          miss on the block core1 is on waits for it if
//!                          that is quicker than translating it
//!   pf_depth=1     with pf=succ: pf_lifo=1 newest request first,
//!                  pf_ret=1 also the return point after a call,
//!                  pf_xpage=1 targets on other pages too, pf_async=1 a
//!                  miss interprets while core1 translates (as pf=async)
//!   pf_store_kb=16 pf_avail=90

use std::collections::{HashMap, HashSet, VecDeque};
use std::fs::File;
use std::io::{BufWriter, Write};

use rvcore::instrument::XipCache;
use rvcore::{ACC_R, ACC_W, ACC_X, Cpu, Priv};
use rvjit::{BlockEnd, JcEntry, Jit, JitMem, PreludeLayout};

const BUDGET_CAP: i64 = 1 << 20;
/// Where code fetched through XIP sits in the cache model: away from guest
/// RAM's tags, sharing its sets.
const XIP_CODE_BASE: u32 = 0x0100_0000;
const KERNEL_VA: u32 = 0xC000_0000;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Evict {
    Flush,
    Fifo,
}

#[derive(Clone, Debug)]
struct Cfg {
    name: String,
    words: usize,
    max_blocks: usize,
    evict: Evict,
    hot: u32,
    store_words: usize,
    xip: bool,
    promote: u32,
    aot: u32,
    jc: usize,
    wscale: u32,
    regions: usize,
    store_ring: bool,
    /// 0 none, 1 ghost, 2 direct, 3 oracle
    pf: u8,
    pf_store_words: usize,
    pf_avail: u32,
    pf_depth: u32,
    pf_lifo: bool,
    pf_ret: bool,
    pf_xpage: bool,
    pf_async: bool,
}

fn parse_cfg(line: &str) -> Result<Cfg, String> {
    let mut it = line.split_whitespace();
    let name = it.next().ok_or("empty policy")?.to_string();
    let mut c = Cfg {
        name,
        words: 192 * 256,
        max_blocks: 4096,
        evict: Evict::Flush,
        hot: 1,
        store_words: 0,
        xip: false,
        promote: 0,
        aot: 0,
        jc: 1024,
        wscale: 100,
        regions: 0,
        store_ring: false,
        pf: 0,
        pf_store_words: 16 * 256,
        pf_avail: 90,
        pf_depth: 1,
        pf_lifo: false,
        pf_ret: false,
        pf_xpage: false,
        pf_async: false,
    };
    for kv in it {
        let (k, v) = kv.split_once('=').ok_or_else(|| format!("bad policy field {kv}"))?;
        let num = || v.parse::<usize>().map_err(|_| format!("bad number in {kv}"));
        match k {
            "kb" => c.words = num()? * 256,
            "blocks" => c.max_blocks = num()?,
            "evict" => {
                c.evict = match v {
                    "flush" => Evict::Flush,
                    "fifo" => Evict::Fifo,
                    _ => return Err(format!("bad evict {v}")),
                }
            }
            "hot" => c.hot = num()?.max(1) as u32,
            "store_kb" => c.store_words = num()? * 256,
            "xip" => c.xip = num()? != 0,
            "promote" => c.promote = num()? as u32,
            "aot" => c.aot = num()? as u32,
            "jc" => c.jc = num()?,
            "wscale" => c.wscale = num()?.clamp(10, 100) as u32,
            "regions" => c.regions = num()?,
            "store_ring" => c.store_ring = num()? != 0,
            "pf" => {
                c.pf = match v {
                    "none" => 0,
                    "ghost" => 1,
                    "direct" => 2,
                    "oracle" => 3,
                    "async" => 4,
                    "succ" => 5,
                    _ => return Err(format!("bad pf {v}")),
                }
            }
            "pf_store_kb" => c.pf_store_words = num()? * 256,
            "pf_avail" => c.pf_avail = num()? as u32,
            "pf_depth" => c.pf_depth = num()? as u32,
            "pf_lifo" => c.pf_lifo = num()? != 0,
            "pf_ret" => c.pf_ret = num()? != 0,
            "pf_xpage" => c.pf_xpage = num()? != 0,
            "pf_async" => c.pf_async = num()? != 0,
            _ => return Err(format!("unknown policy field {k}")),
        }
    }
    if c.xip && c.store_words == 0 {
        return Err(format!("{}: xip=1 needs store_kb", c.name));
    }
    if !c.jc.is_power_of_two() {
        return Err(format!("{}: jc must be a power of two", c.name));
    }
    Ok(c)
}

#[derive(Clone, Copy)]
struct Info {
    n: u8,
    words: u16,
    end: BlockEnd,
}

fn key(va: u32, pa: u32) -> u64 {
    (va as u64) << 32 | pa as u64
}
fn key_va(k: u64) -> u32 {
    (k >> 32) as u32
}
fn key_pa(k: u64) -> u32 {
    k as u32
}

fn imm_j(insn: u32) -> i32 {
    (((insn as i32) >> 31) << 20)
        | (insn & 0xF_F000) as i32
        | ((insn >> 9) & 0x800) as i32
        | ((insn >> 20) & 0x7FE) as i32
}

/// Block shapes from the real translator (measure mode), cached per
/// (va, pa) and dropped when the guest stores to the page.
struct Infos {
    jit: Jit<'static>,
    layout: PreludeLayout,
    cache: HashMap<u64, Info>,
    page_has: Vec<bool>,
    page_keys: HashMap<u32, Vec<u64>>,
    ram_base: u32,
    corpus: Option<BufWriter<File>>,
    corpus_seen: HashSet<(u64, u64)>,
    corpus_blocks: u64,
}

impl Infos {
    fn get(&mut self, cpu: &mut Cpu, va: u32, pa: u32) -> Info {
        let k = key(va, pa);
        if let Some(i) = self.cache.get(&k) {
            return *i;
        }
        let (bi, code, pos) = self.jit.block_code(cpu, va, pa);
        let info = Info { n: bi.n as u8, words: bi.words as u16, end: bi.end };
        if let Some(w) = &mut self.corpus
            && bi.n > 0
        {
            // position-independent form: jal to a shared routine names the
            // routine's word index instead of a pc-relative offset
            let canon: Vec<u32> = code
                .iter()
                .enumerate()
                .map(|(i, &insn)| {
                    if insn & 0x7F == 0x6F {
                        let tgt = (pos + i) as isize + (imm_j(insn) / 4) as isize;
                        if tgt >= 0 && (tgt as usize) < self.layout.code_start {
                            return (insn & 0xFFF) | ((tgt as u32) << 12);
                        }
                    }
                    insn
                })
                .collect();
            let mut h: u64 = 0xcbf2_9ce4_8422_2325;
            for &x in &canon {
                h = (h ^ x as u64).wrapping_mul(0x100_0000_01b3);
            }
            if self.corpus_seen.insert((k, h)) {
                self.corpus_blocks += 1;
                let user = (cpu.privilege == Priv::U) as u32;
                let mut buf = Vec::with_capacity(16 + 4 * canon.len());
                for x in [canon.len() as u32, va, pa, user | (bi.n << 8)] {
                    buf.extend_from_slice(&x.to_le_bytes());
                }
                for x in &canon {
                    buf.extend_from_slice(&x.to_le_bytes());
                }
                let _ = w.write_all(&buf);
            }
        }
        self.cache.insert(k, info);
        let p = (pa - self.ram_base) >> 12;
        self.page_has[p as usize] = true;
        self.page_keys.entry(p).or_default().push(k);
        info
    }

    fn store_to(&mut self, page: u32) {
        if self.page_has[page as usize] {
            self.page_has[page as usize] = false;
            if let Some(v) = self.page_keys.remove(&page) {
                for k in v {
                    self.cache.remove(&k);
                }
            }
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Tier {
    Sram,
    Xip,
}

#[derive(Clone, Copy)]
struct Blk {
    id: u32,
    info: Info,
    tier: Tier,
    /// chained target (key, id); id 0 = not patched
    slot: [(u64, u32); 2],
    execs: u32,
    addr: u32,
    /// allocation sequence of the region holding it (evict=fifo regions)
    seq: u32,
}

#[derive(Clone, Copy)]
enum St {
    Disp { force: bool },
    In { key: u64, k: u8, n: u8, budget: i64 },
    Cold { left: u8 },
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Mode {
    Interp,
    Jit,
    Cold,
}

/// Event counts; `[S, U]` pairs split by privilege.
#[derive(Default)]
struct Counts {
    runs: u64,
    lookups: u64,
    interp: [u64; 2],
    cold_steps: [u64; 2],
    cold_blocks: u64,
    jit_insns: [u64; 2],
    entries: u64,
    block_execs: u64,
    translate_blocks: [u64; 2],
    translate_insns: [u64; 2],
    translate_words: [u64; 2],
    nocode_blocks: u64,
    restore_blocks: u64,
    restore_words: u64,
    aot_blocks: u64,
    aot_words: u64,
    store_write_words: u64,
    store_flushes: u64,
    exit_jump: u64,
    exit_interp: u64,
    exit_mem: u64,
    exit_budget: u64,
    exit_irq: u64,
    chain_attempts: u64,
    chains: u64,
    chained_jumps: u64,
    jc_hits: u64,
    jc_misses: u64,
    ijmp_tlb_misses: u64,
    flushes: u64,
    evicted: u64,
    unlinks: u64,
    smc: u64,
    xip_execs: u64,
    xip_words: u64,
    promotes: u64,
    promote_words: u64,
    desync: u64,
    max_sram_blocks: u64,
    /// chains from a newer region into an older one (need an unlink record)
    chain_recs: u64,
    max_region_recs: u64,
    /// blocks core0 got from core1 (pf), their words and guest insns
    pf_blocks: u64,
    pf_words: u64,
    pf_insns: u64,
    /// core1's translations, their guest insns, and ones already resident
    pf_jobs: u64,
    pf_job_insns: u64,
    pf_skipped: u64,
    /// requests dropped from a full queue
    pf_dropped: u64,
    /// core0's cycles waiting for core1 (succ)
    pf_wait_cyc: u64,
    pf_waits: u64,
}

macro_rules! json_counts {
    ($c:expr, $out:expr, [$($f:ident),*], [$($p:ident),*]) => {{
        $( $out.push_str(&format!("\"{}\": {}, ", stringify!($f), $c.$f)); )*
        $( $out.push_str(&format!("\"{}\": [{}, {}], ", stringify!($p), $c.$p[0], $c.$p[1])); )*
    }};
}

struct Policy {
    cfg: Cfg,
    c: Counts,
    sram: HashMap<u64, Blk>,
    xipb: HashMap<u64, Blk>,
    store: HashMap<u64, u16>,
    /// store_ring: entries in log order (key, words)
    store_fifo: VecDeque<(u64, u16)>,
    store_used: usize,
    pos: usize,
    nblocks: usize,
    fifo: VecDeque<(u64, u32, u32)>, // key, id, start
    store_pos: usize,
    hot: HashMap<u64, u32>,
    jc: Vec<(u64, u32)>,
    sram_pages: Vec<bool>,
    store_pages: Vec<bool>,
    incoming: HashMap<u32, Vec<(u64, u32, u8)>>,
    st: St,
    mode: Mode,
    priv_u: usize,
    next_id: u32,
    xip_idx: Option<usize>,
    code_start: usize,
    /// evict=fifo regions: current region, its sequence number, and live
    /// unlink records per region
    region: usize,
    region_seq: Vec<u32>,
    region_recs: Vec<u64>,
    seq_next: u32,
    // pf: ghost ring (key) and per-page lists, core1's queue, side store,
    // blocks ever evicted (oracle), and core1's clock in cycles
    ghost_ring: VecDeque<u64>,
    ghosts: HashMap<u32, Vec<u64>>,
    pfq: VecDeque<u64>,
    pf_store: HashMap<u64, u16>,
    pf_fifo: VecDeque<(u64, u16)>,
    pf_used: usize,
    evicted_ever: HashSet<u64>,
    core1_cyc: f64,
    /// pf=async: blocks queued on core1, in finishing order (key, done at)
    pending: VecDeque<(u64, f64)>,
    pending_set: HashSet<u64>,
    /// pf=succ: core1's queue (key, depth) and what is in it
    sq: VecDeque<(u64, u32, f64)>,
    sq_set: HashSet<u64>,
    scur: Option<(u64, u32, f64)>,
}

/// pf: ghost entries kept (prefetch.rs GN), and core1's queue length.
const PF_GHOSTS: usize = 2048;
const PF_QUEUE: usize = 512;
/// Board cycles, as scripts/sim.py prices them (CPI 1.12; the translator
/// also misses 0.61 times per guest insn at 137 cycles).
const CPI: f64 = 1.12;
const TR_CYC: f64 = CPI * 358.0 + 0.61 * 137.0;

struct Flags {
    r_miss: bool,
    w_miss: bool,
    misaligned: bool,
    exception: bool,
    store_page: Option<u32>,
}

impl Policy {
    fn new(cfg: Cfg, pages: usize, code_start: usize, xip_idx: Option<usize>) -> Self {
        let jc = vec![(0, 0); cfg.jc];
        Policy {
            cfg,
            c: Counts::default(),
            sram: HashMap::new(),
            xipb: HashMap::new(),
            store: HashMap::new(),
            store_fifo: VecDeque::new(),
            store_used: 0,
            pos: code_start,
            nblocks: 0,
            fifo: VecDeque::new(),
            store_pos: 0,
            hot: HashMap::new(),
            jc,
            sram_pages: vec![false; pages],
            store_pages: vec![false; pages],
            incoming: HashMap::new(),
            st: St::Disp { force: false },
            mode: Mode::Interp,
            priv_u: 0,
            next_id: 1,
            xip_idx,
            code_start,
            region: 0,
            region_seq: Vec::new(),
            region_recs: Vec::new(),
            seq_next: 1,
            ghost_ring: VecDeque::new(),
            ghosts: HashMap::new(),
            pfq: VecDeque::new(),
            pf_store: HashMap::new(),
            pf_fifo: VecDeque::new(),
            pf_used: 0,
            evicted_ever: HashSet::new(),
            core1_cyc: 0.0,
            pending: VecDeque::new(),
            pending_set: HashSet::new(),
            sq: VecDeque::new(),
            sq_set: HashSet::new(),
            scur: None,
        }
    }

    /// core0's modelled board cycles so far (scripts/sim.py's base terms,
    /// without the XIP misses of translated code).
    fn core0_cyc(&self) -> f64 {
        let c = &self.c;
        let s2 = |a: [u64; 2]| (a[0] + a[1]) as f64;
        CPI * (7.54 * s2(c.jit_insns)
            + 101.1 * c.entries as f64
            + 157.6 * c.runs as f64
            + 148.6 * c.chain_attempts as f64
            + 333.0 * s2(c.interp)
            + 176.0 * s2(c.cold_steps)
            + 40.0 * c.cold_blocks as f64
            + 12000.0 * c.flushes as f64
            + 80.0 * c.evicted as f64
            + 30.0 * c.unlinks as f64)
            + TR_CYC * s2(c.translate_insns)
            + c.pf_wait_cyc as f64
            + Self::pf_copy_cyc(self.cfg.pf, c.pf_blocks, c.pf_words)
    }

    /// What core0 pays to take blocks from core1: a copy out of the side
    /// store and a relink (ghost), or a hand-over (direct).
    fn pf_copy_cyc(pf: u8, blocks: u64, words: u64) -> f64 {
        match pf {
            1 => CPI * (150.0 * blocks as f64 + 2.5 * words as f64),
            2 | 4 | 5 => CPI * 100.0 * blocks as f64,
            _ => 0.0,
        }
    }

    /// Read-only Sv32 walk for an executable page (no A/D updates, no TLB).
    fn peek_pa(cpu: &Cpu, va: u32) -> Option<u32> {
        if cpu.satp >> 31 == 0 {
            return Some(va);
        }
        let rd = |pa: u32| -> Option<u32> {
            let off = pa.wrapping_sub(cpu.ram_base()) as usize;
            if off + 4 > cpu.ram_size() as usize {
                return None;
            }
            // SAFETY: inside guest RAM (checked above)
            Some(unsafe { (cpu.ram_ptr().add(off) as *const u32).read_unaligned() })
        };
        let mut table = (cpu.satp & 0x3F_FFFF) << 12;
        for level in [1u32, 0] {
            let pte = rd(table + ((va >> (12 + 10 * level)) & 0x3FF) * 4)?;
            if pte & 1 == 0 {
                return None;
            }
            if pte & 0xA == 0 {
                table = (pte >> 10) << 12;
                continue;
            }
            if pte & 0x8 == 0 {
                return None;
            }
            let ppn = pte >> 10;
            return Some(if level == 1 { (ppn << 12) | (va & 0x3F_FFFF) } else { (ppn << 12) | (va & 0xFFF) });
        }
        None
    }

    /// Successors of block (va, pa): fall-through, branch and jal targets;
    /// with pf_ret the return point after a call, with pf_xpage on other
    /// pages too.
    fn successors(&self, cpu: &Cpu, va: u32, pa: u32, info: Info) -> Vec<u64> {
        let n = info.n as u32;
        let last = va.wrapping_add(4 * (n.max(1) - 1));
        let insn = || {
            let lpa = (pa & !0xFFF) | (last & 0xFFF);
            let off = lpa.wrapping_sub(cpu.ram_base()) as usize;
            if off + 4 > cpu.ram_size() as usize {
                return 0;
            }
            // SAFETY: inside guest RAM (checked above)
            unsafe { (cpu.ram_ptr().add(off) as *const u32).read_unaligned() }
        };
        let fall = va.wrapping_add(4 * n);
        let mut t = Vec::with_capacity(3);
        match info.end {
            BlockEnd::Branch => {
                let i = insn();
                let imm = (((i as i32) >> 31) << 12)
                    | ((i << 4) & 0x800) as i32
                    | ((i >> 20) & 0x7E0) as i32
                    | ((i >> 7) & 0x1E) as i32;
                t.push(last.wrapping_add(imm as u32));
                t.push(fall);
            }
            BlockEnd::Jal => {
                let i = insn();
                t.push(last.wrapping_add(imm_j(i) as u32));
                let rd = (i >> 7) & 31;
                if self.cfg.pf_ret && (rd == 1 || rd == 5) {
                    t.push(fall);
                }
            }
            BlockEnd::Fall => t.push(fall),
            BlockEnd::Jalr | BlockEnd::Interp => {}
        }
        t.into_iter()
            .filter_map(|x| {
                if x & !0xFFF == va & !0xFFF {
                    Some(key(x, (pa & !0xFFF) | (x & 0xFFF)))
                } else if self.cfg.pf_xpage {
                    Self::peek_pa(cpu, x).map(|p| key(x, p))
                } else {
                    None
                }
            })
            .collect()
    }

    fn now1(&self) -> f64 {
        self.core0_cyc() * self.cfg.pf_avail as f64 / 100.0
    }

    /// Queue `g` for core1 (at the front with pf_lifo, or `urgent`).
    fn succ_push(&mut self, g: u64, depth: u32, urgent: bool) {
        if self.sram.contains_key(&g) || self.scur.is_some_and(|c| c.0 == g) {
            return;
        }
        let now = self.now1();
        if !self.sq_set.insert(g) {
            if (urgent || self.cfg.pf_lifo)
                && let Some(i) = self.sq.iter().position(|e| e.0 == g)
            {
                let e = self.sq.remove(i).unwrap();
                self.sq.push_front(e);
            }
            return;
        }
        if self.sq.len() == PF_QUEUE {
            let old = if self.cfg.pf_lifo { self.sq.pop_back() } else { self.sq.pop_front() };
            if let Some((o, _, _)) = old {
                self.sq_set.remove(&o);
            }
            self.c.pf_dropped += 1;
        }
        if urgent || self.cfg.pf_lifo {
            self.sq.push_front((g, depth, now));
        } else {
            self.sq.push_back((g, depth, now));
        }
    }

    fn succ_enqueue(&mut self, cpu: &Cpu, k: u64, info: Info, depth: u32) {
        if depth > self.cfg.pf_depth {
            return;
        }
        let mut v = self.successors(cpu, key_va(k), key_pa(k), info);
        if self.cfg.pf_lifo {
            // pushed to the front one by one: the first successor ends up first
            v.reverse();
        }
        for g in v {
            self.succ_push(g, depth, false);
        }
    }

    /// pf=succ: core1's work up to now. core1 finishes a block once it has
    /// started it (`scur`: key, depth, done at).
    fn succ_drain(&mut self, cpu: &mut Cpu, inf: &mut Infos) {
        let now = self.now1();
        loop {
            if let Some((g, d, done)) = self.scur {
                if done > now {
                    return;
                }
                self.scur = None;
                self.core1_cyc = done;
                self.succ_finish(cpu, inf, g, d);
                continue;
            }
            let Some((g, d, enq)) = self.sq.pop_front() else {
                self.core1_cyc = self.core1_cyc.max(now);
                return;
            };
            self.sq_set.remove(&g);
            if self.sram.contains_key(&g) {
                self.c.pf_skipped += 1;
                continue;
            }
            let info = inf.get(cpu, key_va(g), key_pa(g));
            if info.n == 0 {
                continue;
            }
            let start = self.core1_cyc.max(enq);
            self.core1_cyc = start;
            self.scur = Some((g, d, start + TR_CYC * info.n as f64));
        }
    }

    fn succ_finish(&mut self, cpu: &mut Cpu, inf: &mut Infos, g: u64, d: u32) {
        if self.sram.contains_key(&g) {
            return;
        }
        let mut info = inf.get(cpu, key_va(g), key_pa(g));
        self.c.pf_jobs += 1;
        self.c.pf_job_insns += info.n as u64;
        self.succ_enqueue(cpu, g, info, d + 1);
        info.words = (info.words as u32 * self.cfg.wscale / 100) as u16;
        let rb = inf.ram_base;
        self.sram_insert(g, info, rb);
        self.c.pf_blocks += 1;
        self.c.pf_words += info.words as u64;
        self.c.pf_insns += info.n as u64;
    }

    /// pf=succ with pf_async, on a dispatch miss: interpret while core1
    /// translates (Some), or None for core0 to translate it itself.
    fn succ_async_miss(&mut self, cpu: &mut Cpu, inf: &mut Infos, k: u64) -> Option<Mode> {
        let info = inf.get(cpu, key_va(k), key_pa(k));
        if info.n == 0 {
            return None;
        }
        let busy = self.scur.is_some_and(|c| c.0 == k) || self.sq_set.contains(&k);
        if !busy {
            // core1's backlog ahead of this block: what it is on now
            let ahead = self.scur.map_or(0.0, |c| c.2 - self.now1());
            if ahead > 348_000.0 {
                return None;
            }
        } else {
            self.c.pf_skipped += 1;
        }
        self.succ_push(k, 0, true);
        self.succ_drain(cpu, inf);
        self.c.cold_blocks += 1;
        self.st = St::Cold { left: info.n };
        Some(Mode::Cold)
    }

    /// pf=async: core1's finished blocks, into the code buffer.
    fn async_drain(&mut self, cpu: &mut Cpu, inf: &mut Infos) {
        let now = self.core0_cyc() * self.cfg.pf_avail as f64 / 100.0;
        while let Some(&(g, done)) = self.pending.front() {
            if done > now {
                break;
            }
            self.pending.pop_front();
            if !self.pending_set.remove(&g) || self.sram.contains_key(&g) {
                continue;
            }
            let mut info = inf.get(cpu, key_va(g), key_pa(g));
            info.words = (info.words as u32 * self.cfg.wscale / 100) as u16;
            let rb = inf.ram_base;
            self.sram_insert(g, info, rb);
            self.c.pf_blocks += 1;
            self.c.pf_words += info.words as u64;
            self.c.pf_insns += info.n as u64;
        }
    }

    /// pf=async miss: Some(mode) to interpret, None for core0 to translate.
    fn async_miss(&mut self, cpu: &mut Cpu, inf: &mut Infos, k: u64) -> Option<Mode> {
        let (va, pa) = (key_va(k), key_pa(k));
        let info = inf.get(cpu, va, pa);
        if info.n == 0 {
            return None;
        }
        if !self.pending_set.contains(&k) {
            let now = self.core0_cyc() * self.cfg.pf_avail as f64 / 100.0;
            let start = self.core1_cyc.max(now);
            if start - now > 348_000.0 {
                return None;
            }
            let done = start + TR_CYC * info.n as f64;
            self.core1_cyc = done;
            self.pending.push_back((k, done));
            self.pending_set.insert(k);
            self.c.pf_jobs += 1;
            self.c.pf_job_insns += info.n as u64;
        } else {
            self.c.pf_skipped += 1;
        }
        self.c.cold_blocks += 1;
        self.st = St::Cold { left: info.n };
        Some(Mode::Cold)
    }

    fn ghost_note(&mut self, k: u64) {
        if self.cfg.pf == 3 {
            self.evicted_ever.insert(k);
            return;
        }
        if self.cfg.pf == 0 {
            return;
        }
        if self.ghost_ring.len() == PF_GHOSTS
            && let Some(old) = self.ghost_ring.pop_front()
            && let Some(v) = self.ghosts.get_mut(&(key_pa(old) >> 12))
        {
            v.retain(|&x| x != old);
        }
        self.ghost_ring.push_back(k);
        self.ghosts.entry(key_pa(k) >> 12).or_default().push(k);
    }

    /// core0 translated `k` itself: ask core1 for the page's other ghosts.
    fn pf_request(&mut self, k: u64) {
        if !matches!(self.cfg.pf, 1 | 2) {
            return;
        }
        let Some(v) = self.ghosts.get(&(key_pa(k) >> 12)) else { return };
        for &g in v.iter().rev() {
            if g == k || self.sram.contains_key(&g) || self.pf_store.contains_key(&g) {
                continue;
            }
            if self.pfq.len() == PF_QUEUE {
                self.pfq.pop_front();
                self.c.pf_dropped += 1;
            }
            self.pfq.push_back(g);
        }
    }

    /// core1's work up to core0's present time.
    fn pf_drain(&mut self, cpu: &mut Cpu, inf: &mut Infos) {
        if !matches!(self.cfg.pf, 1 | 2) {
            return;
        }
        let now = self.core0_cyc() * self.cfg.pf_avail as f64 / 100.0;
        while self.core1_cyc < now {
            let Some(g) = self.pfq.pop_front() else {
                // idle: core1's clock keeps up with core0's
                self.core1_cyc = now;
                break;
            };
            if self.sram.contains_key(&g) || self.pf_store.contains_key(&g) {
                self.c.pf_skipped += 1;
                self.core1_cyc += CPI * 100.0;
                continue;
            }
            let mut info = inf.get(cpu, key_va(g), key_pa(g));
            if info.n == 0 {
                continue;
            }
            info.words = (info.words as u32 * self.cfg.wscale / 100) as u16;
            self.core1_cyc += TR_CYC * info.n as f64;
            self.c.pf_jobs += 1;
            self.c.pf_job_insns += info.n as u64;
            if self.cfg.pf == 1 {
                let w = info.words + 4;
                while self.pf_used + w as usize > self.cfg.pf_store_words {
                    let Some((ok, ow)) = self.pf_fifo.pop_front() else { break };
                    self.pf_used -= ow as usize;
                    if self.pf_store.get(&ok) == Some(&ow) {
                        self.pf_store.remove(&ok);
                    }
                }
                self.pf_used += w as usize;
                self.pf_fifo.push_back((g, w));
                self.pf_store.insert(g, w);
            } else {
                let rb = inf.ram_base;
                self.sram_insert(g, info, rb);
                self.c.pf_blocks += 1;
                self.c.pf_words += info.words as u64;
                self.c.pf_insns += info.n as u64;
            }
        }
    }

    fn page(&self, pa: u32, ram_base: u32) -> usize {
        ((pa - ram_base) >> 12) as usize
    }

    fn find(&self, k: u64) -> Option<Blk> {
        self.sram.get(&k).or_else(|| self.xipb.get(&k)).copied()
    }

    fn live(&self, k: u64, id: u32) -> bool {
        id != 0 && (self.sram.get(&k).is_some_and(|b| b.id == id) || self.xipb.get(&k).is_some_and(|b| b.id == id))
    }

    fn blk_mut(&mut self, k: u64, id: u32) -> Option<&mut Blk> {
        if self.sram.get(&k).is_some_and(|b| b.id == id) {
            return self.sram.get_mut(&k);
        }
        if self.xipb.get(&k).is_some_and(|b| b.id == id) {
            return self.xipb.get_mut(&k);
        }
        None
    }

    fn new_id(&mut self) -> u32 {
        self.next_id += 1;
        self.next_id
    }

    fn flush_sram(&mut self) {
        self.sram.clear();
        self.fifo.clear();
        self.incoming.clear();
        self.pos = self.code_start;
        self.nblocks = 0;
        self.sram_pages.fill(false);
        self.jc.fill((0, 0));
        self.c.flushes += 1;
    }

    fn flush_store(&mut self) {
        self.store.clear();
        self.xipb.clear();
        self.store_pos = 0;
        self.store_pages.fill(false);
        self.c.store_flushes += 1;
        if self.cfg.xip {
            // SRAM blocks may be chained into the store
            self.flush_sram();
        }
    }

    fn evict_front(&mut self) {
        let Some((k, id, _)) = self.fifo.pop_front() else { return };
        self.nblocks -= 1;
        self.c.evicted += 1;
        if self.sram.get(&k).is_some_and(|b| b.id == id) {
            self.sram.remove(&k);
            self.ghost_note(k);
        }
        if let Some(v) = self.incoming.remove(&id) {
            for (sk, sid, s) in v {
                if let Some(b) = self.blk_mut(sk, sid)
                    && b.slot[s as usize].1 == id
                {
                    b.slot[s as usize] = (0, 0);
                    self.c.unlinks += 1;
                }
            }
        }
    }

    /// Room for a block in the SRAM buffer, as `translate_inner` checks it
    /// (worst case n * 12 + 16 words); returns the word address.
    fn sram_alloc(&mut self, n: usize, words: usize) -> u32 {
        let need = (n * 12 + 16) * self.cfg.wscale as usize / 100;
        let cap = self.cfg.words;
        match self.cfg.evict {
            Evict::Flush => {
                if self.pos + need > cap || self.nblocks == self.cfg.max_blocks {
                    self.flush_sram();
                }
            }
            Evict::Fifo if self.cfg.regions > 0 => {
                let r = self.cfg.regions;
                if self.region_seq.is_empty() {
                    self.region_seq = vec![0; r];
                    self.region_recs = vec![0; r];
                    self.region_seq[0] = self.seq_next;
                    self.seq_next += 1;
                }
                let rw = (cap - self.code_start) / r;
                let rstart = |i: usize| self.code_start + i * rw;
                let full = self.pos + need > rstart(self.region) + rw;
                if full || self.nblocks >= self.cfg.max_blocks {
                    // move on to the next region, evicting what it holds
                    let next = (self.region + 1) % r;
                    let (s, e) = (rstart(next), rstart(next) + rw);
                    while self.fifo.front().is_some_and(|f| (f.2 as usize) >= s && (f.2 as usize) < e) {
                        self.evict_front();
                    }
                    self.region = next;
                    self.pos = s;
                    self.region_seq[next] = self.seq_next;
                    self.seq_next += 1;
                    self.region_recs[next] = 0;
                    self.c.flushes += 1;
                }
            }
            Evict::Fifo => {
                if self.pos + need > cap {
                    while self.fifo.front().is_some_and(|f| f.2 as usize >= self.pos) {
                        self.evict_front();
                    }
                    self.pos = self.code_start;
                }
                while self.fifo.front().is_some_and(|f| {
                    let s = f.2 as usize;
                    s >= self.pos && s < self.pos + need
                }) {
                    self.evict_front();
                }
                while self.nblocks >= self.cfg.max_blocks {
                    self.evict_front();
                }
            }
        }
        let a = self.pos as u32;
        self.pos += words;
        self.nblocks += 1;
        self.c.max_sram_blocks = self.c.max_sram_blocks.max(self.nblocks as u64);
        a
    }

    fn sram_insert(&mut self, k: u64, info: Info, ram_base: u32) -> Blk {
        let addr = self.sram_alloc(info.n as usize, info.words as usize);
        let id = self.new_id();
        let seq = self.region_seq.get(self.region).copied().unwrap_or(0);
        let b = Blk { id, info, tier: Tier::Sram, slot: [(0, 0); 2], execs: 0, addr, seq };
        self.sram.insert(k, b);
        if self.cfg.evict == Evict::Fifo {
            self.fifo.push_back((k, id, addr));
        }
        if info.n > 0 {
            let p = self.page(key_pa(k), ram_base);
            self.sram_pages[p] = true;
        }
        b
    }

    /// Log space for a block: rvjit's entry is a 4-word header plus the code.
    fn store_alloc(&mut self, k: u64, words: u16, ram_base: u32) -> u32 {
        let words = words + 4;
        if self.cfg.store_ring && !self.cfg.xip {
            while self.store_used + words as usize > self.cfg.store_words {
                let Some((ok, ow)) = self.store_fifo.pop_front() else { break };
                self.store_used -= ow as usize;
                if self.store.get(&ok) == Some(&ow) {
                    self.store.remove(&ok);
                }
            }
            self.store_used += words as usize;
            self.store_fifo.push_back((k, words));
            self.c.store_write_words += words as u64;
            self.store.insert(k, words);
            let p = self.page(key_pa(k), ram_base);
            self.store_pages[p] = true;
            return 0;
        }
        if self.store_pos + words as usize > self.cfg.store_words {
            self.flush_store();
        }
        let a = self.store_pos as u32;
        self.store_pos += words as usize;
        self.c.store_write_words += words as u64;
        self.store.insert(k, words);
        let p = self.page(key_pa(k), ram_base);
        self.store_pages[p] = true;
        a
    }

    fn is_code_page(&self, p: u32) -> bool {
        self.sram_pages[p as usize] || self.store_pages[p as usize]
    }

    fn invalidate(&mut self, p: u32, ram_base: u32) {
        let same = |k: &u64| ((key_pa(*k) - ram_base) >> 12) != p;
        self.sram.retain(|k, _| same(k));
        self.xipb.retain(|k, _| same(k));
        self.store.retain(|k, _| same(k));
        self.pf_store.retain(|k, _| same(k));
        self.evicted_ever.retain(|k| same(k));
        self.pfq.retain(|k| same(k));
        self.pending_set.retain(|k| same(k));
        self.sq.retain(|(k, _, _)| same(k));
        if self.scur.is_some_and(|c| !same(&c.0)) {
            self.scur = None;
        }
        self.sq_set.retain(|k| same(k));
        self.ghosts.remove(&((p << 12).wrapping_add(ram_base) >> 12));
        self.jc.fill((0, 0));
        self.sram_pages[p as usize] = false;
        self.store_pages[p as usize] = false;
        self.c.smc += 1;
    }

    fn aot_hit(&self, va: u32) -> bool {
        match self.cfg.aot {
            0 => false,
            1 => va >= KERNEL_VA,
            _ => true,
        }
    }

    /// A block not in either tier: translate it (or fetch it from the store
    /// or the flash image). Returns the block, now resident.
    fn obtain(&mut self, cpu: &mut Cpu, inf: &mut Infos, k: u64) -> Blk {
        let (va, pa) = (key_va(k), key_pa(k));
        let mut info = inf.get(cpu, va, pa);
        info.words = (info.words as u32 * self.cfg.wscale / 100) as u16;
        let rb = inf.ram_base;
        if info.n == 0 {
            self.c.nocode_blocks += 1;
            return self.sram_insert(k, info, rb);
        }
        let w = info.words as u64;
        let pv = (cpu.privilege == Priv::U) as usize;
        self.pf_drain(cpu, inf);
        if self.cfg.pf == 1 && self.pf_store.remove(&k).is_some() {
            self.c.pf_blocks += 1;
            self.c.pf_words += w;
            self.c.pf_insns += info.n as u64;
            return self.sram_insert(k, info, rb);
        }
        if self.cfg.pf == 3 && self.evicted_ever.contains(&k) {
            let cost = TR_CYC * info.n as f64;
            let now = self.core0_cyc() * self.cfg.pf_avail as f64 / 100.0;
            // core1 works at most 1 ms (348 k cycles) ahead of core0
            self.core1_cyc = self.core1_cyc.max(now - 348_000.0);
            if self.core1_cyc + cost <= now {
                self.core1_cyc += cost;
                self.c.pf_jobs += 1;
                self.c.pf_job_insns += info.n as u64;
                self.c.pf_blocks += 1;
                self.c.pf_words += w;
                self.c.pf_insns += info.n as u64;
                return self.sram_insert(k, info, rb);
            }
        }
        if self.cfg.pf == 5 {
            self.succ_drain(cpu, inf);
            if let Some(b) = self.find(k) {
                return b;
            }
            if let Some((g, d, done)) = self.scur
                && g == k
            {
                // core1 is on it: wait if that's quicker
                let left = (done - self.now1()).max(0.0);
                if left < TR_CYC * info.n as f64 {
                    self.c.pf_wait_cyc += left as u64;
                    self.c.pf_waits += 1;
                    self.scur = None;
                    self.core1_cyc = done;
                    self.succ_finish(cpu, inf, k, d);
                    if let Some(b) = self.find(k) {
                        return b;
                    }
                }
            }
            if self.sq_set.remove(&k) {
                self.sq.retain(|e| e.0 != k);
            }
        }
        let from_store = !self.cfg.xip && self.store.contains_key(&k);
        if from_store {
            self.c.restore_blocks += 1;
            self.c.restore_words += w;
        } else if self.aot_hit(va) {
            self.c.aot_blocks += 1;
            self.c.aot_words += w;
        } else {
            self.c.translate_blocks[pv] += 1;
            self.c.translate_insns[pv] += info.n as u64;
            self.c.translate_words[pv] += w;
            self.pf_request(k);
            if self.cfg.pf == 5 {
                let raw = inf.get(cpu, va, pa);
                self.succ_enqueue(cpu, k, raw, 1);
            }
        }
        if self.cfg.xip {
            let addr = self.store_alloc(k, info.words, rb);
            let id = self.new_id();
            let b = Blk { id, info, tier: Tier::Xip, slot: [(0, 0); 2], execs: 0, addr, seq: 0 };
            self.xipb.insert(k, b);
            return b;
        }
        let b = self.sram_insert(k, info, rb);
        if self.cfg.store_words > 0 && !from_store {
            self.store_alloc(k, info.words, rb);
        }
        b
    }

    fn feed_xip(&self, cpu: &mut Cpu, addr: u32, words: u32) {
        let (Some(i), Some(m)) = (self.xip_idx, cpu.inst.model.as_mut()) else { return };
        let x = &mut m.extra[i];
        let a0 = XIP_CODE_BASE + addr * 4;
        let mut a = a0 & !7;
        while a < a0 + words * 4 {
            x.access(a);
            a += 8;
        }
    }

    /// Enter block `k` with `budget` instructions left; false if the budget
    /// prologue exits (the next step is then interpreted).
    fn start(&mut self, cpu: &mut Cpu, k: u64, budget: i64, rb: u32) -> bool {
        let Some(b) = self.find(k) else {
            self.c.desync += 1;
            self.st = St::Disp { force: false };
            return false;
        };
        let n = b.info.n;
        if n as i64 > budget {
            self.c.exit_budget += 1;
            self.st = St::Disp { force: true };
            return false;
        }
        self.c.block_execs += 1;
        if b.tier == Tier::Xip {
            self.c.xip_execs += 1;
            self.c.xip_words += b.info.words as u64;
            self.feed_xip(cpu, b.addr, b.info.words as u32);
            let execs = b.execs + 1;
            if let Some(bm) = self.xipb.get_mut(&k) {
                bm.execs = execs;
            }
            if self.cfg.promote > 0 && execs >= self.cfg.promote && !self.sram.contains_key(&k) {
                self.c.promotes += 1;
                self.c.promote_words += b.info.words as u64;
                self.sram_insert(k, b.info, rb);
            }
        }
        self.st = St::In { key: k, k: 0, n, budget: budget - n as i64 };
        true
    }

    fn pre(&mut self, cpu: &mut Cpu, inf: &mut Infos, irq: bool) {
        if irq {
            if let St::In { .. } = self.st {
                self.c.exit_irq += 1;
            }
            self.st = St::Disp { force: false };
        }
        if let St::In { key: k, k: i, .. } = self.st
            && cpu.pc != key_va(k).wrapping_add(4 * i as u32)
        {
            self.c.desync += 1;
            self.st = St::Disp { force: false };
        }
        self.priv_u = (cpu.privilege == Priv::U) as usize;
        self.mode = match self.st {
            St::In { .. } => Mode::Jit,
            St::Cold { .. } => Mode::Cold,
            St::Disp { force: true } => {
                self.c.runs += 1;
                Mode::Interp
            }
            St::Disp { force: false } => {
                self.c.runs += 1;
                self.dispatch(cpu, inf)
            }
        };
    }

    fn dispatch(&mut self, cpu: &mut Cpu, inf: &mut Infos) -> Mode {
        let pc = cpu.pc;
        let Ok((pa, _)) = cpu.translate_pc(pc) else { return Mode::Interp };
        let k = key(pc, pa);
        self.c.lookups += 1;
        if self.cfg.pf == 5 && self.cfg.pf_async && self.find(k).is_none() {
            self.succ_drain(cpu, inf);
            if self.find(k).is_none()
                && let Some(m) = self.succ_async_miss(cpu, inf, k)
            {
                return m;
            }
        }
        if self.cfg.pf == 4 && self.find(k).is_none() {
            self.async_drain(cpu, inf);
            if self.find(k).is_none()
                && let Some(m) = self.async_miss(cpu, inf, k)
            {
                return m;
            }
        }
        let b = match self.find(k) {
            Some(b) => b,
            None => {
                if self.cfg.hot > 1 {
                    let cnt = self.hot.entry(k).or_insert(0);
                    *cnt += 1;
                    if *cnt < self.cfg.hot {
                        let info = inf.get(cpu, pc, pa);
                        if info.n == 0 {
                            return Mode::Interp;
                        }
                        self.c.cold_blocks += 1;
                        self.st = St::Cold { left: info.n };
                        return Mode::Cold;
                    }
                }
                self.obtain(cpu, inf, k)
            }
        };
        if b.info.n == 0 {
            return Mode::Interp;
        }
        let jci = ((pc >> 2) as usize) & (self.jc.len() - 1);
        self.jc[jci] = (k, b.id);
        let budget = (cpu.timer_deadline().min(BUDGET_CAP as u64)) as i64;
        self.c.entries += 1;
        if self.start(cpu, k, budget, inf.ram_base) {
            Mode::Jit
        } else {
            // budget exit: the dispatcher interprets this instruction
            self.c.runs += 1;
            self.st = St::Disp { force: false };
            Mode::Interp
        }
    }

    fn post(&mut self, cpu: &mut Cpu, inf: &mut Infos, f: &Flags) {
        let pv = self.priv_u;
        match self.mode {
            Mode::Interp => {
                self.c.interp[pv] += 1;
                self.st = St::Disp { force: false };
            }
            Mode::Cold => {
                self.c.cold_steps[pv] += 1;
                if let St::Cold { left } = self.st {
                    self.st = if left <= 1 || f.exception { St::Disp { force: false } } else { St::Cold { left: left - 1 } };
                }
            }
            Mode::Jit => {
                let St::In { key: k, k: i, n, budget } = self.st else { unreachable!() };
                let to_code = f.store_page.is_some_and(|p| self.is_code_page(p));
                if f.r_miss || f.w_miss || f.misaligned || f.exception || to_code {
                    // TLB helper miss: exit, the dispatcher interprets it
                    self.c.exit_mem += 1;
                    self.c.exit_interp += 1;
                    self.c.interp[pv] += 1;
                    self.c.runs += 1;
                    self.st = St::Disp { force: false };
                } else {
                    self.c.jit_insns[pv] += 1;
                    if i + 1 < n {
                        self.st = St::In { key: k, k: i + 1, n, budget };
                    } else {
                        self.block_end(cpu, inf, k, budget);
                    }
                }
            }
        }
        if let Some(p) = f.store_page
            && self.is_code_page(p)
        {
            self.invalidate(p, inf.ram_base);
        }
    }

    fn block_end(&mut self, cpu: &mut Cpu, inf: &mut Infos, k: u64, budget: i64) {
        let Some(b) = self.find(k) else {
            self.c.desync += 1;
            self.st = St::Disp { force: false };
            return;
        };
        let va = key_va(k);
        let next = cpu.pc;
        match b.info.end {
            BlockEnd::Interp => {
                self.c.exit_interp += 1;
                self.st = St::Disp { force: true };
            }
            BlockEnd::Jalr => self.ijmp(cpu, next, budget, inf.ram_base),
            BlockEnd::Branch => {
                let fall = va.wrapping_add(4 * b.info.n as u32);
                let slot = if next == fall { 1 } else { 0 };
                self.goto(cpu, inf, k, slot, next, budget);
            }
            BlockEnd::Jal | BlockEnd::Fall => self.goto(cpu, inf, k, 0, next, budget),
        }
    }

    /// Leave block `k` through chain slot `slot` (or the jump cache) to `t`.
    fn goto(&mut self, cpu: &mut Cpu, inf: &mut Infos, k: u64, slot: usize, t: u32, budget: i64) {
        let Some(b) = self.find(k) else { return };
        let va = key_va(k);
        if t & !0xFFF != va & !0xFFF {
            self.ijmp(cpu, t, budget, inf.ram_base);
            return;
        }
        let (tk, tid) = b.slot[slot];
        if self.live(tk, tid) {
            self.c.chained_jumps += 1;
            self.start(cpu, tk, budget, inf.ram_base);
            return;
        }
        // chain request: exit, and the dispatcher patches the slot
        self.c.exit_jump += 1;
        self.c.chain_attempts += 1;
        self.st = St::Disp { force: false };
        let Ok((tpa, _)) = cpu.translate_pc(t) else { return };
        let tkey = key(t, tpa);
        self.c.lookups += 1;
        let target = match self.find(tkey) {
            Some(tb) => tb,
            None if self.cfg.hot > 1 => return,
            None => self.obtain(cpu, inf, tkey),
        };
        if target.info.n == 0 || !self.live(tkey, target.id) {
            return;
        }
        if self.cfg.regions > 0 && target.seq < b.seq {
            let r = (target.addr as usize - self.code_start) / ((self.cfg.words - self.code_start) / self.cfg.regions);
            self.c.chain_recs += 1;
            self.region_recs[r] += 1;
            self.c.max_region_recs = self.c.max_region_recs.max(self.region_recs[r]);
        }
        if let Some(src) = self.blk_mut(k, b.id) {
            src.slot[slot] = (tkey, target.id);
            self.c.chains += 1;
            if self.cfg.evict == Evict::Fifo {
                self.incoming.entry(target.id).or_default().push((k, b.id, slot as u8));
            }
        }
    }

    fn ijmp(&mut self, cpu: &mut Cpu, t: u32, budget: i64, rb: u32) {
        self.st = St::Disp { force: false };
        let Some(tpa) = cpu.tlb_probe(t, ACC_X) else {
            self.c.exit_jump += 1;
            self.c.ijmp_tlb_misses += 1;
            return;
        };
        let (jk, jid) = self.jc[((t >> 2) as usize) & (self.jc.len() - 1)];
        if jk == key(t, tpa) && self.live(jk, jid) {
            self.c.jc_hits += 1;
            self.start(cpu, jk, budget, rb);
        } else {
            self.c.exit_jump += 1;
            self.c.jc_misses += 1;
        }
    }

    fn json(&self, cpu: &Cpu) -> String {
        let c = &self.c;
        let mut s = format!(
            "{{\"name\": \"{}\", \"kb\": {}, \"blocks\": {}, \"evict\": \"{:?}\", \"hot\": {}, \"store_kb\": {}, \"xip\": {}, \"promote\": {}, \"aot\": {}, \"jc\": {}, \"wscale\": {}, \"regions\": {}, \"pf\": {}, \"pf_store_kb\": {}, \"pf_avail\": {}, ",
            self.cfg.name,
            self.cfg.words / 256,
            self.cfg.max_blocks,
            self.cfg.evict,
            self.cfg.hot,
            self.cfg.store_words / 256,
            self.cfg.xip as u32,
            self.cfg.promote,
            self.cfg.aot,
            self.cfg.jc,
            self.cfg.wscale,
            self.cfg.regions,
            self.cfg.pf,
            self.cfg.pf_store_words / 256,
            self.cfg.pf_avail
        );
        json_counts!(
            c,
            s,
            [
                runs,
                lookups,
                cold_blocks,
                entries,
                block_execs,
                nocode_blocks,
                restore_blocks,
                restore_words,
                aot_blocks,
                aot_words,
                store_write_words,
                store_flushes,
                exit_jump,
                exit_interp,
                exit_mem,
                exit_budget,
                exit_irq,
                chain_attempts,
                chains,
                chained_jumps,
                jc_hits,
                jc_misses,
                ijmp_tlb_misses,
                flushes,
                evicted,
                unlinks,
                smc,
                xip_execs,
                xip_words,
                promotes,
                promote_words,
                desync,
                max_sram_blocks,
                chain_recs,
                max_region_recs,
                pf_blocks,
                pf_words,
                pf_insns,
                pf_jobs,
                pf_job_insns,
                pf_skipped,
                pf_dropped,
                pf_wait_cyc,
                pf_waits
            ],
            [interp, cold_steps, jit_insns, translate_blocks, translate_insns, translate_words]
        );
        let xm = match (self.xip_idx, cpu.inst.model.as_ref()) {
            (Some(i), Some(m)) => m.extra[i].misses,
            (None, Some(m)) => m.jit.misses,
            _ => 0,
        };
        s.push_str(&format!("\"xip_misses\": {xm}}}"));
        s
    }
}

pub struct Sim {
    inf: Infos,
    pols: Vec<Policy>,
    miss0: [u64; 3],
    exc0: u64,
    steps: u64,
    out: String,
}

fn leak<T: Clone>(v: T, n: usize) -> &'static mut [T] {
    Box::leak(vec![v; n].into_boxed_slice())
}

impl Sim {
    /// `spec`: file of policy lines; `out`: JSON results; `corpus`: optional
    /// dump of every distinct translated block (position-independent form).
    pub fn new(cpu: &mut Cpu, spec: &str, out: &str, corpus: Option<&str>) -> Result<Sim, String> {
        let text = std::fs::read_to_string(spec).map_err(|e| format!("cannot read {spec}: {e}"))?;
        let cfgs = text
            .lines()
            .map(|l| l.split('#').next().unwrap_or("").trim())
            .filter(|l| !l.is_empty())
            .map(parse_cfg)
            .collect::<Result<Vec<_>, _>>()?;
        if cfgs.is_empty() {
            return Err("no policies".into());
        }
        let pages = (cpu.ram_size() >> 12) as usize;
        let jit = Jit::new(
            cpu,
            JitMem {
                code: leak(0u32, 64 * 1024),
                buckets: leak(0u32, 1024),
                code_pages: leak(0u8, pages / 8 + 1),
                jump_cache: leak(JcEntry::EMPTY, 1024),
                regions: 1,
                chain_log: &mut [],
                store: &mut [],
                store_epochs: &mut [],
            },
        );
        // the simulator tracks code pages per policy; keep the interpreter's
        // write TLB unaware of them
        cpu.code_pages = core::ptr::null();
        let layout = jit.prelude_layout();
        let mut corpus_w = None;
        if let Some(p) = corpus {
            let mut w = BufWriter::new(File::create(p).map_err(|e| format!("cannot create {p}: {e}"))?);
            // header: magic, prelude layout
            for x in [
                0x5355_4252u32,
                layout.code_start as u32,
                layout.chain_req as u32,
                layout.ijmp_imm as u32,
                layout.interp_exit as u32,
            ] {
                w.write_all(&x.to_le_bytes()).map_err(|e| e.to_string())?;
            }
            corpus_w = Some(w);
        }
        let m = cpu.inst.model.as_mut().ok_or("the simulator needs the XIP model")?;
        let mut pols = Vec::new();
        for c in cfgs {
            let xi = if c.xip {
                m.extra.push(XipCache::new(16384, 2, 8));
                Some(m.extra.len() - 1)
            } else {
                None
            };
            pols.push(Policy::new(c, pages, layout.code_start, xi));
        }
        let ram_base = cpu.ram_base();
        Ok(Sim {
            inf: Infos {
                jit,
                layout,
                cache: HashMap::new(),
                page_has: vec![false; pages],
                page_keys: HashMap::new(),
                ram_base,
                corpus: corpus_w,
                corpus_seen: HashSet::new(),
                corpus_blocks: 0,
            },
            pols,
            miss0: [0; 3],
            exc0: 0,
            steps: 0,
            out: out.to_string(),
        })
    }

    /// Before `cpu.step`; `irq` is what `check_irq` just returned.
    pub fn pre(&mut self, cpu: &mut Cpu, irq: bool) {
        for p in &mut self.pols {
            p.pre(cpu, &mut self.inf, irq);
        }
        self.miss0 = cpu.stats.tlb_miss;
        self.exc0 = cpu.stats.exceptions;
        cpu.inst.step_store_page = None;
        cpu.inst.step_misaligned = false;
    }

    pub fn post(&mut self, cpu: &mut Cpu) {
        self.steps += 1;
        let f = Flags {
            r_miss: cpu.stats.tlb_miss[ACC_R] != self.miss0[ACC_R],
            w_miss: cpu.stats.tlb_miss[ACC_W] != self.miss0[ACC_W],
            misaligned: cpu.inst.step_misaligned,
            exception: cpu.stats.exceptions != self.exc0,
            store_page: cpu.inst.step_store_page,
        };
        for p in &mut self.pols {
            p.post(cpu, &mut self.inf, &f);
        }
        if let Some(pg) = f.store_page {
            self.inf.store_to(pg);
        }
    }

    pub fn finish(&mut self, cpu: &Cpu) -> Result<(), String> {
        if let Some(w) = &mut self.inf.corpus {
            w.flush().map_err(|e| e.to_string())?;
        }
        let m = cpu.inst.model.as_ref();
        let mut s = format!(
            "{{\"steps\": {}, \"instret\": {}, \"unique_insns\": {}, \"user_instret\": {}, \"xip_jit_misses\": {}, \"xip_jit_accesses\": {}, \"corpus_blocks\": {}, \"policies\": [\n",
            self.steps,
            cpu.instret,
            cpu.stats.unique_insns,
            cpu.stats.user_instret,
            m.map_or(0, |m| m.jit.misses),
            m.map_or(0, |m| m.jit.accesses),
            self.inf.corpus_blocks
        );
        for (i, p) in self.pols.iter().enumerate() {
            s.push_str("  ");
            s.push_str(&p.json(cpu));
            s.push_str(if i + 1 < self.pols.len() { ",\n" } else { "\n" });
        }
        s.push_str("]}\n");
        std::fs::write(&self.out, s).map_err(|e| format!("cannot write {}: {e}", self.out))?;
        eprintln!("[rvemu] simulated {} policies over {} steps -> {}", self.pols.len(), self.steps, self.out);
        Ok(())
    }
}
