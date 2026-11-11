# Building the QNX target without a QNX SDP — what is proven, what is not

Status: 2026-11-11. No QNX SDP 8.0 is installed on the development machine or
on the build server, and none could be obtained during week 1. Rather than
ship unverifiable QNX code, the target layer is built the way the sibling
project `passerelle` (same author, Apache-2.0) already proved works:

1. **All logic is portable C11/C++17** and runs, is tested and is measured on
   the PC against a POSIX backend.
2. **Everything QNX-specific is isolated in `*_qnx.c` files** that include the
   real QNX headers (`<sys/dispatch.h>`, `<sys/iofunc.h>`, `<sys/resmgr.h>`,
   `<sys/neutrino.h>`, `<sys/slog2.h>`, `<sys/trace.h>`, `<sys/dcmd_blk.h>`).
3. Those glue files are **compiled on the PC against stub headers**
   (`target/qnx-stub/include/`, imported from `passerelle` and attributed in
   `NOTICE`). The stubs declare the real types, prototypes and constants with
   their QNX 8.0 documentation provenance, but **have no bodies**.

## What each build proves

| Build | What it proves | What it does NOT prove |
|---|---|---|
| `make` (PC, POSIX backend) | the whole functional chain: capture → trigger → journal → power cut → recovery → MCAP export, with real processes, real files, real `SIGKILL` cuts | nothing about QNX scheduling, drivers or the kernel |
| `make qnx-syntax` (PC, stub headers) | the QNX glue **compiles**: no typo, no wrong prototype, no wrong struct field, no wrong argument type | that it *runs*, that the message numbers and `IOFUNC_*` bits have the expected values, that the kernel grants the abilities we ask for |
| QNX 8.0 target (needs the SDP) | everything above, plus feasibility essays E1–E4, real latency numbers, kernel traces, security-policy enforcement | — |

The stubs are a **substitute for building on the PC only**. Every stub header
says so in its first comment. Nothing in `target/qnx-stub/` is shipped in an
image, and no stub is ever linked into a binary: `make qnx-syntax` produces
object files only, so a missing QNX symbol can never break the PC build.

## Consequences for the campaign numbers

Every published number carries its backend. The PC numbers are real
measurements of the software path (integration, encoding, CRC, table commits,
recovery, exploration of cut states) and are labelled `backend=posix`. The
numbers that only a QNX target can produce — PERF-02 end-to-end freeze latency
including media time, PERF-07 kernel traces, E1 flush propagation, E3 VM cycle
time, SEC-01 policy enforcement — are published as **not measured**, with the
requirement id they answer, rather than estimated.

## What is needed to finish the target

```sh
# 1. install QNX SDP 8.0 (QNX Everywhere licence), then:
source ~/qnx800/qnxsdp-env.sh
# 2. build the five processes for QEMU x86_64
make qnx CC=qcc ARCH=x86_64
# 3. build the boot image and run it under QEMU
mkqnximage -f image/ambre.build -o build/ambre.img
qemu-system-x86_64 -drive file=build/ambre.img,if=virtio ...
# 4. run the feasibility essays and the real power-cut campaign
python3 host/powercut/e1_flush_probe.py --backend qemu --image build/ambre.img
python3 host/powercut/powercut_campaign.py --backend qemu --image build/ambre.img --cycles 200
```

The build files, the security policy and the image fragments under `image/`
are written for those commands; they are reviewed by inspection until an SDP
is available.
