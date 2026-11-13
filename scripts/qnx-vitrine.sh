#!/usr/bin/env bash
# qnx-vitrine.sh — the on-target demo, live, behind a web vitrine.
#
# One loop per cycle, forever:
#   boot QNX 8.0 (VGA console + serial) -> the demo drives the crash
#   -> POWER CUT (SIGKILL on QEMU) -> reboot -> the journal recovers
#   -> the event survived -> next cycle.
#
# The guest writes its serial console to build/qnx-vitrine/console.log, and
# its VGA screen is captured through the QEMU monitor socket. Together they
# feed the vitrine (see systemd/ambre-vitrine-web.service, which runs the
# shared ~/Bureau/qnx-emu/web.py). This script only runs the machine: the
# HTTP layer is a separate unit.
#
# Usage:
#   scripts/qnx-vitrine.sh build      one-shot: make the IFS + journal disk
#   scripts/qnx-vitrine.sh run        the loop (foreground; systemd keeps it alive)
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
VM=$ROOT/build/qnx-vitrine
IFS_BIN=$VM/ifs.bin
DISK=$VM/journal.raw
LOG=$VM/console.log
MON=$VM/mon.sock

mkdir -p "$VM"

mark() { printf '\n=== %s — %s ===\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }

# Kill any QEMU still holding the vitrine journal (its write lock would stop
# the next boot). Matched on /proc, never on a command-line pattern, so this
# script never matches itself.
kill_stale() {
    python3 - "$VM" <<'PY'
import os, signal, sys
vm = sys.argv[1]
for entry in os.listdir('/proc'):
    if not entry.isdigit():
        continue
    try:
        with open(f'/proc/{entry}/cmdline', 'rb') as fh:
            cmd = fh.read().decode('utf-8', 'replace')
    except OSError:
        continue
    if 'qemu-system' in cmd and f'{vm}/journal.raw' in cmd:
        try:
            os.kill(int(entry), signal.SIGKILL)
        except OSError:
            pass
PY
    rm -f "$MON" /tmp/.qemu-lock-*
    sleep 1
}

boot() {
    rm -f "$MON"
    setsid nohup qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 \
        -kernel "$IFS_BIN" \
        -drive file="$DISK",format=raw,if=ide,cache=none,id=drv0 \
        -object rng-random,filename=/dev/urandom,id=rng0 \
        -device virtio-rng-pci,rng=rng0 \
        -display none -vga std \
        -chardev file,id=s0,path="$LOG",append=on -serial chardev:s0 \
        -chardev file,id=s1,path="$LOG",append=on -serial chardev:s1 \
        -monitor "unix:$MON,server,nowait" \
        > "$VM/qemu.log" 2>&1 &
    echo $! > "$VM/qemu.pid"
}

wait_for() {
    local pattern="$1" seconds="${2:-70}" deadline
    deadline=$(( $(date +%s) + seconds ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        grep -aq "$pattern" "$LOG" && return 0
        sleep 0.5
    done
    return 1
}

power_cut() {
    mark "POWER CUT — SIGKILL on QEMU: the 12 V rail is gone"
    kill -9 "$(cat "$VM/qemu.pid")" 2>/dev/null
    sleep 1
}

fresh_disk() {
    # A blank 64 MiB raw disk with ONE partition of type 177 (B1) and no file
    # system: the recorder owns the raw partition (STO-01). Truncating to the
    # same size erases nothing, hence the truncate to 0 first.
    #
    # The partition table is written by scripts/qnx-mbr.py (stdlib only), NOT
    # by sfdisk: sfdisk is ABSENT on the r630, and a disk with no table gives
    # no /dev/hd0t177 — the journal never starts and the demo plays a fake
    # "READY" with nothing recorded on the media.
    truncate -s 0 "$DISK"
    truncate -s 64M "$DISK"
    python3 "$ROOT/scripts/qnx-mbr.py" "$DISK" 64M 2048 0xB1 || {
        echo "vitrine: table de partitions impossible — arrêt (rien ne serait enregistré)" >&2
        return 1
    }
}

build() {
    # The five binaries live in build/qnx (make qnx); the image itself is the
    # VGA-enabled IFS built from image/vitrine.build, same block stack.
    # The SDP 8.0 environment (qcc, mkifs, QNX_TARGET) — the Makefile and
    # mkifs both need it.
    if ! command -v qcc >/dev/null 2>&1; then
        . "$HOME/qnx800/qnxsdp-env.sh" >/dev/null
    fi
    make -s qnx >/dev/null 2>&1 || make qnx
    mkdir -p "$VM"
    python3 host/scenarios/generate.py --preset urban-frontal --seed 2026 \
        --duration-ms 4000 --crash-at-ms 2500 --out "$VM/scenario.txt" >/dev/null
    cp image/demo-qnx.sh image/diagnose.sh "$VM/"
    fresh_disk
    local T="$QNX_TARGET/x86_64"
    local SEARCH="$T:$T/boot/sys:$T/bin:$T/usr/bin:$T/sbin:$T/lib:$T/lib/dll:$T/usr/lib:$PWD/build/qnx:$PWD/build/qnx-vitrine"
    sed "s|@SEARCH@|$SEARCH|" image/vitrine.build > "$VM/vitrine.build"
    mkifs -v "$VM/vitrine.build" "$IFS_BIN" >/dev/null
    echo "built $IFS_BIN ($(du -h "$IFS_BIN" | cut -f1))"
}

run() {
    [ -f "$IFS_BIN" ] || build
    [ -f "$DISK" ] || build
    # First light nothing is there: build if the IFS is missing.
    echo "vitrine: ifs=$IFS_BIN disk=$DISK log=$LOG mon=$MON"
    cycle=0
    while true; do
        cycle=$((cycle + 1))
        kill_stale
        # A fresh journal for every cycle, so the crash is re-acquired each
        # time, and a single console.log that spans boot1-cut-boot2.
        fresh_disk || exit 1
        : > "$LOG"
        mark "cycle $cycle — boot 1: drive the crash"
        boot
        if wait_for "READY" 90; then
            # Leave the acquired event on the screen long enough to be read.
            sleep 12
            power_cut
            sleep 2
        fi
        kill_stale
        mark "cycle $cycle — boot 2: the journal recovers"
        boot
        wait_for "SURVIVED THE POWER CUT" 90
        sleep 18
    done
}

case "${1:-run}" in
    build) build ;;
    run)   run ;;
    *)     echo "usage: $0 [build|run]" >&2; exit 1 ;;
esac