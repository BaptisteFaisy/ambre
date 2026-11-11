#!/usr/bin/env bash
# explore-campaign.sh — multi-scenario power-cut exploration campaign.
#
# The reference 30 s scenario yields 64 591 reachable cut states: that is the
# WHOLE space for that scenario, not a sample. The "complet" level of the spec
# (section 7.7) asks for one million explored states, which no single scenario
# reaches, so the campaign runs several scenarios (different seeds, durations,
# crash instants and delta-v) and sums their states.
#
#   ./scripts/explore-campaign.sh [--scenarios N] [--jobs N] [--out DIR]
#                                 [--duration-ms N] [--nice]
#
# Writes one JSON report per scenario plus campaign.json (the sum), and prints
# a table. Exit code 0 only if every scenario found zero violations.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SCENARIOS=12
JOBS=4
OUT=build/campaign
DURATION=30000
BIN=build/ambre-explore
NICE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --scenarios)   SCENARIOS="$2"; shift 2 ;;
        --jobs)        JOBS="$2"; shift 2 ;;
        --out)         OUT="$2"; shift 2 ;;
        --duration-ms) DURATION="$2"; shift 2 ;;
        --nice)        NICE="nice -n 19"; shift ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
make "$BIN" >/dev/null || { echo "build failed" >&2; exit 1; }

total_states=0
total_events=0
total_violations=0
worst_seconds=0
failed=0

for i in $(seq 1 "$SCENARIOS"); do
    seed=$((1000 + i * 37))
    crash=$(( DURATION / 2 + (i % 7) * 500 ))
    dv=$(( 8 + (i % 5) * 3 ))
    scen="$OUT/scenario-$i.txt"
    json="$OUT/state-$i.json"

    python3 host/scenarios/generate.py --seed "$seed" --duration-ms "$DURATION" \
        --crash-at-ms "$crash" --dv-kmh "$dv" --out "$scen" >/dev/null

    printf 'scenario %2d/%d seed=%d crash=%dms dv=%dkm/h ... ' \
        "$i" "$SCENARIOS" "$seed" "$crash" "$dv"
    start=$(date +%s)
    $NICE "$BIN" --scenario "$scen" --segment-limit 4 --jobs "$JOBS" \
        --variants all --nested --report "$json" >/dev/null 2>&1
    rc=$?
    end=$(date +%s)
    elapsed=$((end - start))

    read -r states events violations <<<"$(python3 - "$json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(d["states_explored"], d["events_acquired"], d["violations"])
PY
)"
    total_states=$((total_states + states))
    total_events=$((total_events + events))
    total_violations=$((total_violations + violations))
    [ "$elapsed" -gt "$worst_seconds" ] && worst_seconds="$elapsed"
    [ "$rc" -ne 0 ] && failed=$((failed + 1))
    printf '%7d states, %6d acquired, %d violation(s), %ds%s\n' \
        "$states" "$events" "$violations" "$elapsed" \
        "$([ "$rc" -ne 0 ] && echo '  <-- FAIL')"
done

python3 - "$OUT/campaign.json" "$total_states" "$total_events" \
    "$total_violations" "$SCENARIOS" "$worst_seconds" <<'PY'
import json, sys, time
path, states, events, violations, scenarios, worst = sys.argv[1:7]
json.dump({
    "tool": "scripts/explore-campaign.sh",
    "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "scenarios": int(scenarios),
    "states_explored": int(states),
    "events_acquired_observed": int(events),
    "violations": int(violations),
    "slowest_scenario_s": int(worst),
    "variants": ["prefix", "torn", "cache", "bitflip", "table", "nested"],
    "invariants_checked": ["I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8"],
    "backend": "posix (simulated disk, exhaustive op-trace prefixes)",
}, open(path, "w"), indent=2)
PY

echo "----------------------------------------------------------------"
printf 'CAMPAIGN: %d scenarios, %d states, %d acquired-event observations, ' \
    "$SCENARIOS" "$total_states" "$total_events"
printf '%d violation(s)\n' "$total_violations"
printf 'report: %s/campaign.json\n' "$OUT"

[ "$total_violations" -eq 0 ] && [ "$failed" -eq 0 ]
