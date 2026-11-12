#!/usr/bin/env bash
# qnx-demo-live.sh — the whole on-target demo, on a loop, live.
#
# Runs the QNX 8.0 demo forever: boot -> drive the impact -> the crash is on
# the disk -> POWER CUT (SIGKILL on QEMU) -> reboot -> the journal recovers ->
# the event survived. The guest's serial console streams to a log, a renderer
# turns that log into a PNG every 2 seconds, and a local server shows it.
#
# Usage:
#   scripts/qnx-vm.sh build-image     (once: binaries + journal disk + IFS)
#   scripts/qnx-demo-live.sh          (then open http://127.0.0.1:8321/live.html)
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$(pwd)
VM=$ROOT/build/qnx-vm
mkdir -p "$VM"
IFS_BIN=$VM/ifs.bin
DISK=$VM/journal.raw
PORT=8321

[ -f "$IFS_BIN" ] || { echo "no image: run scripts/qnx-vm.sh build-image first"; exit 1; }
[ -f "$DISK" ] || { echo "no journal disk: run scripts/qnx-vm.sh build-image first"; exit 1; }

mark() { printf '\n=== %s — %s ===\n' "$(date +%H:%M:%S)" "$*" >> "$VM/live.log"; }

kill_stale() {
  python3 - <<'PY'
import os, signal
for entry in os.listdir('/proc'):
    if not entry.isdigit():
        continue
    try:
        cmd = open(f'/proc/{entry}/cmdline', 'rb').read().decode('utf-8', 'replace')
    except OSError:
        continue
    if ('qemu-system' in cmd and 'journal.raw' in cmd) or \
       ('qnx_bridge.py' in cmd and 'shell.sock' in cmd):
        try:
            os.kill(int(entry), signal.SIGKILL)
        except OSError:
            pass
PY
  rm -f "$VM"/console.sock "$VM"/shell.sock /tmp/.qemu-lock-*
  sleep 1
}

boot() {
  kill_stale
  : > "$VM/live.log"
  setsid nohup qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 \
    -kernel "$IFS_BIN" \
    -drive file="$DISK",format=raw,if=ide,cache=none,id=drv0 \
    -display none -vga none \
    -serial "unix:$VM/console.sock,server=on,wait=off" \
    -serial "unix:$VM/shell.sock,server=on,wait=off" > "$VM/qemu.log" 2>&1 &
  echo $! > "$VM/demo.pid"
  sleep 2
  setsid nohup python3 scripts/qnx_bridge.py "$VM/shell.sock" 4557 "$VM/live.log" \
    >/dev/null 2>&1 &
  mark "booting QNX 8.0"
}

wait_for() {
  local pattern="$1" seconds="$2"
  local deadline=$(( $(date +%s) + seconds ))
  while [ "$(date +%s)" -lt "$deadline" ]; do
    grep -aq "$pattern" "$VM/live.log" && return 0
    sleep 0.5
  done
  return 1
}

power_cut() {
  mark "POWER CUT — SIGKILL on QEMU: the 12 V rail is gone"
  kill -9 "$(cat "$VM/demo.pid")" 2>/dev/null
}

# --- the feed: a PNG every 2 s, served locally ----------------------------
cp image/live.html "$VM/live.html"
(
  while true; do
    python3 scripts/qnx-live-shot.py "$VM/live.log" "$VM/live.png" 2>/dev/null
    sleep 2
  done
) &
SHOT_PID=$!

setsid nohup python3 -m http.server "$PORT" --directory "$VM" >/dev/null 2>&1 &
SRV_PID=$!

trap 'kill "$SHOT_PID" "$SRV_PID" 2>/dev/null; kill_stale; exit 0' INT TERM

echo ""
echo "  LIVE  ->  http://127.0.0.1:$PORT/live.html"
echo "  (an image of the serial console, refreshed every 2 s; Ctrl-C to stop)"
echo ""

cycle=0
while true; do
  cycle=$((cycle + 1))
  boot
  mark "cycle $cycle — boot, crash, power cut, recovery"
  if wait_for "READY" 70; then
    sleep 4
    power_cut
    sleep 2
    kill_stale
    boot
    wait_for "SURVIVED THE POWER CUT" 70
    sleep 7
  fi
  kill_stale
  sleep 2
done