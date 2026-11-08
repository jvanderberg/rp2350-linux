#!/usr/bin/env python3
"""Board time for the translator cache policies simulated by `rvemu -S`.

usage: scripts/sim.py SIM.json [--mhz 348] [--csv OUT]

Host-instruction costs are fitted to QEMU -icount runs of the real translator
(rvemu-virt, new kernel, boot to login:, NJC 1024) at 1024/192/96 KiB; the
simulator's event counts for those configurations match QEMU's within 0.01%.
  translated code   7.54 / guest insn + 101 / entry      (resid < 0.04%)
  dispatcher        158 / run_one + 149 / chain request  (resid < 0.25%)
  interpreter       333 / step via the dispatcher
  translation       358 / guest insn + flush             (resid 5% at 1024 KiB)
Board conversion: cycles = CPI * host insns + MISS * XIP misses, fitted to
the Linux boot on the board (scripts/pico-linux.sh, 192 KiB flush-all
translator: 12.15 s, 5.11 M XIP misses; interpreter: 36.5 s, 9.40 M misses):
CPI 1.12, MISS 137 cycles. Translation also misses on the guest code it
reads (0.61 misses per translated insn on the board). PSRAM/flash transfer
costs are from the board measurements (sequential read ~34 MB/s = 41
cycles/word at 348 MHz; random miss 321 ns = 112 cycles).
"""
import argparse
import json

HOST = {
    "jit": 7.54,
    "entry": 101.1,
    "run": 157.6,
    "chain_req": 148.6,
    "interp": 333.0,
    "tr_insn": 358.0,
    # board flush: 1024 buckets + 1024 jump-cache entries + code-page bits
    "flush": 12000.0,
    # pure interpreter loop (QEMU interp mode on this boot: 176)
    "cold_step": 176.0,
    "cold_block": 40.0,
    "evict": 80.0,
    "unlink": 30.0,
    # invalidate_page walks every bucket chain
    "smc": 30000.0,
}


def board(p, a):
    """(cycles, breakdown) for policy p under assumptions a."""
    jit = sum(p["jit_insns"])
    tr = sum(p["translate_insns"])
    h = {
        # a denser encoding keeps the budget in a register: no lw/sw per block
        "code": HOST["jit"] * jit + HOST["entry"] * p["entries"]
        - (2 * p["block_execs"] if p.get("wscale", 100) < 100 else 0),
        "dispatch": HOST["run"] * p["runs"] + HOST["chain_req"] * p["chain_attempts"],
        "interp": HOST["interp"] * sum(p["interp"]) + HOST["cold_step"] * sum(p["cold_steps"])
        + HOST["cold_block"] * p["cold_blocks"],
        "translate": a["tr_scale"] * HOST["tr_insn"] * tr + HOST["flush"] * p["flushes"],
        "manage": HOST["evict"] * p["evicted"] + HOST["unlink"] * p["unlinks"] + HOST["smc"] * p["smc"],
    }
    cyc = {k: a["cpi"] * v for k, v in h.items()}
    cyc["xip_miss"] = a["miss"] * p["xip_misses"]
    # translation reads the guest code from PSRAM
    cyc["translate"] += a["scan"] * tr
    ratio = a["ratio"]
    per_word_in = a["read"] / ratio + (a["decode"] if ratio > 1 else 0) + a["reloc"]
    probes = sum(p["translate_blocks"]) + p["restore_blocks"] + p["aot_blocks"]
    store = 0.0
    if p["store_kb"]:
        store += a["probe"] * probes
        store += p["restore_words"] * per_word_in
        store += p["store_write_words"] * (a["write"] / ratio + (a["encode"] if ratio > 1 else 0))
    if p["aot"]:
        store += a["probe"] * p["aot_blocks"]
        store += p["aot_words"] * (a["flash_read"] / ratio + (a["decode"] if ratio > 1 else 0) + a["reloc"])
    if p["xip"]:
        store += p["promote_words"] * per_word_in + a["xip_hit"] * p["xip_words"]
    cyc["store"] = store
    return sum(cyc.values()), cyc, h


BASE = {
    "cpi": 1.12,
    "miss": 137.0,
    "scan": 0.61 * 137.0,
    "tr_scale": 1.0,
    "read": 41.0,  # PSRAM sequential read, cycles per word
    "write": 32.0,  # PSRAM uncached sequential write
    "flash_read": 46.0,
    "probe": 250.0,  # store index lookup in PSRAM (~2 random misses)
    "reloc": 2.0,  # ~0.6 routine jal per word to re-point
    "ratio": 1.0,
    "decode": 6.0,
    "encode": 12.0,
    "xip_hit": 0.5,
}

VARIANTS = {
    "base": {},
    "dma": {"read": 24.0, "write": 0.0},  # DMA restore at ~58 MB/s, writes in the background
    "dict": {"ratio": 2.29},  # 4095-entry dictionary, delta form (16 KiB table)
    "dict+dma": {"ratio": 2.29, "read": 24.0, "write": 0.0},
    "tr/2": {"tr_scale": 0.5},  # translator twice as fast
    "slow-mem": {"cpi": 1.25, "miss": 180.0},  # pessimistic CPI / miss cost
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("json")
    ap.add_argument("--mhz", type=float, default=348.0)
    ap.add_argument("--detail", default=None, help="policy name for a cost breakdown")
    a = ap.parse_args()
    with open(a.json) as f:
        d = json.load(f)
    n = d["instret"]
    hz = a.mhz * 1e6
    var = {k: {**BASE, **v} for k, v in VARIANTS.items()}
    print(f"boot to login: {n} guest insns; seconds at {a.mhz:.0f} MHz (MIPS for base)")
    hdr = f"{'policy':15}{'tr insns':>10}{'restored':>10}" + "".join(f"{k:>10}" for k in var) + f"{'MIPS':>7}"
    print(hdr)
    for p in d["policies"]:
        secs = {k: board(p, v)[0] / hz for k, v in var.items()}
        line = f"{p['name']:15}{sum(p['translate_insns']):>10}{p['restore_blocks'] + p['aot_blocks']:>10}"
        line += "".join(f"{secs[k]:>10.2f}" for k in var)
        line += f"{n / secs['base'] / 1e6:>7.1f}"
        print(line)
    if a.detail:
        p = next(q for q in d["policies"] if q["name"] == a.detail)
        tot, cyc, h = board(p, var["base"])
        print(f"\n{a.detail}: {tot / hz:.2f} s, {tot / n:.1f} cycles/insn, {sum(h.values()) / n:.1f} host insns/insn")
        for k, v in cyc.items():
            print(f"  {k:10} {v / hz:6.2f} s  {100 * v / tot:5.1f}%")
        ts, tu = p["translate_insns"]
        print(f"  translated insns: kernel {ts}  user {tu};  interp steps kernel {p['interp'][0]} user {p['interp'][1]}")
        js, ju = p["jit_insns"]
        print(f"  guest insns run translated: kernel {js}  user {ju}")
    # interpreter-only reference: 176 host insns/insn plus guest fetch+data misses
    print("\n(interpreter only on the board, same model: ~"
          f"{(BASE['cpi'] * 176 * n + BASE['miss'] * d.get('xip_interp_misses', 8408545)) / hz:.1f} s)")


if __name__ == "__main__":
    main()
