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

Design contract: [`docs/cahier-des-charges.md`](docs/cahier-des-charges.md)
(functional spec) and [`docs/FORMAT.md`](docs/FORMAT.md) (normative on-disk
format — block layout, allocation table, recovery algorithm, MCAP mapping).

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
├── host/
│   ├── simdisk/         simulated disk + operation log (write, flush, drop)
│   ├── explore/         exhaustive power-cut state explorer, invariants I1–I8
│   ├── powercut/        QEMU power-cut orchestrator, faulty block server,
│   │                    feasibility probes E1 (flush) and E3 (cycle time)
│   ├── mcap/            journal image → MCAP exporter
│   ├── verify/          independent Python trigger twin, cross-check, edr-verify
│   └── scenarios/       scenario generator and reference scenarios
├── target/              QNX 8.0 processes (capture, journal, archive, ctl, replay)
├── image/               mkqnximage fragments and security policy
├── tests/               unit, integration and cross-check tests
├── scripts/             verify-all.sh (full gate), demo.sh (end-to-end demo)
├── foxglove/            Foxglove layout for the exported MCAP
├── codelab/             step-by-step tutorial
└── docs/                spec, journal format, agent brief
```

Status: the portable core (`fmt/`), the PC tools (`host/`) and the tests are in
the tree and build today. `target/` (QNX processes) and `image/` land in week 2
of the five-week plan; `host/scenarios/`, `scripts/demo.sh`, `codelab/` and
`docs/ARCHITECTURE.md` are being written in parallel.

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
make demo               # end-to-end: scenario → journal → MCAP → verify report
./scripts/verify-all.sh # the whole gate before any push (add --quick to skip the
                        # 1 000 000-state exploration)
```

`make demo` generates a crash scenario, drives it through the journal, exports
an MCAP file and verifies it against the scenario; the result opens in Foxglove
with the layout in `foxglove/`. `verify-all.sh` runs the size check, the build,
the C and Python test suites, the C-vs-Python trigger cross-check (10 000
scenarios, outputs compared byte for byte), the state exploration and the MCAP
round-trip, then prints a single green/red summary.

Useful individual steps:

```sh
make explore        # 10 000 power-cut states, all write variants
make explore-full   # 1 000 000 states, nested cuts during recovery
make crosscheck     # C trigger vs independent Python twin
make mcap verify    # export one event and check it against its scenario
```

---

## 8. What is measured today

Nothing in this table is filled in yet, on purpose: no number is published
before it comes out of a campaign. Each row names the requirement it answers
(`docs/cahier-des-charges.md` §5.7, §7) so the gap is auditable.

| Measurement | Requirement | Target | Measured |
|---|---|---|---|
| Trigger latency, last threshold sample → pulse | PERF-01 | ≤ 2 ms | `TODO(campaign)` |
| Trigger → durable ("acquired"), reference hardware | PERF-02 | ≤ 20 ms | `TODO(campaign)` |
| Software share of that deadline, excluding media time | PERF-03 | ≤ 500 µs | `TODO(campaign)` |
| Deadline misses over 10 000 triggers under load | PERF-04 | 0 | `TODO(campaign)` |
| Frame send cost at the producer, p99 | PERF-05 | < 50 µs | `TODO(campaign)` |
| Capture running after kernel start, recovery included | PERF-06 | < 1 s | `TODO(campaign)` |
| Power-cut states explored, zero invariant violations | §7.3 | 1 000 000 | `TODO(campaign)` |
| QEMU power cuts survived, zero data loss | §7.4 | 2 000 | `TODO(campaign)` |
| Worst cases explained by a kernel trace | PERF-07 | 3 | `TODO(campaign)` |

What is verified **today** is functional, not temporal: the unit and
integration suites pass on PC, the recovery code is exercised on simulated
disk images, and the C trigger arithmetic and its independent Python twin
produce byte-identical decision files over generated scenario vectors. Those
are correctness results, not performance results.

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

**Deliberately out of scope:** ISO 26262 functional-safety certification,
mechanical and thermal qualification, crash detection of production quality,
GPS position, video, audio and driver identity — the last four are excluded by
design in the format itself (signal whitelist, no absolute time, no VIN).

---

## 10. License

Apache License 2.0 — see [`LICENSE`](LICENSE). Third-party attributions,
regulatory references and the status of the datasets we intend to use are in
[`NOTICE`](NOTICE). All repository content, code comments and documentation are
in English.
