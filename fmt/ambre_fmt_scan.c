/*
 * ambre_fmt_scan.c - Segment chain scanner and full-disk table rebuild.
 *
 * Implements docs/FORMAT.md section 10 (recovery scan & rebuild fallback).
 * Portable C11, no dynamic allocation, memcpy-only payload decoding.
 */

#include "ambre_fmt.h"
#include <string.h>

#define TABLE_FL_FORMATTED 0x01u
#define TABLE_FL_MEM_FULL  0x02u

/* Defined once in ambre_fmt_table.c (internal cross-TU helper). */
uint32_t ambre_fmt_table_body_crc(const ambre_alloc_table_t *t);

/* Read and validate a single block at (seg, blk). Returns AMBRE_OK on valid
 * block, AMBRE_ERR_IO when the block is outside the disk or unreadable, or
 * the codec error reported by ambre_block_validate(). */
int ambre_scan_segment(const ambre_disk_t *d, uint64_t session_id,
                       uint64_t expected_first_seq, uint32_t seg,
                       uint32_t start_blk, ambre_scan_result_t *out)
{
    uint32_t blk;
    uint64_t seq;

    if (d == NULL || d->read == NULL || out == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (seg >= AMBRE_SEGMENT_COUNT || start_blk > AMBRE_BLOCKS_PER_SEGMENT) {
        return AMBRE_ERR_PARAM;
    }
    memset(out, 0, sizeof(*out));
    out->next_block = start_blk;
    blk = start_blk;
    seq = expected_first_seq;

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

/* --------------------------------------------------------------------------
 * Full-disk table rebuild (REP-02 fallback when both table copies fail).
 * ------------------------------------------------------------------------ */

typedef struct {
    bool     active;
    uint64_t session_id;
    uint64_t first_seq;
    ambre_scan_result_t scan;
} seg_info_t;

static void register_emerg_record(ambre_alloc_table_t *out, uint32_t seg,
                                  uint32_t blk, const ambre_block_t *head,
                                  const uint8_t *payload)
{
    ambre_emerg_t em;
    ambre_ev_desc_t ev;

    if (head->hdr.total_len < sizeof(em)) {
        return;
    }
    memcpy(&em, payload, sizeof(em));
    memset(&ev, 0, sizeof(ev));
    ev.event_id      = em.event_id;
    ev.order         = em.crash_order;
    ev.state         = (uint8_t)AMBRE_EV_OPEN;
    ev.locked        = em.locked;
    ev.file_complete = 0u;
    ev.trig_reason   = em.trig_reason;
    ev.start_seg     = seg;
    ev.start_block   = blk;
    ev.end_seg       = seg;
    ev.end_block     = blk + head->hdr.chunk_count - 1u;
    ev.trig_ts_us    = em.trig_ts_us;
    ev.seq_emerg     = head->hdr.seq;
    ev.seq_last      = head->hdr.seq;

    if (ambre_table_add_event(out, &ev) == AMBRE_OK &&
        (uint64_t)em.event_id >= out->next_event_id) {
        out->next_event_id = (uint64_t)em.event_id + 1u;
    }
}

static void apply_followup_record(ambre_alloc_table_t *out, uint32_t seg,
                                    uint32_t blk, const ambre_block_t *head,
                                    const uint8_t *payload)
{
    ambre_ev_desc_t *ev;
    uint32_t end_blk = blk + head->hdr.chunk_count - 1u;

    if (head->hdr.rec_type == (uint8_t)AMBRE_REC_CONT &&
        head->hdr.total_len >= sizeof(ambre_cont_t)) {
        ambre_cont_t cont;
        memcpy(&cont, payload, sizeof(cont));
        ev = ambre_table_find_event(out, cont.event_id);
        if (ev != NULL && head->hdr.seq > ev->seq_last) {
            ev->end_seg   = seg;
            ev->end_block = end_blk;
            ev->seq_last  = head->hdr.seq;
        }
    } else if (head->hdr.rec_type == (uint8_t)AMBRE_REC_SEAL &&
               head->hdr.total_len >= sizeof(ambre_seal_t)) {
        ambre_seal_t seal;
        memcpy(&seal, payload, sizeof(seal));
        ev = ambre_table_find_event(out, seal.event_id);
        if (ev != NULL && head->hdr.seq >= ev->seq_emerg) {
            ev->state         = (uint8_t)AMBRE_EV_SEALED;
            ev->file_complete = seal.file_complete;
            ev->locked        = seal.locked;
            if (head->hdr.seq > ev->seq_last) {
                ev->end_seg   = seg;
                ev->end_block = end_blk;
                ev->seq_last  = head->hdr.seq;
            }
        }
    }
}

static void scan_seg_records(const ambre_disk_t *d, uint32_t seg,
                             const seg_info_t *info, ambre_alloc_table_t *out,
                             bool emerg_pass)
{
    uint8_t payload[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    uint32_t blk = 0u;
    uint64_t seq = info->first_seq;

    while (blk < info->scan.next_block) {
        ambre_block_t head;
        if (ambre_read_record(d, seg, blk, info->session_id, (int64_t)seq, &head,
                               payload, NULL) != AMBRE_OK) {
            break;
        }
        if (emerg_pass && head.hdr.rec_type == (uint8_t)AMBRE_REC_EMERG) {
            register_emerg_record(out, seg, blk, &head, payload);
        } else if (!emerg_pass) {
            apply_followup_record(out, seg, blk, &head, payload);
        }
        blk += head.hdr.chunk_count;
        seq++;
    }
}

static void mark_event_segments(ambre_alloc_table_t *out, uint32_t max_segs)
{
    for (uint32_t i = 0u; i < AMBRE_MAX_EVENTS; i++) {
        const ambre_ev_desc_t *ev = &out->events[i];
        uint32_t s;
        if (ev->state == (uint8_t)AMBRE_EV_EMPTY) {
            continue;
        }
        if (ev->state == (uint8_t)AMBRE_EV_SEALED) {
            out->sealed_count++;
        }
        s = ev->start_seg;
        for (;;) {
            if (ev->locked != 0u) {
                out->seg_states[s] = (uint8_t)AMBRE_SEG_LOCKED;
            } else if (out->seg_states[s] != (uint8_t)AMBRE_SEG_LOCKED) {
                out->seg_states[s] = (uint8_t)AMBRE_SEG_FROZEN;
            }
            if (s == ev->end_seg) {
                break;
            }
            s = (s + 1u) % max_segs;
            if (s == ev->start_seg) {
                break;
            }
        }
    }
}

static void pick_current_segment(ambre_alloc_table_t *out,
                                 const seg_info_t *segs, uint32_t max_segs,
                                 uint32_t latest_seg, bool have_latest)
{
    if (!have_latest) {
        out->current_seg   = 0u;
        out->current_block = 0u;
        out->seg_states[0] = (uint8_t)AMBRE_SEG_CURRENT;
        return;
    }
    if (segs[latest_seg].scan.next_block < AMBRE_BLOCKS_PER_SEGMENT) {
        out->current_seg   = latest_seg;
        out->current_block = segs[latest_seg].scan.next_block;
        if (out->seg_states[latest_seg] != (uint8_t)AMBRE_SEG_LOCKED) {
            out->seg_states[latest_seg] = (uint8_t)AMBRE_SEG_CURRENT;
        }
        return;
    }
    for (uint32_t i = 1u; i <= max_segs; i++) {
        uint32_t cand = (latest_seg + i) % max_segs;
        if (out->seg_states[cand] == (uint8_t)AMBRE_SEG_FREE) {
            out->current_seg   = cand;
            out->current_block = 0u;
            out->seg_states[cand] = (uint8_t)AMBRE_SEG_CURRENT;
            return;
        }
    }
    out->current_seg   = latest_seg;
    out->current_block = AMBRE_BLOCKS_PER_SEGMENT - 1u;
    out->flags        |= TABLE_FL_MEM_FULL;
}

static void assign_seg_states(ambre_alloc_table_t *out, const seg_info_t *segs,
                              uint32_t max_segs, uint32_t latest_seg,
                              bool have_latest)
{
    for (uint32_t s = 0u; s < max_segs; s++) {
        out->seg_states[s] = segs[s].active
            ? (uint8_t)AMBRE_SEG_ARCHIVED : (uint8_t)AMBRE_SEG_FREE;
    }
    mark_event_segments(out, max_segs);
    pick_current_segment(out, segs, max_segs, latest_seg, have_latest);
}

static void scan_all_segments(const ambre_disk_t *d, seg_info_t *segs,
                              uint32_t max_segs, uint32_t *latest_seg,
                              bool *have_latest)
{
    uint64_t best_seq = 0u;

    for (uint32_t s = 0u; s < max_segs; s++) {
        ambre_block_t b0;
        if (ambre_read_record(d, s, 0u, 0u, -1, &b0, NULL, NULL) == AMBRE_OK &&
            b0.hdr.chunk_index == 0u) {
            segs[s].session_id = b0.hdr.session_id;
            segs[s].first_seq  = b0.hdr.seq;
            if (ambre_scan_segment(d, b0.hdr.session_id, b0.hdr.seq, s, 0u,
                                   &segs[s].scan) == AMBRE_OK &&
                segs[s].scan.valid_blocks > 0u) {
                segs[s].active = true;
                if (!(*have_latest) || segs[s].scan.last_seq >= best_seq) {
                    *have_latest = true;
                    best_seq     = segs[s].scan.last_seq;
                    *latest_seg  = s;
                }
            }
        }
    }
}

int ambre_rebuild_table(const ambre_disk_t *d, ambre_alloc_table_t *out)
{
    seg_info_t segs[AMBRE_SEGMENT_COUNT];
    uint32_t max_segs;
    uint32_t latest_seg = 0u;
    bool have_latest = false;

    if (d == NULL || d->read == NULL || out == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (d->size < (uint64_t)AMBRE_FIRST_DATA_OFFSET + AMBRE_BLOCK_SIZE) {
        return AMBRE_ERR_IO;
    }
    max_segs = (uint32_t)((d->size - AMBRE_FIRST_DATA_OFFSET) / AMBRE_SEGMENT_SIZE);
    if (max_segs == 0u) {
        max_segs = 1u; /* small test disk with partial first segment */
    }
    if (max_segs > AMBRE_SEGMENT_COUNT) {
        max_segs = AMBRE_SEGMENT_COUNT;
    }

    memset(segs, 0, sizeof(segs));
    ambre_table_init(out, 0u);
    scan_all_segments(d, segs, max_segs, &latest_seg, &have_latest);

    for (uint32_t s = 0u; s < max_segs; s++) {
        if (segs[s].active) {
            scan_seg_records(d, s, &segs[s], out, true);
        }
    }
    for (uint32_t s = 0u; s < max_segs; s++) {
        if (segs[s].active) {
            scan_seg_records(d, s, &segs[s], out, false);
        }
    }

    if (have_latest) {
        out->session_id = segs[latest_seg].session_id;
        out->last_seq   = segs[latest_seg].scan.last_seq;
    }
    out->flags = TABLE_FL_FORMATTED;
    assign_seg_states(out, segs, max_segs, latest_seg, have_latest);
    out->crc = ambre_fmt_table_body_crc(out);
    return AMBRE_OK;
}
