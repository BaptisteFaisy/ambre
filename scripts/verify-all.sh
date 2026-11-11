#!/usr/bin/env bash
# verify-all.sh — single entry point: everything that must be green before a push.
#
#   ./scripts/verify-all.sh [--quick]
#
# --quick skips the 1 000 000-state exploration and the 24 h fuzz targets.
# Exit code 0 = the whole gate is green.

set -uo pipefail

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

steps_ok=0
steps_ko=0
report=()

run_step() {
    local name="$1"; shift
    printf '\n=== %s ===\n' "$name"
    if "$@"; then
        steps_ok=$((steps_ok + 1))
        report+=("ok   $name")
    else
        steps_ko=$((steps_ko + 1))
        report+=("FAIL $name")
    fi
}

run_step "sizecheck (files < 500 lines)" make sizecheck
run_step "build (library, tests, tools)" make all
run_step "unit tests" make test
run_step "python tests" bash -c '
    fail=0
    for t in tests/test_*_py.py; do
        [ -f "$t" ] || continue
        echo "-- $t"
        python3 "$t" || fail=1
    done
    exit $fail'

if [ -f host/scenarios/generate.py ]; then
    run_step "scenario generator" bash -c '
        mkdir -p build
        python3 host/scenarios/generate.py --seed 1234 --duration-ms 30000 \
            --crash-at-ms 25000 --out build/scenario.txt'
fi

run_step "exploration 10 000 states (level 1)" make explore

if [ "$QUICK" -eq 0 ]; then
    run_step "trigger cross-check C vs Python (DEC-12)" make crosscheck
    run_step "exploration 1 000 000 states" make explore-full
fi

run_step "mcap export + verify" bash -c 'make mcap && make verify'

printf '\n================ SUMMARY ================\n'
for line in "${report[@]}"; do printf '%s\n' "$line"; done
printf '=========================================\n'
printf 'green: %d   failed: %d\n' "$steps_ok" "$steps_ko"

[ "$steps_ko" -eq 0 ]
