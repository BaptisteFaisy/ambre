/*
 * ambre_journal_event.c — The event path: trigger, continuation, sealing and
 * read back (GEL-01..GEL-05, EXP-01).
 *
 * The trigger path is the only one with a hard deadline: one contiguous
 * write, one table commit, one flush. Nothing here allocates, and nothing
 * waits for a less urgent process (GEL-03).
 */

#include "ambre_journal_internal.h"
#include <string.h>

/* A segment holding an event is no longer free: locked events are never
 * recycled (STO-06, SEC-03). */
static void pin_segment(ambre_journal_t *j, uint32_t seg, bool locked)
{
    if (seg >= AMBRE_SEGMENT_COUNT) {
        return;
    }
    j->table.seg_states[seg] = (uint8_t)(locked ? AMBRE_SEG_LOCKED
                                                : AMBRE_SEG_FROZEN);
}

/* Descriptor of an event whose EMERG record was just written. */
static void build_event_desc(const ambre_journal_t *j, const ambre_emerg_t *emerg,
                             uint64_t seq, uint32_t blocks, ambre_ev_desc_t *desc)
{
    memset(desc, 0, sizeof(*desc));
    desc->event_id      = emerg->event_id;
    desc->order         = emerg->crash_order;
    desc->state         = (uint8_t)AMBRE_EV_OPEN;
    desc->locked        = emerg->locked;
    desc->file_complete = 0u;
    desc->trig_reason   = emerg->trig_reason;
    desc->start_seg     = j->table.current_seg;
    desc->start_block   = j->table.current_block;
    desc->end_seg       = j->table.current_seg;
    desc->end_block     = j->table.current_block + blocks - 1u;
    desc->trig_ts_us    = emerg->trig_ts_us;
    desc->seq_emerg     = seq;
    desc->seq_last      = seq;
}

/* ------------------------------------------------------------- trigger --- */

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

/* -------------------------------------------------------- continuation --- */

int ambre_journal_continue(ambre_journal_t *j, const ambre_cont_t *cont)
{
    uint64_t seq = 0u;
    uint32_t blocks = 0u;
    int rc;

    if (j == NULL || cont == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (j->stats.open_events == 0u) {
        return AMBRE_ERR_STATE;        /* no event open */
    }
    rc = ambre_journal_write_record(j, (uint8_t)AMBRE_REC_CONT,
                                    j->table.last_seq, cont,
                                    (uint32_t)sizeof(*cont), &seq, &blocks);
    if (rc != AMBRE_OK) {
        return rc;
    }
    ambre_journal_advance(j, blocks, seq);
    if (j->disk->flush != NULL && j->disk->flush(j->disk->ctx) != 0) {
        return AMBRE_ERR_IO;
    }
    j->stats.flush_count++;
    if (j->disk->mark != NULL) {
        j->disk->mark(j->disk->ctx, AMBRE_MARK_DATA_FLUSHED, seq, 0u);
    }
    return AMBRE_OK;
}

/* --------------------------------------------------------------- sealing -- */

int ambre_journal_seal(ambre_journal_t *j, const ambre_seal_t *seal)
{
    ambre_ev_desc_t *desc;
    uint64_t seq = 0u;
    uint32_t blocks = 0u;
    int rc;

    if (j == NULL || seal == NULL) {
        return AMBRE_ERR_PARAM;
    }
    desc = ambre_table_find_event(&j->table, seal->event_id);
    if (desc == NULL) {
        return AMBRE_ERR_STATE;
    }
    rc = ambre_journal_write_record(j, (uint8_t)AMBRE_REC_SEAL, j->table.last_seq,
                                    seal, (uint32_t)sizeof(*seal), &seq, &blocks);
    if (rc != AMBRE_OK) {
        return rc;
    }
    ambre_journal_advance(j, blocks, seq);

    desc->state = (uint8_t)AMBRE_EV_SEALED;
    desc->file_complete = seal->file_complete;
    pin_segment(j, desc->start_seg, seal->locked != 0u);
    desc->end_seg = j->table.current_seg;
    desc->end_block = j->table.current_block - 1u;
    desc->seq_last = seq;

    rc = ambre_table_commit(j->disk, &j->table);
    if (rc != AMBRE_OK) {
        return rc;
    }
    if (j->disk->mark != NULL) {
        j->disk->mark(j->disk->ctx, AMBRE_MARK_TABLE_COMMIT, j->table.generation, 0u);
    }
    if (j->disk->flush != NULL && j->disk->flush(j->disk->ctx) != 0) {
        return AMBRE_ERR_IO;
    }
    j->stats.flush_count++;
    if (j->disk->mark != NULL) {
        j->disk->mark(j->disk->ctx, AMBRE_MARK_SEALED, seal->event_id,
                      (uint64_t)seal->file_complete);
    }
    if (j->stats.open_events > 0u) {
        j->stats.open_events--;
    }
    j->stats.sealed_events++;
    return AMBRE_OK;
}

/* ------------------------------------------------------------ read back -- */

#define AMBRE_EVENT_WALK_MAX 64u
/* Walk the records of one event, from its EMERG block to its SEAL record. */
static void walk_event_records(const ambre_journal_t *j,
                               const ambre_ev_desc_t *desc, uint64_t session_id,
                               ambre_event_view_t *view)
{
    uint8_t payload[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    uint32_t blk = desc->start_block;
    uint32_t len = 0u;

    for (uint32_t step = 0u; step < AMBRE_EVENT_WALK_MAX; step++) {
        ambre_block_t head;
        int rc = ambre_read_record(j->disk, desc->start_seg, blk, session_id,
                                   (int64_t)desc->seq_emerg + step, &head,
                                   payload, &len);
        if (rc != AMBRE_OK) {
            return;
        }
        if (head.hdr.rec_type == (uint8_t)AMBRE_REC_CONT &&
            len >= sizeof(ambre_cont_t)) {
            view->cont_count++;
        } else if (head.hdr.rec_type == (uint8_t)AMBRE_REC_SEAL &&
                   len >= sizeof(ambre_seal_t)) {
            memcpy(&view->seal, payload, sizeof(view->seal));
            view->have_seal = true;
        }
        blk += head.hdr.chunk_count;
        if (head.hdr.seq >= desc->seq_last) {
            return;
        }
    }
}



/* Read one record of the wanted type at (seg, blk). */
static int read_one(const ambre_disk_t *disk, uint32_t seg, uint32_t blk,
                    uint64_t session_id, int64_t expect_seq, uint8_t type,
                    uint8_t *payload, uint32_t cap, uint32_t *len_out)
{
    ambre_block_t head;
    int rc = ambre_read_record(disk, seg, blk, session_id, expect_seq, &head,
                               payload, len_out);

    if (rc != AMBRE_OK) {
        return rc;
    }
    if (head.hdr.rec_type != type) {
        return AMBRE_ERR_STATE;
    }
    if (*len_out > cap) {
        return AMBRE_ERR_TOO_BIG;
    }
    return AMBRE_OK;
}

int ambre_journal_read_event(const ambre_journal_t *j, uint32_t event_id,
                             ambre_event_view_t *view)
{
    const ambre_ev_desc_t *desc;
    uint8_t payload[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    uint32_t len = 0u;
    uint64_t session_id;

    if (j == NULL || view == NULL) {
        return AMBRE_ERR_PARAM;
    }
    memset(view, 0, sizeof(*view));
    desc = ambre_table_find_event_const(&j->table, event_id);
    if (desc == NULL) {
        return AMBRE_ERR_STATE;
    }
    session_id = j->table.session_id;

    /* EMERG record. */
    if (read_one(j->disk, desc->start_seg, desc->start_block, session_id,
                 (int64_t)desc->seq_emerg, (uint8_t)AMBRE_REC_EMERG, payload,
                 sizeof(payload), &len) == AMBRE_OK &&
        len >= sizeof(ambre_emerg_t)) {
        memcpy(&view->emerg, payload, sizeof(view->emerg));
        view->have_emerg = true;
        view->first_ts_us = view->emerg.t0_ts_us;
    }

    walk_event_records(j, desc, session_id, view);
    return AMBRE_OK;
}
