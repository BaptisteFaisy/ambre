# Ambre — campaign report

Status: **2026-11-11**, development machine x86-64, 12 cores, gcc 13.3.0,
Python 3.12.3. **Every figure in this report was produced by the command
quoted next to it, run on that machine on that day.** Slots with no
measurement say `TODO(campaign)` and name the command that will produce the
number. Nothing here is extrapolated, and no QNX-target number is published:
no QNX SDP 8.0 is installed (see `docs/QNX-PORT.md`).

---

## 1. Executive summary

| # | Requirement / spec ref | Target | Measured (2026-11-11) | Reproduce with |
|---|---|---|---|---|
| Level 0 | unit suites green (§7.1) | all green | **19 suites, 41 672 checks, 0 failures** (18 C + 1 C++) | `make test` + `make test_archive_cpp` |
| Level 1 | cut states explored, 0 violations (§7.7 socle 10 000 / complet 1 000 000) | exhaustive | **64 591 states = every reachable state**, 46 390 acquisitions observed, **0 violations of I1..I8** | `make explore-full` |
| Level 1 | socle threshold (10 000 states) | 10 000, 0 violations | **10 000 states, 0 violations, 29.3 s** (342 states/s) | `make explore` |
| Level 2 | process cuts, POSIX bench (§7.6, 500 complet) | 500 | **TODO(campaign)** — harness present, campaign owned by another agent, not finished | `python3 host/e2e/e2e_run.py --bins build --scenario build/scenario.txt --cut-at acquired --out report.json` |
| Level 2 | QEMU cuts (§7.4, 200 socle) | 200 | **0/200 — not started**: no QNX SDP or image on the machine; harness green in dry-run (18 tests) | `python3 host/powercut/powercut_campaign.py --image qnx80.img --scenario build/scenario.txt --random-cuts 200 --seed 4242 --jobs 4 --report build/campaign.json` |
| DEC-12 | C trigger = Python twin, byte-identical | 10 000 scenarios | **10 000 scenarios, 2 744 840 decision lines compared, 0 divergences** | `make crosscheck` |
| §12.10 | line coverage of `fmt/` | > 95 % | **97.96 %** (1 343 / 1 371 lines) | `BUILD=build-doc-cov sh scripts/coverage.sh` |
| §12.10 | random-data testing | 24 h | smoke only: **52 hostile images, 0 violations, 5.0 s** (seed 20261111); the 24 h run is `TODO(campaign)` | `make fuzz` (smoke); 24 h: `./build/ambre-fuzz --budget 86400` |
| EXP-02 | MCAP export verified against its scenario | 10/10 | **10/10 checks pass** (incl. `class_a.fidelity`, 11 points) | `make verify` |
| §8 | one-command demo | green | **`DEMO OK`** — 1 000 cut states explored, 0 violations, export verified | `make demo` |
| QNX-01..07 | QNX glue compiles against the stubs | 5/5 | **5/5 glue files OK** | `make qnx-syntax` |
| SEC/VIE | image + security policy statically coherent | all checks | **11/11 static checks pass** | `make image-check` |
| PERF-01..07 | latency (2 ms / 20 ms / 500 µs) | measured on target | **not measured** — requires the QNX target and its kernel trace | see §7 below |
| Level 3 | real electrical cuts (§7.7 extrême, 500) | 500 | **TODO(campaign)** — no Raspberry Pi 5 bench | — |

House rule (`make sizecheck`, `python3 scripts/sizecheck.py`): 172 files
scanned, 4 violations on 2026-11-11 — all four in `host/energy/energy_model.py`
and `host/latency/measure.py`, work-in-progress files owned by other agents;
0 violations in `fmt/`, `target/`, `tests/` and in the documentation.

---

## 2. Level 0 — unit tests (PC)

```sh
make test                # 18 C suites
make test_archive_cpp    # the C++17 archive suite
```

Measured: **`ALL TESTS PASSED`** — 41 601 checks over the 18 C suites, plus
71 checks for the C++ archive suite: **19 suites, 41 672 checks, 0 failures**.
Per suite: block 118, capture 43, coverage_gaps 82, coverage_gaps_journal 118,
coverage_gaps_scan 24 692, coverage_gaps_trigger 33, crc32c 85, edrctl 97,
explore 58, ipc 102, journal 4 195, journal_svc 8 277, port_posix 87,
recovery 124, scan 68, simdisk 3 345, table 44, trigger 33, archive_cpp 71.

Python suites (same day, same machine):

| Suite | Result |
|---|---|
| `python3 tests/test_powercut_py.py` | Ran 18 tests — OK (3.5 s) |
| `python3 tests/test_trigger_py.py` | Ran 28 tests — OK (14.1 s) |
| `python3 tests/test_mcap_py.py` | Ran 24 tests — OK, 2 skipped (0.8 s) |

Each C binary returns its failure count as exit code; 0 means green. The
per-suite mapping to requirements is the traceability matrix of
`docs/VALIDATION.md` §5.

## 3. Level 1 — exhaustive exploration of cut states (PC)

```sh
make scenario        # 30 s scenario, seed 1234, crash at 25 000 ms
make explore         # socle: 10 000 states
make explore-full    # every reachable state, nested cuts included
```

Measured with `make BUILD=build-doc explore-full` (report:
`build-doc/explore-full.json`):

| Figure | Value |
|---|---|
| States explored | **64 591** |
| Recoveries run | 64 591 (one per state) |
| Event acquisitions observed | **46 390** |
| Phantom events | 0 |
| Invariant violations (I1..I8) | **0** — `violations_by_invariant` is 0 for each of I1..I8 |
| By variant | prefix 5 803, torn 14 336, cache 21 240, bitflip 5 803, table 11 606, nested 5 803 |
| Duration | 495.3 s at 130 states/s, `--jobs 12` (this run competed with a concurrent coverage build; an unloaded run of the same command measured 334 s at 193 states/s, `--jobs 10`, week-1 milestone commit `2d7263a`) |

The `--states 1000000` cap of `make explore-full` is never reached: **64 591
is the whole reachable space** of the reference scenario (every prefix of the
operation trace × every variant, nested cuts during recovery included). The
socle threshold (§7.7: 10 000 states) is exceeded ×6.5; the "complet"
threshold (1 000 000) is a round number the reachable space does not contain —
exhaustiveness, not the count, is the guarantee. `make explore` measured
10 000 states, 0 violations, 29.3 s (342 states/s).

Method, variant semantics and the two relaxed expectations (volatile cache,
bit flip) are specified in `docs/VALIDATION.md` §1 (Level 1). The checker
compares against the **injected ground truth** (`ambre_truth_t`), never
against the recovered data alone.

## 4. Level 2 — process cuts (POSIX bench) and QEMU cuts

**Process cuts (`host/e2e/`)** — the bench exists: `e2e_run.py` drives
replay → capture → journal with real processes, `SIGKILL`s the journal at a
chosen instant (`pre`, `acquired`, `sealed`), restarts it on the same disk
file, then recovers, exports and verifies. The campaign itself is
**`TODO(campaign)`**: the owning agent is still finishing it
(`tests/test_e2e_posix.c` and `host/e2e/README.md` are not in the tree yet),
so no count is published. Command that will produce the number:

```sh
python3 host/e2e/e2e_run.py --bins build --scenario build/scenario.txt \
        --cut-at acquired --out report.json
```

Note carried by the bench itself (`host/e2e/e2e_check.py`): `class_a.fidelity`
cannot pass on the real process chain (the journal stamps monotonic time and
the replay does not emit wheel speeds); the bench substitutes the field-level
check of `host/e2e/e2e_fidelity.py`.

**QEMU cuts (`host/powercut/`)** — **0/200 (socle), not started**: no QNX SDP
and no QNX image exist on the machine. The orchestrator, the E1/E3 probes and
the faulty block server are written and their logic is green in dry-run
(`python3 tests/test_powercut_py.py`: 18 tests OK, measured). Commands, once
an image exists (`docs/QNX-PORT.md` §"What is needed to finish the target"):

```sh
python3 host/powercut/powercut_campaign.py --image qnx80.img \
  --scenario build/scenario.txt --sweep-from-ms -50 --sweep-to-ms 500 \
  --step-ms 1 --report build/campaign.json
```

## 5. DEC-12 — C trigger vs the independent Python twin

```sh
make crosscheck
```

Measured: **10 000 random scenarios** (seed 4242; 5 068 of them actually
trigger, 50.7 %), **2 744 840 decision lines compared, 0 divergences** —
`RESULT: OK (byte-identical decisions)`. The C engine (`fmt/ambre_trigger.c`)
and the Python twin (`host/verify/ambre_trigger.py`) are separate
implementations of the same contract (`fmt/ambre_trigger.h`, A1..A9); the
comparison is a byte-level `diff` of the `.dec` files.

## 6. Coverage of `fmt/` (spec §12.10: above 95 % of the lines)

```sh
BUILD=build-doc-cov sh scripts/coverage.sh      # isolated build dir
```

Measured: **97.96 %** — 1 343 of 1 371 instrumented lines covered
(report: `build-doc-cov/coverage.json`).

| File | Covered | Lines | % |
|---|---|---|---|
| `ambre_crc32c.c` | 6 | 6 | 100.00 |
| `ambre_fmt_block.c` | 101 | 102 | 99.02 |
| `ambre_fmt_record.c` | 52 | 55 | 94.55 |
| `ambre_fmt_recovery.c` | 224 | 230 | 97.39 |
| `ambre_fmt_scan.c` | 168 | 170 | 98.82 |
| `ambre_fmt_table.c` | 163 | 164 | 99.39 |
| `ambre_journal_core.c` | 216 | 228 | 94.74 |
| `ambre_journal_event.c` | 140 | 143 | 97.90 |
| `ambre_trigger.c` | 207 | 207 | 100.00 |
| `ambre_trigger_window.c` | 66 | 66 | 100.00 |
| **TOTAL** | **1 343** | **1 371** | **97.96** |

The coverage build compiles `fmt/` + `host/` only, so it runs the 13 C suites
that link against them and honestly reports 5 skipped suites
(`test_capture`, `test_edrctl`, `test_ipc`, `test_journal_svc`,
`test_port_posix` — they need the `target/` process objects). The gate form
is `make coverage-gate` (`COV_MIN=95`).

Random-data testing (§12.10, same item): `make fuzz` measured a 5-second
smoke — seed 20261111, **52 hostile images, 0 violations** (10.4 img/s), with
an ASan+UBSan twin (`make fuzz-asan`). The 24-hour run is `TODO(campaign)`.

## 7. Latency (PERF-01..PERF-07)

**Not measured — and not measurable on this machine.** Every latency figure
of §5.7 requires the QNX target: real priorities, real message passing to the
block driver, and the kernel trace (OBS-03) that turns the 20 ms claim into
evidence. The PC bench under `host/latency/` is still being written by its
owning agent (`docs/CAMPAIGN-LATENCY.md` does not exist yet), so no software
share is published either: `TODO(campaign)`. When it lands, its numbers will
be labelled `backend=posix` and will never be presented as target latencies.

The only timing figures this report publishes are the throughputs of the PC
campaigns themselves (states/s of §3, img/s of §6), which are properties of
the verification tooling, not of the product.

## 8. What remains to be measured on the QNX target

| Requirement | What is missing | Blocked by |
|---|---|---|
| PERF-01 (≤ 2 ms sample → pulse), PERF-02 (≤ 20 ms trigger → acquired), PERF-03 (≤ 500 µs software share), PERF-04 (0 overrun / 10 000 triggers) | the latency campaign on the target, under the three loads of §7.5 | no SDP; needs OBS-03 kernel trace |
| PERF-05 (< 50 µs p99 frame send), PERF-06 (< 1 s to capture), PERF-07 (3 worst cases explained) | same | no SDP |
| QNX-01 (resource manager at runtime), QNX-02 (priority inheritance shown by trace), QNX-03 (pulses), QNX-04 (memory lock) | the glue compiles (`make qnx-syntax`, 5/5) but has never executed | no SDP |
| SEC-01 (kernel enforcement: intruder gets `EPERM`), SEC-06 (IFS permissions at runtime) | `secpolcompile` / `secpolpush` on a real image | no SDP |
| OBS-01 (slog2 at runtime) | glue syntax-checked only | no SDP |
| Essays E1 (flush reaches the virtual disk), E3 (< 10 s per kill/restart cycle), E4 (4 KiB message in 50 µs) | `host/powercut/e1_flush_probe.py`, `e3_cycle_timing.py` | no QNX image to boot |
| §7.4 QEMU cuts: 200 (socle), 2 000 swept (complet), 10 000 with volatile cache via the faulty NBD server (extrême) | the whole level-2 QEMU campaign | no SDP |
| §7.7 level 3: 500 real electrical cuts | relay bench, SD card + NVMe | no Raspberry Pi 5 bench |

## 9. Invariant violations found during development (spec §7.7)

§7.7: *"Any violation met during development is kept as a non-regression
test, with the disk image that reproduces it."* The exploration campaign
caught **two violations — both were bugs of the explorer/checker itself, not
of the journal**, found in week 1 while validating the proof machine (commit
`2d7263a`). Both are fixed; the final 64 591-state run has 0 violations per
invariant. Kept as non-regression tests in `tests/test_explore.c`:

1. **Working image not resynchronised with the prefix writes.** The explorer
   keeps a working image and grows the cut prefix incrementally; writes
   appended since the previous state were not resynchronised into it, so some
   states were checked against a **stale disk** — producing verdicts (in both
   directions) that described an image the journal had never produced.
   *Fix*: resynchronise the working image from the snapshot of the new prefix
   before applying the cheap variants — today the materialisation path is
   `ambre_simdisk_snapshot()` (`host/simdisk/`) into
   `ambre_memdisk_apply_log()` (`host/explore/ambre_memdisk.c`).
   *Non-regression*: `tests/test_explore.c::test_nominal_prefixes` re-runs the
   full explorer (`ambre_explore_run`) over every nominal prefix — exactly the
   incremental path that went stale — and the 64 591-state battery re-exercises
   it on every variant.

2. **I7 ground truth keyed on the durability mark instead of the SEAL write.**
   The checker compared `file_complete` against `MARK_SEALED`, which the
   journal logs **after** the flush, instead of the SEAL **record write**. A
   cut between the write and the flush was therefore expected to give
   `file_complete = 1` while the seal could legitimately still sit in the
   volatile cache — a false I7 violation on every such state.
   *Fix*: the ground truth now distinguishes `sealed_op` (SEAL record write,
   `host/explore/ambre_explore.c`) from `sealed_flush_op` (durability mark,
   `host/explore/ambre_explore.h`), and `check_i7`
   (`host/explore/ambre_invariants.c`) accepts both outcomes when the seal was
   written but not yet flushed — the same distinction that makes the relaxed
   `cache` semantics precise (`docs/VALIDATION.md` §1).
   *Non-regression*: `tests/test_explore.c::test_complete_flag_both_sides`
   pins I7 on both sides of the SEAL write op (`sealed_op`), and
   `tests/test_recovery.c::test_sealed_event_keeps_complete_flag` covers the
   sealed side at unit level.

Reproducing images: the explorer stores the disk image of any violation it
meets (`--violation-dir`); the two historical images predate that flag's use
in CI and are reproduced deterministically by the two tests above.

## 10. Known limitation of §7.4 — killing is not a power cut

Killing the QEMU process (or any process on the POSIX bench) **does not lose
the writes already handed to the host**. Level 2 therefore tests the *order*
and the *interruption* of writes and the recovery path end to end — it does
**not** test the loss of a volatile disk cache. That loss is covered:

- on PC, by level 1: the `cache` variant drops subsets of the not-yet-flushed
  writes (21 240 states measured, §3);
- on the target (extrême level, §7.4), by the faulty block server
  (`host/powercut/faulty_block_server.py`), which retains unflushed writes,
  drops some of them at the cut, and journals the flush orders it receives —
  which is also what will prove QNX really emits them (essay E1).

Every published number carries its backend (`posix`, `dry-run`, `qemu`,
`target`) so that no PC result can be read as a target claim.
