# Ambre — architecture

Ambre is an Event Data Recorder (EDR) for QNX 8.0: an automotive "black box"
that must write to the medium **at the worst possible moment** — the instant a
crash is detected.

> **Implementation status (2026-11-14).** This document describes the *target*
> runtime architecture. The QNX side **exists and runs**: the SDP 8.0 is
> installed (`~/qnx800`, qcc 12.2.0), `make qnx` links the five processes for
> `nto x86_64`, the image boots under QEMU and the chain described below —
> capture, trigger, acquisition on a raw medium, power cut, recovery — runs in
> the guest (`docs/QNX-DEMO.md`). What is still design here: the product image
> `image/ambre.build` (two partitions, policy, HAM) has never been assembled
> nor booted, the latencies are not measured on target, and the security
> policy is not applied by the kernel. Sections 1 to 3 are design; sections 4 to
> 7 describe code that exists and is measured in `docs/VALIDATION.md`.

This document describes the runtime architecture, the critical path that turns a
detected crash into an *acquired* event, the two-stage storage model, and the
guarantees that survive a power cut.

Normative references:

- `docs/FORMAT.md` — binary format of the journal (contract of `fmt/`).
- `docs/cahier-des-charges.md` — functional source of truth (sections 4, 5, 7).
- `docs/VALIDATION.md` — how every claim below is proved.
- `docs/AGENT-BRIEF.md` — frozen headers, frozen CLIs.

---

## 1. Processes

Ambre runs one QNX process per role. Priorities are QNX scheduling priorities:
higher means more urgent.

| Process | Priority | Role | May access |
|---|---|---|---|
| `edr-capture` | 60–61 | Receives frames, timestamps them, computes the trigger, keeps the class A buffers. On trigger it sends the journal the message containing the class A snapshot. | The journal channel only |
| `edr-journal` | 62 | **Sole owner of the raw partition.** Writes, pins, seals, recovers at start-up. | The raw partition |
| `edr-archive` | 8 | Converts sealed events to MCAP; manages retention. | The archive, and the journal **read-only** |
| `edrctl` | 10 | Service tool: status, list, export, verify. | The service interfaces |
| `edr-replay` | 50 | Demonstration producer: replays a scenario. | The input only |

Rules that follow from this table:

- Exactly one process writes to the raw partition (`edr-journal`). No other
  process opens it for writing, so no writer can interleave with the critical
  path.
- `edr-archive` is the only reader of the raw partition, and it only reads
  segments that are already sealed. It never touches the segment currently
  being written.
- `edr-capture` never touches the medium. It hands a *message* to the journal;
  the priority of the journal process is therefore what the media driver
  inherits on the write path.
- `edr-archive` and `edrctl` are strictly lower priority than the capture and
  journal processes, so an archive write can never delay a freeze.

```
  producers                 QNX 8.0 (one process per box)
 ┌────────────┐  frames 10 ms  ┌──────────────┐ trigger      ┌───────────────┐
 │ edr-replay │───────────────►│ edr-capture  │─────────────►│  edr-journal  │
 │ (or CAN,   │  /dev/edr/in   │  + detection │ + class A    │  resource     │
 │  zenoh)    │                │  (prio 60-61)│  snapshot    │  manager      │
 └────────────┘                └──────────────┘─────────────►│  (prio 62)    │
                                                              └──────┬────────┘
  ┌────────────┐  /dev/edr/ctl, status                               │ raw writes
  │  edrctl    │◄───────────────────────────────────────────────┐    ▼
  │ (service)  │                                                │ ┌─────────────────┐
  └────────────┘      ┌──────────────┐  reads sealed segments   │ │ raw partition   │
                     │ edr-archive  │◄─────────────────────────┴─│ (journal)       │
                     │  (prio 8)    │──► MCAP files ──►           └─────────────────┘
                     └──────────────┘    Power-Safe partition
```

*(The diagram is the §4.4 diagram of the specification, with English labels;
the topology is unchanged.)*

---

## 2. The critical path of the freeze

The freeze is the one path in the whole system with a deadline. From the
instant the trigger fires to the instant the event is **acquired**, the target
is **20 ms** (PERF-02), of which the software part must stay under 500 µs
(PERF-03).

The path is exactly three steps, and nothing else may happen on it
(`fmt/ambre_journal.h`, GEL-01/GEL-02/GEL-03):

```
  trigger fires
        │
        ▼
  1. ONE contiguous write of the EMERG record
     (the class A pre-crash snapshot, one or more blocks, one write call)
        │
        ▼
  2. ONE allocation-table commit
     (the copy with the lower generation, generation + 1, then its CRC)
        │
        ▼
  3. ONE disk flush (cache / sync)
        │
        ▼
  on flush acknowledgement → the event is ACQUIRED
```

Why the order is fixed:

- **Step 1 before step 2.** The table describes the events; the EMERG record
  *is* the proof that an event happened. If the table were committed first and
  the power died before the record reached the medium, recovery would find a
  table entry with no data behind it.
- **Step 2 before step 3.** The flush is the durability point. Acknowledging it
  is what makes "acquired" true. Flushing before the table commit would make
  the record durable while the table still ignored it.
- **One write, not several.** The EMERG record is submitted as a single
  contiguous write so that the medium sees one transaction rather than a
  sequence of sector writes that could be interrupted half-way.

Properties of the path:

- No dynamic allocation, no page fault, no wait on a lower-priority process
  (GEL-03). The journal state is a fixed-size struct with pre-allocated
  staging buffers; the trigger path only moves bytes into them.
- The event is "acquired" **only** on the flush acknowledgement, never on the
  submission of the write (GEL-02).
- After acquisition, and only then, the during-event cadence starts: one CONT
  record plus one flush every 10 ms (GEL-04). At the end of the event, a SEAL
  record carries the maximum delta-v, its instant, the restraint deployment
  times and `file_complete = yes` (GEL-05).
- No archive write happens while an event is open (GEL-06).

The timing target of 20 ms is a **target** on the reference hardware; the
on-target measurement is `BLOCKED(not measured on target yet)` — the guest
answers `devctl` STATUS with `last_freeze_us`/`worst_freeze_us`, but no latency
campaign has been run on it (`make latency-all` on the booted image). On the POSIX backend the campaign **has** run and is published
(`docs/CAMPAIGN-LATENCY.md`): trigger → acquired median 2.7–4.0 ms, p99
9.3–35.0 ms, max 108 ms — 99.7 % of it NVMe media time — and the software
share alone is a median of 10 µs against the 500 µs budget.

---

## 3. Why a raw partition and not a file system

The critical path needs a *bounded* durability point. On the QNX 8.0
Power-Safe file system, it does not have one:

- `fsync()` on Power-Safe synchronises the **whole file system**, not the
  file, and is documented as "potentially expensive".
- `O_SYNC` is **ignored** on Power-Safe.
- A single data modification can therefore trigger the copy of a dozen
  blocks, and the cost of that copy is not bounded by the size of the write.

That behaviour is *excellent* for consistency and *bad* for a 20 ms deadline.
Ambre therefore splits storage:

| Stage | Medium | Written by | Purpose |
|---|---|---|---|
| 1 — raw journal | dedicated raw partition, no file system (STO-01) | `edr-journal` only | the durable record, on the critical path |
| 2 — MCAP archive | a separate Power-Safe partition | `edr-archive` | the readable export, off the critical path |

On the raw partition the flush order reaches the medium as a flush order
(no file-system journal to update, no whole-system synchronisation), so the
cost of step 3 is a property of the medium, not of the file system. This is
still an assumption: essay E1 (`host/powercut/e1_flush_probe.py`) exists to
confirm that a write plus a flush on a raw partition reaches the virtual disk
as a flush order. Its result is `BLOCKED(not measured on target yet)`: the essay needs a real
`io-blk` behind the device — on target, build and boot
`mkqnximage -f image/ambre.build`, then run
`python3 host/powercut/e1_flush_probe.py --mode guest --image qnx80.img`
(the host-mode NBD/`qemu-io` fallback needs the same image). The PC side of
the probe is pinned honest by
`test:tests/test_port_posix.c::flush probe is honest`: it reports whether the
flush order was accepted and what it cost, never that it reached the medium.

The archive pays the Power-Safe price instead, and pays it off the critical
path: write to a temporary name, sync, rename, sync the directory. The journal
segment is only released after the archive has been read back successfully
(REP-07).

### Partition layout

```
offset 0      copy A of the allocation table   (512 B)
offset 512    copy B of the allocation table   (512 B)
offset 1024   data segments 0..23, 2 MiB each  (48 MiB)

block_offset(seg, blk) = 1024 + seg * 2097152 + blk * 512
```

- The table exists in **two copies** with a monotonic generation counter and a
  CRC. A commit writes only the copy with the *lower* generation (STO-04); a
  reader keeps the valid copy with the *highest* generation (REP-02).
- A block is never rewritten before its segment is released (STO-03).
- Segment states: `FREE`, `CURRENT`, `FROZEN` (pinned by an acquired event),
  `LOCKED` (holds a locked event, never reused), `ARCHIVED`.

Full field layout: `docs/FORMAT.md` §1, §7, §8.

---

## 4. The two-stage format

### Stage 1 — the raw journal

The journal is a sequence of 512-byte blocks, each one self-describing:

| Field | Purpose |
|---|---|
| `magic` | `"AMBR"`, rejects foreign data |
| `version` | format version |
| `rec_type` | `DATA`, `EMERG`, `CONT`, `SEAL`, `GAP` |
| `flags` | bit 0 = continuation |
| `seq` | 64-bit monotonic sequence number, per session |
| `session_id` | boot session; stale-data detector |
| `first_ts_us` | monotonic µs of the first sample of the record |
| `chunk_index` / `chunk_count` | position of the block inside its record |
| `payload_len` / `total_len` | valid bytes here / in the whole record |
| `payload_crc` | CRC-32C over the payload |
| `block_crc` | CRC-32C over bytes `[0, 508)` |

- A record longer than one block occupies **consecutive** blocks and is
  submitted as **one contiguous write** (GEL-01). A record is valid only if
  every chunk is valid and the bytes reassemble consistently; otherwise the
  whole record is torn and is dropped (REP-05).
- CRC-32C is the Castagnoli variant (reflected polynomial `0x82F63B78`,
  initial and final XOR `0xFFFFFFFF`); check value
  `CRC32C("123456789") = 0xE3069283`.
- The journal is **self-describing**: an `EMERG` record alone proves that an
  event happened. If both table copies are unreadable, recovery rebuilds the
  table by scanning the segments (REP-02).
- The journal carries **no absolute time, no VIN, no position, no driver
  identity** (VIE-01, VIE-02). Time is monotonic microseconds since an
  unspecified epoch, plus an ignition-cycle counter.

### Stage 2 — the MCAP archive

`edr-archive` reads the sealed segments of the journal and writes one MCAP
file per event, with seven channels (SPEC §6.4):

| Channel | Content | Rate |
|---|---|---|
| `/edr/a/precrash` | one class A point | 2 Hz |
| `/edr/a/delta_v` | longitudinal / lateral delta-v | 100 Hz |
| `/edr/a/event` | event summary | 1 per event |
| `/edr/b/accel` | accelerations x, y, z | 1 kHz |
| `/edr/b/vehicle` | dynamic signals | 100 Hz |
| `/edr/b/status` | states | 10 Hz |
| `/edr/meta/gaps` | capture gaps | on demand |

JSON encoding with a JSON schema per channel, chunk CRC enabled, no
compression, summary section and statistics present (EXP-02). **T0 is placed at
60 s inside the file**; no absolute timestamp appears anywhere. Metadata
records: `edr.event`, `edr.vehicle_type`, `edr.build`, `edr.integrity`.

Why two stages: the raw journal is optimised for *durability under a deadline*
(raw blocks, no file system, no index to corrupt); the MCAP archive is
optimised for *readability and interoperability* (Foxglove opens it without an
extension, EXP-03). The conversion is done later, at low priority, from the
journal — so the archive can be rebuilt at any time from the durable source.

---

## 5. Recovery after a power cut

Recovery runs **before any new capture** (REP-01) and follows `docs/FORMAT.md`
§10:

1. Read both table copies; keep the valid one with the highest generation.
2. If neither is valid, rebuild the table by scanning every segment in use.
3. In the current segment, find the last valid block; everything after it is
   garbage and will be overwritten.
4. Drop torn records: any EMERG/CONT whose chunks are not all valid.
5. Register an EMERG record the table does not know yet — this is what makes
   the format self-describing.
6. Seal every event still open with `file_complete = no`, cause `POWERCUT`,
   then commit the table.
7. Never return data whose CRC is invalid; never invent an event.

Recovery only *appends* records and *alternates* table updates, so it can be
interrupted by a new power cut and replayed with the same final state (REP-04,
invariant I8).

---

## 6. What survives a cut

Timing is relative to the trigger and to the acquisition of the event.

```
        before T0            T0    trigger                  end + 30 ms
  ─────────────────────────┬──────────┬───────────────────────────┬──────────────
  continuous journal        │          │ t = 0                     │
  (blocks + flush/100 ms)  │          │  1. emergency record      │ 4. sealing
                           │          │     (class A)             │    "complete = yes"
                           │          │  2. allocation table      │
                           │          │  3. flush  ──► ACQUIRED   │ 5. later, calmly:
                           │          │     (target ≤ 20 ms)      │    MCAP archive
                           │          │  then every 10 ms:        │
                           │          │  continuation + flush     │
```

| Power cut… | What survives |
|---|---|
| before the trigger | Nothing to keep: there is no event |
| between the trigger and "acquired" | **Not guaranteed.** This is the risk window, and it is exactly the freeze delay |
| after "acquired", before sealing | All of the pre-crash data, and the crash up to the last flush (at most 10 ms of loss). The event is restored with "file complete = no" |
| after sealing | Everything, with "file complete = yes" |

The risk window is not a bug: it is the physical meaning of a 20 ms freeze
budget. It is quantified by the latency campaign on the POSIX backend
(`docs/CAMPAIGN-LATENCY.md`: trigger → acquired median 2.7–4.0 ms, 99.7 %
media time); the quantification on the reference hardware is
`BLOCKED(not measured on target yet)`.

---

## 7. The portable core

The same C11 sources build for the PC tools and, once written, for QNX 8.0.
File names below are the real ones in the tree (2026-11-11):

| Layer | Files | Role |
|---|---|---|
| `fmt/` | `ambre_crc32c.c` | CRC-32C (Castagnoli), incremental and one-shot |
| | `ambre_fmt_block.c`, `ambre_fmt_record.c` | 512-byte block codec; record-level access (a record = consecutive blocks sharing seq/session/chunk count) |
| | `ambre_fmt_table.c` | allocation table: two copies, generation counter, commit/read-best, victim policy |
| | `ambre_fmt_scan.c` | segment scan: last valid block, EMERG detection, table rebuild |
| | `ambre_fmt_recovery.c` | power-cut recovery (FORMAT.md §10) |
| | `ambre_journal_core.c`, `ambre_journal_event.c` | journal engine: lifecycle, capture, flush cadence, segment rotation; and the event path: trigger → EMERG write → table commit → flush (acquired), continuation, sealing, read back |
| | `ambre_trigger.c`, `ambre_trigger_window.c` | UN R160 trigger arithmetic (contract A1..A9) and its ring-buffer / monotonic min-max deque helpers |
| `host/simdisk/` | `ambre_simdisk.c` | simulated disk that logs every write, flush and mark in order; cut variants (`ambre_cut_variant_t`: torn, dropped unflushed writes, bit flip, table wipe) |
| `host/explore/` | `ambre_explore.c`, `ambre_explore_gen.c`, `ambre_explore_run.c`, `ambre_explore_worker.c`, `ambre_invariants.c`, `ambre_memdisk.c`, `ambre_scenario.c`, `main.c` | exhaustive exploration of cut states (parallel workers), invariant checker I1..I8, in-memory disk with dirty tracking, scenario loader |
| `host/web/` | `ambre_web_json.c`, `ambre_web_setup.c`, `ambre_web_state.c`, `ambre_web_sweep.c`, `wasm/main_wasm.c` | JSON API over the explorer, used twice: by the native test `tests/test_web_api.c` and by the WebAssembly module the website loads (`scripts/build-web.sh`). It calls the explorer's own steps (`ambre_explore_build_image`, `ambre_explore_judge`, `ambre_journal_read_event`); the only thing it does not link is `ambre_explore_run.c`, whose fork-per-core workers a browser has no use for |
| `host/mcap/` | `export_mcap.py`, `ambre_journal_reader.py`, `ambre_mcap_writer.py` | journal image → MCAP export |
| `host/verify/` | `edr_verify.py`, `ambre_trigger.py`, `ambre_trigger_cli.py`, `gen_vectors.py`, `trigger_crosscheck.py` | independent verification of the export; independent Python trigger twin and DEC-12 cross-check |
| `host/powercut/` | `powercut_campaign.py`, `qemu_vm.py`, `vm_util.py`, `e1_flush_probe.py`, `e3_cycle_timing.py`, `faulty_block_server.py` | QEMU power-cut orchestrator (level 2) — exists and passes its dry-run tests; no real cut run yet (no QNX image) |
| `target/` | **empty** | QNX processes (`edr-capture`, `edr-journal`, `edr-archive`, `edrctl`, `edr-replay`) — not implemented |

The disk is reached through a small vtable (`ambre_disk_t`: `read`, `write`,
`flush`, optional `mark`). On the PC it is the simulated disk; on QNX it is the
raw partition. `fmt/` uses no platform-specific API, which is what makes the
exhaustive exploration of level 1 possible at all.

---

## 8. See also

- `docs/FORMAT.md` — the binary contract.
- `docs/VALIDATION.md` — invariants I1..I8, campaigns, traceability matrix.
- `codelab/README.md` — "Writing a journal that survives power cuts on
  QNX 8.0, and proving it", step by step.
- `scripts/demo.sh` — the one-command demonstration.
