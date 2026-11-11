# Chapter 2 — The allocation table in two copies

*Previous: [Chapter 1 — the block format and CRC-32C](01-block-and-crc.md).
Next: [Chapter 3 — the continuous journal](03-continuous-journal.md).*

**What this chapter proves:** STO-03 (an acknowledged block is not rewritten
before its segment is released), STO-04 (the table exists in two copies with a
generation counter and a CRC; an update touches only one copy) and REP-02
(recovery keeps the most recent valid copy, or rebuilds the table by
scanning).
**Where the proof lives:** `tests/test_table.c`, `tests/test_scan.c`.

---

## 2.1 The problem

Chapter 1 gave you blocks you can judge individually. Now you need to know
*where you are*. Which segment is current? Which events exist? Which are
locked? How far into the current segment has the write pointer advanced?

That information is the **allocation table**, and it changes on every event.
So it is the one structure that is always being updated — which makes it the
one structure most likely to be caught mid-write by the power cut.

If you lose the table, you lose the *map*. The data is physically still on the
medium and becomes unreachable, which from the driver's point of view is
indistinguishable from data loss.

## 2.2 The layout (`docs/FORMAT.md` §1, §7)

```
offset 0      copy A of the allocation table   (512 B)
offset 512    copy B of the allocation table   (512 B)
offset 1024   data segments 0..23, 2 MiB each

block_offset(seg, blk) = 1024 + seg * 2097152 + blk * 512
```

The table covers 24 segments and up to 8 events, with a CRC over its first
508 bytes. The constants are in `fmt/ambre_types.h`:

```c
#define AMBRE_TABLE_A_OFFSET    0u
#define AMBRE_TABLE_B_OFFSET    AMBRE_BLOCK_SIZE
#define AMBRE_FIRST_DATA_OFFSET (2u * AMBRE_BLOCK_SIZE)
```

## 2.3 The real code — commit writes the *older* copy

This is the whole trick, in one function (`fmt/ambre_fmt_table.c`). Read the
comment: the invariant is not "write copy A then copy B", it is **always
overwrite the copy with the lower generation**:

```c
int ambre_table_commit(ambre_disk_t *d, ambre_alloc_table_t *t)
{
    table_pair_t p;
    uint8_t buf[AMBRE_BLOCK_SIZE];
    uint64_t target;
    int rc;

    if (t == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (d == NULL || d->write == NULL) {
        return AMBRE_ERR_PARAM;
    }
    rc = table_load_pair(d, &p);
    if (rc != AMBRE_OK) {
        return rc;
    }
    /* STO-04: always overwrite the OLDER copy; an invalid copy counts as
     * older than any valid one. Both invalid -> fall back to copy A. */
    if (p.valid_a && p.valid_b) {
        target = (p.a.generation <= p.b.generation)
                 ? AMBRE_TABLE_A_OFFSET : AMBRE_TABLE_B_OFFSET;
    } else {
        target = p.valid_a ? AMBRE_TABLE_B_OFFSET : AMBRE_TABLE_A_OFFSET;
    }

    t->generation++;
    t->crc = ambre_fmt_table_body_crc(t);
    ambre_table_pack(t, buf);
    if (d->write(d->ctx, target, buf, AMBRE_BLOCK_SIZE) != 0) {
        return AMBRE_ERR_IO;
    }
    if (d->mark != NULL) {
        d->mark(d->ctx, AMBRE_MARK_TABLE_COMMIT, t->generation, 0u);
    }
    return AMBRE_OK;
}
```

Why this survives a cut: at every instant, one of the two copies is intact and
carries the newest committed generation. If the power dies while the older
copy is being written, that copy may be garbage — the other one is untouched
and one generation ahead of it. `ambre_table_validate()` (magic, version,
generation, CRC) is what decides which copy is usable.

If both copies are invalid, one emergency remains: rebuild the map from the
data by scanning the segment chain (REP-02, chapter 5).

## 2.4 The real code — the reader keeps the highest generation

```c
    /* REP-02: keep the valid copy with the highest generation. */
    if (p.valid_b && (!p.valid_a || p.b.generation > p.a.generation)) {
        *out = p.b;
        if (copy_used != NULL) {
            *copy_used = 1;
        }
    } else {
        *out = p.a;
        if (copy_used != NULL) {
            *copy_used = 0;
        }
    }
    return AMBRE_OK;
```

Note the asymmetry that matters: the writer targets the **lowest**
generation, the reader trusts the **highest**. Those two rules together are
what make a single-copy update atomic enough.

## 2.5 The overwrite policy — and the one thing it never does

When the partition is full, something has to be reclaimed. The policy lives in
`ambre_table_pick_victim()`: free segments first, then archived segments, then
the **oldest unlocked** sealed event. A `locked` segment — an event flagged
non-overwritable by regulation (SEC-03) — is never a victim, at any priority.

```sh
grep -n 'ambre_table_pick_victim' fmt/ambre_fmt_table.c
grep -n 'LOCKED\|locked' fmt/ambre_fmt_table.c
```

The consequence is honest and must be tested: if every event is locked and
there is no free segment, the new event is **refused** — not silently
overwritten, not written over a locked one (`refused_no_space`, `STO-08`).

## 2.6 Run it

```sh
make BUILD=build-mine all
./build-mine/test_table
./build-mine/test_scan
```

Expected (measured 2026-11-11):

```
PASSED: 44 checks, 0 failures     # test_table
PASSED: 68 checks, 0 failures     # test_scan
```

What to observe:

- `table_read_best_highest_gen` — with two valid copies, the highest
  generation wins;
- `table_commit_alternates_corrupt` — a commit alternates the copy it writes,
  and corrupting the copy just written leaves the other one usable;
- `table_pick_victim_policy` — the oldest unlocked segment is chosen, a locked
  one never;
- `scan_valid_chain_and_emerg`, `scan_stops_on_crc_seq_session` — the scanner
  follows the chain and stops at the first incoherence;
- `scan_rebuild_table_exact_events` — with the table gone, the rebuild finds
  exactly the injected events and nothing else.

That last case is the bridge to chapter 3: the map can always be rebuilt
because the data is self-describing.

---

Next: [Chapter 3 — the continuous journal](03-continuous-journal.md).
