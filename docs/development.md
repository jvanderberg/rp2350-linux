# Development and verification

Run the commands below from the repository root unless a command changes
into another directory. The [main README](../README.md) describes using the
board; this guide describes building and checking changes.

## Start with the host

Install Git, a C compiler, Make, Bash, Python 3.9 or newer, and Rust via
rustup. On macOS the compiler and Make come from Xcode Command Line Tools;
on Ubuntu install `build-essential`, `git`, and `python3`.

The root `rust-toolchain.toml` selects Rust 1.97.1, the version used for the
host checks when this workflow was added. Cargo lockfiles are committed and
the build scripts use `--locked`. Cross targets and LLVM tools are optional
until the QEMU or board steps below.

```sh
scripts/check.sh host
```

This checks Bash/Python syntax, tests the verification runner's success,
failure and timeout handling, runs Cargo tests, builds the host Rust crates
in release mode, and builds the C reference. It needs neither a guest image
nor a board. The initial Rust build can download dependencies.

## Build the guests

Install Docker and start its daemon. The wrapper checks the daemon and the
Buildroot revision before building. The first build downloads and compiles
a cross toolchain, so allow substantially longer than an incremental build.

```sh
(cd guest && ./run-build.sh all)    # initramfs: comparison, QEMU, boot benchmark
(cd guest && ./run-build.sh disk)   # squashfs: interactive board and performance gate
```

Outputs live in `guest/out` and `guest/out-disk`. Each successful invocation
writes `build.json` beside its outputs, including the source commit, dirty
status, source and image hashes, tool versions, Buildroot commit, and Docker
image ID. Console output is retained in `guest/build-STAGE.log`. A failed
build removes the previous success manifest; output files may still be
partial or left over from the preceding build.

The `disk` and `disk-rootfs` stages also build `guest/claude-remote`
(the `claude-guest-mcp` binary) on the host with Cargo, so they need Rust
with the `riscv32im-unknown-none-elf` target; the result goes into the
root filesystem through `post-build-disk.sh`.

The wrapper clones Buildroot tag `2026.08.1`, verifies its pinned commit
(`BR_REV` in `guest/run-build.sh`), and refuses a dirty checkout or
a different HEAD. It does not reset local changes. Kernel-only stages require
the matching Buildroot toolchain from a prior full build. See
[guest/README.md](../guest/README.md) for incremental stages.

Builds share the Docker volume `rvemu-guest-work` and image `rvemu-guest`.
Do not run guest builds concurrently across checkouts. The Debian base tag
and apt repositories are not frozen: the manifest provides provenance, not
a claim of bit-for-bit reproducibility. Partial rebuilds also retain earlier
outputs; use full `all`/`disk` builds for release or benchmark evidence.

## Check under QEMU (no board access)

Install `dtc` and QEMU (`brew install dtc qemu` on macOS;
`sudo apt-get install device-tree-compiler qemu-system-misc` on Ubuntu).
Then:

```sh
rustup target add riscv32imac-unknown-none-elf
scripts/check.sh qemu
```

This requires `guest/out/Image` and `guest/out/rootfs.cpio`. It compares the
C and Rust host interpreters using one shared device tree (including the
same fixed entropy seed), normalizes the single extra Rust host setup TLB
flush from `set_readonly_prefix(0)`, then boots the initramfs guest under QEMU in
interpreter, JIT and lockstep modes. A successful process exit alone is not
sufficient: every boot must reach the requested prompt, and lockstep must
report a nonzero count of matched events. A failed prerequisite is an error,
not a skipped check.

The runner's per-command timeout defaults to 1,800 seconds. Override with
`--timeout SECONDS` for a slower machine; it terminates the command's process
group on timeout or SIGTERM cancellation (including descendants that ignore SIGTERM). Each tier is independent: run `host` as well as `qemu`.
For an interactive QEMU session or disk guest, see
[rvemu-rs/README.md](../rvemu-rs/README.md).

## Check on the board (flashes and reboots hardware)

The existing board scripts require macOS, the documented RP2350B board,
USB, and an SWD probe. Install `picotool` and the RP2350-capable probe-rs
build described in [rp2350/README.md](../rp2350/README.md); `PROBE_RS` can
select its executable. Close any terminal using the board's serial port.

```sh
rustup target add riscv32imac-unknown-none-elf riscv32im-unknown-none-elf
rustup component add llvm-tools-preview
```

Build both guests first. Establish a performance baseline on the revision
and board configuration you intend to compare against:

```sh
(cd rvemu-rs && scripts/perf-gate.sh -s)  # flashes/reboots; deliberately replaces baseline
scripts/check.sh board                  # flashes/reboots; compares against baseline
```

The board tier builds the firmware, checks core1's SRAM-to-flash references,
runs the JIT test guest against the host reference, then runs the existing
Linux boot/performance/stability gate. It refuses a missing, incomplete or failed-checksum baseline instead
of treating the first run as a successful performance comparison. Baseline metrics must be
finite, and provenance must bind the metrics and fixed checksum list by SHA-256.
Older bundles without these hashes are rejected; recover a complete validated
bundle or deliberately establish a new baseline on the intended reference revision. It leaves
the disk guest running. Guest writes during these tests can update `/home`.
The JIT test script rebuilds the tracked `jittest.bin`; review any resulting
binary change before committing.

The board tier retains a copy of `target/perf-gate` (under `rvemu-rs`), including
baseline, baseline provenance, fixed checksum list, performance records and boot logs. To move a
baseline between checkouts, copy both `perf-baseline.json` and `md5-files.list`
and `baseline-provenance.json` (written by `perf-gate.sh -s`)
into the receiving checkout's `rvemu-rs/target/perf-gate/`. Keep the exported
record with them, and compare only equivalent hardware, clocks and guests.
Never reset a baseline merely to make a regression pass.

## Native crypto on the board

These flash and reboot the board too, and need the disk guest. The test
image is the disk guest with the test programs and a vector file added
(`guest/tests/build-board-crypto.sh`), in `guest/out-crypto`:

```sh
mkdir -p guest/out-crypto
python3 rvemu-rs/scripts/board-crypto-vectors.py guest/out-crypto/crypto-vectors.bin
docker run --rm --user 0 -v rvemu-guest-work:/work:ro -v "$PWD:/repo:ro" \
  -v "$PWD/guest/out-crypto:/out" rvemu-guest bash /repo/guest/tests/build-board-crypto.sh
```

`board-crypto-vectors.py` writes the RFC 7748, RFC 8032, RFC 8439, SSH
draft and sntrup761 draft vectors, the largest legal buffers, tampered
inputs and malformed requests, and checks every expected output against
the host build of the engine. `rvemu-rs/scripts/pico-crypto-test.py` loads
the image (`pico-shell.sh -d ../guest/out-crypto`), logs in on the USB
console and runs one of:

| Command | On the board |
|---|---|
| `vectors` | the ioctl tests, every vector three times on one descriptor, each operation's cycles |
| `ssh` | `ssh-board-test integration` (authentication, exit status, transfers both ways, a wrong key) with both ends native, both in software, and each end alone |
| `connection -n N` | N fresh boots each native and software, one connection each, timed from connect to authentication |
| `responsive` | ping over Wi-Fi and console round trips while native operations run back to back, and a `kill -9` in the middle of one |
| `remote` | the Mac's OpenSSH logging in to a second Dropbear on the board, native and software |

Close any terminal on the console port first. Results (JSON) and the
console transcripts go to `rvemu-rs/target/pico-crypto/`. `ssh-board-test`
keeps its keys in `/run`; nothing is written to flash.

Under QEMU, `rvemu-rs/scripts/ssh-eval.py` runs the same SSH tests in an
initramfs guest (`guest/tests/build-ssh-eval.sh`; it needs `guest/run-build.sh
all`), and `rvemu-rs/scripts/native-crypto-eval.py` runs the vectors in
translator/interpreter lockstep.

## Results and coverage boundaries

Every check invocation writes a new `target/checks/TIMESTAMP-TIER/` directory.
`--artifacts PATH` selects a new directory instead. `result.json` records
initial provenance, final source hashes/status, commands, exit codes, elapsed times and overall outcome; each
command has a separate log. The board tier also saves and hashes the generated
`jittest.bin` immediately after generation, before reference or board execution.
Failure logs are retained. Missing tools have
null version entries. No environment dump or Wi-Fi settings are collected.
When sharing measurements, attach this directory and the guest's `build.json`,
and identify the board and clock configuration. Old README measurements
without those records remain historical observations, not newly validated
results.

| Check | Establishes | Does not establish |
|---|---|---|
| Host Cargo tests | Three emitter encoding/immediate tests pass | MMU, device or full JIT correctness |
| Script syntax and runner tests | Scripts parse; runner rejects false success, errors and timeouts | Hardware scripts work on a connected board |
| C/Rust comparison | Initramfs console and counters agree through the marker, after the documented single setup-flush adjustment | Independent ISA conformance; both can share a bug |
| QEMU lockstep | JIT and interpreter state agree at checked events for this guest | Every instruction sequence, hardware timing, PSRAM or radio behavior |
| Board JIT test guest | Directed ISA/MMU/SMC cases match host signatures and instruction counts | Exhaustive ISA or memory-model validation |
| core1 SRAM check | Static references satisfy the existing cold-path allowlist | Proof that every indirect call or allowed path is safe |
| Board performance gate | Workload times, available memory and filesystem checksums satisfy the baseline | Long-duration reliability, power-cut recovery or all peripherals |

Flash power-cut injection, swap stress, network traffic, pin loopbacks and
TinyCC self-host tests remain separate, documented workflows. The default
QEMU tier uses the initramfs guest; it does not exercise disk-only features.

## CI

`.github/workflows/check.yml` runs the host tier on Ubuntu 24.04 and macOS 15
for pushes and pull requests. A manual workflow dispatch with `guest` enabled
builds the initramfs guest and runs the QEMU tier on Ubuntu. This keeps the
expensive toolchain build explicit while the pipeline is established.

Host dependencies/build outputs are cached by OS, toolchain and lockfiles.
The guest job caches source downloads only, rebuilds the guest, and uploads
the images, build log, manifest and verification artifacts even on failure.
Hardware checks remain local and explicit; CI never accesses a board.

Tool versions validated locally should be distinguished from CI configuration:
Rust 1.97.1 passed the host build/tests and both firmware cross-builds when
these scripts were introduced. QEMU 11.1.2 with dtc 1.8.1 passed the existing
JIT guest in lockstep (2,477 events); the core1 SRAM check also passed. A
fresh initramfs build and the complete `scripts/check.sh qemu` tier passed
locally, including C/Rust comparison and all three QEMU modes. The runner
tests and C build also passed in a Debian container. Physical board tests
and the hosted GitHub Actions jobs were not run during this change.
Docker, dtc and QEMU report their actual versions in each run's metadata;
no unexecuted CI or full board run is implied by this guide.
