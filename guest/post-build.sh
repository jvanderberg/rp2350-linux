#!/bin/sh
# Buildroot post-build: drop init scripts that make no sense on this guest.
set -e
TARGET_DIR=$1
rm -f "$TARGET_DIR/etc/init.d/S40network"   # kernel has no networking
