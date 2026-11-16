#!/bin/sh
# run.sh: in the guest (the board, or the Mac emulator), from a copy of this
# directory. Builds pio-selftest with the guest's cc and runs it, then
# rp2_selftest.py. The last line is "pio-tests: PASS" or "pio-tests: FAIL".
cd "$(dirname "$0")" || exit 1
rc=0
if cc -o pio-selftest pio-selftest.c -lpio -lpthread; then
    ./pio-selftest || rc=1
else
    echo "FAIL pio-selftest doesn't build"
    rc=1
fi
micropython rp2_selftest.py || rc=1
if [ $rc = 0 ]; then echo "pio-tests: PASS"; else echo "pio-tests: FAIL"; fi
exit $rc
