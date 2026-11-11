# Chapter 6 — Exhaustive exploration of the cuts

*Previous: [Chapter 5 — idempotent recovery](05-idempotent-recovery.md).*

**What this chapter proves:** essentially everything, including ROB-01 ("a
power cut at any instant respects the invariants of §7.2") and the whole of
§7.3 of the specification.
**Where the proof lives:** `host/explore/`, run by `make explore` and
`make explore-full`; `docs/VALIDATION.md` §1 (Level 1).

---

## 6.1 The problem

"Survives a power cut" sounds like an infinite claim, and it is usually
argued with a story: *we cut the power twenty times and it always came back*.

Twenty times out of a continuum of instants is not evidence. It is a demo.

But the claim is not actually infinite. The journal performs a **finite**
sequence of operations. Cut the power after operation *k*, for *k* from 0 to
*N*, and you have a finite set of possible disk states — plus a few more once
you model a torn write, a volatile cache and a media error. Enumerate them
all, recover from each one, check the invariants against the injected data,
and the claim stops being a story: it becomes a count.

## 6.2 The pipeline

1. **Record.** Run the journal on the simulated disk (chapter 3) over a
   scenario, with its operation log and its marks. This produces both the
   trace **and the ground truth**: the samples that were injected, and the
   byte-exact Class A snapshot of each event.
2. **Generate.** For every prefix of the trace, materialise the disk image as
   it would have looked at that instant. Add the variants: last write torn at
   a sector boundary, subsets of the not-yet-flushed writes dropped, one bit
   flipped, one or both table copies wiped.
3. **Recover.** Run the *real* recovery code (chapter 5) on that image.
4. **Check.** Verify I1..I8 **against the injected ground truth** — never
   against the recovered data. That is the detail that makes the exploration
   worth anything: a recovery that invents state cannot hide, because the
   expected values come from the input side.
5. **Nest.** Replay the same exploration on the **recovery's own trace**: a cut
   during recovery, then a new recovery (I8).

## 6.3 The eight invariants, and where the relaxation lives

| ID | Invariant |
|---|---|
| I1 | Recovery terminates, in a bounded number of steps, without crashing. |
| I2 | Every event acquired before the cut is present, and its pre-crash Class A data is identical to the injected data. |
| I3 | No event appears that was not triggered. |
| I4 | Every restored sample is identical to the injected sample of the same instant. |
| I5 | For an acquired event, the last restored sample is less than one flush period plus one block old at the moment of the cut. |
| I6 | A locked event survives any subsequent sequence of events and cuts. |
| I7 | "File complete" is "yes" if and only if the event was sealed before the cut. |
| I8 | A cut during recovery or during archiving, followed by a new recovery, gives the same final state. |

Two expectations are deliberately relaxed, and the reason is a property of the
**medium**, not a concession:

- **Volatile cache** — a seal that was *written* but *not flushed* may
  legitimately disappear. `file_complete = 0` is then a correct outcome even
  though the seal write precedes the cut. Everything that was flushed is
  demanded in full: the flush acknowledgement **is** the durability promise.
- **Bit flip** — this is a media error, not a power cut. The journal has no
  redundancy, so what is demanded is **detection and isolation** (I1, I3, I8,
  ROB-07), not repair. Demanding I2/I4/I5/I6 there would be demanding an
  error-correcting code the format does not claim to implement.

The gate is visible in the checker (`host/explore/ambre_invariants.c`):

```c
    bool media_fault = (variant_type == AMBRE_VAR_BITFLIP);

    check_i1(rec, report);
    if (!media_fault) {
        check_i2(truth, rec, recovered_disk, prefix, report);
    }
    check_i3(truth, rec, report);
    if (!media_fault) {
        check_i4_i5(truth, rec, recovered_disk, prefix, sd, report);
        check_i6(truth, rec, prefix, report);
    }
    check_i7(truth, rec, prefix, variant_type, report);
    check_i8(rec, recovered_disk, report);
    return (report->violated_mask == 0) ? 0 : -1;
```

`check_i7` takes `variant_type` as an argument for the same reason: with a
volatile cache it must accept both outcomes when the seal was written but not
flushed.

## 6.4 The invariants caught two bugs — in the proof itself

The exhaustive campaign is also a test of the explorer, and it earned its keep
during development. Two violations were found; **both were bugs in the
explorer/checker, not in the journal**:

1. **The working image was not resynchronised with new prefix writes.** The
   explorer keeps a working image and grows the prefix incrementally; writes
   appended since the previous state were not resynchronised, so some states
   were checked against a **stale disk** — verdicts describing an image the
   journal had never produced. Fixed by resynchronising from the snapshot of
   the new prefix before applying the cheap variants.
2. **I7 ground truth was keyed on the durability mark instead of the SEAL
   write.** The checker compared `file_complete` against `MARK_SEALED` (logged
   *after* the flush) instead of the SEAL record *write*, so every cut between
   the two was reported as an I7 violation. Fixed by distinguishing `sealed_op`
   (record written) from `sealed_flush_op` (durability mark) — which is exactly
   what makes the relaxed cache semantics above precise.

Both are now non-regression tests (`explore_nominal_prefixes`,
`explore_complete_flag_both_sides`), as the specification §7.7 requires: *any
violation met during development is kept as a non-regression test, with the disk
image that reproduces it.*

This is the honest shape of the result: the journal itself never produced an
invariant violation in the final campaign, and the campaign is credible
precisely because it was capable of failing.

## 6.5 Run it

```sh
make BUILD=build-mine scenario        # 30 s scenario, seed 1234, crash at 25 000 ms
make BUILD=build-mine explore         # the socle form: 10 000 states
make BUILD=build-mine explore-full    # every reachable state, nested included
```

Or directly (the CLI is frozen, see `docs/AGENT-BRIEF.md`):

```sh
./build-mine/ambre-explore --scenario build-mine/scenario.txt \
  --states 10000 --jobs "$(nproc)" --variants all \
  --report build-mine/explore.json
```

Expected output, measured on 2026-11-11 on this repository:

```
$ make BUILD=build-doc explore
ambre-explore: 10000 states in 29.260s (342 states/s), 0 violations
```

```
$ make BUILD=build-doc explore-full
ambre-explore: 64591 states in 495.263s (130 states/s), 0 violations
```

Timings depend on what else the machine is doing: the same `make explore` took
76.7 s (130 states/s) when other campaigns were running in parallel, and an
earlier unloaded `make explore-full` reported 334.3 s. The **state count and the
violation count do not move** — those are properties of the enumeration, not of
the load.

The `explore-full` report (`build-doc/explore-full.json`) reads:

```json
{
  "states_explored": 64591,
  "violations": 0,
  "states_per_second": 130.4,
  "recoveries_run": 64591,
  "events_acquired": 46390,
  "events_phantom": 0,
  "states_by_variant": {"prefix": 5803, "torn": 14336, "cache": 21240,
                        "bitflip": 5803, "table": 11606, "nested": 5803},
  "violations_by_invariant": {"I1": 0, "I2": 0, "I3": 0, "I4": 0,
                              "I5": 0, "I6": 0, "I7": 0, "I8": 0}
}
```

What to observe:

- **64 591 is not a cap that was hit — it is the whole reachable space** of
  that scenario. The `--states 1000000` of `make explore-full` is an upper
  bound the enumeration never reaches. Exhaustiveness, not a round number, is
  the guarantee.
- `recoveries_run` equals `states_explored`: there is exactly one recovery per
  state, no sampling.
- `events_acquired` (46 390) is the number of *observations of a successful
  acquisition*, not the number of distinct events — many states share the same
  acquired event.
- `violations_by_invariant` is all zeros, invariant by invariant, so a single
  non-zero counter would name the property that broke.
- The explorer returns 0 only when nothing was violated; a failing state is
  saved so it can become a test (`--violation-dir`).

## 6.6 Where it stops — and why saying so matters

The PC explorer proves the **logic**. It does not prove that QNX emits the
flush orders, nor that the hardware behaves.

- **Level 2** (real processes, real `SIGKILL`, real recovery) exists in
  `host/e2e/`; its campaign is `TODO(campaign)` — see `docs/CAMPAIGN.md` §4.
- **Level 2 under QEMU** (`host/powercut/`) is written and green in dry-run,
  but **0 cuts have been run**: there is no QNX SDP, therefore no image, on
  this machine.
- Killing a VM is **not** a power cut: it does not lose writes already handed
  to the host. That is a known limitation of §7.4, stated in `docs/CAMPAIGN.md`
  §10 — the volatile-cache case is covered here, at level 1. The extreme-level
  answer is the faulty block server (`host/powercut/faulty_block_server.py`),
  which retains unflushed writes and drops some of them at the cut.

The claim this chapter earns is exact, and it is the one worth making:

> For every disk state reachable by a cut during the reference scenario —
> 64 591 of them, torn writes, lost cache lines and media errors included — the
> real recovery code returns a state that violates none of I1..I8.

---

Back to the [codelab index](README.md).
