# Ambre — validation plan

This document says **how every claim of the project is proved**, and where a
claim is still an open obligation. It covers the four test levels of the
specification (§7.1), the eight invariants that must hold after any power cut
(§7.2), the success thresholds per level (§7.7), the exact commands to
reproduce each campaign, and a traceability matrix from every level-S
requirement of §5 to the test that covers it.

Source of truth for the requirements: `docs/cahier-des-charges.md` §5 and §7.
Binary contract: `docs/FORMAT.md`.

**No measured figure appears in this document.** Every campaign result slot is
marked `TODO(campaign)` until the campaign has actually been run and its
result committed. Nothing here may be read as a claim that a measurement
exists.

---

## 1. The four test levels

| Level | Where | What is tested |
|---|---|---|
| 0 — unit | PC | Block encoding, choice of the table copy, the recovery state machine, the trigger computation |
| 1 — exhaustive exploration | PC | Every disk state reachable by a cut during a scenario |
| 2 — real cuts | QEMU | The complete chain, the virtual machine killed then restarted |
| 3 — hardware | Raspberry Pi 5 | Real electrical cuts, real latencies |

The format and recovery library is written in portable C11 and compiled
identically for the PC and for QNX. **That is what makes level 1 possible**: the
exhaustive exploration runs the very same code that will run on the target.

### Level 0 — unit tests (PC)

All C tests live in `tests/`, use the harness `tests/ambre_test.h`, and return
the number of failed checks as their exit code (0 = green).

| Test binary | Covers |
|---|---|
| `tests/test_crc32c.c` | CRC-32C check value, empty buffer, incremental vs one-shot, single bit flip |
| `tests/test_block.c` | pack/unpack round trip, corruption error codes, invalid lengths and chunks, multi-block reassembly and disk I/O, offset ↔ (segment, block) round trip |
| `tests/test_table.c` | table init and validate, read-best picks the highest generation, commit alternates and survives a corrupt copy, victim selection policy |
| `tests/test_scan.c` | valid chain and EMERG detection, scan stops on CRC / seq / session mismatch, table rebuild yields exactly the injected events |
| `tests/test_simdisk.c` | disk creation, write/read round trip, operation-log exactness, marks and last flush, prefix application and idempotency, torn write snapshot, volatile-cache snapshot, table corruption snapshot, overlapping writes, bounds and overflow, image restore, read logging gate, payload pool |
| `tests/test_trigger.c` | the DEC-08 boundary cases, lateral axis, DEC-04 locking with and without front restraint, DEC-03 external restraint, DEC-10 VRU, DEC-11 off-profile, DEC-05 T0, DEC-06 end of event, DEC-09 crash grouping, DEC-13 invalid sample, fixed memory size, 200 k samples without overflow, determinism |
| `tests/test_explore.c` | nominal prefixes, no crash → no event, counter consistency, acquisition boundary, complete-flag on both sides, double recovery, bit-flip detection, table destruction |
| `tests/test_mcap_py.py` | CRC check value and known vectors, layout sizes, table copy selection, multi-chunk reassembly, EMERG/CONT/SEAL fields, open event is not sealed, corrupt block dropped, torn multi-chunk record dropped, bad table CRC falls back to the other copy, export counts, MCAP structure, T0 at 60 s, verify passes, verify detects a wrong scenario, multi-chunk export stays valid, verify rejects an absolute timestamp in metadata, verify rejects a truncated file, Foxglove layout matches the exported topics, event listing, default event is the first sealed one, message count matches |
| `tests/test_trigger_py.py` | the Python twin of the trigger: division truncation, threshold conversion, int64 wrap, the three DEC-08 cases, long and lateral axes, restraint and VRU locking, T0 onset, event end, crash grouping, off-profile reasons, invalid sample exclusion, determinism, 200 k samples, dt clamping, non-monotonic timestamps, event close reset |
| `tests/test_powercut_py.py` | the power-cut orchestrator: start/probe/cut/restart cycle, guest command and cleanup, work dir handling, event parsing, bounded wait, five cuts and a valid report, resume does not replay, unflushed window produces violations, bucket classification, scenario expectations, cycle timing dry run, block-server cache semantics (retained/dropped writes, flush order journal, drop ratio) |

### Level 1 — exhaustive exploration of cut states (PC)

Method (`host/explore/`, described in `host/explore/ambre_explore.h`):

1. The journal runs on the PC against the simulated disk, which records every
   write and every flush in order.
2. For each prefix of that trace, the corresponding disk image is built.
3. Variants are added: last write torn at each sector boundary; subsets of the
   not-yet-flushed writes dropped (volatile disk cache); one flipped bit; both
   table copies wiped.
4. On each image, recovery runs and invariants I1..I8 are checked against the
   injected ground truth.
5. The same exploration is then replayed on the trace of the recovery itself
   (I8).

Ground truth is never taken from the recovered data: the checker compares
against the injected samples and the byte-exact injected class A snapshot
(`ambre_truth_t`), so a recovery that "invents" state is caught.

### Level 2 — real cuts under QEMU

The orchestrator (`host/powercut/`) starts QEMU, runs the scenario, kills the
QEMU process at a chosen instant, restarts it, exports the events and compares
them with the injected data.

- **Sweep**: one cut instant per millisecond, from 50 ms before the trigger to
  500 ms after, then random instants.
- **Known limit**: killing QEMU does not lose the writes already handed to the
  host. This level tests the *order* and the *interruption* of writes, not the
  loss of a volatile cache — level 1 covers that.
- **Extreme level**: the virtual disk is served by a small block server that
  retains unflushed writes and drops some of them at the cut (NBD). It also
  journals the flush orders it receives, which confirms QNX emits them.

### Level 3 — hardware (Raspberry Pi 5)

Real electrical cuts driven by a relay, on an SD card and on NVMe, with the
same comparison as level 2, plus the real latency measurements. Not started:
`TODO(campaign)`.

---

## 2. Invariants I1..I8

The invariants of the specification §7.2, their verification method, and the
test that covers them.

| ID | Invariant | Verification method | Covered by |
|---|---|---|---|
| **I1** | Recovery terminates, in a bounded number of steps, without crashing. | The checker counts the scanned blocks and the table commits and flags a violation when the bound is exceeded (`check_i1`, bound 64 commits). | `tests/test_explore.c` (`explore_nominal_prefixes`, `explore_counters_consistent`) + level 1 exploration, all prefixes |
| **I2** | Every event acquired before the cut is present, and its pre-crash class A data is identical to the injected data. | Byte comparison of the restored class A snapshot against the injected one, plus the presence of the EMERG block at the recorded start position (`check_i2`). | `tests/test_explore.c` (`explore_acquisition_boundary`) + level 1 exploration |
| **I3** | No event appears that was not triggered. | The set of events in the recovered table is compared with the set of injected triggers; a phantom event is flagged (`check_i3`). | `tests/test_explore.c` (`explore_no_crash_no_event`) + level 1 exploration |
| **I4** | Every restored sample is identical to the injected sample of the same instant. | Every restored sample is located by timestamp and compared with the injected value (`check_i4`); a mismatch is flagged. | level 1 exploration (sample-by-sample comparison) |
| **I5** | For an acquired event, the last restored sample is less than one flush period plus one block old at the moment of the cut. | The newest restored DATA timestamp is compared with the reference timestamp of the journal at the cut; the bound is a flush period plus one block (`AMBRE_I5_MAX_STALENESS_US` = 250 ms at the default 100 ms cadence) (`check_i4_i5`). | level 1 exploration + `test:tests/test_explore.c::explore_counters_consistent` |
| **I6** | A locked event survives any subsequent sequence of events and cuts. | The locked flag of every locked event is compared before and after recovery (`check_i6`); a lost or unlocked event is flagged. | level 1 exploration (variant `table`, overwrite policy applied) + `test:tests/test_trigger_py.py::test_lock_persists_after_event` |
| **I7** | "File complete" is "yes" if and only if the event was sealed before the cut. | The `file_complete` flag of the restored event is compared with the presence of the SEAL record and the seal cause (`check_i7`). | `tests/test_explore.c` (`explore_complete_flag_both_sides`) + `tests/test_mcap_py.py` (`test_open_event_is_not_sealed`, `test_verify_passes`) |
| **I8** | A cut during recovery or during archiving, followed by a new recovery, gives the same final state. | Recovery is run twice on the same image and the two resulting tables are compared field by field (`check_i8`); the nested variant replays the exploration on the recovery's own trace. | `tests/test_explore.c` (`explore_double_recovery`) + level 1 exploration with `--nested` (the "complet" level) |

The checker lives in `host/explore/ambre_invariants.c`. A violation sets the
corresponding `AMBRE_Ix_*` bit and keeps an actionable message (event id,
prefix, expected and found values). The level 1 report JSON carries
`violations_by_invariant` with one counter per invariant.

**Zero violations are tolerated at every level.** Any violation met during
development is kept as a non-regression test, together with the disk image
that reproduces it (`--violation-dir`).

---

## 3. Success thresholds per level

From the specification §7.7. "Socle" = level S, "Complet" = level C,
"Extrême" = level X.

| Measure | Socle | Complet | Extrême |
|---|---|---|---|
| Cut states explored | 10 000 | 1 000 000, nested cuts included | same, plus traces recorded on the target |
| QEMU cuts | 200 | 2 000 swept | 10 000 with a simulated volatile cache |
| Triggers measured | 1 000 | 10 000 under load | same on Raspberry Pi 5 |
| Forced process stops | — | 500 | 5 000 |
| Real electrical cuts | — | — | 500 |
| **Invariant violations tolerated** | **0** | **0** | **0** |

Current status of each threshold:

| Threshold | Result | Where |
|---|---|---|
| 10 000 cut states | `TODO(campaign)` | `build/explore.json` |
| 1 000 000 cut states, nested | `TODO(campaign)` | `build/explore-full.json` |
| 200 QEMU cuts | `TODO(campaign)` | campaign report JSON |
| 2 000 QEMU cuts, swept | `TODO(campaign)` | campaign report JSON |
| 10 000 triggers measured | `TODO(campaign)` | latency report |
| 500 forced process stops | `TODO(campaign)` | fault-injection report |
| 5 000 forced process stops | `TODO(campaign)` | fault-injection report |
| 500 real electrical cuts | `TODO(campaign)` | hardware report |
| Invariant violations | `TODO(campaign)` (target: 0) | `violations_by_invariant` in the report JSON |

The latency figures of PERF-01..PERF-05 follow the same rule: the target
(20 ms from trigger to acquired, 2 ms from the threshold-crossing sample,
500 µs of software time, 50 µs at the 99th percentile for a frame send) is a
**target**, and the measured value is `TODO(campaign)`.

---

## 4. Reproducing each campaign

All commands are run from the repository root. The repository uses a private
build directory per worker; `BUILD=build` below is the shared default of the
Makefile.

### 4.1 The whole gate (recommended)

```sh
./scripts/verify-all.sh          # everything that must be green
./scripts/verify-all.sh --quick  # skips the 1 000 000-state exploration
```

`scripts/verify-all.sh` runs, in order: `make sizecheck`, `make all`,
`make test`, the Python test suites, the scenario generator, the 10 000-state
exploration, the trigger cross-check, the 1 000 000-state exploration, and
`make mcap && make verify`. It prints a summary and exits 0 only when every
step is green.

### 4.2 Level 0 — unit tests

```sh
make all
make test                      # every tests/test_*.c binary
```

Individually:

```sh
./build/test_crc32c
./build/test_block
./build/test_table
./build/test_scan
./build/test_simdisk
./build/test_trigger
./build/test_explore
python3 tests/test_mcap_py.py
python3 tests/test_trigger_py.py
python3 tests/test_powercut_py.py
```

Each binary returns the number of failed checks: 0 means green.

### 4.3 Level 1 — exhaustive exploration

```sh
make scenario                  # generates build/scenario.txt (seed 1234)
make explore                   # 10 000 states, all variants
make explore-full              # 1 000 000 states, nested cuts included

# underlying commands:
build/ambre-explore --scenario build/scenario.txt \
  --states 10000 --jobs "$(nproc)" --variants all \
  --report build/explore.json
build/ambre-explore --scenario build/scenario.txt \
  --states 1000000 --jobs "$(nproc)" --variants all --nested \
  --report build/explore-full.json
```

The explorer returns 0 when no invariant was violated, 1 otherwise, and prints
the number of states explored, the rate and the first violation.

Scenario generation is independent of the Makefile:

```sh
python3 host/scenarios/generate.py --seed 1234 --duration-ms 30000 \
  --crash-at-ms 25000 --out build/scenario.txt
```

### 4.4 Trigger cross-check (C against the independent Python twin)

```sh
make crosscheck

# underlying commands:
python3 host/verify/gen_vectors.py --scenarios 10000 --seed 4242 \
  --out build/trigger_vectors.vec
build/ambre-dump-truth --vectors build/trigger_vectors.vec > build/trigger_c.dec
python3 host/verify/trigger_crosscheck.py \
  --vectors build/trigger_vectors.vec --c-output build/trigger_c.dec
```

The two implementations must produce byte-identical `.dec` files (`diff`).

### 4.5 Level 2 — real cuts under QEMU

```sh
# feasibility essay E1: does a write + flush on a raw partition reach the
# virtual disk as a flush order?
python3 host/powercut/e1_flush_probe.py --mode guest --image qnx80.img \
  --report build/e1.json

# feasibility essay E3: kill + restart + service again, N cycles
python3 host/powercut/e3_cycle_timing.py --image qnx80.img --cycles 20 \
  --report build/e3.json

# the campaign: one cut instant per millisecond, -50 ms to +500 ms
python3 host/powercut/powercut_campaign.py --image qnx80.img \
  --scenario build/scenario.txt \
  --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 1 \
  --report build/campaign.json --violation-file build/violations.jsonl

# the same campaign with random instants, several machines in parallel,
# resumable: interrupt it and re-run with --resume
python3 host/powercut/powercut_campaign.py --image qnx80.img \
  --scenario build/scenario.txt \
  --random-cuts 200 --seed 4242 --jobs 4 \
  --report build/campaign-random.json --resume
```

The orchestrator also runs without a QNX image, against a fake guest, which is
how its own test suite is exercised:

```sh
python3 host/powercut/powercut_campaign.py --backend dry-run \
  --scenario build/scenario.txt \
  --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 50 \
  --report build/campaign-dryrun.json
```

Extreme level (simulated volatile cache, NBD block server):

```sh
python3 host/powercut/faulty_block_server.py --socket /tmp/ambre-nbd.sock &
python3 host/powercut/powercut_campaign.py --image qnx80.img \
  --scenario build/scenario.txt --nbd-socket /tmp/ambre-nbd.sock \
  --random-cuts 10000 --jobs 4 --report build/campaign-nbd.json
```

### 4.6 Level 3 — hardware (Raspberry Pi 5)

Not started. The relay-driven cut driver, the SD card and the NVMe runs, and
the latency histogram on real hardware are `TODO(campaign)`.

### 4.7 Latency campaign

The latency campaign reuses the kernel trace events of OBS-03: the trigger,
the submission of the emergency write, the acknowledgement of the flush and the
sealing. The published result is a histogram with median, 99th percentile,
99.9th percentile, maximum, and the three worst cases explained. Not started:
`TODO(campaign)`.

### 4.8 Export and verification

```sh
make mcap        # scenario + journal image + MCAP export
make verify      # edr-verify against the scenario
```

Underlying commands:

```sh
python3 host/scenarios/generate.py --seed 7 --duration-ms 30000 \
  --crash-at-ms 20000 --out build/demo-scenario.txt
build/ambre-explore --scenario build/demo-scenario.txt --states 1 \
  --export-image build/journal.img
python3 host/mcap/export_mcap.py --image build/journal.img \
  --out build/event.mcap
python3 host/verify/edr_verify.py --mcap build/event.mcap \
  --scenario build/demo-scenario.txt
```

`edr-verify` checks the structure, the statistics, the channels, the
monotonicity of the times, the position of T0 at 60 s, the absence of any
absolute timestamp, the fidelity of the class A points against the injected
scenario, the mandatory metadata and the coherence of `file_complete`.

---

## 5. Traceability matrix — level S requirements

One line per level-S requirement of the specification §5. The column "how it
is verified" names the test or the campaign. Requirements that no test covers
yet are marked explicitly with `TODO(week 2+)`; no result is invented.

Legend of the verification column: `test:<file>::<case>` = a named case of a
test file; `campaign:<name>` = a campaign that produces the evidence.

### 5.1 Acquisition (ACQ)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| ACQ-01 | Frames from several producers accepted on `/dev/edr/in`, ≤ 10 ms and ≤ 4 KiB per frame | `TODO(week 2+)` — the QNX resource manager and the frame path do not exist yet; only the portable codec is covered |
| ACQ-02 | Timestamp on reception, monotonic clock, ≤ 100 µs resolution, no absolute timestamp | **partial** — `TODO(week 2+)` for the reception path; partially covered today by `test:tests/test_trigger_py.py::test_non_monotonic_ts_no_integration` and by `test:tests/test_mcap_py.py::test_verify_rejects_an_absolute_timestamp_in_metadata` |
| ACQ-03 | Supported rates 1 kHz / 100 Hz / 10 Hz, list fixed at build time | `TODO(week 2+)` — the signal whitelist table exists in `docs/FORMAT.md` §5, the runtime enforcement does not yet |
| ACQ-04 | A signal outside the whitelist is rejected and counted | `TODO(week 2+)` — rejection is specified in `docs/FORMAT.md` §5 but not implemented in `fmt/` yet |
| ACQ-06 | A capture interruption longer than three periods is marked with a `GAP` record | **partial** — `campaign:level 1` (the `GAP` record type exists in the format; the injection of a gap in the explorer is `TODO(week 2+)`) |
| ACQ-05 | A replay producer reads a scenario file and injects it | Level S but marked "D" (demonstration) in §5.1 — `scripts/demo.sh` (the scenario generator feeds the explorer) |

### 5.2 Trigger (DEC)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| DEC-01 | Trigger when the longitudinal speed variation exceeds 8 km/h in 150 ms or less | `test:tests/test_trigger.c::dec08_case2_81kmh_150ms_triggers` + `campaign:crosscheck` (C against the Python twin over 10 000 random scenarios) |
| DEC-02 | Same rule on the lateral axis | `test:tests/test_trigger.c::dec02_lateral_trigger` + `test:tests/test_trigger_py.py::test_lat_axis_trigger` |
| DEC-03 | Trigger on external "non-reversible restraint deployed"; the event is locked | `test:tests/test_trigger.c::dec03_external_restraint_locks` + `test:tests/test_trigger_py.py::test_restraint_fires_and_locks` |
| DEC-04 | Lock when the longitudinal variation exceeds 25 km/h in 150 ms or less and the configuration declares no non-reversible front restraint | `test:tests/test_trigger.c::dec04_lock_with_front_restraint`, `test:tests/test_trigger.c::dec04_lock_without_front_restraint` |
| DEC-05 | T0 is the first instant where the cumulative variation exceeds 0.8 km/h in 20 ms (5 ms lateral); for an external signal, the instant of the signal | `test:tests/test_trigger.c::dec05_t0_detection` + `test:tests/test_trigger_py.py::test_t0_is_onset_of_0_8kmh`, `test:tests/test_trigger_py.py::test_external_t0_is_signal_instant` |
| DEC-06 | The end of the event is the instant where the cumulative variation over 20 ms falls back to 0.8 km/h or less | `test:tests/test_trigger.c::dec06_end_of_event` + `test:tests/test_trigger_py.py::test_event_ends_when_window_falls` |
| DEC-07 | Bounded time, fixed memory, integer arithmetic | `test:tests/test_trigger.c::fixed_memory_size`, `test:tests/test_trigger.c::fixed_memory_no_overflow_200k` (inspection of `fmt/ambre_trigger.h`, contract A1..A9) |
| DEC-08 | Boundary cases: 7.9 km/h in 150 ms does not trigger, 8.1 km/h in 150 ms triggers, 8.1 km/h reached in 151 ms does not trigger | `test:tests/test_trigger.c::dec08_case1_79kmh_150ms_no_trigger`, `dec08_case2_81kmh_150ms_triggers`, `dec08_case3_81kmh_151ms_no_trigger_anchored` + the same three cases in `tests/test_trigger_py.py` |

### 5.3 Freeze and recording (GEL)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| GEL-01 | On trigger, the emergency record (pre-crash class A) is written in one contiguous write | `campaign:level 1` (the explorer asserts the EMERG write is a single operation) + inspection of `fmt/ambre_journal.h`; a dedicated unit test is `TODO(week 2+)` |
| GEL-02 | The emergency write is followed by the allocation-table update, then a flush of the medium cache; the event is *acquired* at the flush acknowledgement | `campaign:level 1` (acquisition boundary: `test:tests/test_explore.c::explore_acquisition_boundary`) |
| GEL-03 | The path from trigger to "acquired" allocates nothing, page-faults nothing and waits on no lower-priority process | **partial** — inspection of `fmt/ambre_journal.h` (fixed-size state, pre-allocated staging buffers) + `campaign:latency` for the software part |
| GEL-04 | During the event, a continuation record is written and flushed every 10 ms | `campaign:level 1` (the during-event flush cadence is part of the recorded trace) + inspection of `fmt/ambre_journal.h`; a dedicated unit test is `TODO(week 2+)` |
| GEL-05 | At the end of the event, a sealing record carries the maximum delta-v, its instant, the deployment times and "file complete = yes" | `test:tests/test_mcap_py.py::test_emerg_cont_seal_fields` + `campaign:level 1` |
| GEL-06 | No archive write happens while an event is open | **partial** — inspection of the process roles (`docs/ARCHITECTURE.md` §1: `edr-archive` reads only sealed segments) — a runtime test is `TODO(week 2+)` |

| GEL-07 and GEL-08 are level C (not in this table). GEL-07 — the class B data
already written before the trigger, and the loss bound — is covered by
invariant **I5** (see §2), which the level 1 exploration checks on every
state.
### 5.4 Storage and capacity (STO)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| STO-01 | The critical path writes to a dedicated raw partition, without going through a file system | **partial** — inspection: `docs/ARCHITECTURE.md` §3 and the disk vtable `ambre_disk_t` in `fmt/ambre_fmt.h` (no file-system call anywhere in `fmt/`) |
| STO-02 | Every block carries a magic, a version, a 64-bit sequence number, a session id, a type, a length and a CRC-32C | `test:tests/test_block.c::block_pack_unpack_roundtrip`, `block_corruption_error_codes` + `test:tests/test_crc32c.c::crc32c_check_value` |
| STO-03 | An acknowledged block is not rewritten before its segment is released | `test:tests/test_simdisk.c::overlapping_writes`, `logging_exactness` + inspection of the append-only write path in `fmt/ambre_journal.h` |
| STO-04 | The allocation table exists in two copies with a generation counter and a CRC; an update touches only one copy | `test:tests/test_table.c::table_read_best_highest_gen`, `table_commit_alternates_corrupt` |
| STO-05 | The journal holds at least three frozen events at the same time (target: six) | `campaign:level 1` (multi-event scenarios, overwrite policy) + `test:tests/test_table.c::table_pick_victim_policy` |
| STO-06 | When space runs out, the unlocked events are overwritten from the oldest to the newest; a locked event is never overwritten | `test:tests/test_table.c::table_pick_victim_policy` + invariant I6 in `campaign:level 1` |

### 5.5 Recovery after a cut (REP)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| REP-01 | At start-up, recovery runs before any new capture | **partial** — inspection of `fmt/ambre_journal.h` (`ambre_journal_open` runs `ambre_recover` unless the partition was just formatted) — a runtime test on the target is `TODO(week 2+)` |
| REP-02 | Recovery keeps the most recent valid table copy; if none is valid, it rebuilds the table by scanning | `test:tests/test_table.c::table_read_best_highest_gen` + `test:tests/test_scan.c::scan_rebuild_table_exact_events` + `test:tests/test_explore.c::explore_table_destruction` |
| REP-03 | Every event left open is sealed with "file complete = no" and the cause | `campaign:level 1` — invariant I7 + `test:tests/test_explore.c::explore_complete_flag_both_sides` |
| REP-04 | Recovery gives the same result if run twice, or interrupted by a new cut then relaunched | `test:tests/test_explore.c::explore_double_recovery` + invariant I8 in `campaign:level 1` (nested variant) |
| REP-05 | Recovery never surfaces an event that was not triggered, and never returns data with an invalid CRC | `campaign:level 1` — invariants I3 and I4 + `test:tests/test_explore.c::explore_bitflip_detected` |

### 5.6 Restitution (EXP)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| EXP-01 | `edrctl status` and `edrctl list` show the state, the events, their locking and their completeness | `TODO(week 2+)` — `edrctl` (the QNX service tool) does not exist yet; the equivalent on the PC is `build/ambre-explore --export-image` plus `python3 host/mcap/export_mcap.py --list` |
| EXP-02 | Each event is exported to one valid MCAP file: summary, index, chunk CRCs | `test:tests/test_mcap_py.py::test_mcap_structure`, `test_export_counts`, `test_message_count_matches` + `campaign:make verify` |
| EXP-03 | The file opens in Foxglove without an extension; a screen layout is provided | `test:tests/test_mcap_py.py::test_foxglove_layout_matches_the_exported_topics` + `foxglove/ambre-layout.json`; the opening in Foxglove itself is a manual demonstration, `TODO(campaign)` |

### 5.7 Real time (PERF)

All the PERF requirements are verified by **measurement** (M in §5.7). No
measured value exists yet; every figure below is a target.

| ID | Requirement (S) | How it is verified |
|---|---|---|
| PERF-01 | From the sample that crosses the threshold to the trigger pulse: ≤ 2 ms | `TODO(week 2+)` — the latency campaign has not been run: `campaign:latency` — `TODO(campaign)` |
| PERF-02 | From the trigger to "acquired": ≤ 20 ms on the reference hardware | `TODO(week 2+)` — the latency campaign has not been run: `campaign:latency` — `TODO(campaign)` |
| PERF-03 | The software part of that delay, excluding the medium, does not exceed 500 µs | `TODO(week 2+)` — the latency campaign has not been run: `campaign:latency` — `TODO(campaign)` |
| PERF-04 | No overrun over 1 000 triggers (S), 10 000 under processor and disk load (C) | `TODO(week 2+)` — the latency campaign has not been run: `campaign:latency` — `TODO(campaign)` |
| PERF-05 | Sending a frame costs the producer less than 50 µs at the 99th percentile | `TODO(week 2+)` — the input path does not exist yet; the campaign slot is `TODO(campaign)` |

### 5.8 Robustness (ROB)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| ROB-01 | A power cut at any instant respects the invariants of §7.2 | `campaign:level 1` (every prefix of the trace, all variants) + `campaign:level 2` (QEMU cuts) |
| ROB-02 | A crash of `edr-archive` or `edrctl` has no effect on capture nor on the freeze | `TODO(week 2+)` — requires the QNX processes; the architectural guarantee is that neither process touches the raw partition for writing (`docs/ARCHITECTURE.md` §1) |
| ROB-07 | A corrupted block (bit flipped) is detected and discarded, without affecting the neighbouring blocks | `test:tests/test_explore.c::explore_bitflip_detected` + `test:tests/test_block.c::block_corruption_error_codes` + `test:tests/test_mcap_py.py::test_corrupt_block_is_dropped` |

### 5.9 Security and privacy (VIE, SEC)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| VIE-01 | The system cannot record a VIN, a position, an absolute date or time, nor the driver identity; the signal whitelist imposes it | **partial** — `test:tests/test_mcap_py.py::test_verify_rejects_an_absolute_timestamp_in_metadata` + inspection of the whitelist in `docs/FORMAT.md` §5; the runtime rejection of a non-whitelisted signal is `TODO(week 2+)` |
| VIE-02 | Time is expressed relative to T0 and in ignition cycles | `test:tests/test_mcap_py.py::test_t0_is_at_sixty_seconds` + `test:tests/test_mcap_py.py::test_verify_rejects_an_absolute_timestamp_in_metadata` |
| SEC-05 | A malformed frame is rejected and counted, without effect on the service | `TODO(week 2+)` — the input path does not exist yet; the codec rejects an invalid block at the format level (`test:tests/test_block.c::block_invalid_lengths_and_chunks`) |

### 5.10 Observability (OBS)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| OBS-01 | The processes write to the QNX system log (slog2) | `TODO(week 2+)` — requires the QNX processes |
| OBS-02 | `/dev/edr/status` gives the state as text: mode, current segment, events, counters, health | `TODO(week 2+)` — the counters exist in `ambre_journal_stats_t` (`fmt/ambre_journal.h`) but the resource manager interface does not exist yet |
| OBS-03 | User events are inserted into the kernel trace at the trigger, at the submission of the write, at the flush acknowledgement and at the sealing | `TODO(week 2+)` — the equivalent milestones exist in the portable core as the journal marks `AMBRE_MARK_EMERG_WRITTEN`, `AMBRE_MARK_ACQUIRED`, `AMBRE_MARK_SEALED` (`fmt/ambre_journal.h`), but the kernel trace insertion is on the target |

### 5.11 Use of QNX-specific technologies (QNX)

| ID | Requirement (S) | How it is verified |
|---|---|---|
| QNX-01 | The journal is a resource manager; its interface is reduced to `open`, `write`, `read` and `devctl` | `TODO(week 2+)` — `target/` is empty; the portable core exposes exactly the four operations through the disk vtable `read` / `write` / `flush` (+ optional `mark`) |
| QNX-02 | A kernel trace shows that the disk driver handles the emergency write at the journal's priority | `TODO(week 2+)` — requires the QNX image; this is the demonstration item of the video (§8, 2:25–2:45) |
| QNX-03 | Timeouts and inter-process notifications use pulses | `TODO(week 2+)` — requires the QNX processes |
| QNX-04 | The critical processes lock their memory at start-up | `TODO(week 2+)` — requires the QNX processes; on the PC the equivalent property is checked by the fixed-size structures of `fmt/ambre_journal.h` |

### 5.12 Summary of the coverage

Three columns, counted line by line from the tables above:

- **Covered**: a named test case or a named campaign proves the requirement.
- **Partial**: the requirement is only inspected, or only part of it is proved
  (the cell says which part).
- **TODO**: the line is marked `TODO(week 2+)`; nothing is claimed yet.

| Family | Level-S requirements | Covered | Partial | TODO(week 2+) |
|---|---|---|---|---|
| ACQ | 6 | 1 | 2 | 3 |
| DEC | 8 | 8 | 0 | 0 |
| GEL | 6 | 2 | 2 | 2 |
| STO | 6 | 5 | 1 | 0 |
| REP | 5 | 4 | 1 | 0 |
| EXP | 3 | 2 | 0 | 1 |
| PERF | 5 | 0 | 0 | 5 |
| ROB | 3 | 2 | 0 | 1 |
| VIE / SEC | 3 | 1 | 1 | 1 |
| OBS | 3 | 0 | 0 | 3 |
| QNX | 4 | 0 | 0 | 4 |
| **Total** | **52** | **25** | **7** | **20** |

Note on the counts: the "Covered" column counts a requirement only when a
**named test case or a named campaign** proves it. A requirement that is only
**inspected** — the raw partition with no file system, the fixed-size journal
state, the process roles — is counted as "Partial", because inspection is
weaker than a test. When a line carries both `**partial**` and
`TODO(week 2+)`, it is counted in the "Partial" column, and the TODO marker
says which part is still missing. GEL-07 and GEL-08 are level C and are not
counted here. The PERF family is entirely TODO because the latency campaign
has not been run: a target is not a verification. The gap is otherwise
concentrated in the requirements that need the QNX target (`target/` is still
empty): the resource manager, the service interfaces, the security policy, the
kernel trace. They are scheduled in weeks 2 and 3 of the plan (specification
§10).

---

## 6. Rules of the campaign

1. A violation met during development is kept: the disk image that reproduces
   it is stored (`--violation-dir`) and a non-regression test is written.
2. No result is published without the command that produced it.
3. Under QEMU, durations do not represent real hardware. They are measured in
   instruction-count mode and presented as emulated.
4. The 20 ms freeze delay is a target until the latency campaign says
   otherwise.
