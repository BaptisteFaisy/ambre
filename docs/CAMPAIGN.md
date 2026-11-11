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
| Level 0 | unit suites green (§7.1) | all green | **22 suites, 42 249 checks, 0 failures** (21 C + 1 C++) | `make test` + `make test_archive_cpp` |
| Level 1 | cut states explored, 0 violations (§7.7 socle 10 000 / complet 1 000 000) | exhaustive | **64 591 states = every reachable state**, 46 390 acquisitions observed, **0 violations of I1..I8** | `make explore-full` |
| Level 1 | socle threshold (10 000 states) | 10 000, 0 violations | **10 000 states, 0 violations, 29.3 s** (342 states/s) | `make explore` |
| Level 2 | process cuts, POSIX bench (§7.6, 500 complet) | 500 | **200 of 500 run**: 0 errors, **0 invariant violations**, 199 `ok` + **1 `check_failed`** (class-A fidelity, cycle 67) + 2 upstream findings — open | `python3 host/processcut/campaign.py --bins build --scenario build/scenario.txt --cycles 200 --report out.json`; artefact `build-e2e/processcut/report-200.json` |
| Level 2 | QEMU cuts (§7.4, 200 socle) | 200 | **0/200 — not started**: no QNX SDP or image on the machine; harness green in dry-run (18 tests) | `python3 host/powercut/powercut_campaign.py --image qnx80.img --scenario build/scenario.txt --random-cuts 200 --seed 4242 --jobs 4 --report build/campaign.json` |
| DEC-12 | C trigger = Python twin, byte-identical | 10 000 scenarios | **10 000 scenarios, 2 744 840 decision lines compared, 0 divergences** | `make crosscheck` |
| §12.10 | line coverage of `fmt/` | > 95 % | **98.40 %** (1 349 / 1 371 lines) | `BUILD=build-doc-cov2 sh scripts/coverage.sh` |
| §12.10 | random-data testing | 24 h | smoke only: **52 hostile images, 0 violations, 5.0 s** (seed 20261111); the 24 h soak is `TODO(campaign)` | `make fuzz` (smoke); soak: `./build/ambre-fuzz --hours 24 --jobs 8 --report build/fuzz-24h.json` |
| EXP-02 | MCAP export verified against its scenario | 10/10 | **10/10 checks pass** (incl. `class_a.fidelity`, 11 points) | `make verify` |
| §8 | one-command demo | green | **`DEMO OK`** — 3 real processes, `SIGKILL` on the journal in the `acquired` window (+6 033 ms), restart + recovery, 1 event `sealed locked=yes complete_file=no`, MCAP export rc=0, official checks green, class A **11/11 points identical** (alignment 29 ms, worst row slop 4 ms). The demo also prints its own one-shot "acquired in 4.428 ms" under load — a single sample, not a campaign figure; the medians are in `docs/CAMPAIGN-LATENCY.md` | `make demo` |
| QNX-01..07 | QNX glue compiles against the stubs | 5/5 | **5/5 glue files OK** | `make qnx-syntax` |
| SEC/VIE | image + security policy statically coherent | all checks | **11/11 static checks pass** | `make image-check` |
| PERF-01..08 | latency (2 ms / 20 ms / 500 µs) | measured on target | **POSIX only**: PERF-03 software share median **10 µs** (target ≤ 500 µs) — met; PERF-01 **exceeded** at the tail (p99 up to 203 ms under disk load); PERF-02 not proven (99.7 % of it is NVMe media time); PERF-04 volume not reached; PERF-05 not met. Nothing measured on QNX | `docs/CAMPAIGN-LATENCY.md`; `make latency-all` |
| Level 3 | real electrical cuts (§7.7 extrême, 500) | 500 | **TODO(campaign)** — no Raspberry Pi 5 bench | — |

House rule: `make sizecheck` (which runs `scripts/sizecheck.py --tracked-only`)
reports **175 tracked files, 0 violations**, and a bare
`python3 scripts/sizecheck.py` — which also scans untracked work-in-progress
files — reports **191 files, 0 violations** (measured 18:50 on 2026-11-11; an
18:00 run of the same bare command showed 3 violations in `host/e2e/` and
`host/processcut/`, since fixed by their owning agents).

---

## 2. Level 0 — unit tests (PC)

```sh
make test                # 21 C suites
make test_archive_cpp    # the C++17 archive suite
```

Measured: **`ALL TESTS PASSED`** — 42 178 checks over the 21 C suites, plus
71 checks for the C++ archive suite: **22 suites, 42 249 checks, 0 failures**
(measured 2026-11-11 ~18:10; three suites — `test_policy`, `test_policy_us`,
`test_coverage_gaps_edges` — landed from other agents *during* this campaign,
which is why earlier reports of the same day quote 19 suites / 41 672 checks).
Per suite: block 118, capture 43, coverage_gaps 82, coverage_gaps_edges 46,
coverage_gaps_journal 118, coverage_gaps_scan 24 692, coverage_gaps_trigger 33,
crc32c 85, edrctl 97, explore 58, ipc 102, journal 4 195, journal_svc 8 277,
policy 498, policy_us 33, port_posix 87, recovery 124, scan 68, simdisk 3 345,
table 44, trigger 33, archive_cpp (C++17) 71.

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
| Duration | 495.3 s at 130.4 states/s, `--jobs 12` (this run competed with two concurrent campaigns; an earlier unloaded run of the same command reported 334.3 s, see `README.md` §7 and the week-1 milestone commit `2d7263a`) |

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

**Process cuts — harness complete, campaign `TODO(campaign)`.** Two pieces now
exist and are documented by their owning agent:

- `host/e2e/` — the single-cut bench (`e2e_run.py`, `e2e_cycle.py`,
  `e2e_check.py`, `e2e_fidelity.py`, `README.md`): replay → capture → journal
  over AF_UNIX with the frozen `target/edr_ipc.h` protocol, a real `SIGKILL` on
  the journal at a chosen instant (`pre`, `acquired`, `sealed`, `recovery`,
  `random`, or a millisecond), restart on the **same** disk file so the recovery
  runs (REP-01), then service calls, MCAP export and verification.
- `host/processcut/campaign.py` — the campaign runner over that bench, with a
  sweep (`--sweep-from-ms -50 --sweep-to-ms 500 --step-ms 1`), random instants,
  `--jobs`, `--resume` and a violations directory.

```sh
python3 host/e2e/e2e_run.py --bins build --scenario build/scenario.txt \
        --cut-at acquired --out build/e2e-report.json
python3 host/processcut/campaign.py --bins build --scenario build/scenario.txt \
        --cycles 200 --report build/processcut-report.json
```

One single cut *is* demonstrated end to end, by the demo itself: `make demo`
ran green on 2026-11-11 (7.1 s) — replay → capture → journal, `SIGKILL` on the
journal in the `acquired` window (+6 033 ms after the replay started), restart on
the same disk file, recovery, 1 event recovered `sealed locked=yes
complete_file=no`, MCAP export rc=0, official checks green, class A **11/11
points identical** to the injected rows (alignment 29 ms, worst row slop 4 ms).
One cut is not a campaign.

**The 200-cycle campaign finished while this report was being written**
(`build-e2e/processcut/report-200.json`, stamped 2026-11-11 18:41, run by the
owning agent of `host/processcut/`). Its own summary, quoted as it stands:

| Field | Value |
|---|---|
| cuts | **200** (`SIGKILL` on `edr-journal`) |
| errors | 0 |
| invariant violations | **0** |
| verdicts | 199 `ok`, **1 `check_failed`** |
| failing check | `class_a.fidelity(bench)`, cycle 67 (`sweep+285`): 5 of 6 class A points matched, one point (`rel_time_ms=114`) matched no injected row within 250 ms |
| upstream findings | 2, on `timestamps.monotonic(/edr/meta/gaps)` |
| cut windows reached | 62 before acquisition, 21 acquired-open, 117 sealed |
| freeze (trigger → flush), 138 samples | median 3.276 ms, p99 15.923 ms, max 46.795 ms |
| cycle time | median 3.65 s, max 19.59 s |

Read this the way the report asks: **zero invariant violations over 200 real
process cuts**, and **one class-A fidelity failure that is not resolved** — it is
published here as a failure, with its cycle number and its message, because a
campaign whose only blemish is hidden is worth nothing. The two upstream
findings on `/edr/meta/gaps` monotonicity are likewise open. Both belong to the
owning agent of the bench; neither is reinterpreted here. The 500 forced
process stops of §7.7 (level "complet") remain `TODO(campaign)`.

Two honesty notes carried by the bench itself, and repeated here because they
are the difference between a claim and a measurement:

- `class_a.fidelity` cannot pass on the real process chain (the journal stamps
  monotonic time, the replay does not emit wheel speeds); `host/e2e/e2e_fidelity.py`
  substitutes a field-level check of what the chain really carries.
- This is **POSIX**, not a power cut: the "raw partition" is a regular file
  (`pwrite` + `fdatasync`) and the "cut" is a `SIGKILL`. It exercises the order
  and the interruption of writes and the whole recovery path — not the loss of a
  volatile cache (see §10).

**QEMU cuts (`host/powercut/`)** — **0/200 (socle), not started**: no QNX SDP
and no QNX image exist on the machine. The orchestrator, the E1/E3 probes and
the faulty block server are written and their logic is green in dry-run
(`python3 tests/test_powercut_py.py`: Ran 18 tests, OK, 3.5 s — re-measured
2026-11-11). Commands, once an image exists (`docs/QNX-PORT.md` §4):

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
BUILD=build-doc-cov2 sh scripts/coverage.sh     # isolated build dir
```

Measured: **98.40 %** — 1 349 of 1 371 instrumented lines covered
(report: `build-doc-cov2/coverage.json`), with **all 21 C suites** running
under instrumentation and none skipped.

| File | Covered | Lines | % |
|---|---|---|---|
| `ambre_crc32c.c` | 6 | 6 | 100.00 |
| `ambre_fmt_block.c` | 101 | 102 | 99.02 |
| `ambre_fmt_record.c` | 52 | 55 | 94.55 |
| `ambre_fmt_recovery.c` | 224 | 230 | 97.39 |
| `ambre_fmt_scan.c` | 168 | 170 | 98.82 |
| `ambre_fmt_table.c` | 163 | 164 | 99.39 |
| `ambre_journal_core.c` | 221 | 228 | 96.93 |
| `ambre_journal_event.c` | 141 | 143 | 98.60 |
| `ambre_trigger.c` | 207 | 207 | 100.00 |
| `ambre_trigger_window.c` | 66 | 66 | 100.00 |
| **TOTAL** | **1 349** | **1 371** | **98.40** |

An earlier run of the same script, three hours before and before
`test_policy` / `test_policy_us` / `test_coverage_gaps_edges` had landed,
measured **97.96 %** (1 343 / 1 371) with 13 suites and 5 skipped; the gate
(`COV_MIN=95`, `make coverage-gate`) was already met then and is met now with
more margin. Both figures are above the 95 % the specification asks for
(§12.10).

The coverage script compiles `fmt/` with `--coverage` and the rest of the
library without it, then runs every C suite that links against them and
**reports honestly which suites it skipped**. In the 97.96 % run five suites
were skipped (`test_capture`, `test_edrctl`, `test_ipc`, `test_journal_svc`,
`test_port_posix` — they need the `target/` process objects); in the 98.40 %
run none was. The gate form is `make coverage-gate` (`COV_MIN=95`).

Random-data testing (§12.10, same item): `make fuzz` measured a 5-second
smoke — seed 20261111, **52 hostile images, 0 violations** (10.4 img/s), with
an ASan+UBSan twin (`make fuzz-asan`). The 24-hour run is `TODO(campaign)`.

## 7. Latency (PERF-01..PERF-07)

The latency campaign has been run and is published separately, by its owning
agent, in **`docs/CAMPAIGN-LATENCY.md`** (backend `posix`, four loads × 200
measured trigger → acquired cycles, 2026-11-11 16:43–16:48 UTC, i5-11600K /
12 cores / ext4-on-NVMe, journal file on NVMe for the media pass and on tmpfs
for the software pass). This report does not re-derive those numbers; it states
what they are and — more importantly — what they are **not**.

Reproduce with:

```sh
make BUILD=build-mine latency LATENCY_LOAD=none   # one load
make BUILD=build-mine latency-all                 # none, cpu, disk, mem
```

Headline figures as published in `docs/CAMPAIGN-LATENCY.md` §4.1 / §6
(`backend=posix`, µs):

| Requirement | Target | POSIX measurement | Verdict |
|---|---|---|---|
| PERF-01 | ≤ 2 ms | median 1.1–2.2 ms; p99 9.2–203 ms (worst under `disk` load: the capture was descheduled behind a busy device, its own flush taking 108 ms) | **exceeded** at the tail |
| PERF-02 | ≤ 20 ms on reference hardware | median 2.7–4.0 ms, p99 9.3–35.0 ms, max 108 ms — but 99.7 % of it is *media* time on an NVMe file | measured **on PC only**; says nothing about eMMC |
| PERF-03 | ≤ 500 µs software share | median **10 µs**, p99 52–108 µs, p99.9 64–831 µs (2 events of 200 over budget, `disk` load only) | met at median and p99 under every load |
| PERF-04 | 0 overrun over 1 000 (S) / 10 000 (C) triggers | 200 triggers per campaign | **volume not reached** — `TODO(campaign)` |
| PERF-05 | < 50 µs p99 frame send | p99 ≤ 5 ms (bucket ceiling) | **not met** — see that report's §4.5 |
| PERF-06 | capture < 1 s after kernel start | — | not measured, needs the target |
| PERF-07 | 3 worst cases explained by a **kernel** trace | worst cases decomposed from the `AMBRE_TRACE` log | partial: the decomposition exists, the kernel trace does not |
| PERF-08 | same on a Raspberry Pi 5 | — | not measured, no bench |

Read with care, because the honest part is in the caveats:

- **PERF-02 is not proven.** 99.7 % of the measured freeze is media time on a
  PC NVMe file with `fdatasync()`. The 20 ms deadline on the reference hardware
  (QNX + eMMC/UFS behind `io-blk`) remains a **target**; only PERF-03, the
  software share, is a statement about our code.
- **The tail is the story, not the median.** Under `disk` load the worst
  trigger→pulse was 203 ms and the worst freeze 108 ms. Publishing the median
  alone would hide the only thing a regulator cares about.
- **Nothing here comes from QNX.** No priority, no `MsgSend` to a driver, no
  kernel trace: `backend=posix`, AF_UNIX sockets and a file.

The only timing figures *this* report publishes of its own are the throughputs
of the PC verification campaigns (states/s of §3, img/s of §6), which are
properties of the tooling, not of the product.

## 8. What remains to be measured on the QNX target

| Requirement | What is missing | Blocked by |
|---|---|---|
| PERF-01 (≤ 2 ms sample → pulse), PERF-02 (≤ 20 ms trigger → acquired), PERF-03 (≤ 500 µs software share), PERF-04 (0 overrun / 10 000 triggers) | the same campaign **on the target**, under the three loads of §7.5. The POSIX pass is published (`docs/CAMPAIGN-LATENCY.md`): PERF-03 median 10 µs met, PERF-01 exceeded at the tail, PERF-02 not proven (media share), PERF-04 volume not reached | no SDP; needs OBS-03 kernel trace |
| PERF-05 (< 50 µs p99 frame send), PERF-06 (< 1 s to capture), PERF-07 (3 worst cases explained by a *kernel* trace), PERF-08 (Raspberry Pi 5) | PERF-05 measured on POSIX and **not met** (p99 ≤ 5 ms bucket ceiling); PERF-06/07/08 need the target or the Pi bench | no SDP, no Pi bench |
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
