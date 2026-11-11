# Ambre — a QNX 8.0 Event Data Recorder that survives the crash

**One sentence:** Ambre is an automotive Event Data Recorder (EDR, "black box")
built on QNX 8.0 that still has the crash on disk when power dies 20 ms after
the impact — and that *proves* it by exhaustively exploring every power-cut
state instead of running a handful of lucky reboots.

QNX Global Hackathon 2026 — Automotive & Mobility.

---

## 1. The problem

Since 6 July 2022 every new vehicle type sold in the European Union must carry
an Event Data Recorder: **Regulation (EU) 2019/2144** makes it mandatory, and
**UN Regulation No. 160** defines what the device must record — the delta-v,
the pre-crash speed, the belt and brake states, the airbag status, the
time-to-T0, a "complete file" flag.

The engineering difficulty is not the sensor list. It is the **timing**:

> The event you must record is the event that destroys your power supply.

Between the impact and the loss of the 12 V rail there are tens of
milliseconds. Everything the recorder owes the regulator — about a kilobyte of
Class A data — must be *durable*, not merely *buffered*, inside that window. A
general-purpose stack gives you no bound on when a task wakes up, no bound on
time spent inside the disk driver, and a file system that syncs a whole volume
rather than a file. Ambre is built around that one hard deadline, from the
first byte on disk to the last line of the report.

---

## 2. Why QNX, and why real time

| QNX property | What it buys Ambre |
|---|---|
| **Drivers are ordinary processes** | The journal talks to `devb-*` by message passing, so the driver runs the request **at the caller's priority** (priority inheritance through `MsgSend`). The critical write path cannot be preempted by lower-priority work: it stays at priority 62 from the trigger to the flush acknowledgement. |
| **Microkernel isolation** | Capture, journal, archive and service tool are separate processes with separate address spaces. A crash in `edr-archive` cannot corrupt the journal; when a peer dies, the kernel tells its correspondents (`CHANNEL` disconnect), so `edr-capture` knows the journal is gone and marks a `GAP` instead of silently dropping data. |
| **Resource managers** | The journal is a resource manager exposing `/dev/edr/...`: one process owns the raw partition, everybody else reaches it through a stable, typed, message-based API — no shared file, no locking protocol to get wrong. |
| **Pulses and timers** | Periodic capture, the 10 ms continuation writes and the flush tick are driven by kernel timers and pulses, not by a scheduler-friendly `sleep()` loop. |
| **Power-Safe file system** | The archive partition is Power-Safe: after a power cut it is consistent by construction, with no `fsck` on boot. But Power-Safe syncs **whole-file-system**, and `O_SYNC` is ignored — which is exactly why the journal is *not* a file (see idea 2). |
| **Kernel trace (`tracelogger`)** | The freeze deadline is a claim; the trace is the proof. Priorities, message-passing events and driver activity are captured so that the three worst cases can be explained sample by sample. |

---

## 3. Three load-bearing ideas

**1. Regulatory data first — about 1 KiB, in one write.**
On trigger, the capture process sends a single `EMERG` record (452-byte payload,
one 512-byte block, one CRC) holding the whole Class A pre-crash snapshot. It is
the first thing written after T0, it is written **once**, and it is enough on
its own to prove that the event happened. The 1.1 MiB of high-resolution Class B
data is already on disk from the continuous journal, so nothing bulky stands
between the impact and the durable record.

**2. A raw continuous journal on a partition with no file system.**
The journal owns a 48 MiB raw partition: 24 segments of 2 MiB, 512-byte blocks,
each with magic, version, sequence number, session id and CRC-32C, plus an
allocation table in two alternating copies. No file system means no metadata
journal, no block allocation, no `fsync` of a whole volume: one write request
per record, straight to the device. A block is never rewritten before its
segment is released. MCAP archives are produced later, by a low-priority
process, on a Power-Safe partition.

**3. Proof by exhaustive exploration, not by a few reboots.**
`ambre-explore` enumerates power-cut states on a simulated disk: every cut point
in the write stream, plus torn-write, cache-loss, bit-flip and table-corruption
variants, nested cuts *during recovery*, in parallel. Each resulting image is
re-opened with the real recovery code and checked against eight invariants
(I1–I8: no invented event, no invalid CRC ever returned, idempotent recovery,
monotonic sequence, sealed ⇒ readable, "complete file" flag truthful, locked
events never overwritten, recovery replayable). The claim "we survive power
loss" becomes a count of explored states with zero violations — a number the
jury can reproduce with one command.

---

## 4. Architecture

Five processes, priorities chosen so that the write path always wins:

```
  scenario file / CAN / zenoh
            │  frames @ 10 ms
            ▼
   ┌─────────────────┐  snapshot + trigger   ┌────────────────────┐
   │  edr-capture    │  message (MsgSend,    │    edr-journal     │  raw sector
   │  prio 60 .. 61  │  priority inherited   │    prio 62         │  writes
   │  trigger maths  │ ────────────────────► │  resource manager  │ ─────────┐
   │  Class A buffer │                       │  /dev/edr/{in,ctl} │          │
   └─────────────────┘                       └─────────┬──────────┘          ▼
            ▲                                          │ reads sealed   ┌─────────────┐
            │ frames                                   │ segments       │ raw partition│
   ┌────────┴────────┐                                 ▼                │ no FS       │
   │   edr-replay    │                        ┌────────────────────┐    │ 48 MiB      │
   │   prio 50       │                        │    edr-archive     │    └─────────────┘
   │ demo producer   │                        │    prio 8          │ ──► MCAP files on
   └─────────────────┘                        └────────────────────┘     Power-Safe
   ┌─────────────────┐                                                       │
   │     edrctl      │  prio 10 — service tool: status, list, export, verify
   └─────────────────┘                                                       ▼
                                                                    Foxglove / edr-verify
```

| Process | Role | Priority |
|---|---|---|
| `edr-capture` | Frames in, timestamps, runs the trigger arithmetic (UN R160 thresholds), keeps the Class A pre-crash buffer, sends the snapshot on trigger | 60–61 |
| `edr-journal` | Sole owner of the raw partition: writes, pins, seals, recovers at boot | 62 |
| `edr-archive` | Converts sealed events to MCAP, applies retention | 8 |
| `edrctl` | Service tool: state, list, export, verify | 10 |
| `edr-replay` | Demo producer: replays a scenario file | 50 |

**The portability layer, and why it is the load-bearing wall.**
`target/port/ambre_port.h` is one frozen interface — identity, monotonic clock,
logging, kernel trace, real-time priority, memory lock, the disk vtable, the
message transport, access control, supervision — with **two backends**:

| Backend | Files | Status |
|---|---|---|
| POSIX (PC) | `ambre_port_posix.c`, `ambre_port_posix_disk.c`, `ambre_port_posix_ipc.c` | compiled and **tested** (`tests/test_port_posix.c`, 87 checks) |
| QNX 8.0 | `ambre_port_qnx.c`, `ambre_port_disk_qnx.c`, `ambre_port_ipc_qnx.c` | **compiles** against the stub headers (`make qnx-syntax`), never executed |

Every process keeps its logic portable (compiled and tested on the PC) and
pushes everything OS-specific into a `<proc>_qnx.c` file. That is what allows a
single implementation to be measured on the PC and shipped to QNX without a
second code path — and it is why the exhaustive exploration of chapter 6
exercises the very code that runs on the target.

Design contract: [`docs/cahier-des-charges.md`](docs/cahier-des-charges.md)
(functional spec) and [`docs/FORMAT.md`](docs/FORMAT.md) (normative on-disk
format — block layout, allocation table, recovery algorithm, MCAP mapping).
Campaign results: [`docs/CAMPAIGN.md`](docs/CAMPAIGN.md).

---

## 5. Repository layout

```
ambre/
├── README.md            this file
├── LICENSE              Apache-2.0
├── NOTICE               attributions, regulatory references, dataset caveats
├── Makefile             one build for PC (gcc) and QNX (qcc)
├── fmt/                 portable C11 core: block codec, allocation table, scan,
│                        recovery, journal engine, trigger arithmetic (no OS API)
├── target/              the five QNX 8.0 processes
│   ├── port/            portability layer: POSIX backend (tested) + QNX glue
│   │                    (`*_qnx.c`, syntax-checked against target/qnx-stub/)
│   ├── ipc/             wire protocol helpers (header, message types, framing)
│   ├── capture/         edr-capture: frames in, trigger maths, Class A buffer
│   ├── journal/         edr-journal: resource manager /dev/edr, raw partition
│   ├── archive/         edr-archive: C++17 MCAP writer on a Power-Safe partition
│   ├── ctl/             edrctl: status, list, export, verify
│   └── replay/          edr-replay: demo producer, plays a scenario file
├── image/               mkqnximage fragments (ambre.build, buildfile), the
│                        security policy (ambre.sec), supervision script
├── host/
│   ├── simdisk/         simulated disk + operation log (write, flush, drop)
│   ├── explore/         exhaustive power-cut state explorer, invariants I1–I8
│   ├── powercut/        QEMU power-cut orchestrator, faulty block server,
│   │                    feasibility probes E1 (flush) and E3 (cycle time)
│   ├── mcap/            journal image → MCAP exporter
│   ├── verify/          independent Python trigger twin, cross-check, edr-verify
│   ├── scenarios/       scenario generator and reference scenarios
│   ├── e2e/             POSIX bench: real processes, real SIGKILL, real recovery
│   ├── fuzz/            random hostile disk images against the recovery path
│   ├── latency/         latency campaign pieces (in progress)
│   └── energy/          energy-reserve model (in progress)
├── tests/               22 C + 1 C++ suites + 10 Python suites
├── scripts/             verify-all.sh (full gate), demo.sh (end-to-end demo),
│                        coverage.sh + coverage_report.py, sizecheck.py,
│                        check-image.sh (static image/security check)
├── foxglove/            Foxglove layout for the exported MCAP
├── codelab/             step-by-step tutorial, six chapters + index
└── docs/                spec, journal format, architecture, campaign report,
                         validation plan, QNX port status, security model
```

Status: the portable core (`fmt/`), the five processes (`target/`), the PC
tools (`host/`) and the test suites are all in the tree and green today
(measured results in §8, full campaign report in
[`docs/CAMPAIGN.md`](docs/CAMPAIGN.md)). The QNX-specific glue
(`target/*/*_qnx.c`) compiles against the QNX stub headers
(`make qnx-syntax`: 5/5) but has **never executed**: no QNX SDP is installed on
the development machine, so `image/` is validated statically only
(`make image-check`) and never built or booted. See
[`docs/QNX-PORT.md`](docs/QNX-PORT.md).

---

## 6. Build

**PC / host side (works today) — C11, gcc, no dependency beyond libc:**

```sh
make            # library, unit tests, tools
make test       # run every unit/integration test; exit code = failures
make sizecheck  # house rule: files < 500 lines
```

Strict flags by default: `-std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpedantic -Werror`.
Use a private build directory if you build in parallel: `make BUILD=build-mine`.

**QNX 8.0 target (week 2):**

| Item | Value |
|---|---|
| SDK | QNX Software Development Platform **8.0**, obtained through QNX Everywhere |
| Target | **QEMU x86_64** (secondary, optional: Raspberry Pi 5) |
| Toolchain | **`qcc`** (GCC-based QNX toolchain), `QNX_TARGET` + `QNX_HOST` set by `qnxsdp-env.sh` |
| Image | built with **`mkqnximage`**, fragments from `image/` |
| Language | C11 for the format library and the critical processes; C++17 for the MCAP archive; Python 3 for host tools |

The same `fmt/` sources compile unchanged on both sides: the library uses no
OS-specific call, no dynamic allocation on a critical path, and fixed-size
structures throughout. On QNX the processes are cross-compiled with `qcc` and
the tools are not built.

---

## 7. Demonstration — one command

```sh
make demo                # end-to-end: scenario → 3 real processes → SIGKILL →
                         # recovery → MCAP export → official verifier
make explore             # 10 000 cut states, all write variants
make explore-full        # every reachable cut state, nested cuts included
make verify              # export one event and check it against its scenario
make crosscheck          # C trigger vs the independent Python twin
./scripts/verify-all.sh  # the whole gate before any push (add --quick to skip
                         # the exhaustive exploration and the cross-check;
                         # AMBRE_BUILD_DIR=build-mine for an isolated build)
```

All six are real, runnable targets; use `make BUILD=build-mine …` to keep a
private build tree. Measured on 2026-11-11 — the second timing of each row is
the same command re-run while three other campaigns were loading the machine,
which is why the counts stay identical and the durations do not:

| Command | Result (identical on both runs) | Time |
|---|---|---|
| `make demo` | **`DEMO OK`** — real chain replay → capture → journal, `SIGKILL` on the journal in the `acquired` window (+6 033 ms), restart and recovery, 1 event recovered `sealed locked=yes complete_file=no`, MCAP export rc=0, official checks green, **class A 11/11 points identical** to the injected rows | 7.1 s |
| `make explore` | **10 000 states, 0 violations** | 29 s (342 states/s) — 77 s under load |
| `make explore-full` | **64 591 states = every reachable state**, 46 390 acquisitions, 0 violations (I1..I8 all zero) | 334 s idle — 495 s under load |
| `make verify` | **10/10 checks pass** (`mcap.structure`, `statistics`, `channels`, `timestamps.*`, `class_a.fidelity` on 11 points, `metadata.required`, `event.file_complete`) | 4.9 s |
| `make crosscheck` | **10 000 scenarios, 2 744 840 decision lines, byte-identical, 0 divergences** | 2 min 32 — 6 min 28 under load |
| `./scripts/verify-all.sh --quick` | **7/7 steps green** (`AMBRE_BUILD_DIR=build-doc`, re-measured 18:00) | ~1 min |

`make demo` generates a frontal-impact scenario (9 000 samples, seed 1234,
impact at 6 000 ms, 12 km/h, restraint deployed → locked event), replays it
through the **three real processes**, kills the journal with `SIGKILL` once the
event is acquired, restarts it on the same disk file so the recovery runs, then
exports the event to MCAP and checks it with the official verifier. Measured on
this one run: *acquired in 4.428 ms* (trigger → flush, a single sample under
load, **not** a campaign figure — the medians are in `docs/CAMPAIGN-LATENCY.md`);
the event
came back `sealed`, `locked=yes`, `complete_file=no` (exactly what §4.3 of the
specification expects for a cut between acquisition and sealing). The result
opens in Foxglove with the layout in `foxglove/`. The demo prints its own list
of what it does **not** prove (`backend=posix`, a `SIGKILL` is not a power cut,
the media share needs the target) — read it, it is part of the output. `verify-all.sh` runs the size check, the build,
the C and Python test suites, the scenario generator, the exploration and the
MCAP round-trip (plus, without `--quick`, the trigger cross-check and the
exhaustive exploration), then prints a single green/red summary. A failing step
is reported `FAIL` — the script never fakes a success.

**House rule:** `make sizecheck` (which runs `scripts/sizecheck.py
--tracked-only`) reports **191 tracked files, 0 violations**, and a bare
`python3 scripts/sizecheck.py` — which also scans untracked work-in-progress
files — reports **195 files, 0 violations** (measured 18:50; an 18:00 run of the
same bare command showed 3 violations in `host/e2e/` and `host/processcut/`,
fixed since by their owning agents).

---

## 8. What is measured today

Every figure below comes out of the command quoted next to it, run on
2026-11-11 on the development machine (x86-64, 12 cores, gcc 13.3.0, Python
3.12.3). Nothing is extrapolated. Measurements that need the QNX target say
**not measured**, because no QNX SDP 8.0 is installed on this machine
(`target/*/*_qnx.c` only compiles against the stubs — `docs/QNX-PORT.md`):
no number is published before it comes out of a campaign on the real target. Each row names the requirement it answers
(`docs/cahier-des-charges.md` §5.7, §7) so the gap stays auditable.

### Validation status

| Area | Status | Evidence (reproduce with) |
|---|---|---|
| Format: block codec, CRC-32C, allocation table, scan, record | **green** | `make test` — 23 suites (22 C + 1 C++), **48 936 checks**, 0 failures |
| Journal engine, five processes' portable logic, power-cut recovery | **green** | `make test` — incl. `test_journal` (4 195), `test_journal_svc` (8 277), `test_simdisk` (3 345), `test_rates` (6 685), `test_recovery` (124), `test_capture` (43), `test_ipc` (102), `test_edrctl` (97) |
| Trigger arithmetic (UN R160) + independent Python twin | **green** | `make crosscheck` — 10 000 scenarios, 2 744 840 decision lines, byte-identical, 0 divergences |
| Exhaustive exploration of power-cut states | **green** | `make explore-full` — 64 591 states for the 30 s reference scenario (its whole reachable space); **campaign of record: 1 379 264 states over 18 scenarios, 2 567 421 acquisitions, 0 violations of I1..I8** (`docs/CAMPAIGN.md`); `make explore` 10 000 states in 29 s |
| MCAP export + `edr-verify` | **green** | `make verify` — 10/10 checks incl. `class_a.fidelity` (11 points); `python3 tests/test_mcap_py.py` — 24 tests OK |
| C++17 MCAP archive | **green** | `make test_archive_cpp` — 71 checks, 0 failures |
| One-command demo | **green** | `make demo` — `DEMO OK`: real 3-process chain, `SIGKILL` in the `acquired` window, recovery, export verified, class A 11/11 points identical |
| Coverage of `fmt/` (spec §12.10) | **green** | `BUILD=build-doc-cov2 sh scripts/coverage.sh` — **98.40 %** (1 349 / 1 371 lines), all 22 C suites instrumented |
| Random-data testing of the recovery path (§12.10) | **green (smoke)** | `make fuzz` — 52 hostile images, 0 violations, 5 s; the 24 h run is `BLOCKED(wall-clock: 24 h of exclusive machine time)` (command: `./build/ambre-fuzz --hours 24 --jobs 8 --report build/fuzz-24h.json`) |
| House rule (files < 500 lines, functions < 50) | **green** | `make sizecheck` (`--tracked-only`) — 191 files, 0 violations; bare `python3 scripts/sizecheck.py` — 195 files, 0 violations |
| QNX glue compiles against the stubs | **green** | `make qnx-syntax` — 5/5 glue files OK |
| Static image + security-policy coherence | **green** | `make image-check` — 11/11 checks pass |
| Forced process cuts (POSIX bench, real `SIGKILL`) | **500 run, 2 open I7 verdicts** | `build-close/processcut/report-500.json` (close-out): 500 cuts, 0 errors, 498 `ok` + **2 `inconsistent_state` (I7)** published unresolved — qualified in `docs/VALIDATION.md` §3 (`seal_cause=NORMAL` → the seal reached the disk before the kill; a bench-window race, not data loss). The earlier 200-cut run stands as published (0 violations, 1 `check_failed`, 2 upstream findings) |
| Latency PERF-01..PERF-08 | **measured on PC only** | `docs/CAMPAIGN-LATENCY.md` — 4 loads × 200 trigger→acquired cycles + a 1 000-trigger level-S run (`build-close/latency-none.json`), `backend=posix`: PERF-03 software share median **10 µs** (target ≤ 500 µs) met; PERF-01 **exceeded** at the tail (p99 up to 203 ms under disk load); PERF-02 **not proven** (99.7 % media time on an NVMe file); PERF-04 **measured at the S volume: not met** (18 overshoots of the 20 ms freeze deadline over 1 007 cycles); PERF-05 not met. Reproduce with `make latency-all` / `make BUILD=build-close latency LATENCY_LOAD=none LATENCY_TRIGGERS=1000`. **Nothing measured on QNX** |
| Executing the five processes on QNX: resource manager `/dev/edr`, `edrctl`, security policy, kernel trace | **missing** | no QNX SDP: `target/*/*_qnx.c` and `image/` are syntax-/statically checked only, never built or booted |
| Feasibility essays E1–E4, real QEMU power cuts | **missing** | 0/200 socle cuts run: harnesses exist and pass their dry-run tests, but there is no image to cut |

### Measurement table

| Measurement | Requirement | Target | Measured |
|---|---|---|---|
| Trigger latency, last threshold sample → pulse | PERF-01 | ≤ 2 ms | **median 1.1–2.2 ms, p99 9.2–203 ms** (`backend=posix`, 4 loads × 200 cycles — 1 000-trigger level-S run: median 1.1 ms over 127 crash triggers, 36 overshoots) — at the limit in the median, **exceeded** in the tail; on target: `BLOCKED(no QNX SDP)` |
| Trigger → durable ("acquired"), reference hardware | PERF-02 | ≤ 20 ms | **median 2.7–4.0 ms, p99 9.3–35.0 ms, max 108 ms on PC** — but 99.7 % of it is NVMe media time, so this is *not* a statement about the reference hardware; on target: `BLOCKED(no QNX SDP)` |
| Software share of that deadline, excluding media time | PERF-03 | ≤ 500 µs | **median 10 µs, p99 52–108 µs, p99.9 64–831 µs** (`backend=posix`, journal on tmpfs) — met at median and p99 under every load except `disk` at p99.9 (2 events of 200) |
| Deadline misses over 10 000 triggers under load | PERF-04 | 0 | **level-S volume measured on POSIX, NOT met** — 1 007 trigger → acquired cycles (`build-close/latency-none.json`, load `none`): 18 overshoots of the 20 ms freeze deadline (median 5.4 ms, p99 29.9 ms); the C volume (10 000 under CPU+disk load) is `BLOCKED(campaign wall-clock ≈ 4–5 h per load)`; on target `BLOCKED(no QNX SDP)` |
| Frame send cost at the producer, p99 | PERF-05 | < 50 µs | **not met on POSIX**: p99 falls in the ≤ 5 ms bucket ceiling of the replay histogram — see `docs/CAMPAIGN-LATENCY.md` §4.5 |
| Capture running after kernel start, recovery included | PERF-06 | < 1 s | `BLOCKED(no QNX SDP)` — on target: boot `mkqnximage -f image/ambre.build` and read the kernel-start → first-frame delay (recovery included) from the `slog2info` timestamps |
| Power-cut states explored, zero invariant violations | §7.3 | 1 000 000 | **64 591 states, 0 violations** — the whole reachable space of the reference scenario (the 1 000 000 cap is never reached); `make explore-full` |
| QEMU power cuts survived, zero data loss | §7.4 | 2 000 | **0 — `BLOCKED(no QNX SDP)`**: no SDP or image on this machine; on target: `python3 host/powercut/powercut_campaign.py --image qnx80.img --scenario build/scenario.txt --random-cuts 200 --seed 4242 --jobs 4 --report build/campaign.json` |
| Forced process stops, real `SIGKILL` + recovery | §7.6 | 500 | **500 done** (`build-close/processcut/report-500.json`, close-out): 500 cuts, 0 errors, 498 ok + **2 `inconsistent_state` (I7)** published unresolved — qualified in `docs/VALIDATION.md` §3 (`seal_cause=NORMAL` → the seal reached the disk before the kill; bench-window race, not data loss). The earlier 200-cut run stands as published (0 violations, 1 `check_failed`, 2 upstream findings). On target: `BLOCKED(no QNX SDP)` |
| Worst cases explained by a kernel trace | PERF-07 | 3 | **partial**: the three worst cases are decomposed from the `AMBRE_TRACE` log on POSIX (`docs/CAMPAIGN-LATENCY.md` §4.6); a *kernel* trace needs the QNX target |
| Line coverage of `fmt/` | §12.10 | > 95 % | **98.40 %** (1 349 / 1 371 lines); `BUILD=build-doc-cov2 sh scripts/coverage.sh` |

What is verified **today** is functional, not temporal: 23 suites and 48 936
checks pass on PC plus 162 Python tests (10 suites), the recovery code is exercised on every
reachable simulated disk image (64 591 states, zero invariant violations), and
the C trigger arithmetic and its independent Python twin produce byte-identical
decision files over 10 000 generated scenarios (2 744 840 compared lines).
Those are correctness results, not performance results; every latency figure
still requires the QNX target.

---

## 9. Scope and honesty

Ambre is **aligned with UN Regulation No. 160**. It is *not* "compliant",
*not* "certified", *not* "homologated", and it is not an airbag ECU: it applies
the regulation's thresholds and records its data elements, nothing more.

**Verified against the published texts**

- EDR mandate and dates: Regulation (EU) 2019/2144.
- UN R160 thresholds, T0, locking, capacity, data elements and "complete file"
  flag: read from the version published in the EU Official Journal; the 01
  series of amendments (the one the EU references) could not be consulted and
  must be re-read before submission.
- Commission Delegated Regulation (EU) 2022/545, 49 CFR Part 563 (US profile).
- Power-Safe behaviour (`fsync()` is volume-wide, `O_SYNC` ignored, snapshot
  every 10 s) and raw-partition exposure under `/dev`: QNX 8.0 documentation.
- MCAP record layout, CRCs and encodings: the published specification.

**Hypotheses, stated as such**

- That a flush request on a raw partition reaches the virtual disk as a flush
  order — probed by experiment E1, not yet proven.
- That priority inheritance carries through the disk driver on this path —
  expected from QNX message passing, to be shown on a kernel trace.
- That killing QEMU keeps writes already handed to the host — reasoning about
  the host cache, not tested.
- The 20 ms freeze deadline is a **target**, not a measurement.
- Timings under QEMU are **emulated**: they are reported as emulated, and the
  software share is separated from media time. Real-hardware numbers need a
  Raspberry Pi 5 run.

**The former `edrctl`-on-POSIX gap is closed (week-5 close-out)**

The frozen port header now defines `AMBRE_PORT_HAS_CONN_REQUEST` and declares
`ambre_port_conn_request()`; the POSIX port implements it
(`target/port/ambre_port_posix_ipc.c`) and `target/ctl/edr_ctl.c` compiles its
real request/reply binding. `edrctl status` / `list` now complete end to end on
a real AF_UNIX socket on PC (`make demo` prints `service probe: build/edrctl
answered (rc=0)`), and `tests/test_ctl_live_py.py` pins it: the full
STATUS_REPLY body arrives (the old 32-byte truncation gap is gone), usage
errors exit 2, a refused export exits 1 with an explicit NAK, and a `edrctl`
killed mid-exchange leaves the journal serving. EXP-01 is therefore *covered*
on the PC backend; the same exchange over the QNX `MsgSend`/`MsgReceive`
transport is `BLOCKED(no QNX SDP)` — on target: boot the image and run
`edrctl --journal /dev/edr status` / `list`.

**Deliberately out of scope:** ISO 26262 functional-safety certification,
mechanical and thermal qualification, crash detection of production quality,
GPS position, video, audio and driver identity — the last four are excluded by
design in the format itself (signal whitelist, no absolute time, no VIN).

### What could NOT be measured, because there is no QNX SDP

No QNX Software Development Platform 8.0 is installed on the development
machine or on the build server, and none was obtained during the project. The
project therefore does exactly one thing about it: **it states the boundary
instead of papering over it**. `target/*/*_qnx.c` (5 files) compiles against
stub headers — `make qnx-syntax`, 5/5 OK — which proves there is no typo, no
wrong prototype, no wrong struct field and no wrong argument type in the QNX
glue. It proves **nothing** about runtime.

| Not measured, and why | Requirement | What it would take |
|---|---|---|
| Trigger → acquired latency (≤ 20 ms), software share (≤ 500 µs), ≤ 2 ms sample → pulse, 0 misses over 10 000 triggers **on the reference hardware** — the POSIX pass is published, the target pass does not exist | PERF-01..PERF-04 | a booted QNX image + `tracelogger`; on target the 20 ms deadline is still a **target**, not a result |
| Frame send cost at the producer, p99 < 50 µs (essay E4) — measured on POSIX and **not met**; on QNX message passing it has never been measured | PERF-05 | the QNX `MsgSend` path under load |
| Capture running < 1 s after kernel start, recovery included | PERF-06 | a real boot sequence |
| The three worst cases explained by a kernel trace | PERF-07 | `tracelogger` on the target, priorities 8/10/50/60/61/62 visible |
| That a flush on a raw partition reaches the virtual disk as a flush order | essay E1 | QEMU + `e1_flush_probe.py --backend qemu` |
| That QEMU can be killed and restarted in under 10 s per cycle | essay E3 | a built image to cycle |
| Kernel enforcement of the security policy (an intruder refused) | SEC-01, SEC-06 | `secpolcompile` + `secpolpush` on a real image; today only the **decision function** is tested on PC (`test_port_posix.c`) and the policy is statically coherent (`make image-check`) |
| The resource manager `/dev/edr` accepting `open`/`read`/`write`/`devctl` from real clients | QNX-01, QNX-03 | `procnto` + a client process |
| Priority inheritance through the disk driver on the freeze path | QNX-02 | the same kernel trace as PERF-07; expected from QNX message passing, **not** demonstrated |
| `mlockall()` actually preventing page faults on the critical path | QNX-04 | the target; on PC the equivalent property is the fixed-size journal state |
| `slog2` output, resource consumption, supervision by HAM | OBS-01, QNX-06 | the target; `image/ambre-supervise.sh` is a **substitute** to be replaced by HAM |
| 200 real QEMU power cuts (socle) / 2 000 swept / 10 000 with a volatile cache | §7.4 | an image, then `host/powercut/powercut_campaign.py` |
| 500 real electrical cuts on a Raspberry Pi 5 | §7.7 extrême | a relay bench and hardware |

What *is* measured on PC and is honestly labelled `backend=posix` is listed in
§8 and in [`docs/CAMPAIGN.md`](docs/CAMPAIGN.md): 23 suites / 48 936 checks,
64 591 explored cut states with zero invariant violations, 2 744 840
byte-identical trigger decisions, 98.40 % line coverage of `fmt/`, and a demo
that runs end to end. Those are claims about **logic**, not about QNX
scheduling, drivers or the kernel — and no sentence in this repository should
be read as conflating the two.

---

## 10. License

Apache License 2.0 — see [`LICENSE`](LICENSE). Third-party attributions,
regulatory references and the status of the datasets we intend to use are in
[`NOTICE`](NOTICE). All repository content, code comments and documentation are
in English.
