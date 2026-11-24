#!/bin/sh
# Golden tests for Dolphin. Usage: run_tests.sh path/to/dolphin
#   cases/X.dol + X.out    stdout must match, exit 0
#   cases/X.dol + X.err    stderr must match, exit nonzero
#   cases/X.dol + X.needs  `dolphin --needs` output must match
# Plus examples/grib_ls.dol against data/synthetic.grib2.
set -u
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
cd "$(dirname "$0")/cases" || exit 2
pass=0 fail=0
report() { if [ "$1" = 0 ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $2"; fi; }
for src in *.dol; do
    name=${src%.dol}
    if [ -f "$name.out" ]; then
        "$BIN" "$src" 2>/dev/null | diff -u "$name.out" - >/dev/null; report $? "$name (stdout)"
    fi
    if [ -f "$name.err" ]; then
        if "$BIN" "$src" >/dev/null 2>/tmp/dolphin_err.$$; then report 1 "$name (expected failure)"
        else diff -u "$name.err" /tmp/dolphin_err.$$ >/dev/null; report $? "$name (stderr)"; fi
    fi
    if [ -f "$name.needs" ]; then
        "$BIN" --needs "$src" 2>/dev/null | diff -u "$name.needs" - >/dev/null; report $? "$name (needs)"
    fi
done
rm -f /tmp/dolphin_err.$$
cd .. && "$BIN" ../examples/grib_ls.dol data/synthetic.grib2 | diff -u grib_ls.out - >/dev/null; report $? "grib_ls example"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
