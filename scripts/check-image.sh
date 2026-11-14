#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# scripts/check-image.sh - Static verification of QNX image, buildfile,
# security policy, and supervision files without a QNX SDP installation.
#
# Exit code: 0 if all checks pass, 1 on any failure.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FAILS=0

pass() {
    printf '  OK   %s\n' "$1"
}

fail() {
    printf '  FAIL %s\n' "$1" >&2
    FAILS=$((FAILS + 1))
}

check_required_files() {
    local f
    for f in image/ambre.build image/buildfile image/ambre.sec \
             image/ambre-supervise.sh image/README.md docs/SECURITY.md \
             target/port/ambre_port.h Makefile; do
        [ -f "$f" ] || fail "missing required file: $f"
    done
    [ -x image/ambre-supervise.sh ] || fail "image/ambre-supervise.sh is not executable"
    pass "required image/security files exist"
}

check_processes_in_makefile() {
    local procs proc
    procs=$(grep -E '^\[perms=0755\] /(proc/boot|usr/bin)/edr' image/ambre.build \
        | sed -E 's#.*/(edr[-a-z]+)=.*#\1#' | sort -u)
    if [ -z "$procs" ]; then
        fail "no edr processes found in image/ambre.build"
        return
    fi
    grep -q '\$(BUILD)/edr-%:' Makefile || {
        fail "Makefile missing \$(BUILD)/edr-% target rule"
        return
    }
    for proc in $procs; do
        if ! grep -qE "^\[name=${proc} " image/buildfile; then
            fail "process '$proc' from ambre.build missing in image/buildfile"
            continue
        fi
        pass "process target '$proc' wired in Makefile (edr-%) and image/buildfile"
    done
}

extract_buildfile_prio() {
    local name="$1"
    grep -E "^\[name=${name} " image/buildfile \
        | sed -nE 's/.*priority=([0-9]+).*/\1/p' | head -n1
}

check_prio_rule() {
    local name="$1" expected_re="$2" label="$3"
    local prio
    prio=$(extract_buildfile_prio "$name")
    if ! printf '%s\n' "$prio" | grep -qE "^(${expected_re})$"; then
        fail "image/buildfile priority for $name is '$prio' (expected §4.4: $label)"
        return 1
    fi
    return 0
}

check_priorities_match_spec() {
    local ok=1
    check_prio_rule "edr-capture" "60|61" "60-61" || ok=0
    check_prio_rule "edr-journal" "62" "62" || ok=0
    check_prio_rule "edr-archive" "8" "8" || ok=0
    check_prio_rule "edrctl" "10" "10" || ok=0
    check_prio_rule "edr-replay" "50" "50" || ok=0

    # Also verify that the IFS startup script comments explicitly state the
    # target priorities (which are applied at startup via ambre_port_set_priority).
    grep -qE 'edr-journal.*priority=62' image/ambre.build || {
        fail "image/ambre.build missing priority=62 annotation for edr-journal"; ok=0
    }
    grep -qE 'edr-capture.*priority=60-61' image/ambre.build || {
        fail "image/ambre.build missing priority=60-61 annotation for edr-capture"; ok=0
    }
    grep -qE 'edr-archive.*priority=8' image/ambre.build || {
        fail "image/ambre.build missing priority=8 annotation for edr-archive"; ok=0
    }
    [ "$ok" -eq 1 ] && pass "process priorities match spec §4.4 (capture 60-61, journal 62, archive 8, edrctl 10, replay 50)"
}

check_secpol_peer_types() {
    local enums sym ok=1
    enums=$(awk '/typedef enum \{/,/\} ambre_peer_type_t;/' target/port/ambre_port.h \
        | grep -oE 'AMBRE_PEER_[A-Z]+' | sort -u)
    if [ -z "$enums" ]; then
        fail "could not parse ambre_peer_type_t from target/port/ambre_port.h"
        return
    fi
    for sym in $enums; do
        if ! grep -q "$sym" image/ambre.sec; then
            fail "ambre_peer_type_t member '$sym' missing from image/ambre.sec"
            ok=0
        fi
    done
    for sym in edr_journal_t edr_capture_t edr_producer_t edr_service_t edr_archive_t; do
        if ! grep -q "^type ${sym};" image/ambre.sec; then
            fail "type ${sym} missing in image/ambre.sec"
            ok=0
        fi
    done
    [ "$ok" -eq 1 ] && pass "all ambre_peer_type_t types mapped in image/ambre.sec"
}

check_no_network_in_image() {
    local hits
    # VIE-03: no file under image/ may reference a network command or stack.
    # Comment lines (starting with '#') may name the forbidden tools only to
    # document their absence.
    hits=$(grep -rnE '\b(io-sock|io-pkt|dhclient|ifconfig|dhcp\.client|sshd|inetd|pfctl|ftp|telnetd)\b' \
        image/ \
        | grep -vE '^[a-zA-Z0-9_./-]+:[0-9]+:[[:space:]]*#' || true)
    if [ -n "$hits" ]; then
        fail "network command/stack found in image/ (violates VIE-03): $hits"
        return
    fi
    if grep -qE '^LIBS_(C|CXX)[[:space:]]*=.*socket' image/buildfile; then
        fail "socket library linked in image/buildfile (violates VIE-03)"
        return
    fi
    pass "no network stack, daemon, or socket library in image/ (VIE-03)"
}

check_distinct_partitions_and_boot_order() {
    local jdev adev jline mline
    # Journal raw disk is /dev/hd0t177; Power-Safe archive is /dev/hd1t177.
    # The --disk argument sits on the continuation line of the edr-journal
    # launch, so match the argument itself (there is exactly one in the file).
    jdev=$(grep -E -- '--disk /dev/hd' image/ambre.build | grep -oE '/dev/hd[0-9]+t[0-9]+' | head -n1)
    adev=$(grep -E 'fs-qnx6\.so' image/ambre.build | grep -oE '/dev/hd[0-9]+t[0-9]+' | head -n1)
    if [ -z "$jdev" ] || [ -z "$adev" ] || [ "$jdev" = "$adev" ]; then
        fail "raw journal partition ($jdev) and Power-Safe archive ($adev) are not distinct (STO-01/STO-07)"
        return
    fi
    grep -q '/usr/lib/io-blk\.so' image/ambre.build || { fail "io-blk.so missing in image/ambre.build"; return; }
    grep -q '/usr/lib/fs-qnx6\.so' image/ambre.build || { fail "fs-qnx6.so missing in image/ambre.build"; return; }
    jline=$(grep -n 'edr-journal' image/ambre.build | head -n1 | cut -d: -f1)
    mline=$(grep -n 'fs-qnx6\.so' image/ambre.build | head -n1 | cut -d: -f1)
    if [ -z "$jline" ] || [ -z "$mline" ] || [ "$jline" -ge "$mline" ]; then
        fail "edr-journal must start BEFORE fs-qnx6.so (QNX-07)"
        return
    fi
    if grep -E ":dev\b" -B1 image/ambre.sec | grep -E "allow " | grep -qv "edr_journal_t"; then
        fail "non-journal type granted raw device access in image/ambre.sec (SEC-01)"
        return
    fi
    pass "partitions distinct ($jdev raw vs $adev qnx6), edr-journal starts before mount (STO-01/STO-07/QNX-07)"
}

check_no_disable_command() {
    local hits
    hits=$(grep -rnE '\b(edr-stop|edr-disable|edrctl[[:space:]]+(stop|disable)|AMBRE_MSG_(STOP|DISABLE|ERASE))\b' \
        image/ target/edr_ipc.h \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#' || true)
    if [ -n "$hits" ]; then
        fail "recorder stop/disable command found (violates SEC-02): $hits"
        return
    fi
    pass "no recorder stop/disable command exists (SEC-02)"
}

main() {
    echo "=== Ambre static image & security check ==="
    check_required_files
    check_processes_in_makefile
    check_priorities_match_spec
    check_secpol_peer_types
    check_no_network_in_image
    check_distinct_partitions_and_boot_order
    check_no_disable_command
    if [ "$FAILS" -ne 0 ]; then
        printf 'IMAGE CHECK FAILED (%d error(s))\n' "$FAILS" >&2
        exit 1
    fi
    # Ce script juge l'image PRODUIT (image/ambre.build) SANS l'assembler : ce
    # qu'il prouve est donc la cohérence interne (processus, priorités, politique,
    # partitions), pas un démarrage. Le distingo importe, parce que l'image de
    # DÉMO, elle, est assemblée et amorcée pour de vrai : `mkifs` sur
    # image/vitrine.build puis QEMU, avec les cinq processus qui enregistrent un
    # choc sur un média brut et le récupèrent après une coupure
    # (docs/QNX-DEMO.md). Rien ici ne doit être lu comme l'un ou l'autre.
    echo "IMAGE CHECK PASSED (statique sur l'image PRODUIT; l'image de démo, elle, est assemblée par mkifs et amorcée sous QEMU — docs/QNX-DEMO.md)"
    exit 0
}

main "$@"
