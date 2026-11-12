#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# diagnose.sh — what does the guest see? (QNX 8.0 demo image)
#
# The journal disk is attached as an IDE device; the QNX 8.0 legacy driver
# (devb-ahci, which carries its own block layer) is started here by hand so
# that every error it prints is visible. There is no grep in this image, so
# the output is printed as it comes.

echo "=== devb-ahci starts ==="
devb-ahci
echo "=== rc=$? ==="
echo "=== the processes now running ==="
pidin -F "a b"
echo "=== the journal starts on the raw device /dev/hd0 ==="
/proc/boot/edr-journal --disk /dev/hd0 --listen /dev/edr/journal --mode continuous --segment-limit 4
echo "=== rc=$? ==="
echo "=== the end ==="
while true; do
    read _x
done
