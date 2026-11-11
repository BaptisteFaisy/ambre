# Chapter 5 — Idempotent recovery

*Previous: [Chapter 4 — the critical path of the freeze](04-freeze-critical-path.md).
Next: [Chapter 6 — exhaustive exploration of the cuts](06-exhaustive-exploration.md).*

**What this chapter proves:** REP-01 (recovery runs before any new capture),
REP-02 (keep the most recent valid table copy, else rebuild by scanning),
REP-03 (every event left open is sealed with "file complete = no" and the
cause), REP-04 (recovery is replayable: same result if run twice, or after a
cut during recovery) and REP-05 (never surface an event that was not
triggered, never return data with an invalid CRC).
**Where the proof lives:** `tests/test_recovery.c`,
`tests/test_explore.c::explore_double_recovery`, `explore_no_crash_no_event`,
`explore_bitflip_detected`, `explore_table_destruction`, and invariants I1,
I3, I7, I8 in the exhaustive exploration.

---

## 5.1 The problem

Recovery is the code that runs **after** the failure it is supposed to handle.
That makes it the most dangerous code in the system: it will be interrupted by
exactly the event it is recovering from, and then it will run again.

Two properties are therefore non-negotiable:

1. **It must be safe to run on anything** — a half-written record, a wiped
   table, a partition of noise.
2. **It must be idempotent** — running it twice, or running it after being
   itself interrupted, must give the same final state (REP-04, invariant I8).

## 5.2 The procedure (`docs/FORMAT.md` §10)

```
1. read both table copies; keep the valid one with the highest generation
2. if neither is valid: rebuild by scanning every segment in use
3. in the current segment, find the last valid block; everything after it is
   garbage and will be overwritten
4. drop torn records: any EMERG/CONT whose chunks are not all valid
5. an EMERG record the table does not know yet is registered
6. seal every event still open with file_complete = 0, cause POWERCUT,
   and commit the table
7. never return data whose CRC is invalid; never invent an event
```

## 5.3 The real code — the whole recovery, top to bottom

`fmt/ambre_fmt_recovery.c`, `ambre_recover()`. Note how short it is once the
steps have names:

```c
int ambre_recover(ambre_disk_t *d, ambre_recovery_result_t *out)
{
    seg_scan_t segs[AMBRE_SEG_SCAN_MAX];
    ambre_alloc_table_t table;
    uint64_t next_seq;
    uint64_t disk_last_seq = 0u;
    bool changed = false;
    bool rebuilt = false;
    int rc;

    if (d == NULL || d->read == NULL || out == NULL) {
        return AMBRE_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));
    memset(segs, 0, sizeof(segs));

    rc = load_table(d, &table, &rebuilt);
    if (rc != AMBRE_OK) {
        return rc;
    }
    out->table_rebuilt = rebuilt;
    out->events_before = table.event_count;

    scan_segments(d, &table, rebuilt, segs, out);

    changed = merge_disk_state(&table, segs, AMBRE_SEG_SCAN_MAX, &disk_last_seq);
    if (choose_resume_point(&table, segs)) {
        changed = true;
    }

    next_seq = resume_seq(&table, disk_last_seq);

    rc = seal_open_events(d, &table, &next_seq, out);
    if (rc != AMBRE_OK) {
        return rc;
    }
    if (out->sealed_incomplete > 0u) {
        changed = true;
    }

    table.last_seq = next_seq - 1u;

    rc = finish_recovery(d, &table, changed || rebuilt, out);
    if (rc != AMBRE_OK) {
        return rc;
    }
    out->table = table;
    out->events_after = table.event_count;
    return AMBRE_OK;
}
```

Everything is **fixed-size** (`segs[AMBRE_SEG_SCAN_MAX]`, one table struct, no
allocation): recovery itself must not be able to fail for lack of memory, and
must terminate in a bounded number of steps (invariant I1, whose bound the
explorer counts).

## 5.4 The subtle step: the record wins over the table

Step 5 is where most crash-recovery designs get it wrong. After a cut the table
may be *behind* the data: the `EMERG` record reached the medium but the table
commit did not. A recovery that trusted the table would **lose an event the
medium holds**.

So the merge is driven by what the disk contains, and the table is the thing
that gets corrected:

```c
        for (uint32_t r = 0u; r < segs[s].rec_count; r++) {
            const rec_entry_t *e = &segs[s].rec[r];

            if (e->type == (uint8_t)AMBRE_REC_EMERG) {
                if (add_unknown_event(t, e)) {
                    changed = true;
                }
            } else if (e->type == (uint8_t)AMBRE_REC_SEAL) {
                ambre_ev_desc_t *d = ambre_table_find_event(t, e->event_id);

                if (d != NULL && d->state != (uint8_t)AMBRE_EV_SEALED) {
                    apply_seal(t, e);
                    changed = true;
                }
            }
        }
```

`add_unknown_event()` is what makes the format self-describing: an `EMERG`
alone proves the event happened. And note the honesty of the inverse direction
in `apply_seal()` — a `SEAL` found on disk sets `file_complete` to *what the
record says*, not to "yes":

```c
    d->state         = (uint8_t)AMBRE_EV_SEALED;
    d->file_complete = e->file_complete;
```

An event that was sealed by a **previous** recovery (cause `POWERCUT`) stays
`file_complete = 0` for ever. That is invariant I7: "complete file" is "yes" if
and only if the event was sealed before the cut.

## 5.5 Sealing what was left open

```c
    memset(&seal, 0, sizeof(seal));
    seal.event_id        = desc->event_id;
    seal.file_complete   = 0u;
    seal.seal_cause      = (uint8_t)AMBRE_SEAL_POWERCUT;
    seal.locked          = desc->locked;
```

Recovery **appends** this record and then commits the table (alternating
copies, chapter 2). Appending plus alternating is precisely what makes it
replayable: a cut in the middle of recovery leaves either the seal record
missing (so the next recovery writes it again) or the table commit missing (so
the next recovery has to redo it) — never a half-state that a third run would
interpret differently.

## 5.6 Run it

```sh
make BUILD=build-mine all
./build-mine/test_recovery
./build-mine/test_explore
```

Expected (measured 2026-11-11):

```
PASSED: 124 checks, 0 failures     # test_recovery
PASSED: 58 checks, 0 failures      # test_explore
```

What to observe, case by case in `tests/test_recovery.c`:

- `empty_partition` — a virgin partition recovers to "no event";
- `acquired_event_survives` — the normal case;
- `sealed_event_keeps_complete_flag` — I7 on the sealed side;
- `event_found_without_table_entry` — step 5: the `EMERG` alone registers the
  event;
- `torn_emerg_is_dropped` — step 4: a torn record never surfaces;
- `both_tables_destroyed` — step 2: rebuild by scanning;
- `bitflip_discards_one_block` — step 7 and ROB-07: detected and isolated, the
  neighbours untouched (**detected, not repaired** — the format has no
  redundancy and does not claim to);
- `recovery_is_idempotent` — REP-04: run twice, same state;
- `noise_partition_terminates` — I1: a partition full of garbage still
  terminates;
- `locked_event_is_kept` — I6: a locked event survives recovery.

The same properties are then exercised on **every reachable disk state** in
chapter 6, which is where "recovery is idempotent" stops being a unit test and
becomes a claim about every possible cut.

---

Next: [Chapter 6 — exhaustive exploration of the cuts](06-exhaustive-exploration.md).
