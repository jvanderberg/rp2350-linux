#!/usr/bin/env bash
# The on-device performance and stability gate. Every change is measured on
# the board with this before it lands; nothing may make it appreciably worse.
#   scripts/perf-gate.sh [-n LABEL] [-s] [-B]
# Needs the board lock (run it as rp2350/tools/board-lock run NAME --
# scripts/perf-gate.sh) and the guests already built in this checkout's
# guest/out and guest/out-disk.
#   1. rvemu-pico-linux benchmark: initramfs guest to login: (deterministic
#      instruction count, board ms; checked against the Mac reference).
#   2. rvemu-pico-shell with the disk guest: wall time from firmware start to
#      login:, then a fixed workload over the console, timed on the Mac:
#      first runs of five programs, MicroPython and Lua loops, 200
#      fork+exec, and checksums of every file under /usr /lib /bin /sbin
#      (twice) against the squashfs image's own, which is the stability
#      check: one flipped bit fails the gate.
# Results go to $RP2350_LOCK_HOME/perf.log (one JSON line per run) and are
# compared with perf-baseline.json there. -s makes this run the baseline
# (the integrator does that, nobody else). -B skips step 1.
# Fails (exit 1) on: any boot or workload time more than 3% (boot) / 5%
# (workload) slower than the baseline, MemAvailable 128 KiB or more below it,
# a checksum mismatch, a Mac/board mismatch, or no login:.
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/pico-common.sh
LABEL=""; SET=0; SKIP_BENCH=0
while getopts "n:sB" o; do case $o in n) LABEL=$OPTARG;; s) SET=1;; B) SKIP_BENCH=1;; *) exit 2;; esac; done
need_board
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
CMD=../rp2350/tools/board-cmd.py
now() { python3 -c 'import time; print(f"{time.time():.3f}")'; }
R="$T/results"; : > "$R"
put() { echo "$1=$2" >> "$R"; echo "perf-gate: $1 = $2"; }

# timed NAME TIMEOUT COMMAND: run COMMAND on the board, record wall seconds
timed() {
  local name=$1 to=$2 s e; shift 2
  s=$(now)
  python3 "$CMD" --timeout "$to" -- "$@" > "$T/$name.out"
  e=$(now)
  put "$name" "$(python3 -c "print(f'{$e-$s:.2f}')")"
}

if [ $SKIP_BENCH = 0 ]; then
  scripts/pico-linux.sh 300 | tee "$T/bench.out"
  line=$(grep -a "^RESULT translator " "$T/bench.out")
  put initramfs_ms "$(sed -E 's/.* ms ([0-9]+) .*/\1/' <<<"$line")"
  put initramfs_instret "$(sed -E 's/.* instret ([0-9]+) .*/\1/' <<<"$line")"
fi

# the squashfs image's own checksums (unsquashfs from the guest build volume)
DISK=../guest/out-disk/rootfs.squashfs
docker run --rm -v "$(cd ../guest/out-disk && pwd):/d:ro" -v rvemu-guest-work:/work:ro rvemu-guest sh -c \
  '/work/br-disk/host/bin/unsquashfs -q -d /tmp/r /d/rootfs.squashfs >/dev/null && cd /tmp/r && find usr lib bin sbin -type f | sort | xargs md5sum' > "$T/manifest"
[ -s "$T/manifest" ] || { echo "perf-gate: could not make the checksum manifest of $DISK"; exit 1; }

scripts/pico-shell.sh
put boot_s "$(python3 ../rp2350/tools/boot-wait.py "$(cat "$LOCK_HOME/last-launch")" --timeout 180)"
python3 "$CMD" --timeout 60 -- true > /dev/null          # log in
python3 "$CMD" -- "grep -E '^(MemTotal|MemAvailable):' /proc/meminfo" > "$T/mem"
put mem_total_kb "$(awk '/MemTotal/{print $2}' "$T/mem")"
put mem_avail_kb "$(awk '/MemAvailable/{print $2}' "$T/mem")"
# Pages parked on the per-CPU free lists are missing from MemAvailable and swing
# it by hundreds of KiB between boots; mem_adj_kb adds them back (4 KiB pages).
python3 "$CMD" -- "awk '/^ +count:/{s+=\$2} END{print s+0}' /proc/zoneinfo" > "$T/pcp"
put pcp_pages "$(tr -dc 0-9 < "$T/pcp")"
put mem_adj_kb "$(( $(awk '/MemAvailable/{print $2}' "$T/mem") + 4 * $(tr -dc 0-9 < "$T/pcp") ))"
timed first_runs_s 300 "htop --version >/dev/null; tmux -V >/dev/null; micropython -c 'print(1)' >/dev/null; lua -v >/dev/null; nano --version >/dev/null"
timed mpy_s 300 "micropython -c 'print(sum(i*i for i in range(30000)))'"
timed lua_s 300 "lua -e 'local s=0 for i=1,300000 do s=s+i end print(s)'"
timed fork_exec_s 300 'i=0; while [ $i -lt 200 ]; do /bin/true; i=$((i+1)); done'
ok=1
for pass in cold warm; do
  timed "md5_${pass}_s" 600 "cd / && find usr lib bin sbin -type f | sort | xargs md5sum"
  if ! diff <(tr -d '\r' < "$T/md5_${pass}_s.out") "$T/manifest" > "$T/md5.diff"; then
    echo "perf-gate: CHECKSUM MISMATCH ($pass pass) -- possible memory corruption:"; head -20 "$T/md5.diff"; ok=0
  fi
done
put checksums_ok $ok

python3 - "$R" "$LOCK_HOME" "$LABEL" "${BOARD_LOCK_OWNER:-}" "$SET" <<'P'
import json, subprocess, sys, time, os
res_path, home, label, owner, set_base = sys.argv[1:]
r = {}
for l in open(res_path):
    k, v = l.strip().split("=", 1)
    r[k] = float(v) if "." in v else int(v)
git = lambda *a: subprocess.run(["git", *a], capture_output=True, text=True).stdout.strip()
r.update(label=label, owner=owner, branch=git("rev-parse", "--abbrev-ref", "HEAD"),
         commit=git("rev-parse", "--short", "HEAD"), dirty=bool(git("status", "--porcelain")),
         date=time.strftime("%F %T"))
with open(os.path.join(home, "perf.log"), "a") as f:
    f.write(json.dumps(r) + "\n")
bp = os.path.join(home, "perf-baseline.json")
if set_base == "1":
    json.dump(r, open(bp, "w"), indent=1)
    print("perf-gate: this run is now the baseline")
    sys.exit(0)
if not os.path.exists(bp):
    print("perf-gate: no baseline yet"); sys.exit(0 if r.get("checksums_ok") else 1)
b = json.load(open(bp))
fail = []
print(f"\n{'metric':<18}{'baseline':>12}{'this run':>12}{'change':>9}")
# (metric, % limit, absolute floor): a slowdown fails only past both. The floors
# are the run-to-run noise measured on unchanged code (two dev/base runs: Lua
# 8%, warm md5 6%, everything else within 3%).
for k, lim, floor in [("initramfs_ms", 3, 0), ("boot_s", 3, 0.3), ("first_runs_s", 5, 0.3),
                      ("mpy_s", 5, 0.3), ("lua_s", 5, 0.3), ("fork_exec_s", 5, 0.5),
                      ("md5_cold_s", 5, 0.8), ("md5_warm_s", 5, 1.5)]:
    if k not in r or k not in b:
        continue
    d = 100.0 * (r[k] - b[k]) / b[k]
    flag = "  SLOWER" if d > lim and r[k] - b[k] > floor else ""
    if flag:
        fail.append(f"{k} +{d:.1f}% (limit {lim}% and {floor})")
    print(f"{k:<18}{b[k]:>12}{r[k]:>12}{d:>8.1f}%{flag}")
# memory: MemAvailable plus the per-CPU free lists when both runs have it
mk = "mem_adj_kb" if "mem_adj_kb" in b and "mem_adj_kb" in r else "mem_avail_kb"
if mk in b:
    d = r[mk] - b[mk]
    print(f"{mk:<18}{b[mk]:>12}{r[mk]:>12}{d:>+8} KiB")
    if d <= -128:
        fail.append(f"{mk} {d} KiB")
if not r.get("checksums_ok"):
    fail.append("checksum mismatch (stability)")
print("\nperf-gate: " + ("FAIL: " + "; ".join(fail) if fail else "PASS"))
sys.exit(1 if fail else 0)
P
