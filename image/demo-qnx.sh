#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# demo-qnx.sh — the live demo, running INSIDE the QNX 8.0 guest.
#
# The recorder owns /dev/hd0, a raw device with no file system and no
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
# (echo, printf, read -t) and the five Ambre binaries are available.

JOURNAL=/dev/edr/journal
DISK=/dev/hd0
CTL=/proc/boot/edrctl

banner() {
    echo ''
    echo '=============================================================='
    echo "== $*"
    echo '=============================================================='
}

# There is no sleep(1) in this image. The wait is a poll: every edrctl call is
# a round trip to the journal, so the loop costs real time.
nap() {
    n="${1:-1}"
    i=0
    while [ "$i" -lt "$n" ]; do
        $CTL --journal $JOURNAL status > /dev/null 2>&1
        i=$((i + 1))
    done
}

banner "Ambre EDR — live on QNX 8.0"
echo "journal device : $DISK  (raw block device, no file system)"
echo "service path   : $JOURNAL"

banner "1. edr-journal starts: it opens the partition and runs the recovery (REP-01)"
/proc/boot/edr-journal --disk $DISK --listen $JOURNAL --mode continuous --segment-limit 4 &
nap 3

banner "2. edrctl status — the service answers"
$CTL --journal $JOURNAL status

BEFORE=$($CTL --journal $JOURNAL list 2>/dev/null)
if [ -z "$BEFORE" ]; then
    banner "3. first boot: the partition is empty, so we drive the crash"
    echo '-- edr-capture starts (priority 61) and listens for frames'
    /proc/boot/edr-capture --journal $JOURNAL --listen /dev/edr/in --profile eu &
    nap 2
    echo '-- edr-replay: 4 s of driving, frontal impact at 2.5 s'
    /proc/boot/edr-replay --capture /dev/edr/in --scenario /proc/boot/scenario.txt &
    nap 8
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
    nap 5
done
