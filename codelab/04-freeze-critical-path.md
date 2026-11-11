# Chapter 4 — The critical path of the freeze

*Previous: [Chapter 3 — the continuous journal](03-continuous-journal.md).
Next: [Chapter 5 — idempotent recovery](05-idempotent-recovery.md).*

**What this chapter proves:** GEL-01 (on trigger, the pre-crash Class A
snapshot leaves in one contiguous write), GEL-02 (the write is followed by the
table update, then a flush; the event is *acquired* at the flush
acknowledgement) and GEL-03 (the path allocates nothing, faults nothing, waits
on no lower-priority process).
**Where the proof lives:** `fmt/ambre_journal_event.c`,
`tests/test_journal.c::trigger_path_is_write_commit_flush`,
`tests/test_explore.c::explore_acquisition_boundary`, plus invariants I2 and
I5 in the exhaustive exploration (chapter 6).

---

## 4.1 The problem

This is the chapter the whole tutorial exists for.

> The event you must record is the event that destroys your power supply.

From the impact to the loss of the 12 V rail there are tens of milliseconds.
Everything the regulator asks for — about a kilobyte of Class A data — must be
**durable**, not merely buffered, inside that window.

The target is **20 ms from trigger to acquired**, of which at most **500 µs**
may be software (PERF-02, PERF-03). Those are targets, not measurements: no
QNX SDP is available here, so no latency figure is published in this
repository. What *is* proven is the **structure** of the path — and the
structure is what a latency budget depends on.

## 4.2 The real code — three steps, no more

`fmt/ambre_journal_event.c`, `ambre_journal_trigger()`. This single function is
the freeze:

```c
int ambre_journal_trigger(ambre_journal_t *j, const ambre_emerg_t *emerg)
{
    ambre_ev_desc_t desc;
    uint64_t seq = 0u;
    uint32_t blocks = 0u;
    int rc;

    if (j == NULL || emerg == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (j->stats.open_events > 0u) {
        j->stats.refused_busy++;
        return AMBRE_ERR_STATE;        /* one event at a time (GEL-06) */
    }

    /* GEL-01: the class A snapshot leaves in ONE contiguous write. */
    rc = ambre_journal_write_record(j, (uint8_t)AMBRE_REC_EMERG,
                                    emerg->t0_ts_us, emerg,
                                    (uint32_t)sizeof(*emerg), &seq, &blocks);
    if (rc != AMBRE_OK) {
        return rc;
    }
    if (j->disk->mark != NULL) {
        j->disk->mark(j->disk->ctx, AMBRE_MARK_EMERG_WRITTEN, emerg->event_id, seq);
    }

    build_event_desc(j, emerg, seq, blocks, &desc);

    ambre_journal_advance(j, blocks, seq);
    pin_segment(j, desc.start_seg, desc.locked != 0u);

    /* GEL-02: table update, then the flush that makes the event acquired. */
    rc = ambre_journal_register_event(j, &desc);
    if (rc != AMBRE_OK) {
        return rc;
    }
    if (j->disk->flush != NULL && j->disk->flush(j->disk->ctx) != 0) {
        return AMBRE_ERR_IO;
    }
    j->stats.flush_count++;
    if (j->disk->mark != NULL) {
        j->disk->mark(j->disk->ctx, AMBRE_MARK_ACQUIRED, emerg->event_id, seq);
    }
    j->stats.open_events++;
    j->stats.acquired_events++;
    return AMBRE_OK;
}
```

Read it as a timeline:

```
trigger fires
     |
     v
1. ONE contiguous write of the EMERG record        -> MARK_EMERG_WRITTEN
     |
     v
2. ONE allocation-table commit (older copy, gen+1)
     |
     v
3. ONE disk flush
     |
     v
flush acknowledged                                 -> MARK_ACQUIRED
```

## 4.3 Why the order is not arbitrary

The `EMERG` record **is** the proof that the event happened; the table merely
describes it. Writing the record first means that a cut between steps 1 and 2
leaves an `EMERG` on the medium that the table does not know about — and
recovery registers it (chapter 5). That is what "self-describing" buys.

The reverse order would leave a table entry pointing at data that never
arrived: an event that is *known* but *empty*, which is worse than no event at
all because it is indistinguishable from a real one whose payload was lost.

## 4.4 "Acquired" means flushed, never submitted

`GEL-02` is the sentence that makes the guarantee testable:

> An event is acquired when the flush that follows its emergency write has been
> acknowledged.

Not when `write()` returned. Not when the table said so. When the medium said
so. Everything downstream — the "file complete = no" flag after a cut, the
explorer's acquisition boundary, invariant I2 — is defined against that single
instant. In the chapter 6 exploration the boundary is crossed at **every**
possible operation, which is what `explore_acquisition_boundary` checks: one
operation earlier the event must not exist, one operation later it must exist
in full and its Class A payload must be byte-identical to the injected one.

## 4.5 Nothing else runs on the path

`GEL-03`, visible in the code rather than asserted in prose:

- `ambre_emerg_t` arrives as a pointer to a caller-owned struct and is staged
  into `j->rec_buf`, a **pre-allocated buffer of the journal state** (chapter
  3). No `malloc`, no growth, no copy into a queue.
- The event descriptor `desc` is a stack value.
- `ambre_journal_register_event()` updates the in-memory table and commits one
  512-byte copy — a fixed-size write.
- The journal state is fixed-size; there is no dynamic structure to fault in.

On QNX the third property extends outside the process: because the driver is
an ordinary process reached by `MsgSend`, it runs the request **at the
journal's priority** (62) — priority inheritance through message passing. That
is the claim `QNX-02` asks a kernel trace to show. It is a **target** claim:
no QNX target was available, and `docs/QNX-PORT.md` says so explicitly.

## 4.6 Why not a file system

On the QNX 8.0 Power-Safe file system `fsync()` synchronises the **whole file
system**, not your file, and `O_SYNC` is **ignored**. Excellent for
consistency, hopeless for a 20 ms deadline, because the cost is not bounded by
the size of your write.

So the journal owns a **raw partition** (`STO-01`), where a flush request is a
flush order, and the MCAP archive — which wants a file system and can afford to
wait — lives on a separate Power-Safe partition, built later by a low-priority
process (priority 8). See `docs/ARCHITECTURE.md`.

## 4.7 After the acquisition

Only once the event is acquired does the during-event cadence start: one
`CONT` record plus one flush every 10 ms (`GEL-04`). At the end of the event a
`SEAL` record carries the maximum delta-v, its instant, the deployment times
and `file_complete = yes` (`GEL-05`). No archive write happens while an event is
open (`GEL-06`): the journal answers a NAK to `EXPORT`, and the archive simply
waits — `target/archive/edr_archive_ipc.cpp`, exercised by
`tests/test_archive_cpp.cpp::test_gel06_no_write_while_event_open`.

## 4.8 Run it

```sh
make BUILD=build-mine all
./build-mine/test_journal
./build-mine/test_explore
```

Expected (measured 2026-11-11):

```
PASSED: 4195 checks, 0 failures    # test_journal
PASSED: 58 checks, 0 failures      # test_explore
```

What to observe:

- `test_journal::trigger_path_is_write_commit_flush` — the recorded operation
  trace of a trigger is exactly: one `EMERG` write, one table commit, one
  flush, in that order. If a refactor ever adds a fourth operation to the
  path, this test says so.
- `test_journal::acquired_event_reads_back_identical` — the Class A payload
  read back is byte-identical to what was injected.
- `test_explore::explore_acquisition_boundary` — the same boundary, walked on
  both sides at the operation level.

The measured latency is a separate matter and stays open: `campaign:latency` —
`TODO(campaign)` (see `docs/CAMPAIGN.md` §7).

---

Next: [Chapter 5 — idempotent recovery](05-idempotent-recovery.md).
