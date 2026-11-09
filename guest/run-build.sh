#!/usr/bin/env bash
# Host wrapper: build the guest kernel + rootfs in Docker. Usage: ./run-build.sh [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]
set -euo pipefail
cd "$(dirname "$0")"
. ../scripts/preflight.sh
stage=${1:-all}
case "$stage" in all|rootfs|kernel|disk|disk-rootfs|disk-kernel) ;; *)
  echo "Usage: $0 [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]" >&2; exit 2;;
esac
for tool in git docker python3; do need_tool "$tool" 'See docs/development.md.'; done
docker info >/dev/null || { echo 'Start Docker and retry.' >&2; exit 1; }
# Buildroot itself is not in the repository: fetch the release the configs are for
BR_TAG=2026.08.1
BR_REV=7fdeadb3b9f12f78e5fccca914d7fef47090ab9a
[ -d src/buildroot ] || git clone --depth 1 --branch "$BR_TAG" https://gitlab.com/buildroot.org/buildroot.git src/buildroot
expected=$(git -C src/buildroot rev-parse "refs/tags/$BR_TAG^{commit}") || {
  echo "Buildroot tag $BR_TAG is missing; fetch that tag in guest/src/buildroot." >&2; exit 1;
}
[ "$expected" = "$BR_REV" ] || {
  echo "Buildroot tag $BR_TAG must resolve to $BR_REV; found $expected." >&2; exit 1;
}
actual=$(git -C src/buildroot rev-parse HEAD)
if [ "$actual" != "$expected" ] || [ -n "$(git -C src/buildroot status --porcelain)" ]; then
  echo "guest/src/buildroot must be clean at $BR_TAG ($expected); found $actual. Save local work and check out the expected tag." >&2
  exit 1
fi
mkdir -p out out-disk
# Kernel-only stages require the matching Buildroot toolchain from a full build.
case "$stage" in
  kernel) tree=br;; disk-kernel) tree=br-disk;; *) tree=;;
esac
if [ -n "$tree" ]; then
  docker run --rm -v rvemu-guest-work:/work:ro debian:bookworm test -x "/work/$tree/host/bin/riscv32-buildroot-linux-musl-gcc" || {
    echo 'Build the corresponding full guest first: ./run-build.sh all (or disk).' >&2; exit 1;
  }
fi
out=out; [[ $stage != disk* ]] || out=out-disk
# claude-guest-mcp goes into the disk rootfs; it is built here, with the host's
# Rust (the build container has none), and post-build-disk.sh installs it
if [[ $stage == disk || $stage == disk-rootfs ]]; then
  need_tool cargo 'Install Rust (rustup) to build claude-guest-mcp.'
  rustup target list --installed | grep -qx riscv32im-unknown-none-elf || {
    echo 'Run: rustup target add riscv32im-unknown-none-elf' >&2; exit 1;
  }
  (cd claude-remote && cargo build --release --locked)
fi
# A failed rebuild must not leave a stale success manifest beside its outputs.
rm -f "$out/build.json"
# Build trees live in the Docker volume rvemu-guest-work, so rebuilds are incremental
docker build -q -t rvemu-guest . >/dev/null
docker volume create rvemu-guest-work >/dev/null
docker run --rm -v "$PWD/src:/src" -v "$PWD/out:/out" -v "$PWD/out-disk:/out-disk" -v "$PWD:/guest:ro" \
  -v rvemu-guest-work:/work --user 0 rvemu-guest bash -c '
    set -e
    output_owner=$1
    build_stage=$2
    restore_owner() { chown -R "$output_owner" /out /out-disk; }
    trap restore_owner EXIT
    chown builder /work
    chown -R builder /out /out-disk
    su builder -c "/guest/build.sh $build_stage"
  ' _ "$(id -u):$(id -g)" "$stage" 2>&1 | tee "build-$stage.log"
python3 ../scripts/provenance.py --stage "$stage" --output "$out/build.json"
