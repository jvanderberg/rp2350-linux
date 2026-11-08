#!/usr/bin/env bash
# Host wrapper: build the guest kernel + rootfs in Docker. Usage: ./run-build.sh [all|rootfs|kernel|disk|disk-rootfs|disk-kernel]
set -euo pipefail
cd "$(dirname "$0")"
# Buildroot itself is not in the repository: fetch the release the configs are for
BR_TAG=2026.08.1
[ -d src/buildroot ] || git clone --depth 1 --branch "$BR_TAG" https://gitlab.com/buildroot.org/buildroot.git src/buildroot
mkdir -p out out-disk
# Build trees live in the Docker volume rvemu-guest-work, so rebuilds are incremental
docker build -q -t rvemu-guest . >/dev/null
docker volume create rvemu-guest-work >/dev/null
docker run --rm -v "$PWD/src:/src" -v "$PWD/out:/out" -v "$PWD/out-disk:/out-disk" -v "$PWD:/guest:ro" \
  -v rvemu-guest-work:/work --user 0 rvemu-guest bash -c 'chown builder /work /out /out-disk && exec su builder -c "/guest/build.sh '"${1:-all}"'"'
