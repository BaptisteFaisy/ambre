# Chapter 3 — The continuous journal

*Previous: [Chapter 2 — the table in two copies](02-table-two-copies.md).
Next: [Chapter 4 — the critical path of the freeze](04-freeze-critical-path.md).*

**What this chapter proves:** ACQ-06 (a capture interruption longer than three
periods is marked with a `GAP`), STO-01 (the critical path writes to a raw
partition, without a file system) and STO-03 (an acknowledged block is never
rewritten before its segment is released).
**Where the proof lives:** `tests/test_simdisk.c`, `tests/test_scan.c`,
`tests/test_journal.c`.

---

## 3.1 The problem

A crash is short. Around it there is a car doing 90 km/h, and the interesting
data — speed, pedal, yaw rate, the last five seconds — is being produced
continuously, long before anyone knows an event will happen.

So the journal cannot be an event log. It must be **always on**: a rolling
stream of bands of samples, appended at a fixed cadence, on a medium with no
file system in the way.

## 3.2 Start with a disk that lies to you

Before the journal, write the thing that makes the whole tutorial verifiable:
a simulated disk (`host/simdisk/`) that exposes the same four operations as the
real medium — `read`, `write`, `flush`, and an optional `mark` — and **logs
every operation in order**. That log is the ground truth: for any cut point it
says exactly what was durable.

The `mark` operation is how the journal records a milestone in the timeline:

```
AMBRE_MARK_EMERG_WRITTEN   AMBRE_MARK_ACQUIRED   AMBRE_MARK_SEALED
AMBRE_MARK_DATA_FLUSHED    AMBRE_MARK_TABLE_COMMIT
```

The disk vtable the whole library is written against is in `fmt/ambre_fmt.h`:

```c
typedef struct ambre_disk {
    void    *ctx;
    uint64_t size;               /* bytes; >= AMBRE_JOURNAL_BYTES           */
    uint32_t sector_size;        /* 512 or 4096 (spec 4.5)                  */
    int (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    int (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
    int (*flush)(void *ctx);     /* cache flush / sync: durability point    */
    /* Optional: durability milestones for the crash explorer. The journal
     * emits ambre_mark_kind_t values; the QNX backend leaves this NULL. */
    void (*mark)(void *ctx, uint32_t kind, uint64_t value, uint64_t aux);
} ambre_disk_t;
```

Because `fmt/` only ever talks to that vtable, the *same* code runs on the
simulated disk (chapter 6), on the POSIX backend (real processes, real
`SIGKILL`), and on the QNX raw partition (`target/port/ambre_port_disk_qnx.c`).
That is the single design decision that turns "we tested it" into "we tested
the code that ships".

## 3.3 The real code — one record, one contiguous write

`fmt/ambre_journal_core.c`. This function is the whole append path; read the
three comments inside it, they are the requirements:

```c
int ambre_journal_write_record(ambre_journal_t *j, uint8_t rec_type,
                               uint64_t ts_us, const void *payload,
                               uint32_t payload_len, uint64_t *seq_used,
                               uint32_t *blocks_used)
{
    uint16_t chunks;
    uint64_t seq = j->next_seq;
    int rc;

    if (payload_len > AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX) {
        return AMBRE_ERR_TOO_BIG;
    }
    chunks = (uint16_t)((payload_len + AMBRE_BLOCK_PAYLOAD_MAX - 1u) /
                        AMBRE_BLOCK_PAYLOAD_MAX);
    if (chunks == 0u) {
        chunks = 1u;
    }
    /* STO-03: an acknowledged block is never rewritten. When the segment has
     * no room for this record, move to another one first. */
    if (j->table.current_block + chunks > AMBRE_BLOCKS_PER_SEGMENT) {
        rc = rotate_segment(j);
        if (rc != AMBRE_JOURNAL_OK) {
            return rc;
        }
    }

    rc = stage_record_chunks(j, rec_type, ts_us, payload, payload_len, seq,
                             chunks);
    if (rc != AMBRE_JOURNAL_OK) {
        return rc;
    }

    /* One single contiguous write: that is what GEL-01 asks for. */
    rc = j->disk->write(j->disk->ctx, ambre_journal_write_offset(j), j->rec_buf,
                        (size_t)chunks * AMBRE_BLOCK_SIZE);
    if (rc != 0) {
        return AMBRE_ERR_IO;
    }
    j->stats.bytes_written += (uint64_t)chunks * AMBRE_BLOCK_SIZE;
    j->stats.blocks_written += chunks;

    if (seq_used != NULL) {
        *seq_used = seq;
    }
    if (blocks_used != NULL) {
        *blocks_used = chunks;
    }
    return AMBRE_JOURNAL_OK;
}
```

Three properties, each one a requirement:

1. **The chunks are staged into `j->rec_buf`, a pre-allocated buffer of the
   journal state** — no `malloc`, no page fault, nothing that can block on the
   critical path (GEL-03). Look at `stage_record_chunks()` just above it: it
   builds each 512-byte chunk straight into the buffer at
   `ci * AMBRE_BLOCK_SIZE`.
2. **A record never straddles two segments.** If it does not fit, the journal
   rotates first. A record split across a segment boundary could not be
   written in one request.
3. **Exactly one `write` call**, `chunks * 512` bytes at the current offset
   (GEL-01).

Nothing here writes over a block that was already acknowledged: the pointer
only advances (`ambre_journal_advance`) or rotates.

## 3.4 The scan — how the map is rebuilt from the data

`fmt/ambre_fmt_scan.c` walks a segment following the sequence chain:

```c
    while (blk < AMBRE_BLOCKS_PER_SEGMENT) {
        ambre_block_t head;
        if (ambre_read_record(d, seg, blk, session_id, (int64_t)seq, &head, NULL, NULL) != AMBRE_OK) {
            break;
        }
        out->valid_blocks += head.hdr.chunk_count;
        out->last_seq      = head.hdr.seq;
        out->last_ts_us    = head.hdr.first_ts_us;
        if (head.hdr.rec_type == (uint8_t)AMBRE_REC_EMERG && !out->hit_emerg) {
            out->hit_emerg   = true;
            out->emerg_seq   = head.hdr.seq;
            out->emerg_block = blk;
        }
        blk += head.hdr.chunk_count;
        out->next_block = blk;
        seq++;
    }
    return AMBRE_OK;
}
```

It stops at the first block that fails magic, version, CRC, session or `seq`
continuity, and reports the last valid block, the next free block, and whether
it met an `EMERG` record. This is the recovery's fallback when both table
copies are unusable (chapter 5) — and the reason the format is
*self-describing*: an `EMERG` record found by the scan is enough to prove the
event happened, even with no table entry for it.

## 3.5 Gaps are data too

If the capture process stalls for more than three sampling periods, the journal
does not silently pretend the data is continuous. It writes a `GAP` record
(`ambre_journal_append_gap`). A hole that is *recorded* is honest; a hole that
is silently stitched over is a false claim about the vehicle's behaviour.

## 3.6 Run it

```sh
make BUILD=build-mine all
./build-mine/test_simdisk
./build-mine/test_scan
./build-mine/test_journal
```

Expected (measured 2026-11-11):

```
PASSED: 3345 checks, 0 failures    # test_simdisk
PASSED: 68 checks, 0 failures      # test_scan
PASSED: 4195 checks, 0 failures    # test_journal
```

What to observe:

- `test_simdisk::logging_exactness` — the operation log records exactly the
  calls made, in order, with their sizes;
- `test_journal::data_capture_and_flush_cadence` — `DATA` records are appended
  and flushed at the configured cadence (100 ms by default);
- `test_journal::event_only_mode_writes_nothing_before_trigger` — the journal
  can be told to stay silent until a trigger;
- `test_journal::segment_rotation_and_recycle` — a full segment rotates, and a
  reclaimed one is only reused when its event is sealed and unlocked;
- `test_journal::locked_event_is_never_recycled` — the SEC-03 case;
- `test_journal::memory_full_is_refused` — STO-08: refusal, not overwrite;
- `test_journal::stats_are_coherent` — the counters (`bytes_written`,
  `blocks_written`, `flush_count`, `refused_*`) agree with the trace.

---

Next: [Chapter 4 — the critical path of the freeze](04-freeze-critical-path.md).
