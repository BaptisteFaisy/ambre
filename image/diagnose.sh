#!/bin/sh
# diagnose.sh — reproduce the refusal and dump what the journal logged.

echo "=== journal ==="
/proc/boot/edr-journal --disk /dev/hd0t177 --listen /dev/edr/journal --mode continuous --segment-limit 4 &
i=0
while [ "$i" -lt 400 ]; do
    /proc/boot/edrctl --journal /dev/edr/journal status > /dev/null 2>&1 && break
    i=$((i + 1))
done
echo "=== edrctl status ==="
/proc/boot/edrctl --journal /dev/edr/journal status
echo "=== edrctl list ==="
/proc/boot/edrctl --journal /dev/edr/journal list
echo "=== what the journal logged ==="
slog2info
while true; do
    read _x
done
