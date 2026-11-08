#!/usr/bin/env bash
# board-test.sh: copy these tests to the board's /tmp/tcc-test over the
# console (push.py) and run them there with run.sh, for at most
# TCC_TEST_MAX_S seconds (600). (run.sh selfhost, tcc building itself, is a
# stress case for the emulator, not the board.)
set -euo pipefail
cd "$(dirname "$0")"
CMD=../../../../../rp2350/tools/board-cmd.py
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir "$W/tcc-test"
cp ./*.c ./*.h ./*.expect run.sh "$W/tcc-test/"
tar czf "$W/t.tgz" -C "$W" tcc-test
start=$(date +%s)
"$CMD" -- 'rm -rf /tmp/tcc-test /tmp/t.tgz'
./push.py "$W/t.tgz" /tmp/t.tgz
"$CMD" -- 'cd /tmp && gunzip -c t.tgz | tar xf - && rm t.tgz && ls tcc-test | wc -l'
echo "board-test: copied in $(($(date +%s) - start)) s"
# On a timeout board-cmd prints what has arrived and interrupts run.sh
start=$(date +%s)
rc=0
"$CMD" --timeout "${TCC_TEST_MAX_S:-600}" -- "sh /tmp/tcc-test/run.sh" || rc=$?
echo "board-test: ran in $(($(date +%s) - start)) s (wall, from the Mac), rc $rc"
exit "$rc"
