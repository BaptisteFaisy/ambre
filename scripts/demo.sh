#!/usr/bin/env bash
#
# demo.sh — the Ambre demonstration, one command.
#
#   ./scripts/demo.sh
#
# It runs, in order:
#   1. build            (make all)
#   2. generate         a scenario file with the fixed generator
#   3. write the journal (amber-explore --export-image)
#   4. cut the power    (amber-explore explores every reachable cut state)
#   5. recover          (the explorer recovers from every one of them)
#   6. export           the journal image to one MCAP file
#   7. verify           edr-verify checks the export against the scenario
#   8. summary          a readable recap
#
# Only the CLIs frozen in docs/AGENT-BRIEF.md are used:
#   make
#   build/ambre-explore
#   python3 host/scenarios/generate.py
#   python3 host/mcap/export_mcap.py
#   python3 host/verify/edr_verify.py
#
# Exit code: 0 when the whole chain succeeded, non-zero otherwise. Nothing is
# faked: if a binary is missing or a step fails, the script stops with an
# explicit message.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${AMBRE_BUILD_DIR:-build}"
SCENARIO="${BUILD_DIR}/demo-scenario.txt"
IMAGE="${BUILD_DIR}/demo-journal.img"
MCAP="${BUILD_DIR}/demo-event.mcap"
EXPLORE_REPORT="${BUILD_DIR}/demo-explore.json"
STATES="${AMBRE_DEMO_STATES:-1000}"

SEED=1234
DURATION_MS=30000
CRASH_AT_MS=25000

step()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info()  { printf '    %s\n' "$*"; }
die()   { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

need_file() {
    [ -f "$1" ] || die "missing required file: $1
       This tool is not present in this tree. Run 'make all' first, then
       re-run ./scripts/demo.sh."
}

need_exec() {
    [ -x "$1" ] || die "missing required binary: $1
       Run 'make all' to build it, then re-run ./scripts/demo.sh."
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

# ---------------------------------------------------------------- 0. checks --

step "Checking the tools"

need_cmd make
need_cmd python3
info "make       : $(command -v make)"
info "python3    : $(command -v python3)"

need_file host/scenarios/generate.py
need_file host/mcap/export_mcap.py
need_file host/verify/edr_verify.py
info "generator  : host/scenarios/generate.py"
info "exporter   : host/mcap/export_mcap.py"
info "verifier   : host/verify/edr_verify.py"

# ------------------------------------------------------------------ 1. build --

step "Building the library, the tests and the tools"

BUILD_OK=1
if ! make BUILD="${BUILD_DIR}" all; then
    BUILD_OK=0
    info "the full build reported errors (see above)"
fi

if [ ! -x "${BUILD_DIR}/ambre-explore" ]; then
    die "the build did not produce ${BUILD_DIR}/ambre-explore.
       This demo needs the explorer binary; run 'make all' and fix the errors
       reported above."
fi
if [ "${BUILD_OK}" -eq 1 ]; then
    info "build OK"
else
    info "build reported errors, but the explorer is present"
fi
need_exec "${BUILD_DIR}/ambre-explore"

# -------------------------------------------------------------- 2. scenario --

step "Generating the scenario"

mkdir -p "${BUILD_DIR}"

python3 host/scenarios/generate.py \
    --seed "${SEED}" \
    --duration-ms "${DURATION_MS}" \
    --crash-at-ms "${CRASH_AT_MS}" \
    --out "${SCENARIO}"

[ -s "${SCENARIO}" ] || die "the generator produced an empty scenario file"
info "scenario   : ${SCENARIO} (seed ${SEED}, crash at ${CRASH_AT_MS} ms)"

# ------------------------------------------------- 3. write the journal ------

step "Writing the journal (capture, trigger, EMERG, seal)"

if ! "${BUILD_DIR}/ambre-explore" \
        --scenario "${SCENARIO}" \
        --states 1 \
        --export-image "${IMAGE}"; then
    die "the journal write failed"
fi

[ -s "${IMAGE}" ] || die "the journal image was not produced"
info "journal    : ${IMAGE}"

# ---------------------------------------------------- 4/5. cut and recover --

step "Cutting the power and recovering (${STATES} reachable states)"

if ! "${BUILD_DIR}/ambre-explore" \
        --scenario "${SCENARIO}" \
        --states "${STATES}" \
        --variants all \
        --report "${EXPLORE_REPORT}"; then
    die "the exploration found an invariant violation, see ${EXPLORE_REPORT}"
fi
info "exploration: ${EXPLORE_REPORT}"

# --------------------------------------------------------------- 6. export --

step "Exporting the event to MCAP"

if ! python3 host/mcap/export_mcap.py \
        --image "${IMAGE}" \
        --out "${MCAP}"; then
    die "the MCAP export failed"
fi

[ -s "${MCAP}" ] || die "the MCAP file was not produced"
info "mcap       : ${MCAP}"

# --------------------------------------------------------------- 7. verify --

step "Verifying the export against the injected scenario"

if ! python3 host/verify/edr_verify.py \
        --mcap "${MCAP}" \
        --scenario "${SCENARIO}"; then
    die "edr-verify reported a mismatch between the export and the scenario"
fi

# --------------------------------------------------------------- 8. summary --

json_int() {
    # json_int <file> <key> -> the integer value of "<key>: <n>" in the report
    local file="$1" key="$2"
    grep -o "\"${key}\": *[0-9][0-9]*" "${file}" 2>/dev/null \
        | head -n 1 | grep -o '[0-9][0-9]*$' || printf '0'
}

step "Summary"

STATES_EXPLORED="$(json_int "${EXPLORE_REPORT}" states_explored)"
VIOLATIONS="$(json_int "${EXPLORE_REPORT}" violations)"
RECOVERIES="$(json_int "${EXPLORE_REPORT}" recoveries_run)"
ACQUIRED="$(json_int "${EXPLORE_REPORT}" events_acquired)"
PHANTOM="$(json_int "${EXPLORE_REPORT}" events_phantom)"

printf '    %-34s %s\n' "scenario"                 "${SCENARIO}"
printf '    %-34s %s\n' "journal image"            "${IMAGE}"
printf '    %-34s %s\n' "MCAP export"              "${MCAP}"
printf '    %-34s %s\n' "exploration report"       "${EXPLORE_REPORT}"
printf '\n'
printf '    %-34s %s\n' "cut states explored"      "${STATES_EXPLORED}"
printf '    %-34s %s\n' "recoveries run"           "${RECOVERIES}"
printf '    %-34s %s\n' "events acquired"          "${ACQUIRED}"
printf '    %-34s %s\n' "phantom events"           "${PHANTOM}"
printf '    %-34s %s\n' "invariant violations"     "${VIOLATIONS}"
printf '\n'

if [ "${VIOLATIONS}" -ne 0 ]; then
    die "the exploration reported ${VIOLATIONS} invariant violation(s)"
fi

printf '\033[1;32mDEMO OK\033[0m — the journal survived every explored cut, '
printf 'the export verified against the injected scenario.\n'
