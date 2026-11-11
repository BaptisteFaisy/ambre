# Ambre process-cut campaign (spec 7.4, level 2 on the PC backend)

Two hundred brutal cuts of the recorder, zero tolerance for lost data.

```sh
# the socle campaign: 200 cuts, zero loss (spec 7.7)
python3 host/processcut/campaign.py --bins build --scenario build/scenario.txt \
        --cycles 200 --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 5 \
        --jobs 4 --report build/processcut/report-200.json

# resume an interrupted campaign (the state file is append-only JSONL)
python3 host/processcut/campaign.py ... --resume

# a fast smoke campaign, no export/verification
python3 host/processcut/campaign.py --bins build --scenario s.txt --cycles 20 \
        --random-cuts 20 --jobs 2 --no-verify
```

## HONESTY — read this before quoting any number

**This is the POSIX backend, not a power cut, and not QNX.**

* The "raw partition" is a regular file written with `pwrite` and flushed with
  `fdatasync`.
* The "cut" is `SIGKILL` on the `edr-journal` process: no handler, no flush,
  no goodbye.
* **Killing a process does not lose the writes already handed to the host
  kernel.** This is exactly the known limitation the specification states for
  the QEMU campaign (§7.4): this level exercises the *order* and the
  *interruption* of the writes, plus the whole recovery path. It cannot
  exercise the loss of a volatile disk cache.
* The strong proof for volatile-cache loss is level 1: the exhaustive
  exploration of every reachable cut state, torn writes and dropped unflushed
  writes included — `host/explore/` (64 591 states, 0 violation).
* Nothing here says anything about QNX scheduling, `io-blk.so`, the resource
  manager, priorities or the security policy (`docs/QNX-PORT.md`).

Every number published from this campaign must carry `backend=posix`,
`cut=SIGKILL on edr-journal`.

## What one cycle does

1. Start `edr-journal` on a virgin disk file, `edr-capture`, `edr-replay`
   (the frozen CLIs of `docs/W2-PLAN.md`, replay at real-time rate).
2. Play the scenario; poll the journal over the frozen wire protocol about
   every millisecond and record when the event is **acquired** and when it is
   **sealed**.
3. `SIGKILL` the journal at the chosen instant.
4. Stop the producer and the capture, restart `edr-journal` on the **same**
   disk file: the recovery must run (REP-01).
5. Read the recovered state: `status`, `list`, `verify` per event.
6. Export the event to MCAP (`host/mcap/export_mcap.py`), run the official
   verifier (`host/verify/edr_verify.py`), and compare the class A points with
   the injected scenario rows (`host/e2e/e2e_fidelity.py`).
7. Classify, append the record to the state file, delete the temporary tree.

For `--cut-at recovery` (and the `recovery` cycles of the plan) the restart is
killed again while its recovery is running, then started a third time: the two
final states must be identical (I8).

## Cut points and expectations (cahier §4.3)

| Cut point | Where it lands | Expected after the restart |
|---|---|---|
| `pre` | before the trigger | no event (nothing to keep) |
| `<ms>` / sweep / `random` | a numeric instant, ms after the replay start | whichever window it lands in |
| `acquired` | after the flush, before the seal | the event exists, sealed by the recovery, `complete file = no` |
| `sealed` | after the seal | the event exists, `complete file = yes` |
| `recovery` | during the recovery of the restart | two restarts reach the same state |

The plan of a campaign is built in this order: the sweep instants
(`--sweep-from-ms .. --sweep-to-ms` by `--step-ms`, in ms relative to the
scenario's impact), then `--random-cuts` random instants, then a rotation of
the four named windows alternating with random numeric instants, until
`--cycles` is reached. The composition actually run is written to the report.

## Verdicts

| Verdict | Meaning |
|---|---|
| `ok` | the window's expectation holds, the export verifies, the class A data equals the injected data |
| `unexpected_loss` | an event acquired before the cut is gone after the recovery (I2) — **a violation** |
| `inconsistent_state` | an invariant broke: I3 (an event with no trigger), I7 (`complete file` disagrees with the cut window), I8 (a cut during the recovery changed the final state), or an event the recovery left unsealed — **a violation** |
| `check_failed` | the data is all there, but an export/verification check failed; the failing check and its detail are in the record |
| `error` | the harness could not run the cycle (a process did not come up in time). Harness errors are retried up to three times and the attempt count is recorded; a violation is never retried |

Every cycle that is not `ok` keeps its artifacts in
`<report>-violations/cycle-NNNN/`: the disk image, the exported MCAP, the
verifier's JSON report and the parameters (`params.json`), so that it becomes
a non-regression test as spec §7.7 requires.

The exit code is 0 only when there is no violation, no check failure and no
error.

## Class A fidelity, and the two documented exceptions

The comparison with the injected data is done by `host/e2e/e2e_fidelity.py`
(see `host/e2e/README.md` for the full rationale). In short:

* `speed_kmh` is not compared: `edr-replay` never emits the wheel-speed
  signals, so the class A speed of the real chain is always 0 (upstream gap).
* `class_a.fidelity` of the official verifier is set aside: it maps points by
  absolute timestamp, while `edr-capture` stamps them with the monotonic
  clock. Every other official check counts.
* `timestamps.monotonic` on the `/edr/meta/gaps` channel is reported as an
  **upstream finding**, proved from the file itself: the gap records of
  several signals are appended in detection order, so that channel can go
  backwards with or without a cut. It is counted, named in the summary, and
  never treated as a cut-induced failure. Any other non-monotonic channel is
  a `check_failed`.
* A point that does not match at its nominal instant is searched in a window
  around it, and the lag actually needed is measured and reported
  (`skew_ms`, `lag_ms_per_point`). That lag is the harness backlog under load
  — the PC ran this campaign at load average 20-40 — not a recorder defect.
  A value that matches no injected row at all stays a failure. The summary
  says how many cycles were strict and how many needed a lag, and the largest
  lag measured.

## Level map (cahier §7.1)

| Spec level | Tool | Backend | Status |
|---|---|---|---|
| 0 unit | `make test` | PC | green |
| 1 exhaustive cut states | `host/explore/` | PC, simulated disk + cache variants | 64 591 states, 0 violation |
| **2 real cuts, whole chain** | **this campaign** | **PC, POSIX backend, SIGKILL** | **see the report** |
| 2 real cuts, QEMU | `host/powercut/` | QEMU + QNX image | orchestrator ready, dry-run only: no QNX SDP |
| 3 hardware | — | Raspberry Pi 5 | not attempted |

## Last measured run (real numbers, no estimate)

```
$ python3 host/processcut/campaign.py --bins build-e2e \
      --scenario build-e2e/processcut/scenario.txt \
      --cycles 200 --sweep-from-ms -50 --sweep-to-ms 500 --step-ms 5 \
      --jobs 4 --report build-e2e/processcut/report-200.json --seed 20261111

process-cut campaign — backend=posix, cut=SIGKILL on the edr-journal process
  cycles executed : 200 / 200 requested (jobs=4)
  verdicts        : ok=200
  cut windows     : acquired_open=23 before_acquisition=60 sealed=117
  violations      : 0
  errors          : 0 (up to 1 attempts per cycle, 0 retried)
  check failures  : 0
  upstream finding: 3 cycle(s) hit the export-ordering defect of
                    timestamps.monotonic(/edr/meta/gaps)
  class A fidelity: 140 cycle(s) strict (row slop <= 4 ms), 0 with a harness
                    lag, 0 with a producer-stall drift
  freeze (ms)     : min 1.627  median 3.433  p99 36.626  max 54.940  (n=140)
  cycle time (s)  : min 2.71  median 3.61  p99 4.18  max 4.34  (n=200)
  throughput      : 67.1 cuts/min (178.8 s wall, 0.89 s/cut with 4 job(s))
```

Run on 2026-11-11 on the development PC (12 cores, load average 17-19 from
other work at the time), scenario: `generate.py --seed 1234 --duration-ms 6000
--crash-at-ms 3000` (12 km/h frontal impact, no external signal), plan: 111
sweep instants from 50 ms before to 500 ms after the impact (5 ms step) + 12
`pre` + 11 `acquired` + 11 `sealed` + 11 `recovery` + 44 random numeric
instants. 772 events were recovered across the 200 cycles (the trigger engine
re-fires while the 150 ms delta-v window still holds the impact, so one crash
yields up to 8 grouped events — DEC-09; each one was checked).

`freeze` is `status.last_freeze_us`: the software share of the acquisition
(encode + write + flush syscall), **not** the freeze deadline on real media.

## Files

| File | Role |
|---|---|
| `campaign.py` | CLI, plan, state file, workers |
| `campaign_report.py` | summary, JSON report, text histograms, exit code |
| `../e2e/e2e_cycle.py` | the cycle itself (shared with the single-run bench: no duplication) |
