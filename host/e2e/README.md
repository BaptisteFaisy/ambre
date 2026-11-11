# Ambre end-to-end bench (level 2 on the PC backend)

Real processes, real files, real `SIGKILL`. This directory runs the whole
software chain of the recorder — `edr-replay` → `edr-capture` → `edr-journal`
→ raw partition → restart/recovery → service calls → MCAP export → verifier —
and kills the journal brutally at a chosen instant.

```sh
# one run, one cut, chosen window (a scenario is generated if none is given)
python3 host/e2e/e2e_run.py --bins build --cut-at acquired --out report.json

# with an explicit scenario and an explicit working directory
python3 host/e2e/e2e_run.py --bins build --scenario build/scenario.txt \
        --cut-at sealed --workdir build/demo/run

# the whole demonstration in one command
./scripts/demo.sh
```

`--cut-at` accepts `pre`, `acquired`, `sealed`, `recovery`, `random`, or a
numeric instant in milliseconds after the replay started. `--rate` defaults to
1.0, real time (see "Why the replay must run in real time" below).

## What this bench proves

| Claim | How it is checked |
|---|---|
| The processes really talk to each other over the frozen protocol (AF_UNIX `SOCK_SEQPACKET`, `target/edr_ipc.h`) | an event travels replay → capture → journal and is read back through the service socket |
| A trigger is acquired, and the acquisition is measured (PERF-02, software share) | `status.last_freeze_us`; the bench prints "acquired in N ms" |
| A brutal death of the journal (no handler, no flush) loses nothing that was acquired | the journal restarts on the **same** disk file and the event is listed again (I2) |
| The recovery runs at start-up and seals what was left open (REP-01, REP-03) | the recovered event is `sealed`, with `complete file` exactly as the cut window demands (I7) |
| A cut during the recovery changes nothing (I8) | `--cut-at recovery` kills the restart while it recovers, restarts again, and compares the two final states |
| No event appears out of nowhere (I3) | every recovered event carries a trigger reason |
| The exported event is a valid MCAP whose class A data equals the injected data (I4) | `host/mcap/export_mcap.py`, `host/verify/edr_verify.py` and `e2e_fidelity.py` |

## What this bench does NOT prove

* **It is not a power cut.** The cut is `SIGKILL` on one process. Writes
  already handed to the host kernel survive it, so the loss of a volatile disk
  cache is out of reach here — the very limitation the specification states for
  the QEMU campaign (§7.4). The strong proof for that is level 1: the
  exhaustive exploration of every reachable cut state in `host/explore/`.
* **It is not QNX.** The backend is `posix`: the "raw partition" is a regular
  file (`pwrite` + `fdatasync`), not `/dev/hd0tNNN` through `io-blk.so`. No QNX
  scheduling, no resource manager, no priority inheritance, no security policy
  (`docs/QNX-PORT.md`).
* **The freeze time is the software share only.** `last_freeze_us` covers
  encode + write + the flush syscall. The media share needs the QNX target
  (PERF-02, essay E1).
* **The replay cannot be sped up.** `edr-capture` timestamps samples on
  arrival, and the trigger integrates delta-v over those timestamps: replaying
  faster than real time shrinks the measured delta-v by the same factor and the
  event never triggers. `--fast` is therefore unusable for the chain (measured:
  20× faster, no trigger); it stays useful for transport-throughput tests.

Every number this tool prints must be quoted as `backend=posix`.

## Verdicts

| Verdict | Meaning |
|---|---|
| `ok` | the window's expectation holds, the export verifies, the class A data equals the injected rows |
| `unexpected_loss` | an event acquired before the cut is gone after the recovery (I2) |
| `inconsistent_state` | I3, I7 or I8 broke, or the recovery left an event unsealed |
| `check_failed` | the data is all there but an export/verification check failed (the check and its detail are in the report) |
| `error` | the harness could not run the cycle — including the case where the requested cut window was never observed, which is never reported as a pass |

A cycle that is not `ok` keeps its artifacts (disk image, MCAP, verifier
report, parameters) in the directory given by `artifact_dir`, so that it can
become a non-regression test (spec §7.7).

## Upstream gaps this bench works around (reported, never hidden)

1. **`edrctl` cannot answer yet.** `target/port/ambre_port.h` has no
   client-side receive (`ambre_port_conn_request()`), so the real binary prints
   "status unavailable" / "list unavailable" — the gap is documented at the top
   of `target/ctl/edr_ctl.c`. The bench therefore speaks the same frozen
   protocol itself (`ambre_wire.py`: same 16-byte `EDR1` header, same bodies,
   same JSON keys), it *also* runs the real binary on every cycle and records
   its answer verbatim (`edrctl` in the report), and the summary says which
   probe answered. Nothing has to change here when the port gains the function.
2. **`edr-capture` never acknowledges `EXTERNAL` messages**, while the
   transport is acknowledgement-based: with a scenario that deploys a restraint,
   `edr-replay` times out 500 ms after the signal and exits 1. The external
   *was* processed — the trigger fires and the event is locked — so the bench
   keeps going and reports the producer's exit status. This is also the only
   way to obtain a locked event today: the dv25 lock is disabled while the
   trigger configuration declares a front restraint (`has_front_restraint = 1`,
   the default of `ambre_trig_cfg_default`).
3. **The class A speed is always 0 on this chain.** `edr-replay` never emits
   the wheel-speed signals 10..13, so `edr-capture` has no speed to store, and
   `host/verify/edr_verify.py` fails `class_a.fidelity` — which also maps points
   by absolute timestamp while the capture stamps them with the monotonic clock.
   `e2e_fidelity.py` does the comparison on the fields the chain really carries
   (accelerations, rpm, throttle, brake, steering), with the verifier's own
   1-LSB tolerance, and reports the skipped field. The official verifier is
   still run as-is and its verdict reported; `class_a.fidelity` is the only
   check set aside, and it is named in every report.
4. **The `/edr/meta/gaps` channel can go backwards.** Gap records of different
   signals are appended in detection order, so `timestamps.monotonic` fails on
   that channel — with or without a cut. The bench proves it from the MCAP file
   itself (`non_monotonic_topics`) and reports it as an *upstream finding*
   instead of counting it as a cut-induced failure. Any other non-monotonic
   channel is a `check_failed`.
5. **After the producer dies, the capture keeps sending `CONTINUE` forever**:
   nothing ends the event when the producer's connection closes, so the journal
   service channel can be starved for seconds under load. The bench tolerates
   late replies (2 s per request, three attempts, re-synchronised connection),
   and reports a starvation as an `error` — never as a pass.

## Class A comparison: how strict is it?

1. The scenario-relative T0 is found **by value**: every candidate instant
   around the impact is scored against the whole set of points, and the best
   score wins. One point alone cannot anchor the alignment — in the quiet part
   of a drive many rows are identical.
2. **Strict pass** (`points_strict`): every point equals the injected row at
   its nominal instant, within a 4 ms row slop (`rel_time_ms` is a truncated
   integer) and the official 1-LSB field tolerance.
3. **Harness lag** (`skew_ms`, up to 250 ms): the state a point carries trails
   its own timestamp by the processing delay of a loaded PC bench. Measured,
   reported per point (`lag_ms_per_point`).
4. **Producer-stall drift** (`worst_drift_ms`): a point whose values match one
   injected row and only rows far from its nominal instant. The value is
   provably genuine — it was injected — but its timestamp pairing is wrong,
   which is what happens when `edr-replay` stalls: `edr-capture` stamps samples
   on arrival, so old values get new instants. Measured and reported, never
   silent. (Measured case: one point 480 ms off during a 200-cut campaign at
   load average 20-40; its values matched exactly one row of the scenario.)
5. **Ordering**: older points must map to older rows. A mapping that is not
   monotonic fails, which is what a recorder mixing up class A points would
   look like.
6. A point whose values match **no** injected row anywhere fails the check:
   that is what corruption or a fabricated record would look like.
7. Points older than the first injected sample have no counterpart (the class A
   ring reaches 5.5 s back, a short scenario does not): counted apart as
   `outside_scenario`, never silently accepted.
8. Negative control, measured: comparing an export with a *different* scenario
   matches 0 points out of 6 — the check is not vacuous.

## Demonstration

`scripts/demo.sh` runs the whole story of cahier §8 in one command: build,
scenario with a frontal impact and restraint deployment (so the event is
**locked**), impact, "acquired in N ms", `SIGKILL` on `edr-journal`, restart on
the same disk file, the service list showing the event locked with
`complete file = no`, MCAP export, verification, and a summary that ends with
what the run does **not** prove.

```sh
./scripts/demo.sh                             # builds into ./build
AMBRE_BUILD_DIR=build-mine ./scripts/demo.sh   # isolated build tree
```

It exits non-zero if any stage fails, and it prints the known upstream gaps it
hit (the `edrctl` transport, the missing `EXTERNAL` acknowledgement, the absent
wheel speeds) instead of hiding them.

## The four cut windows (cahier §4.3)

| `--cut-at` | Cut lands | Expected after the restart |
|---|---|---|
| `pre` | before the trigger | no event at all |
| `acquired` | after the flush, before the seal | event present, `sealed`, `complete file = no` |
| `sealed` | after the seal | event present, `sealed`, `complete file = yes` |
| `recovery` | during the recovery of the restart | two restarts reach the same state (I8) |
| `<ms>` / `random` | a numeric instant | whichever window it lands in, still consistent |

## Level map (cahier §7.1)

| Spec level | Where it lives | Backend |
|---|---|---|
| 0 — unit | `make test` | PC |
| 1 — exhaustive cut states | `host/explore/` | PC, simulated disk, torn writes and volatile-cache variants |
| **2 — real cuts, whole chain** | **`host/e2e/` (single run) and `host/processcut/` (campaign)** | **PC, POSIX backend, `SIGKILL`** |
| 2 — real cuts, QEMU | `host/powercut/` | QEMU + QNX image (dry-run today: no SDP) |
| 3 — hardware | not attempted | Raspberry Pi 5 |

## Files

| File | Role |
|---|---|
| `ambre_wire.py` | client of the frozen wire protocol (status / list / verify / export), plus the real `edrctl` shim |
| `e2e_env.py` | process bench: start, `SIGKILL`, restart, logs, guaranteed cleanup (`atexit` + fatal-signal handlers, own process group per child) |
| `e2e_cycle.py` | one cut cycle, its classification (I1–I8) and its artifacts |
| `e2e_check.py` | MCAP export, official verifier, verdict folding, upstream findings |
| `e2e_fidelity.py` | class A comparison against the injected scenario |
| `e2e_run.py` | the single-run CLI |
| `demo_summary.py` | the demonstration recap printed by `scripts/demo.sh` |
| `../../tests/test_e2e_py.py` | five cuts (one per window) plus bench hygiene, in under 60 s |
| `../processcut/` | the campaign (200 cuts, sweep, random, jobs, resume) |

Housekeeping rule: no process and no temporary file survives a run. Every child
is started in its own process group and killed on the way out — including when
the bench itself is killed (`SIGTERM`/`SIGINT`/`SIGHUP` handlers and `atexit`),
and the working tree is a `tempfile.mkdtemp()` directory removed at the end
unless `--keep` or `--workdir` is given.
