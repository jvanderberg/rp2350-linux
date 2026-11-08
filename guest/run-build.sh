#!/usr/bin/env bash
# Host wrapper: build the guest kernel + rootfs in Docker. Usage: ./run-build.sh [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]
# PINS=1 ./run-build.sh disk: the disk guest with the board's pins (GPIO, I2C, SPI, PWM, ADC)
set -euo pipefail
cd "$(dirname "$0")"
# Buildroot itself is not in the repository: fetch the release the configs are for
BR_TAG=2026.08.1
[ -d src/buildroot ] || git clone --depth 1 --branch "$BR_TAG" https://gitlab.com/buildroot.org/buildroot.git src/buildroot
mkdir -p out out-disk
# Every checkout builds in the one rvemu-guest-work volume (Docker's disk has
# no room for a copy each), so builds take turns under the guest-build lock.
# Never hold the board while building: release it first.
L=../rp2350/tools/board-lock
[ -n "${BOARD_LOCK_OWNER:-}" ] || { echo "run-build.sh: set BOARD_LOCK_OWNER (your name for rp2350/tools/board-lock)"; exit 1; }
if "$L" check "$BOARD_LOCK_OWNER" >/dev/null 2>&1; then
  echo "run-build.sh: you hold the board lock; release it before building ($L release $BOARD_LOCK_OWNER)"; exit 1
fi
docker build -q -t rvemu-guest . >/dev/null
docker volume create rvemu-guest-work >/dev/null
"$L" run "$BOARD_LOCK_OWNER" -r guest-build -w 240 -l 30 -- docker run --rm -v "$PWD/src:/src" -v "$PWD/out:/out" -v "$PWD/out-disk:/out-disk" -v "$PWD:/guest:ro" \
  -v rvemu-guest-work:/work --user 0 rvemu-guest bash -c 'chown builder /work /out /out-disk && exec su builder -c "PINS='"${PINS:-0}"' /guest/build.sh '"${1:-all}"'"'
