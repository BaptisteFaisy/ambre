#!/usr/bin/env bash
#
# demo.sh — the Ambre demonstration, in one command.
#
#   ./scripts/demo.sh                       # build in ./build (shared tree)
#   AMBRE_BUILD_DIR=build-mine ./scripts/demo.sh   # isolated build tree
#
# It runs the level-2 chain of the specification (cahier section 8) with the
# REAL processes: a drive is replayed, the impact triggers, the event is
# acquired and frozen, edr-journal is killed with SIGKILL, it is restarted on
# the same disk file, the recovery runs, the service list shows the event, the
# event is exported to MCAP and verified against the injected scenario.
#
#   1. build      the five processes (make tools)
#   2. scenario   a frontal impact with restraint deployment (locked event)
#   3. chain      replay -> capture -> journal: "acquired in N ms"
#   4. the cut    SIGKILL on edr-journal, right after the acquisition
#   5. restart    the same disk file: the recovery must run (REP-01)
#   6. list       the event, locked, with "complete file = no"
#   7. export     the recovered journal image -> one MCAP file
#   8. verify     the official verifier + the class A comparison
#   9. summary    the story of the run, and what it does not prove
#
# Only documented CLIs are used (docs/AGENT-BRIEF.md, docs/W2-PLAN.md):
#   make, host/scenarios/generate.py, host/e2e/e2e_run.py,
#   host/mcap/export_mcap.py, host/verify/edr_verify.py
#
# Exit code: 0 when every stage succeeded, non-zero otherwise. Nothing is
# faked: a missing binary, a stage that fails, or a run whose verdict is not
# "ok" all stop the script with an explicit message.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${AMBRE_BUILD_DIR:-build}"
WORK="${AMBRE_DEMO_WORK:-${BUILD_DIR}/demo}"
SCENARIO="${WORK}/scenario.txt"
REPORT="${WORK}/report.json"
RUNDIR="${WORK}/run"

# A locked event needs the restraint deployment of this preset (DEC-03): the
# dv25 lock is disabled while the vehicle declares a front restraint.
SEED="${AMBRE_DEMO_SEED:-1234}"
DURATION_MS="${AMBRE_DEMO_DURATION_MS:-9000}"
CRASH_AT_MS="${AMBRE_DEMO_CRASH_MS:-6000}"
CUT_AT="${AMBRE_DEMO_CUT:-acquired}"
CAMPAIGN_REPORT="${AMBRE_DEMO_CAMPAIGN:-${BUILD_DIR}/processcut/report-200.json}"

step()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info()  { printf '    %s\n' "$*"; }
die()   { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

need_file() {
    [ -f "$1" ] || die "missing required file: $1
       Run 'make all' first, then re-run ./scripts/demo.sh."
}

need_exec() {
    [ -x "$1" ] || die "missing required binary: $1
       Run 'make BUILD=${BUILD_DIR} tools' to build it, then re-run
       ./scripts/demo.sh."
}

# ---------------------------------------------------------------- 0. checks --

step "Checking the tools"

need_cmd make
need_cmd python3
info "make        : $(command -v make)"
info "python3     : $(command -v python3)"

for tool in host/scenarios/generate.py host/mcap/export_mcap.py \
            host/verify/edr_verify.py host/e2e/e2e_run.py \
            host/e2e/demo_summary.py; do
    need_file "$tool"
    info "tool        : $tool"
done

# ---------------------------------------------------------------- 1. build ---

step "Building the five processes"

if ! make BUILD="${BUILD_DIR}" tools; then
    die "the build reported errors (see above); the demo does not continue"
fi
for binary in edr-journal edr-capture edr-replay edr-archive; do
    need_exec "${BUILD_DIR}/${binary}"
    info "binary      : ${BUILD_DIR}/${binary}"
done
# The service tool is named edrctl by the Makefile and edr-ctl in the plan.
if [ -x "${BUILD_DIR}/edrctl" ]; then
    info "binary      : ${BUILD_DIR}/edrctl"
elif [ -x "${BUILD_DIR}/edr-ctl" ]; then
    info "binary      : ${BUILD_DIR}/edr-ctl"
else
    die "missing service tool: ${BUILD_DIR}/edrctl (or edr-ctl)"
fi

# ------------------------------------------------------------- 2. scenario --

step "Generating the scenario (frontal impact, restraint deployed)"

mkdir -p "${WORK}"
# A run always starts on a virgin partition: a leftover image would be
# recovered and the demonstration would report somebody else's event.
rm -rf "${RUNDIR}"
python3 host/scenarios/generate.py \
    --preset urban-frontal --seed "${SEED}" \
    --duration-ms "${DURATION_MS}" --crash-at-ms "${CRASH_AT_MS}" \
    --out "${SCENARIO}"
[ -s "${SCENARIO}" ] || die "the generator produced an empty scenario file"
info "scenario    : ${SCENARIO}"
info "impact      : at ${CRASH_AT_MS} ms, 12 km/h, restraint fires -> locked event"

# --------------------------------------------------- 3..8. the cut cycle ----

step "Replaying the drive, cutting the journal, recovering, exporting"

info "chain       : edr-replay -> edr-capture -> edr-journal -> ${RUNDIR}/journal.img"
info "cut         : SIGKILL on edr-journal, window '${CUT_AT}'"
info ""
if ! python3 host/e2e/e2e_run.py \
        --bins "${BUILD_DIR}" --scenario "${SCENARIO}" \
        --cut-at "${CUT_AT}" --workdir "${RUNDIR}" \
        --out "${REPORT}"; then
    die "the end-to-end bench did not reach the expected state;
       see the output above and ${REPORT}"
fi
[ -s "${REPORT}" ] || die "the bench produced no report"

# --------------------------------------------------------------- 9. summary --

step "Summary of the demonstration"

python3 host/e2e/demo_summary.py "${REPORT}" || die "the run is not green"

if [ -f "${CAMPAIGN_REPORT}" ]; then
    step "Cut campaign on this backend (already run, read from its report)"
    python3 - "$CAMPAIGN_REPORT" <<'PY'
import json, sys
with open(sys.argv[1]) as handle:
    report = json.load(handle)
summary = report.get("summary", {})
print("    cuts executed      : %s" % summary.get("cuts"))
print("    verdicts           : %s" % json.dumps(summary.get("verdicts"),
                                                 sort_keys=True))
print("    invariant violations: %s" % summary.get("violations"))
print("    cut windows        : %s" % json.dumps(summary.get("windows"),
                                                 sort_keys=True))
print("    backend            : %s (%s)" % (report.get("backend"),
                                            report.get("cut_kind")))
PY
else
    info "no campaign report at ${CAMPAIGN_REPORT}"
    info "run it with: python3 host/processcut/campaign.py --bins ${BUILD_DIR} \\"
    info "             --scenario <file> --cycles 200 --jobs 4 --report <out.json>"
fi

printf '\n'
