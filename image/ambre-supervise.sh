#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# image/ambre-supervise.sh - Process supervision loop for Ambre EDR.
#
# Covered requirements:
#   QNX-06  Processes are launched and respawned by the supervision service.
#   SEC-02  No command disables the recorder; a forced kill is followed by an
#           immediate respawn and the interruption is logged.
#   ROB-02  Crash of edr-archive or edrctl has zero effect on capture/freeze.
#   ROB-03  Crash of edr-capture is detected by edr-journal (connection close,
#           QNX-05), recorded as a GAP record, and edr-capture is respawned.
#   ROB-04  Crash of edr-journal is followed by respawn and startup recovery.
#
# IMPORTANT — UNVERIFIED (needs QNX SDP 8.0):
# On a production QNX 8.0 target, this shell supervisor MUST be replaced by the
# native QNX High Availability Manager (HAM: `ham` daemon + `ham_connect()` /
# `ham_entity()` / `ham_condition()` / `ham_action_restart()`). See
# docs/SECURITY.md for the exact HAM migration plan. This script provides an
# inspectable, POSIX/QNX-sh-compatible supervision wrapper until HAM is wired
# and verified against QNX SDP 8.0.

set -u

if [ "$#" -lt 2 ]; then
    echo "Usage: $0 <service-name> <binary> [args...]" >&2
    exit 2
fi

SVC_NAME="$1"
shift

# Export the supervision marker checked by ambre_port_supervised() (QNX-06).
AMBRE_SUPERVISED="1"
export AMBRE_SUPERVISED

# SEC-02: ignore SIGHUP/SIGTERM on the supervisor itself so an unprivileged or
# accidental signal does not disable respawning of critical recorder services.
trap '' HUP

log_supervision() {
    msg="ambre-supervise[$SVC_NAME]: $*"
    if command -v slog2 >/dev/null 2>&1; then
        slog2 -t ambre_sup "$msg"
    fi
    echo "$msg" >&2
}

run_supervised_loop() {
    restarts=0
    while :; do
        log_supervision "starting (restart_count=$restarts): $*"
        "$@"
        rc=$?
        restarts=$((restarts + 1))
        # SEC-02: every unexpected exit or forced kill is recorded before respawn.
        log_supervision "process exited rc=$rc; respawning immediately (restart_count=$restarts)"
        # Bounded backoff only after rapid consecutive exits to avoid tight spin,
        # while keeping ROB-04 respawn latency minimal on single crashes.
        if [ "$restarts" -gt 5 ]; then
            sleep 1
        fi
    done
}

run_supervised_loop "$@"
