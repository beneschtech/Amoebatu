#!/bin/sh
# Fuzz the .dcl loader and verifier. Usage:
#   fuzz_dcl.sh BUILD_DIR [ITERATIONS] [SEED]
# BUILD_DIR must hold dolphin and dcl_fuzz (`ninja dcl_fuzz` / `make dcl_fuzz`);
# a DOLPHIN_SANITIZE=ON build is the useful one.
#
# 1. Compiles every test program and example to .dcl.
# 2. dcl_fuzz loads ITERATIONS mutants in-process: each must be rejected with
#    a DclError or load; nothing else (no other exception, no crash).
# 3. The mutants that loaded are run, each in its own process with a time
#    limit (a mutated program may loop forever): no signal, no sanitizer report.
set -u
B=$(cd "$1" && pwd)
N=${2:-20000}
SEED=${3:-1}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$(dirname "$0")" || exit 2
for f in cases/*.dol ../examples/*.dol; do
    "$B/dolphin" -c -o "$T/$(basename "$f" .dol).dcl" "$f" 2>/dev/null
done
"$B/dolphin" -c --strip -o "$T/metar_stripped.dcl" ../examples/metar.dol
echo "fuzzing $(ls "$T"/*.dcl | wc -l) files"
mkdir "$T/keep"
"$B/dcl_fuzz" "$SEED" "$N" "$T/keep" 400 "$T"/*.dcl || { echo "FAIL: dcl_fuzz"; exit 1; }
bad=0 ran=0 timeouts=0
for m in "$T"/keep/*.dcl; do
    [ -f "$m" ] || continue
    ran=$((ran+1))
    timeout 5 "$B/dolphin" --threads 2 "$m" data/metar_edge.txt </dev/null >/dev/null 2>"$T/err"
    rc=$?
    if [ $rc = 124 ]; then timeouts=$((timeouts+1)); continue; fi
    if [ $rc -ge 128 ] || grep -q "Sanitizer\|runtime error:" "$T/err"; then
        bad=$((bad+1))
        echo "FAIL: mutant crashed (exit $rc): kept as /tmp/dcl_crash_$bad.dcl"
        cp "$m" "/tmp/dcl_crash_$bad.dcl"
        head -5 "$T/err"
    fi
done
echo "ran $ran loaded mutants: $bad crashed, $timeouts hit the time limit"
[ $bad = 0 ]
