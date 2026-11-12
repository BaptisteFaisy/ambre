# Building the QNX target — what is proven, what is not

> **Update 2026-11-12: the QNX SDP 8.0 is installed on the development machine**
> (`~/qnx800`, qcc 12.2.0, `gcc_ntox86_64`), licensed by the project owner. The
> five target processes now **really compile and link** with it:
>
> ```sh
> source ~/qnx800/qnxsdp-env.sh && make qnx      # -> build/qnx/edr-* (ELF, x86-64, nto)
> ```
>
> What that proves: every line of `fmt/`, `target/ipc/`, the QNX half of
> `target/port/`, the four C processes and the C++17 archive compile against the
> **real** `$QNX_TARGET` headers with `-Wall -Wextra -Wshadow -Wpedantic
> -Werror`, and link into five QNX binaries (no stub header is involved any
> more). What it does not prove: that they run correctly — nothing has booted
> yet, and the journal's resource-manager service wiring is still the
> `TODO(target)` of `target/journal/edr_journal_qnx.c` (the resmgr attaches and
 * serves read/devctl, but `io_write` does not yet hand the frame to
 * `ambre_edr_service_handle()`).
>
> **Eight real API mismatches the stubs had hidden**, all fixed and now compiled
> by the real toolchain:
>
> | # | The stub said | QNX 8.0 says |
> |---|---|---|
> | 1 | `ClockTime(id, NULL)` returns the time | `int ClockTime(clockid_t, const _Uint64t *new, _Uint64t *old)` — the time comes back in `old` |
> | 2 | `slog2_buffer_set_config_t.buffer[].num_lines/buffer_size` | `.buffer_config[].num_pages` (4K pages), and `slog2f(buffer, code, severity, fmt, ...)`; there is no `SLOG2_NO_FLAGS` |
> | 3 | a `coid_t` type | there is none: `ConnectAttach()` returns an `int` (scoid) |
> | 4 | `_PULSE_DISCONNECT` | `_PULSE_CODE_DISCONNECT` (-33) |
> | 5 | `devctl(fd, DCMD_FLUSH_CACHE, ...)` | no such devctl in 8.0 (`dcmd_blk.h` exposes PGCACHE DISCARD/RESIZE only): the flush of a raw partition is `fsync(fd)`, which io-blk turns into a cache flush + SYNCHRONIZE CACHE. Essay E1 now measures that |
> | 6 | `TimerTimeout(..., _NTO_TIMEOUT_NEAREST, NULL, 0, &nsecs)` as a relative timeout | `TimerTimeout(clockid_t, flags, notify, const _Uint64t *absolute_deadline, remain)` with `_NTO_TIMEOUT_SEND \| _NTO_TIMEOUT_REPLY` for a request/reply |
> | 7 | a resmgr thread pool on `dispatch_context_alloc/free`, `dispatch_block`, `dispatch_handler` | `resmgr_context_alloc/free`, `resmgr_block`, `resmgr_handler`; and the `iofunc_*` helpers take `iofunc_ocb_t *`, so the custom OCB (which embeds it first) is cast |
> | 8 | `edr-archive` talking AF_UNIX sockets itself | it now calls `ambre_port_conn_request()` — one MsgSend on QNX, so **no libsocket is linked** and VIE-03 (closed loop, no network) holds on target |
>
> `make qnx-syntax` (the PC build against the stub headers) stays: it is what CI
> can run without an SDP. The stubs are now known to be *approximations* of the
> real headers in the eight ways above.


## History: how the target layer was built before the SDP arrived

Status: 2026-11-11. No QNX SDP 8.0 was installed on the development machine or
on the build server at that time. Rather than
ship unverifiable QNX code, the target layer was built the way the sibling
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
| `make image-check` (PC, static) | the image fragments, the security policy and the supervision script are **internally coherent** and match the process list, the priorities and the peer types | that `mkqnximage` accepts the buildfile, that `secpolcompile` accepts the policy, that anything boots |
| QNX 8.0 target (needs the SDP) | everything above, plus feasibility essays E1–E4, real latency numbers, kernel traces, security-policy enforcement | — |

The stubs are a **substitute for building on the PC only**. Every stub header
says so in its first comment. Nothing in `target/qnx-stub/` is shipped in an
image, and no stub is ever linked into a binary: `make qnx-syntax` produces
object files only, so a missing QNX symbol can never break the PC build.

---

## 1. The five QNX glue files, exactly

`make qnx-syntax` globs `target/*/*_qnx.c`. There are **five** such files on
2026-11-11, and this is what each of them contains:

| File | Lines | What it implements on QNX |
|---|---|---|
| `target/port/ambre_port_qnx.c` | 234 | the "everything else" half of the portability layer: process identity, `ClockTime(CLOCK_MONOTONIC)` + `ClockCycles()`, the `slog2` sink with one buffer set per process (OBS-01), `trace_logi()` user kernel-trace events (OBS-03), `SchedSet(SCHED_FIFO)` and `mlockall(MCL_CURRENT \| MCL_FUTURE)` (QNX-04), the access-control decision hook (SEC-01), and HAM/ppid supervision detection (QNX-06) |
| `target/port/ambre_port_disk_qnx.c` | 166 | the raw-partition disk backend: opens the journal device (e.g. `/dev/hd0t177` exposed by `io-blk`, STO-01), `pread`/`pwrite` sector I/O, and an explicit cache flush with `devctl(fd, DCMD_FLUSH_CACHE, ...)` from `<sys/dcmd_blk.h>`. Also carries `ambre_port_disk_flush_probe()`, the machinery of essay **E1**: it reports whether `io-blk` *accepted* the flush order and what it cost — never that the order *reached* the medium, which is only observable on target |
| `target/port/ambre_port_ipc_qnx.c` | 240 | the message-passing transport: `ChannelCreate(_NTO_CHF_UNBLOCK \| _NTO_CHF_DISCONNECT)`, `ConnectAttach`/`ConnectDetach`, `MsgReceive`/`MsgReply`, and `_PULSE_CODE_DISCONNECT` detection of a dead client without polling (QNX-03, QNX-05) |
| `target/journal/edr_journal_qnx.c` | 499 | the journal **resource manager** `/dev/edr` (QNX-01): `read()` → text status (OBS-02), `devctl()` → `DCMD_EDR_STATUS`, `DCMD_EDR_LIST`, `DCMD_EDR_EXPORT`, `DCMD_EDR_AUTOTEST`, and `/dev/edr/events/<id>` → raw event blocks (EXP-01, EXP-08). Requests run in the receiving pool thread at the **caller's** priority (QNX-03); `iofunc_client_info()` + `ambre_port_access_allowed()` enforce SEC-01 |
| `target/capture/edr_capture_qnx.c` | 220 | the capture process glue: SCHED_FIFO priorities 60 (intake) and 61 (detection), `mlockall()` so the freeze path cannot page fault (GEL-03, QNX-04), one channel for producers, a periodic **pulse** timer for the batch tick and the heartbeat (QNX-03), `MsgSend` to `edr-journal` for the emergency snapshot (GEL-01), and `_PULSE_CODE_DISCONNECT` as the producer-death notification (QNX-05) |

The QNX headers involved, all declared by the stubs:
`sys/dispatch.h`, `sys/iofunc.h`, `sys/resmgr.h`, `sys/neutrino.h`,
`sys/slog2.h`, `sys/trace.h`, `sys/dcmd_blk.h`, plus `sys/procmgr.h`,
`sys/iomsg.h`, `sys/ftype.h`, `sys/mount.h`.

`target/journal/edr_journal_qnx.c` carries an explicit, deliberately
un-faked `TODO(target)` block at the top: wiring
`edr_journal_qnx_set_source()` / `edr_journal_qnx_start()` from `main.c`, the
group → peer-type mapping once `image/ambre.sec` is frozen, and `readdir()` on
`/dev/edr/events`. Those are target-side integration steps, not hidden gaps.

---

## 2. `make qnx-syntax` — the command and its real output

```sh
make BUILD=build-doc qnx-syntax
```

Measured on 2026-11-11:

```
  qnx-syntax target/capture/edr_capture_qnx.c              OK
  qnx-syntax target/journal/edr_journal_qnx.c              OK
  qnx-syntax target/port/ambre_port_disk_qnx.c             OK
  qnx-syntax target/port/ambre_port_ipc_qnx.c              OK
  qnx-syntax target/port/ambre_port_qnx.c                  OK
qnx-syntax: 5 glue file(s) compile against the stubs
```

The rule (from the `Makefile`) is a per-file `-fsyntax-only` compile with the
same strict warning set as the rest of the project, so warnings are errors:

```
$(CC) -std=c11 -O0 $(WARN) -Werror -fsyntax-only \
      -Itarget/qnx-stub/include -Ifmt -Itarget -Itarget/port -Itarget/ipc \
      -I. "<file>"
```

No object is produced, no link is attempted: that is what guarantees the stubs
can never contaminate a real build, and it is why the PC build is unaffected by
the QNX glue.

Numeric ABI values — `DCMD_*` numbers, `IOFUNC_*` bits, message-type
constants — are recorded with their documentation provenance in
`docs/QNX-ABI.md` and are **`UNVERIFIED (needs QNX SDP 8.0)`**: a stub can
prove a type, not a number.

---

## 3. `make image-check` — the eleven static checks

```sh
make image-check      # → bash scripts/check-image.sh
```

Measured on 2026-11-11 — `IMAGE CHECK PASSED (static PC checks OK; target
build/boot UNVERIFIED without QNX SDP 8.0)`, 11/11 checks:

| # | Check | Requirement |
|---|---|---|
| 1 | the required files exist and `image/ambre-supervise.sh` is executable | — |
| 2 | `edr-archive` is wired in the `Makefile` (`edr-%` rule) **and** in `image/buildfile` | — |
| 3 | `edr-capture` wired the same way | — |
| 4 | `edrctl` wired the same way | — |
| 5 | `edr-journal` wired the same way | — |
| 6 | `edr-replay` wired the same way | — |
| 7 | process priorities match spec §4.4 (capture 60–61, journal 62, archive 8, edrctl 10, replay 50), read from `image/buildfile` and cross-checked against the annotations in `image/ambre.build` | §4.4 |
| 8 | every `ambre_peer_type_t` member of `target/port/ambre_port.h` is mapped to a `type` in `image/ambre.sec` | SEC-01 |
| 9 | no network stack, daemon or socket library is referenced under `image/` | VIE-03 |
| 10 | the raw journal partition (`/dev/hd0t177`, disk 0) and the Power-Safe archive (`/dev/hd1t177`, disk 1) are distinct, and `edr-journal` starts **before** `fs-qnx6.so` | STO-01, STO-07, QNX-07 |
| 11 | no recorder `stop`/`disable` command exists in `image/` | SEC-02 |

What it does **not** do: assemble anything. `mkqnximage` is not run,
`secpolcompile` is not run, no image is produced. All of that is the next
section, and all of it is `UNVERIFIED` until an SDP exists.

---

## 4. The exact procedure to obtain the SDP and build the image

These are the commands written for this repository — **none of them has been
executed here**, and none of their outputs may be quoted as a result. Every
step says what must be observed for it to count as passing.

### Step 0 — obtain the SDP

QNX SDP 8.0 is distributed by BlackBerry QNX; the free route is the **QNX
Everywhere** developer programme (`https://www.qnx.com/products/everywhere/`).
Download the SDP 8.0 for Linux x86_64, install it, and activate the
environment:

```sh
source ~/qnx800/qnxsdp-env.sh
qcc --version          # must report the QNX 8.0 gcc toolchain
echo "$QNX_TARGET $QNX_HOST"
```

*Passing* = `qcc --version` prints a QNX target banner and `$QNX_TARGET` is a
real directory containing `usr/include` and `x86_64/lib`.

### Step 1 — cross-compile the five processes

```sh
make clean
make qnx CC=qcc ARCH=x86_64        # UNVERIFIED: the `qnx` target wiring
```

*Passing* = five binaries (`edr-capture`, `edr-journal`, `edr-archive`,
`edrctl`, `edr-replay`) build with `-Werror` and no warnings, and
`file build/edr-journal` reports an x86_64 ELF for QNX.

### Step 2 — compile the security policy

```sh
secpolcompile -o build/ambre.sec.bin image/ambre.sec
```

*Passing* = no diagnostic; the statement forms (`type`, `allow`,
`allow_attach`, `provides`) are accepted. A rejected `provides` binding is
exactly the kind of error the stubs cannot catch, and it must be fixed in
`image/ambre.sec`, not worked around.

### Step 3 — stage the binaries and build the boot image

The file list of `image/ambre.build` names the five binaries (and the SDP files
`procnto-smp-instr`, `devb-virtio`, `fs-qnx6.so`, `io-blk.so`); `mkqnximage`
resolves the SDP ones from `$QNX_TARGET`, and the Ambre ones must be staged
under exactly those names.

```sh
# 3a. direct route, with image/ambre.build as the buildfile:
mkqnximage -f image/ambre.build -o build/ambre.img

# 3b. equivalent route from an mkqnximage workspace:
mkqnximage --type=qemu --arch=x86_64 --graphics=no --noprompt
#   append the file list of image/ambre.build to
#   local/snippets/system_files.custom,
#   append its start-up lines to local/snippets/post_start.custom, then:
mkqnximage --build --noprompt
```

*Passing* = `mkqnximage` produces `build/ambre.img` without an unexplained
warning. Any file it cannot resolve is a real gap in `image/ambre.build`.

### Step 4 — boot it under QEMU, with two disks and no network

```sh
qemu-system-x86_64 \
  -machine q35 -m 1024 -smp 2 \
  -kernel build/ambre.img \
  -drive file=build/journal.img,if=virtio,format=raw \
  -drive file=build/archive.img,if=virtio,format=raw \
  -serial stdio -display none
```

No `-netdev`, no `-nic`: the closed-loop property (VIE-03) is checked
statically today and must remain true by construction here.

*Passing* = `edr-journal` starts before `fs-qnx6.so` (QNX-07), `/dev/edr`
appears, `slog2info -b ambre` shows the process start lines (OBS-01), and
`edrctl status` answers.

### Step 5 — the feasibility essays, then the campaigns

```sh
python3 host/powercut/e1_flush_probe.py --mode guest --image build/ambre.img \
        --report build/e1.json
python3 host/powercut/e3_cycle_timing.py --image build/ambre.img --cycles 20 \
        --report build/e3.json
python3 host/powercut/powercut_campaign.py --image build/ambre.img \
        --scenario build/scenario.txt --random-cuts 200 --seed 4242 --jobs 4 \
        --report build/campaign.json
```

*Passing* = E1 shows the `DCMD_FLUSH_CACHE` order accepted and journaled by the
block server, E3 shows a kill/restart cycle under 10 s, and the campaign
reports **zero** data loss over 200 cuts. Only then may §7.4 of the
specification be called met.

The build files, the security policy and the image fragments under `image/`
are written for exactly those commands. Until they have been run, they are
reviewed by inspection and by `make image-check`, and nothing more.

---

## 5. Consequences for the campaign numbers

Every published number carries its backend. The PC numbers are real
measurements of the software path (integration, encoding, CRC, table commits,
recovery, exploration of cut states) and are labelled `backend=posix`. The
numbers that only a QNX target can produce — PERF-02 end-to-end freeze latency
including media time, PERF-07 kernel traces, E1 flush propagation, E3 VM cycle
time, SEC-01 policy enforcement — are published as **not measured**, with the
requirement id they answer, rather than estimated. The full list is
`docs/CAMPAIGN.md` §8 and `README.md` §9.

Three statements in this repository are hypotheses and are labelled as such
wherever they appear: that a flush request on a raw partition reaches the
virtual disk as a flush order (essay E1); that priority inheritance carries
through the disk driver on the freeze path (QNX-02, kernel trace); and that a
20 ms freeze deadline is achievable at all (PERF-02, a target, never a
measurement).
