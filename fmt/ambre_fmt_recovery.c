/*
 * ambre_fmt_recovery.c — Recovery after a power cut (spec 4.6, REP-01..REP-05).
 *
 * The journal is self-describing: an EMERG record on the disk is proof that
 * an event happened, even if the allocation table never got the update. This
 * module rebuilds the truth from the two table copies and from the blocks
 * themselves, then seals every event that was still open when the power died.
 *
 * Hard requirements:
 *   - idempotent (REP-04, invariant I8): running it twice, or interrupting it
 *     with another power cut and running it again, yields the same state;
 *   - it never returns data with an invalid CRC and never invents an event
 *     (REP-05, invariants I2, I3, I4);
 *   - it terminates in a bounded number of steps on any image, including a
 *     partition filled with random noise (invariant I1);
 *   - no dynamic allocation: every buffer is a fixed-size local array.
 */

#include "ambre_fmt.h"
#include <string.h>

#define AMBRE_REC_SCAN_MAX 16u   /* records collected per segment */
#define AMBRE_SEG_SCAN_MAX AMBRE_SEGMENT_COUNT
#define AMBRE_SEG_LOOKAHEAD 16u  /* blocks looked at to find a chain start */

/* One event-relevant record found on disk. */
typedef struct {
    uint8_t  type;            /* AMBRE_REC_EMERG or AMBRE_REC_SEAL       */
    uint32_t seg;
    uint32_t block;           /* block index of the record head           */
    uint32_t chunks;
    uint64_t seq;
    uint64_t ts_us;
    uint32_t event_id;
    uint8_t  locked;
    uint8_t  file_complete;
    uint8_t  trig_reason;
    uint8_t  event_number;
    uint32_t order;
    uint32_t time_since_first_ms;
} rec_entry_t;

/* What one segment holds. */
typedef struct {
    bool      active;
    uint64_t  session_id;
    uint64_t  first_seq;
    uint64_t  last_seq;
    uint32_t  next_block;     /* first block after the valid chain        */
    uint32_t  rec_count;
    rec_entry_t rec[AMBRE_REC_SCAN_MAX];
} seg_scan_t;

/* ------------------------------------------------------------- scanning --- */

/* Record one EMERG or SEAL entry found while walking a segment's chain. */
static void collect_record(seg_scan_t *out, uint32_t seg, uint32_t blk,
                           const ambre_block_t *head, const uint8_t *payload)
{
    rec_entry_t *e;

    if (head->hdr.rec_type != (uint8_t)AMBRE_REC_EMERG &&
        head->hdr.rec_type != (uint8_t)AMBRE_REC_SEAL) {
        return;
    }
    if (out->rec_count >= AMBRE_REC_SCAN_MAX) {
        return;
    }
    e = &out->rec[out->rec_count++];
    memset(e, 0, sizeof(*e));
    e->type         = head->hdr.rec_type;
    e->seg          = seg;
    e->block        = blk;
    e->chunks       = head->hdr.chunk_count;
    e->seq          = head->hdr.seq;
    e->ts_us        = head->hdr.first_ts_us;
    e->trig_reason  = (uint8_t)AMBRE_TRIG_NONE;
    e->event_number = 1u;

    if (head->hdr.rec_type == (uint8_t)AMBRE_REC_EMERG &&
        head->hdr.total_len >= sizeof(ambre_emerg_t)) {
        ambre_emerg_t em;

        memcpy(&em, payload, sizeof(em));
        e->event_id    = em.event_id;
        e->locked      = em.locked;
        e->trig_reason = em.trig_reason;
        e->event_number = em.event_number;
        e->order       = em.crash_order;
    } else if (head->hdr.rec_type == (uint8_t)AMBRE_REC_SEAL &&
               head->hdr.total_len >= sizeof(ambre_seal_t)) {
        ambre_seal_t sl;

        memcpy(&sl, payload, sizeof(sl));
        e->event_id            = sl.event_id;
        e->locked              = sl.locked;
        e->file_complete       = sl.file_complete;
        e->event_number        = sl.event_number;
        e->time_since_first_ms = sl.time_since_first_ms;
    }
}

/* Walk the chain of one segment and collect the event-relevant records.
 *
 * The chain starts at the first readable record: a recycled segment may have
 * a short hole at its beginning, so the scan looks ahead a bounded number of
 * blocks before giving up (invariant I1: bounded work). */
static void collect_segment(const ambre_disk_t *d, uint32_t seg, seg_scan_t *out)
{
    uint8_t payload[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    ambre_block_t head;
    uint32_t blk = 0u;
    uint32_t probe;
    uint64_t seq;
    int rc;

    for (probe = 0u; probe < AMBRE_SEG_LOOKAHEAD; probe++) {
        rc = ambre_read_record(d, seg, probe, 0u, -1, &head, NULL, NULL);
        if (rc == AMBRE_OK) {
            break;
        }
    }
    if (probe >= AMBRE_SEG_LOOKAHEAD) {
        return;                         /* empty or unusable segment */
    }
    blk = probe;
    out->session_id = head.hdr.session_id;
    out->first_seq  = head.hdr.seq;
    seq = head.hdr.seq;

    while (blk < AMBRE_BLOCKS_PER_SEGMENT) {
        rc = ambre_read_record(d, seg, blk, out->session_id, (int64_t)seq, &head,
                               payload, NULL);
        if (rc != AMBRE_OK) {
            break;                      /* end of the valid chain */
        }
        collect_record(out, seg, blk, &head, payload);
        blk += head.hdr.chunk_count;
        seq++;
        out->last_seq   = head.hdr.seq;
        out->next_block = blk;
    }
    out->active = out->next_block > 0u;
}

/* ---------------------------------------------------------- table merge --- */

static void mark_segment_state(ambre_alloc_table_t *t, uint32_t seg, bool locked)
{
    if (seg >= AMBRE_SEGMENT_COUNT) {
        return;
    }
    if (locked) {
        t->seg_states[seg] = (uint8_t)AMBRE_SEG_LOCKED;
    } else if (t->seg_states[seg] == (uint8_t)AMBRE_SEG_CURRENT) {
        t->seg_states[seg] = (uint8_t)AMBRE_SEG_FROZEN;
    }
}

/* Register an event the table does not know yet (self-describing format). */
static bool add_unknown_event(ambre_alloc_table_t *t, const rec_entry_t *e)
{
    ambre_ev_desc_t desc;

    if (t->event_count >= AMBRE_MAX_EVENTS) {
        return false;
    }
    if (ambre_table_find_event(t, e->event_id) != NULL) {
        return false;
    }
    memset(&desc, 0, sizeof(desc));
    desc.event_id      = e->event_id;
    desc.order         = e->order;
    desc.state         = (uint8_t)AMBRE_EV_OPEN;
    desc.locked        = e->locked;
    desc.file_complete = 0u;
    desc.trig_reason   = e->trig_reason;
    desc.start_seg     = e->seg;
    desc.start_block   = e->block;
    desc.end_seg       = e->seg;
    desc.end_block     = e->block + e->chunks - 1u;
    desc.trig_ts_us    = e->ts_us;
    desc.seq_emerg     = e->seq;
    desc.seq_last      = e->seq;

    if (ambre_table_add_event(t, &desc) != AMBRE_OK) {
        return false;
    }
    mark_segment_state(t, e->seg, e->locked != 0u);
    return true;
}

/* Apply a SEAL record found on disk to the matching descriptor. */
static void apply_seal(ambre_alloc_table_t *t, const rec_entry_t *e)
{
    ambre_ev_desc_t *d = ambre_table_find_event(t, e->event_id);

    if (d == NULL) {
        return;
    }
    if (d->state == (uint8_t)AMBRE_EV_SEALED) {
        return;                         /* already known: keep the table */
    }
    d->state         = (uint8_t)AMBRE_EV_SEALED;
    d->file_complete = e->file_complete;
    d->end_seg       = e->seg;
    d->end_block     = e->block + e->chunks - 1u;
    d->seq_last      = e->seq;
    mark_segment_state(t, e->seg, d->locked != 0u);
}

/* Merge what the disk holds into the table. Returns true when the table
 * changed and must be committed. */
static bool merge_disk_state(ambre_alloc_table_t *t, const seg_scan_t *segs,
                              uint32_t nsegs, uint64_t *last_seq_out)
{
    bool changed = false;
    uint64_t last_seq = 0u;

    for (uint32_t s = 0u; s < nsegs; s++) {
        if (!segs[s].active) {
            continue;
        }
        if (segs[s].last_seq > last_seq) {
            last_seq = segs[s].last_seq;
        }
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
    }
    *last_seq_out = last_seq;
    return changed;
}

/* ------------------------------------------------------------- sealing ---- */

/* Write the "sealed after a power cut" record for one open event. */
static int write_seal_record(ambre_disk_t *d, ambre_alloc_table_t *t,
                              ambre_ev_desc_t *desc, uint64_t *next_seq)
{
    ambre_seal_t seal;
    ambre_block_t blk;
    uint64_t off;
    int rc;

    memset(&seal, 0, sizeof(seal));
    seal.event_id        = desc->event_id;
    seal.file_complete   = 0u;
    seal.seal_cause      = (uint8_t)AMBRE_SEAL_POWERCUT;
    seal.locked          = desc->locked;
    seal.event_number    = 1u;
    seal.time_since_first_ms = 0u;

    rc = ambre_block_build(&blk, AMBRE_REC_SEAL, *next_seq, t->session_id,
                           desc->trig_ts_us, &seal, sizeof(seal));
    if (rc != AMBRE_OK) {
        return rc;
    }
    off = ambre_block_offset(t->current_seg, t->current_block);
    rc = ambre_block_write(d, off, &blk);
    if (rc != AMBRE_OK) {
        return rc;
    }
    desc->state         = (uint8_t)AMBRE_EV_SEALED;
    desc->file_complete = 0u;
    desc->end_seg       = t->current_seg;
    desc->end_block     = t->current_block;
    desc->seq_last      = *next_seq;
    t->current_block++;
    (*next_seq)++;
    return AMBRE_OK;
}

/* Seal every event that was still open when the power died (REP-03). */
static int seal_open_events(ambre_disk_t *d, ambre_alloc_table_t *t,
                            uint64_t *next_seq, ambre_recovery_result_t *res)
{
    int rc;

    for (uint32_t i = 0u; i < t->event_count; i++) {
        ambre_ev_desc_t *desc = &t->events[i];

        if (desc->state != (uint8_t)AMBRE_EV_OPEN) {
            continue;
        }
        rc = write_seal_record(d, t, desc, next_seq);
        if (rc != AMBRE_OK) {
            return rc;
        }
        res->sealed_incomplete++;
    }
    return AMBRE_OK;
}

/* Next sequence number: never reuse a seq the disk or the table already saw. */
static uint64_t resume_seq(const ambre_alloc_table_t *table, uint64_t disk_last)
{
    uint64_t from_disk = disk_last + 1u;
    uint64_t from_table = table->last_seq + 1u;

    return (from_table > from_disk) ? from_table : from_disk;
}

/* Commit the table when the recovery changed anything, and report. */
static int finish_recovery(ambre_disk_t *d, ambre_alloc_table_t *table,
                           bool must_commit, ambre_recovery_result_t *out)
{
    if (!must_commit) {
        return AMBRE_OK;
    }
    if (ambre_table_commit(d, table) != AMBRE_OK) {
        return AMBRE_ERR_IO;
    }
    out->table_commits = 1u;
    return AMBRE_OK;
}

/* ------------------------------------------------------------- public ----- */

/* Step 1 (spec 4.6): the valid table copy with the highest generation, or a
 * table rebuilt by scanning the blocks when both copies are unusable. */
static int load_table(const ambre_disk_t *d, ambre_alloc_table_t *table,
                      bool *rebuilt)
{
    int copy_used = -1;
    int rc = ambre_table_read_best(d, table, &copy_used);

    *rebuilt = false;
    if (rc == AMBRE_OK) {
        return AMBRE_OK;
    }
    *rebuilt = true;
    rc = ambre_rebuild_table(d, table);
    if (rc != AMBRE_OK) {
        /* Nothing usable at all: empty or foreign partition. */
        ambre_table_init(table, 0u);
        table->current_seg = 0u;
        table->current_block = 0u;
    }
    return AMBRE_OK;
}

/* Step 2: walk the chains of the segments that are in use. */
static void scan_segments(const ambre_disk_t *d, const ambre_alloc_table_t *table,
                          bool rebuilt, seg_scan_t *segs,
                          ambre_recovery_result_t *out)
{
    for (uint32_t s = 0u; s < AMBRE_SEG_SCAN_MAX; s++) {
        bool in_use = rebuilt || table->seg_states[s] != (uint8_t)AMBRE_SEG_FREE;

        if (!in_use) {
            continue;                   /* free segment: stale blocks only */
        }
        collect_segment(d, s, &segs[s]);
        if (segs[s].active) {
            out->scanned_blocks += segs[s].next_block;
        }
    }
}

/* Where does writing resume? The disk chain is the authority: a block that
 * was acknowledged but lost (volatile cache) must not be rewritten (STO-03),
 * so the pointer only ever moves forward. */
static bool choose_resume_point(ambre_alloc_table_t *table, const seg_scan_t *segs)
{
    uint32_t best_seg = table->current_seg;
    uint32_t best_blk = table->current_block;
    uint64_t best_seq = 0u;

    for (uint32_t s = 0u; s < AMBRE_SEG_SCAN_MAX; s++) {
        if (!segs[s].active) {
            continue;
        }
        if (segs[s].last_seq >= best_seq) {
            best_seq = segs[s].last_seq;
            best_seg = s;
            best_blk = segs[s].next_block;
        }
    }
    if (best_blk <= table->current_block && best_seg == table->current_seg) {
        return false;
    }
    if (best_blk <= table->current_block) {
        return false;                   /* never move the pointer backwards */
    }
    table->current_seg = best_seg;
    table->current_block = best_blk;
    return true;
}

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
