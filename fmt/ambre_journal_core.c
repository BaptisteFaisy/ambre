/*
 * ambre_journal_core.c — Journal engine: lifecycle, capture, flush cadence
 * and segment rotation (GEL-03, GEL-06..GEL-08, STO-03, STO-09).
 *
 * The critical path (trigger to "acquired") lives in ambre_journal_event.c.
 * Everything here runs without dynamic allocation: the state is a fixed-size
 * struct and the record staging buffer is pre-allocated.
 */

#include "ambre_journal_internal.h"
#include <string.h>

#define AMBRE_JOURNAL_OK AMBRE_OK

static int rotate_segment(ambre_journal_t *j);

/* ------------------------------------------------------------- helpers --- */

/* Build every chunk of a record into the journal's staging buffer. */
static int stage_record_chunks(ambre_journal_t *j, uint8_t rec_type,
                               uint64_t ts_us, const void *payload,
                               uint32_t payload_len, uint64_t seq,
                               uint16_t chunks)
{
    for (uint16_t ci = 0u; ci < chunks; ci++) {
        uint32_t off_in_payload = (uint32_t)ci * AMBRE_BLOCK_PAYLOAD_MAX;
        uint32_t len = payload_len - off_in_payload;
        ambre_block_t blk;
        int rc;

        if (len > AMBRE_BLOCK_PAYLOAD_MAX) {
            len = AMBRE_BLOCK_PAYLOAD_MAX;
        }
        rc = ambre_block_build_chunk(&blk, (ambre_rec_type_t)rec_type, seq,
                                     j->table.session_id, ts_us, ci, chunks,
                                     (const uint8_t *)payload + off_in_payload,
                                     len, payload_len);
        if (rc != AMBRE_OK) {
            return rc;
        }
        ambre_block_pack(&blk, j->rec_buf + (size_t)ci * AMBRE_BLOCK_SIZE);
    }
    return AMBRE_OK;
}

uint64_t ambre_journal_write_offset(const ambre_journal_t *j)
{
    return ambre_block_offset(j->table.current_seg, j->table.current_block);
}

void ambre_journal_advance(ambre_journal_t *j, uint32_t blocks, uint64_t seq)
{
    j->table.current_block += blocks;
    j->table.last_seq = seq;
    j->next_seq = seq + 1u;
}

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

/* ---------------------------------------------------------- event table -- */

int ambre_journal_register_event(ambre_journal_t *j, const ambre_ev_desc_t *desc)
{
    int rc = ambre_table_add_event(&j->table, desc);

    if (rc != AMBRE_JOURNAL_OK) {
        return rc;
    }
    if (j->table.next_event_id <= desc->event_id) {
        j->table.next_event_id = desc->event_id + 1u;
    }
    return ambre_table_commit(j->disk, &j->table);
}

/* --------------------------------------------------------- segment pool -- */

static bool segment_holds_event(const ambre_alloc_table_t *t, uint32_t seg)
{
    for (uint32_t i = 0u; i < t->event_count; i++) {
        const ambre_ev_desc_t *e = &t->events[i];

        if (e->state == (uint8_t)AMBRE_EV_EMPTY) {
            continue;
        }
        if (e->start_seg == seg || e->end_seg == seg) {
            return true;
        }
    }
    return false;
}

static bool segment_is_reclaimable(const ambre_alloc_table_t *t, uint32_t seg)
{
    bool holds = false;

    if (t->seg_states[seg] == (uint8_t)AMBRE_SEG_LOCKED) {
        return false;
    }
    for (uint32_t i = 0u; i < t->event_count; i++) {
        const ambre_ev_desc_t *e = &t->events[i];

        if (e->state == (uint8_t)AMBRE_EV_EMPTY) {
            continue;
        }
        if (e->start_seg != seg && e->end_seg != seg) {
            continue;
        }
        holds = true;
        if (e->locked) {
            return false;               /* a locked event is never recycled */
        }
        if (e->state != (uint8_t)AMBRE_EV_SEALED) {
            return false;               /* an open event keeps its segment */
        }
    }
    return holds;
}

int ambre_journal_pick_segment(ambre_journal_t *j, uint32_t *seg_out)
{
    uint32_t limit = j->cfg.segment_limit;

    if (limit == 0u || limit > AMBRE_SEGMENT_COUNT) {
        limit = AMBRE_SEGMENT_COUNT;
    }
    for (uint32_t k = 1u; k <= limit; k++) {
        uint32_t seg = (j->table.current_seg + k) % limit;

        if (j->table.seg_states[seg] == (uint8_t)AMBRE_SEG_FREE) {
            *seg_out = seg;
            return AMBRE_JOURNAL_OK;
        }
    }
    for (uint32_t k = 1u; k <= limit; k++) {
        uint32_t seg = (j->table.current_seg + k) % limit;

        if (segment_is_reclaimable(&j->table, seg)) {
            *seg_out = seg;
            return AMBRE_JOURNAL_OK;
        }
    }
    j->stats.refused_no_space++;
    j->table.flags |= 0x02u;           /* memory full */
    return AMBRE_ERR_FULL;
}

int ambre_journal_overwrite_segment(ambre_journal_t *j, uint32_t seg)
{
    for (uint32_t i = 0u; i < j->table.event_count; i++) {
        ambre_ev_desc_t *e = &j->table.events[i];

        if (e->state == (uint8_t)AMBRE_EV_EMPTY) {
            continue;
        }
        if (e->start_seg != seg && e->end_seg != seg) {
            continue;
        }
        e->state = (uint8_t)AMBRE_EV_SEALED;
        e->file_complete = 0u;         /* the data is about to be recycled */
    }
    j->table.seg_states[seg] = (uint8_t)AMBRE_SEG_FREE;
    return AMBRE_JOURNAL_OK;
}

/* Move to the next segment when the current one is full (STO-03: an
 * acknowledged block is never rewritten before its segment is released). */
static int rotate_segment(ambre_journal_t *j)
{
    uint32_t seg;
    int rc = ambre_journal_pick_segment(j, &seg);

    if (rc != AMBRE_JOURNAL_OK) {
        return rc;
    }
    if (segment_holds_event(&j->table, j->table.current_seg)) {
        j->table.seg_states[j->table.current_seg] = (uint8_t)AMBRE_SEG_FROZEN;
    } else {
        j->table.seg_states[j->table.current_seg] = (uint8_t)AMBRE_SEG_FREE;
    }
    if (j->table.seg_states[seg] != (uint8_t)AMBRE_SEG_FREE) {
        rc = ambre_journal_overwrite_segment(j, seg);
        if (rc != AMBRE_JOURNAL_OK) {
            return rc;
        }
    }
    j->table.current_seg = seg;
    j->table.current_block = 0u;
    j->table.seg_states[seg] = (uint8_t)AMBRE_SEG_CURRENT;
    return ambre_journal_register_event_marker(j);
}

/* Commit the table after a segment change (no event is registered). */
int ambre_journal_register_event_marker(ambre_journal_t *j)
{
    return ambre_table_commit(j->disk, &j->table);
}

/* ------------------------------------------------------------ lifecycle -- */

void ambre_journal_cfg_default(ambre_journal_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->session_id = 1u;
    cfg->segment_limit = AMBRE_SEGMENT_COUNT;
    cfg->flush_period_us = 100000u;    /* 100 ms in continuous mode (GEL-07) */
    cfg->event_flush_period_us = 10000u; /* 10 ms during an event (GEL-04)  */
    cfg->continuous_mode = true;
    cfg->ignition_cycle = 1u;
}

static int zero_area(ambre_disk_t *disk, uint64_t off, uint64_t len)
{
    uint8_t zeros[4096];

    memset(zeros, 0, sizeof(zeros));
    while (len > 0u) {
        size_t chunk = (len > sizeof(zeros)) ? sizeof(zeros) : (size_t)len;

        if (disk->write(disk->ctx, off, zeros, chunk) != 0) {
            return AMBRE_ERR_IO;
        }
        off += chunk;
        len -= chunk;
    }
    return AMBRE_OK;
}

int ambre_journal_format(ambre_disk_t *disk, const ambre_journal_cfg_t *cfg)
{
    ambre_alloc_table_t t;
    uint32_t limit;
    int rc;

    if (disk == NULL || cfg == NULL) {
        return AMBRE_ERR_PARAM;
    }
    limit = (cfg->segment_limit == 0u) ? AMBRE_SEGMENT_COUNT : cfg->segment_limit;
    if (limit > AMBRE_SEGMENT_COUNT) {
        limit = AMBRE_SEGMENT_COUNT;
    }

    /* A fresh partition must not keep stale blocks: the recovery scan would
     * otherwise find events that belong to a previous journal. */
    rc = zero_area(disk, AMBRE_FIRST_DATA_OFFSET,
                   (uint64_t)limit * AMBRE_SEGMENT_SIZE);
    if (rc != AMBRE_OK) {
        return rc;
    }

    ambre_table_init(&t, cfg->session_id);
    t.current_seg = 0u;
    t.current_block = 0u;
    t.next_event_id = 1u;
    t.last_seq = 0u;

    {
        uint8_t buf[AMBRE_BLOCK_SIZE];

        ambre_table_pack(&t, buf);
        if (disk->write(disk->ctx, AMBRE_TABLE_A_OFFSET, buf, sizeof(buf)) != 0 ||
            disk->write(disk->ctx, AMBRE_TABLE_B_OFFSET, buf, sizeof(buf)) != 0) {
            return AMBRE_ERR_IO;
        }
    }
    if (disk->flush != NULL) {
        (void)disk->flush(disk->ctx);
    }
    return AMBRE_OK;
}

int ambre_journal_open(ambre_disk_t *disk, const ambre_journal_cfg_t *cfg,
                       bool run_recovery, ambre_journal_t *j)
{
    int copy_used = -1;
    int rc;

    if (disk == NULL || cfg == NULL || j == NULL) {
        return AMBRE_ERR_PARAM;
    }
    memset(j, 0, sizeof(*j));
    j->disk = disk;
    j->cfg = *cfg;

    if (run_recovery) {
        rc = ambre_recover(disk, &j->last_recovery);
        if (rc != AMBRE_OK) {
            return rc;
        }
        j->table = j->last_recovery.table;
        j->recovered = true;
    } else {
        rc = ambre_table_read_best(disk, &j->table, &copy_used);
        if (rc != AMBRE_OK) {
            /* No usable table: format a fresh journal. */
            rc = ambre_journal_format(disk, cfg);
            if (rc != AMBRE_OK) {
                return rc;
            }
            ambre_table_init(&j->table, cfg->session_id);
        }
    }
    j->next_seq = j->table.last_seq + 1u;
    j->table.seg_states[j->table.current_seg] = (uint8_t)AMBRE_SEG_CURRENT;
    return AMBRE_OK;
}

/* -------------------------------------------------------------- capture -- */

int ambre_journal_append_data(ambre_journal_t *j, uint64_t ts_us,
                              const ambre_sample_t *samples, uint32_t count)
{
    uint64_t seq;
    uint32_t blocks;
    int rc;

    if (j == NULL || samples == NULL || count == 0u) {
        return AMBRE_ERR_PARAM;
    }
    if (!j->cfg.continuous_mode) {
        return AMBRE_OK;               /* event-only mode (GEL-08) */
    }
    rc = ambre_journal_write_record(j, (uint8_t)AMBRE_REC_DATA, ts_us, samples,
                                    count * (uint32_t)sizeof(ambre_sample_t),
                                    &seq, &blocks);
    if (rc != AMBRE_OK) {
        return rc;
    }
    ambre_journal_advance(j, blocks, seq);
    return AMBRE_OK;
}

int ambre_journal_append_gap(ambre_journal_t *j, const ambre_gap_t *gap)
{
    uint64_t seq;
    uint32_t blocks;
    int rc;

    if (j == NULL || gap == NULL) {
        return AMBRE_ERR_PARAM;
    }
    rc = ambre_journal_write_record(j, (uint8_t)AMBRE_REC_GAP, gap->start_ts_us,
                                    gap, (uint32_t)sizeof(*gap), &seq, &blocks);
    if (rc != AMBRE_OK) {
        return rc;
    }
    ambre_journal_advance(j, blocks, seq);
    return AMBRE_OK;
}

/* ---------------------------------------------------------------- flush -- */

int ambre_journal_flush(ambre_journal_t *j, uint64_t now_us)
{
    if (j == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (j->disk->flush != NULL && j->disk->flush(j->disk->ctx) != 0) {
        return AMBRE_ERR_IO;
    }
    j->stats.flush_count++;
    j->last_flush_ts_us = now_us;
    if (j->disk->mark != NULL) {
        j->disk->mark(j->disk->ctx, AMBRE_MARK_DATA_FLUSHED, j->next_seq - 1u, 0u);
    }
    return AMBRE_OK;
}

int ambre_journal_tick(ambre_journal_t *j, uint64_t now_us)
{
    int rc;

    if (j == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (j->table.current_block >= AMBRE_BLOCKS_PER_SEGMENT) {
        rc = rotate_segment(j);
        if (rc != AMBRE_OK) {
            return rc;
        }
    }
    if (!j->cfg.continuous_mode) {
        return AMBRE_OK;
    }
    if (j->last_flush_ts_us != 0u &&
        now_us - j->last_flush_ts_us < j->cfg.flush_period_us) {
        return AMBRE_OK;
    }
    return ambre_journal_flush(j, now_us);
}

/* ------------------------------------------------------------ accessors -- */

const ambre_alloc_table_t *ambre_journal_table(const ambre_journal_t *j)
{
    return (j != NULL) ? &j->table : NULL;
}

const ambre_journal_stats_t *ambre_journal_stats(const ambre_journal_t *j)
{
    return (j != NULL) ? &j->stats : NULL;
}

const ambre_recovery_result_t *ambre_journal_recovery(const ambre_journal_t *j)
{
    return (j != NULL) ? &j->last_recovery : NULL;
}
