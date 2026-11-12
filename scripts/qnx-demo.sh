#!/usr/bin/env bash
# qnx-demo.sh — the live demo, end to end, on the real target.
#
#   boot QNX 8.0 under QEMU with the Ambre image
#   -> the demo script drives capture -> trigger -> acquire
#   -> POWER CUT: SIGKILL on QEMU (the 12 V rail is gone)
#   -> reboot: the journal recovers the partition (REP-01)
#   -> edrctl list shows what survived
#
# The console is a TCP socket (the guest's second serial port), so the whole
# session is readable while it happens.
#
# Usage: scripts/qnx-demo.sh
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
VM=$ROOT/build/qnx-vm
PORT=4556
QEMU_LOG=$VM/qemu.log

mkdir -p "$VM"

log() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

boot_vm() {
    rm -f /tmp/.qemu-lock* "$VM/console.sock" "$VM/shell.sock"
    setsid nohup qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 \
        -kernel "$VM/ifs.bin" \
        -drive file="$VM/journal.raw",format=raw,if=ide,cache=none,id=drv0 \
        -display none -vga none \
        -serial "unix:$VM/console.sock,server=on,wait=off" \
        -serial "unix:$VM/shell.sock,server=on,wait=off" \
        > "$QEMU_LOG" 2>&1 &
    echo $! > "$VM/demo.pid"
    sleep 3
    setsid nohup socat "UNIX-CONNECT:$VM/shell.sock" \
        "TCP-LISTEN:$PORT,reuseaddr,fork" > "$VM/bridge.log" 2>&1 &
    echo "   QEMU pid $(cat "$VM/demo.pid"), console on 127.0.0.1:$PORT"
}

capture() {
    local seconds="$1" out="$2"
    python3 - "$PORT" "$out" "$seconds" <<'PY'
import socket, sys, time
port, out, seconds = int(sys.argv[1]), sys.argv[2], float(sys.argv[3])
try:
    s = socket.create_connection(('127.0.0.1', port), timeout=5)
except OSError as exc:
    open(out, 'w').write(f"(no console: {exc})\n")
    raise SystemExit
s.settimeout(0.5)
end = time.time() + seconds
with open(out, 'wb') as log:
    while time.time() < end:
        try:
            data = s.recv(65536)
        except socket.timeout:
            continue
        if not data:
            break
        log.write(data)
        log.flush()
PY
}

show() {
    tr -d '\r' < "$1" | grep -vE '^\s*$' | sed 's/^/   /'
}

power_cut() {
    local pid
    pid=$(cat "$VM/demo.pid")
    log "POWER CUT — SIGKILL on QEMU (pid $pid)"
    kill -9 "$pid"
    sleep 1
    if kill -0 "$pid" 2>/dev/null; then echo "   QEMU survived"; else echo "   the machine is off"; fi
}

# Kill any QEMU still holding the journal disk (its write lock would stop the
# next boot). Matching on /proc, not on a command-line pattern, so this script
# never matches itself.
kill_stale() {
    python3 - <<'PY'
import os, signal
for entry in os.listdir('/proc'):
    if not entry.isdigit():
        continue
    try:
        with open(f'/proc/{entry}/cmdline', 'rb') as fh:
            cmd = fh.read().decode('utf-8', 'replace')
    except OSError:
        continue
    if ('qemu-system' in cmd and 'journal.raw' in cmd) or \
       ('socat' in cmd and ('shell.sock' in cmd or 'console.sock' in cmd)):
        try:
            os.kill(int(entry), signal.SIGKILL)
            print(f"   killed the stale QEMU {entry}")
        except OSError:
            pass
PY
    sleep 1
}

[ -f "$VM/ifs.bin" ] || { echo "no IFS: build it first (see docs/QNX-DEMO.md)"; exit 1; }
[ -f "$VM/journal.raw" ] || { echo "no journal disk: build/qnx-vm/journal.raw"; exit 1; }

log "BOOT 1 — the drive, the impact, the acquisition"
kill_stale
boot_vm
capture 55 "$VM/run1.log"
show "$VM/run1.log"

power_cut

log "BOOT 2 — same disk, the journal recovers what the crash left"
kill_stale
boot_vm
capture 55 "$VM/run2.log"
show "$VM/run2.log"

if grep -q "READY" "$VM/run1.log" && grep -q "SURVIVED THE POWER CUT" "$VM/run2.log"; then
    log "RESULT: the event survived the power cut, on QNX 8.0, for real"
    exit 0
fi
log "RESULT: incomplete (see the transcript above)"
exit 1
