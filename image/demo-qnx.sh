#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# demo-qnx.sh — the live demo, running INSIDE the QNX 8.0 guest.
#
# The recorder owns /dev/hd0t177, a raw partition with no file system and no
# partition table (STO-01). The capture feeds it from a replay of the drive;
# when the impact passes the UN Regulation No. 160 threshold, the EMERG
# record goes out in ONE contiguous write, the allocation table is committed
# and the flush is acknowledged: the event is ACQUIRED, and the black box
# holds the pre-crash class A data.
#
# The power cut is NOT in this script: it is a SIGKILL on QEMU, sent by the
# host (scripts/qnx-vm.sh powercut). After the machine comes back up, this
# same script runs again, the journal recovers the partition (REP-01) and
# edrctl list shows what survived.
#
# The demo image has no file system and no coreutils: only the shell builtins
# (echo) and the five Ambre binaries are available. The wait is a poll on the
# journal's own answer — every edrctl call is a round trip, so the loop costs
# real time.

JOURNAL=/dev/edr/journal
DISK=/dev/hd0t177
CTL=/proc/boot/edrctl

banner() {
    echo ''
    echo '=============================================================='
    echo "== $*"
    echo '=============================================================='
}

# Wait until the journal answers (or give up after a bounded number of
# round trips).
wait_ready() {
    i=0
    while [ "$i" -lt 400 ]; do
        if $CTL --journal $JOURNAL status > /dev/null 2>&1; then
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

# Wait until the journal lists an event.
wait_event() {
    i=0
    while [ "$i" -lt 800 ]; do
        if [ -n "$($CTL --journal $JOURNAL list 2>/dev/null)" ]; then
            return 0
        fi
        i=$((i + 1))
    done
    return 1
}

banner "Ambre EDR — live on QNX 8.0"
echo "journal partition : $DISK  (raw, no file system)"
echo "service path      : $JOURNAL"

banner "1. edr-journal starts: it opens the partition and runs the recovery (REP-01)"
/proc/boot/edr-journal --disk $DISK --listen $JOURNAL --mode continuous --segment-limit 4 &
wait_ready

banner "2. edrctl status — the service answers"
$CTL --journal $JOURNAL status
echo "-- what the journal logged (slogger2 buffer):"
slog2info

BEFORE=$($CTL --journal $JOURNAL list 2>/dev/null)
if [ -z "$BEFORE" ]; then
    banner "3. first boot: the partition is empty, so we drive the crash"
    echo '-- edr-capture starts (priority 61) and listens for the frames'
    /proc/boot/edr-capture --journal $JOURNAL --listen /dev/edr/in --profile eu &
    i=0
    while [ "$i" -lt 200 ]; do
        i=$((i + 1))
    done
    echo '-- edr-replay: 4 s of driving, frontal impact at 2.5 s'
    /proc/boot/edr-replay --capture /dev/edr/in --scenario /proc/boot/scenario.txt &
    wait_event
    banner "4. edrctl list — the event the recorder acquired"
    $CTL --journal $JOURNAL list
    banner "5. edrctl status after the impact"
    $CTL --journal $JOURNAL status
    banner "READY — the crash is on the media. The host now cuts the power (SIGKILL on QEMU)."
else
    banner "3. AFTER THE POWER CUT — the journal recovered the partition at boot"
    echo '-- what the black box still holds:'
    $CTL --journal $JOURNAL list
    banner "4. edrctl status after the recovery"
    $CTL --journal $JOURNAL status
    banner "THE EVENT SURVIVED THE POWER CUT"
fi

echo ''
echo '-- the demo stays alive; cut the power whenever you want.'
while true; do
    read _x
done