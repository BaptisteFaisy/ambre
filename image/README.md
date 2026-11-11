# Ambre QNX 8.0 Boot Image

This directory holds the pieces of the QNX 8.0 boot image for the Ambre Event
Data Recorder (target: QEMU `x86_64`): the IFS buildfile, the QNX buildfile of
the five processes, the security policy, and the supervision wrapper.

> **Status without a QNX SDP.** No QNX SDP 8.0 is installed on the development
> machine, so nothing here could be assembled with `mkqnximage`, compiled with
> `qcc`/`secpolcompile`, or booted under QEMU. The files are reviewed by
> inspection and checked statically on the PC by `scripts/check-image.sh`
> (`make image-check`). Everything that needs the SDP is marked
> `UNVERIFIED (needs QNX SDP 8.0)` in the files themselves, and listed in
> §5 below.

## Files

| File | Role | Requirements |
|---|---|---|
| `ambre.build` | IFS buildfile: kernel, drivers, the 5 processes, start-up order | QNX-07, STO-01, STO-07, OBS-01, SEC-06, VIE-03 |
| `buildfile` | QNX buildfile of the 5 processes: objects, libraries, §4.4 priorities | §4.4, QNX-04, VIE-03 |
| `ambre.sec` | QNX 8.0 security policy: types, provides, allows | SEC-01..SEC-03, SEC-06, VIE-03 |
| `ambre-supervise.sh` | respawn wrapper (stand-in for the QNX HAM, see `docs/SECURITY.md` §5) | QNX-06, SEC-02, ROB-02/03/04 |

## 1. Building the image (needs QNX SDP 8.0)

```sh
# 1. SDP environment
source ~/qnx800/qnxsdp-env.sh

# 2. The five binaries for x86_64 (image/buildfile; see docs/QNX-PORT.md)
make qnx CC=qcc ARCH=x86_64          # UNVERIFIED: target `qnx` wiring

# 3. The security policy
secpolcompile -o ambre.sec.bin image/ambre.sec

# 4. The boot image. Two equivalent routes:
#    a) direct, with ambre.build as the buildfile:
mkqnximage -f image/ambre.build -o build/ambre.img
#    b) the snippet route of an existing mkqnximage workspace:
#       mkqnximage --type=qemu --arch=x86_64 --graphics=no --noprompt
#       then append the file list of image/ambre.build to
#       local/snippets/system_files.custom, its start-up lines to
#       local/snippets/post_start.custom, and: mkqnximage --build --noprompt
```

The binaries and the SDP files named in `ambre.build` (`procnto-smp-instr`,
`devb-virtio`, `fs-qnx6.so`, `io-blk.so`, the `lib*` list) are resolved by
`mkqnximage` from `$QNX_TARGET` and from the staging directory; stage the five
Ambre binaries under the names used in the file list (`edr-journal`, ...).

## 2. The two storage devices (STO-01, STO-07)

Ambre uses **two separate virtio-blk disks**, so the journal partition and the
archive partition are physically distinct:

| QEMU disk | QNX device | Content | Owner |
|---|---|---|---|
| disk 0, partition type QNX (`t177`) | `/dev/hd0t177` | **raw journal**, no file system (STO-01): 48 MiB layout of `docs/ARCHITECTURE.md` §3 (2 table copies + 24 x 2 MiB segments) | `edr-journal` only |
| disk 1, partition type QNX (`t177`) | `/dev/hd1t177` | **Power-Safe** archive (STO-07), `fs-qnx6.so`, mounted on its device path; >= 128 MiB for 16+ MCAP events | `edr-archive` (write), `edrctl` (read) |

```sh
# Host-side preparation (sketch):
truncate -s 64M  build/ambre-journal.img    # raw journal disk
truncate -s 256M build/ambre-archive.img    # Power-Safe archive disk
```

Both images need a partition table with one QNX-type partition each, and the
archive partition needs a QNX6 file system. The host-side tools for that come
from the SDP (`mkqnx6fs` and the SDP disk utilities).
**UNVERIFIED (needs QNX SDP 8.0)**: the exact tool names/invocations to create
the partition tables and to format the archive partition from the host; and
that two `devb-virtio` instances enumerate as `hd0`/`hd1` in that QEMU slot
order.

## 3. Running under QEMU x86_64

```sh
qemu-system-x86_64 \
    -machine q35 -cpu max -smp 2 -m 1G \
    -nographic -nodefaults -no-user-config \
    -serial mon:stdio \
    -kernel build/ambre.img \
    -drive file=build/ambre-journal.img,if=none,id=jrn,format=raw \
    -device virtio-blk-pci,drive=jrn \
    -drive file=build/ambre-archive.img,if=none,id=arc,format=raw \
    -device virtio-blk-pci,drive=arc
```

No `-nic`, no `-net`: the guest has no network device, matching VIE-03 (the
image itself contains no network stack either). Machine/cpu flags follow the
QNX 8.0 x86_64 QEMU convention (`q35`, `-cpu max`).

Power-cut campaigns kill QEMU and restart it (`host/powercut/`): the journal
disk image is the medium that must survive, which is exactly what levels 1-2
of `docs/VALIDATION.md` measure.

## 4. Loading the security policy (SEC-01)

`ambre.build` ships the compiled policy at `/etc/system/secpol.bin` and the
start-up script applies it with `secpolpush` **before** the first EDR process
starts, so every process is created with its type.
**UNVERIFIED (needs QNX SDP 8.0)**: whether boot-time enforcement is done by
`secpolpush` in the start-up script or by a `procnto` option; confirm with the
SDP 8.0 System Security Guide and adjust the one line in `ambre.build`.

Intrusion test to run on target (proof for SEC-01): from the boot shell, try
to `open /dev/hd0t177` and to connect to `/dev/edr/in` with an untyped
process; both must fail with `EPERM`.

## 5. What could NOT be verified without QNX SDP 8.0

1. `mkqnximage`/`mkifs` assembly of `ambre.build`: every `$QNX_TARGET` file
   path, the image size, and bootability under QEMU.
2. `devb-virtio` invocation and the `io-blk.so` validation levels (cahier
   §14 lists them as documented in QNX 8.0; the exact option spelling must be
   taken from the io-blk.so manual page), and the resulting device node names
   (`/dev/hd0t177`, `/dev/hd1t177`).
3. `fs-qnx6.so` mount syntax (device-path mount point assumed, the classic
   QNX idiom) and host-side formatting of the archive partition.
4. `image/buildfile`: the `../../mkfiles/pre.o` recursive-make machinery, the
   `[name=... priority=...]` syntax, and the C++ runtime library name for
   `edr-archive`.
5. `secpolcompile` acceptance of `image/ambre.sec`: the `type`, `allow
   self:ability` and `allow_attach` forms come from a policy that compiles
   (passerelle), but the `provides` bindings and the `channel`/`dev`/`dir`/
   `file` rules need the SDP compiler as the acceptance test.
6. Boot-time policy loading (`secpolpush` in the start-up script vs a
   `procnto` option) and kernel enforcement (`EPERM` on the intrusion test).
7. QNX-07 measured on target: `edr-journal` serving `/dev/edr/journal` before
   `fs-qnx6.so` runs (kernel trace), and PERF-06 (capture < 1 s after kernel).
8. QNX HAM replacing `ambre-supervise.sh` (see `docs/SECURITY.md` §5) and the
   ROB-04 respawn budget (< 100 ms).

## Provenance

Structure of `ambre.build` (boot section, start-up script, PCI fragment,
library/binary lists) adapted from the sibling project `passerelle`
(https://github.com/BaptisteFaisy/passerelle, Apache-2.0, same author) and
from the QNX 8.0 x86_64 reference IFS of the QNX virtio project (MIT,
Abe Kohandel). Statement forms of `ambre.sec` (`type`, `allow self:ability`,
`allow_attach`) come from `passerelle`'s compiled policy. All Ambre-specific
content (processes, storage split, priorities, access matrix) is original and
serves the requirements cited in the comments. See `NOTICE`.
