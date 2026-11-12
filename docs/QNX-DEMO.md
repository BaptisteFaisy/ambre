# QNX 8.0 live demo — bench and status

The SDP 8.0 is installed on the dev machine (`~/qnx800`, qcc 12.2.0), so the
five processes build for the real target (`make qnx`, see `docs/QNX-PORT.md`).
This file is the state of the **live on-target demo**: it boots QNX 8.0 under
QEMU and runs the recorder on a raw disk, with the power cut being a SIGKILL
on QEMU (spec §7.4).

## What provably works today

1. `make qnx` links the five binaries for `nto x86_64`.
2. `image/qemu.build` is a minimal boot image (IFS): the kernel, ksh, the PCI
   server, the five Ambre processes, a demo script. It builds with
   `mkifs` (see below) and **boots QNX 8.0 under QEMU**.
3. On target, the demo script (`image/demo-qnx.sh`, run from the IFS startup)
   executes and the `edrctl` binary runs (the guest prints the demo banners
   and the journal's diagnostics on the serial console).

## What blocks the full chain (the one remaining lever)

`edr-journal` starts but cannot open `/dev/hd0`: the guest's block driver does
not expose the QEMU disk. The minimal IFS carries `devb-eide` / `devb-ahci`,
`io-blk.so`, `libcam.so.2`, `cam-disk.so` and the PCI capabilities
`pci_cap-0x01/0x05/0x10/0x11`, but the device node never appears, so the
journal has nothing to open. On the SDP's own quickstart image (which has a
much larger module set), the same QEMU disk IS exposed and mounts — so the
minimal image is missing a module or a PCI capability, most likely for the
storage controller. Finding that one module is the next step; nothing else is
missing, the recorder itself runs.

## Bench commands (scripts/qnx-vm.sh)

```sh
source ~/qnx800/qnxsdp-env.sh
make qnx -B                                   # the five binaries
python3 host/scenarios/generate.py --preset urban-frontal --seed 2026 \
    --duration-ms 4000 --crash-at-ms 2500 --out build/qnx-vm/scenario.txt
# build the IFS:
SEARCH="$QNX_TARGET/x86_64:$QNX_TARGET/x86_64/boot/sys:$QNX_TARGET/x86_64/bin:$QNX_TARGET/x86_64/usr/bin:$QNX_TARGET/x86_64/sbin:$QNX_TARGET/x86_64/lib:$QNX_TARGET/x86_64/lib/dll:$QNX_TARGET/x86_64/usr/lib:$PWD/build/qnx:$PWD/build/qnx-vm"
sed "s|@SEARCH@|$SEARCH|" image/qemu.build > build/qnx-vm/qemu.build
mkifs -v build/qnx-vm/qemu.build build/qnx-vm/ifs.bin
# boot:
qemu-system-x86_64 -enable-kvm -cpu host -smp 2 -m 1024 \
    -kernel build/qnx-vm/ifs.bin \
    -drive file=build/qnx-vm/journal.raw,format=raw,if=ide,cache=none \
    -display none -vga none -serial unix:build/qnx-vm/console.sock,server=on,wait=off \
    -serial unix:build/qnx-vm/shell.sock,server=on,wait=off
# the demo runs on the second serial (socat UNIX-CONNECT:shell.sock TCP-LISTEN:4556):
socat UNIX-CONNECT:build/qnx-vm/console.sock TCP-LISTEN:4555,reuseaddr,fork &
socat UNIX-CONNECT:build/qnx-vm/shell.sock  TCP-LISTEN:4556,reuseaddr,fork &
# watch:     scripts/qnx_console.py 4556
# read:      scripts/qnx_bridge.py build/qnx-vm/shell.sock 4556 build/qnx-vm/shell.log
```

`scripts/qnx_console.py` logs in over the serial console (the demo image has no
login) and runs commands; `scripts/qnx_bridge.py` is the one-shot-stream bridge
between a QEMU unix socket and a TCP port.

## Honesty

The journal disk is `build/qnx-vm/journal.raw` (64 MiB, `cache=none`, no file
system). The power cut is `kill -9` on the QEMU pid. Nothing here emulates the
12 V rail: whatever the guest had not flushed through the block driver is gone,
exactly as the spec's level-2 essay states. Until the block-driver module is
found, the chain on target stops at "journal cannot open the device": the
recovery-after-power-cut part is not yet demonstrated on target, and it stays
`BLOCKED` in the validation pages until it is.