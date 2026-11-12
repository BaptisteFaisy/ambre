#!/usr/bin/env bash
# qnx-vm.sh — the QNX 8.0 test bench: one virtual machine, one raw journal
# disk, and a power cut that is a SIGKILL on QEMU (spec §7.4).
#
# The guest is the QNX SDP 8.0 quickstart image (prebuilt ifs.bin + disk), so
# nothing here rebuilds an IFS: the five Ambre binaries are pushed in on a
# payload disk and run exactly as they would from /proc/boot.
#
# Three disks, in this order:
#   hd0  the guest system (a plain copy of the SDP disk, so the SDP's own
#        image is never written to and never locked by another VM)
#   hd1  the journal: a raw file with NO partition table and NO file system,
#        attached with cache=none (host O_DIRECT). That is what makes the power
#        cut honest — killing QEMU loses what the guest had not flushed.
#   hd2  the payload disk (raw): the binaries and the scenario, unpacked into
#        /data/ambre by the guest itself.
#
# The SDP image has no sshd, so the serial console (a TCP socket) is the
# control channel: scripts/qnx_console.py logs in and runs the commands.
#
# Usage:
#   scripts/qnx-vm.sh start        boot the VM and wait for the shell
#   scripts/qnx-vm.sh push         pack the binaries + scenario onto the payload disk
#   scripts/qnx-vm.sh run '<cmd>'  run a command in the guest
#   scripts/qnx-vm.sh status       what the guest sees
#   scripts/qnx-vm.sh powercut     SIGKILL QEMU — the 12 V rail is gone
#   scripts/qnx-vm.sh console      the last lines of the console
#   scripts/qnx-vm.sh stop         stop the VM
#
# Environment: AMBRE_QNX_IMAGE (default ~/qnx800/images/qemu/qemu/output),
# AMBRE_VM_SERIAL_PORT (4555), AMBRE_VM_MEM (2048), AMBRE_VM_JOURNAL_MB (64).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
IMAGE_DIR="${AMBRE_QNX_IMAGE:-$HOME/qnx800/images/qemu/qemu/output}"
VM=$ROOT/build/qnx-vm
SERIAL_PORT="${AMBRE_VM_SERIAL_PORT:-4555}"
MEM="${AMBRE_VM_MEM:-2048}"
JOURNAL_MB="${AMBRE_VM_JOURNAL_MB:-64}"
PIDFILE=$VM/qemu.pid
SERIAL=$VM/console.log
JOURNAL=$VM/journal.raw
GUEST=$VM/guest-disk.raw
PAYLOAD=$VM/payload.raw
CONSOLE=$ROOT/scripts/qnx_console.py

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { printf 'qnx-vm: %s\n' "$*" >&2; exit 1; }

running() { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; }

guest() { python3 "$CONSOLE" "$SERIAL_PORT" --timeout "${GUEST_TIMEOUT:-5}" "$@"; }

cmd_start() {
    running && { log "already running (pid $(cat "$PIDFILE"))"; return 0; }
    [ -f "$IMAGE_DIR/ifs.bin" ] || die "no ifs.bin in $IMAGE_DIR (set AMBRE_QNX_IMAGE)"
    command -v qemu-system-x86_64 >/dev/null || die "qemu-system-x86_64 not found"
    mkdir -p "$VM"
    if [ ! -f "$JOURNAL" ]; then
        log "creating the raw journal disk: $JOURNAL_MB MiB, no partition, no file system"
        truncate -s "$((JOURNAL_MB * 1024 * 1024))" "$JOURNAL"
    fi
    if [ ! -f "$GUEST" ]; then
        log "copying the guest disk (sparse copy of the SDP image)"
        cp --sparse=always "$IMAGE_DIR/disk-qemu" "$GUEST"
    fi
    local payload_drive=""
    [ -f "$PAYLOAD" ] && payload_drive="-drive file=$PAYLOAD,format=raw,if=ide,id=drv2"
    rm -f "$VM/console.sock"
    log "booting QNX 8.0 (console on 127.0.0.1:$SERIAL_PORT)"
    # setsid so the VM survives the shell that started it, and no -daemonize:
    # the pid is the one we write ourselves.
    setsid nohup qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m "$MEM" \
        -kernel "$IMAGE_DIR/ifs.bin" \
        -drive file="$GUEST",format=raw,if=ide,id=drv0 \
        -drive file="$JOURNAL",format=raw,if=ide,cache=none,aio=native,id=drv1 \
        $payload_drive \
        -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
        -object rng-random,filename=/dev/urandom,id=rng0 -device virtio-rng-pci,rng=rng0 \
        -display none -vga none \
        -serial "unix:$VM/console.sock,server=on,wait=on" \
        > "$VM/qemu.log" 2>&1 &
    echo $! > "$PIDFILE"
    # The serial socket is a one-shot stream: a client that connects late sees
    # nothing. The bridge keeps ONE reader attached to the guest for the whole
    # life of the VM and hands the stream to whoever asks on the TCP port. It
    # retries until QEMU has created the socket (QEMU waits for a client
    # before it boots the guest).
    setsid nohup bash -c "for i in \$(seq 60); do
        socat UNIX-CONNECT:$VM/console.sock TCP-LISTEN:$SERIAL_PORT,reuseaddr,fork && exit 0
        sleep 1
    done" > "$VM/bridge.log" 2>&1 &
    echo $! > "$VM/bridge.pid"
    if guest --wait-login --timeout 12 >/dev/null 2>&1; then
        log "guest is up: $(guest 'uname -sr' | tail -1)"
        return 0
    fi
    die "the guest did not answer on the console (port $SERIAL_PORT)"
}

cmd_push() {
    [ -d build/qnx ] || die "no build/qnx — run: source ~/qnx800/qnxsdp-env.sh && make qnx"
    local scen="${1:-build/qnx-vm/scenario.txt}"
    if [ ! -f "$scen" ]; then
        log "generating $scen"
        python3 host/scenarios/generate.py --preset urban-frontal --seed 2026 \
            --duration-ms 4000 --crash-at-ms 2500 --out "$scen" >/dev/null
    fi
    rm -rf "$VM/stage"
    mkdir -p "$VM/stage"
    cp build/qnx/edr-journal build/qnx/edr-capture build/qnx/edr-replay \
       build/qnx/edrctl build/qnx/edr-archive "$VM/stage/"
    cp "$scen" "$VM/stage/scenario.txt"
    tar czf "$VM/payload.tgz" -C "$VM/stage" .
    truncate -s 4M "$PAYLOAD"
    dd if="$VM/payload.tgz" of="$PAYLOAD" conv=notrunc status=none
    log "payload written to the payload disk ($(du -h "$VM/payload.tgz" | cut -f1))"
    guest 'mkdir -p /data/ambre; dd if=/dev/hd2 bs=512 count=8000 2>/dev/null | tar xzf - -C /data/ambre; chmod +x /data/ambre/edr*; ls -l /data/ambre' \
        | tail -12
}

cmd_status() {
    if running; then
        log "running (pid $(cat "$PIDFILE")), console port $SERIAL_PORT"
        guest 'uname -sr; ls /dev/hd*; ls -l /data/ambre 2>/dev/null | head -8'
    else
        log "stopped"
    fi
}

cmd_powercut() {
    running || die "the VM is not running"
    local pid; pid=$(cat "$PIDFILE")
    log "POWER CUT: SIGKILL on qemu pid $pid — what the guest had not flushed is gone"
    kill -9 "$pid"
    sleep 1
    running && die "qemu survived SIGKILL"
    log "the machine is off"
}

cmd_console() { python3 "$CONSOLE" "$SERIAL_PORT" --wait-login; }

cmd_stop() {
    running || { log "already stopped"; return 0; }
    kill "$(cat "$PIDFILE")" 2>/dev/null || true
    for _ in $(seq 15); do running || break; sleep 1; done
    running && kill -9 "$(cat "$PIDFILE")" 2>/dev/null || true
    log "stopped"
}

case "${1:-help}" in
    start)    cmd_start ;;
    status)   cmd_status ;;
    push)     shift; cmd_push "$@" ;;
    powercut) cmd_powercut ;;
    console)  cmd_console ;;
    stop)     cmd_stop ;;
    run)      shift; guest "$@" ;;
    *) sed -n '2,32p' "$0" ;;
esac
