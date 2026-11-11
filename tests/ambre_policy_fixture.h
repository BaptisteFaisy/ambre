/*
 * ambre_policy_fixture.h - Shared fixture of the week-4 policy suite.
 *
 * Two things live here, and nothing else:
 *
 *   1. a journal fixture (sessions, events, segment filling, read-back checks)
 *      on the frozen in-memory disk of tests/ambre_test_disk.h;
 *   2. a hand-built image fixture, for the recovery sweep: the test writes the
 *      blocks and the allocation table itself, so it can stop at any
 *      intermediate state of a multi-event sequence.
 *
 * Header-only and static inline, exactly like ambre_test_disk.h and
 * ambre_gap_disk.h, so tests/test_policy.c links without extra objects.
 */

#ifndef AMBRE_POLICY_FIXTURE_H
#define AMBRE_POLICY_FIXTURE_H

#include "ambre_test.h"
#include "ambre_test_disk.h"
#include "ambre_journal.h"
#include "ambre_simdisk.h"
#include <stdlib.h>
#include <string.h>

#define POLICY_SESSION 0x5151u
#define POLICY_TS      1000000ull

/* --------------------------------------------------------------- journal -- */

typedef struct {
    ambre_test_disk_t   td;
    ambre_journal_t     j;
    ambre_journal_cfg_t cfg;
} policy_fx_t;

static inline ambre_journal_cfg_t policy_cfg(uint32_t segments)
{
    ambre_journal_cfg_t cfg;

    ambre_journal_cfg_default(&cfg);
    cfg.session_id = POLICY_SESSION;
    cfg.segment_limit = segments;
    cfg.continuous_mode = true;
    return cfg;
}

/* Allocate the disk, format it, open the journal. False when malloc fails. */
static inline bool policy_create(policy_fx_t *fx, uint32_t segments)
{
    size_t size = (size_t)AMBRE_FIRST_DATA_OFFSET +
                  (size_t)segments * AMBRE_SEGMENT_SIZE;
    uint8_t *mem = (uint8_t *)malloc(size);

    if (mem == NULL) {
        AMBRE_CHECK_MSG(false, "cannot allocate %u bytes", (unsigned)size);
        return false;
    }
    ambre_test_disk_init(&fx->td, mem, (uint64_t)size);
    fx->cfg = policy_cfg(segments);
    AMBRE_CHECK_OK(ambre_journal_format(&fx->td.disk, &fx->cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&fx->td.disk, &fx->cfg, false, &fx->j));
    return true;
}

static inline void policy_destroy(policy_fx_t *fx)
{
    free(fx->td.mem);
    fx->td.mem = NULL;
}

/* Class A snapshot whose every point carries a pattern unique to (id, order),
 * so a read-back comparison proves the data survived untouched. */
static inline void policy_make_emerg(ambre_emerg_t *em, uint32_t id,
                                     uint8_t locked, uint32_t order)
{
    memset(em, 0, sizeof(*em));
    em->event_id = id;
    em->trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    em->locked = locked;
    em->event_number = 1u;
    em->ignition_cycle = 2u;
    em->t0_ts_us = POLICY_TS + (uint64_t)id * 1000u;
    em->trig_ts_us = em->t0_ts_us;
    em->crash_order = order;
    em->point_count = 3u;
    for (uint16_t k = 0u; k < 3u; k++) {
        em->points[k].speed_kmh_x4 = (uint16_t)(400u - id * 10u - k);
        em->points[k].throttle_pct = (uint8_t)(id * 3u + k);
        em->points[k].engine_rpm = (uint16_t)(900u + id * 7u + k);
        em->points[k].accel_long_mm_s2 = (int16_t)(id * 100 + k);
        em->points[k].rel_time_ms = (uint16_t)(5000u - k * 2000u);
    }
}

static inline void policy_make_seal(ambre_seal_t *sl, uint32_t id,
                                    uint8_t locked, uint8_t complete)
{
    memset(sl, 0, sizeof(*sl));
    sl->event_id = id;
    sl->file_complete = complete;
    sl->seal_cause = (uint8_t)AMBRE_SEAL_NORMAL;
    sl->locked = locked;
    sl->event_number = 1u;
    sl->max_dv_long_mm_s = 2500;
    sl->time_since_first_ms = id * 100u;
}

/* Trigger then seal one event; returns the library's status code. */
static inline int policy_event(ambre_journal_t *j, uint32_t id, uint8_t locked,
                               uint32_t order)
{
    ambre_emerg_t em;
    ambre_seal_t sl;
    int rc;

    policy_make_emerg(&em, id, locked, order);
    rc = ambre_journal_trigger(j, &em);
    if (rc != AMBRE_OK) {
        return rc;
    }
    policy_make_seal(&sl, id, locked, 1u);
    return ambre_journal_seal(j, &sl);
}

/* Append DATA records until the write pointer leaves the current segment. */
static inline int policy_fill_segment(ambre_journal_t *j)
{
    ambre_sample_t s;
    uint32_t seg = j->table.current_seg;
    int rc = AMBRE_ERR_STATE;

    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    for (uint32_t guard = 0u; guard <= AMBRE_BLOCKS_PER_SEGMENT; guard++) {
        if (j->table.current_seg != seg) {
            return AMBRE_OK;
        }
        rc = ambre_journal_append_data(j, (uint64_t)guard * 1000u, &s, 1u);
        if (rc != AMBRE_OK) {
            return rc;
        }
    }
    return rc;
}

/* The class A data of an event must read back byte for byte. */
static inline void policy_expect_intact(const ambre_journal_t *j, uint32_t id,
                                        uint8_t locked, uint32_t order)
{
    ambre_event_view_t view;
    ambre_emerg_t want;

    memset(&view, 0, sizeof(view));
    AMBRE_CHECK_OK(ambre_journal_read_event(j, id, &view));
    AMBRE_CHECK_MSG(view.have_emerg, "event %u: no EMERG record", id);
    if (!view.have_emerg) {
        return;
    }
    policy_make_emerg(&want, id, locked, order);
    AMBRE_CHECK_MEM_EQ(&view.emerg, &want, sizeof(want));
    AMBRE_CHECK(view.have_seal);
}

/* An event whose segment was recycled: its class A data is gone (STO-06). */
static inline void policy_expect_recycled(const ambre_journal_t *j, uint32_t id)
{
    ambre_event_view_t view;
    const ambre_ev_desc_t *d = ambre_table_find_event_const(&j->table, id);

    memset(&view, 0, sizeof(view));
    AMBRE_CHECK_OK(ambre_journal_read_event(j, id, &view));
    AMBRE_CHECK_MSG(!view.have_emerg, "event %u should have been recycled", id);
    if (d != NULL) {
        AMBRE_CHECK_EQ_U64(d->file_complete, 0u);
    }
}

/* ------------------------------------------------ operation-log metering -- */

/* Totals rebuilt from a simdisk operation log. `record_bytes` counts the
 * write path (payload blocks); `table_bytes` counts the two allocation-table
 * copies, which are format overhead and never capture volume (STO-04/STO-09). */
typedef struct {
    uint64_t record_bytes;
    uint64_t record_blocks;
    uint64_t table_bytes;
    uint64_t flushes;
} policy_meter_t;

static inline void policy_meter(const ambre_simdisk_t *sd, uint32_t from,
                                policy_meter_t *out)
{
    memset(out, 0, sizeof(*out));
    for (uint32_t i = from; i < ambre_simdisk_op_count(sd); i++) {
        const ambre_op_t *op = ambre_simdisk_op(sd, i);

        if (op == NULL) {
            continue;
        }
        if (op->kind == (uint8_t)AMBRE_OP_WRITE) {
            if (op->off < AMBRE_FIRST_DATA_OFFSET) {
                out->table_bytes += op->len;
            } else {
                out->record_bytes += op->len;
                out->record_blocks++;
            }
        } else if (op->kind == (uint8_t)AMBRE_OP_FLUSH) {
            out->flushes++;
        }
    }
}

/* Write one record as `chunks` consecutive blocks of one segment: the exact
 * mechanism a payload larger than one block relies on (GEL-01 says the whole
 * record leaves in a single contiguous write; the codec is the same). */
static inline int policy_write_chunked(ambre_disk_t *d, uint32_t seg,
                                       ambre_rec_type_t type, uint64_t seq,
                                       uint64_t session_id, uint64_t ts_us,
                                       const uint8_t *payload,
                                       uint32_t total_len, uint16_t chunks)
{
    ambre_block_t b;

    for (uint16_t ci = 0u; ci < chunks; ci++) {
        uint32_t off = (uint32_t)ci * AMBRE_BLOCK_PAYLOAD_MAX;
        uint32_t len = total_len - off;
        int rc;

        if (len > AMBRE_BLOCK_PAYLOAD_MAX) {
            len = AMBRE_BLOCK_PAYLOAD_MAX;
        }
        rc = ambre_block_build_chunk(&b, type, seq, session_id, ts_us, ci,
                                     chunks, payload + off, len, total_len);
        if (rc != AMBRE_OK) {
            return rc;
        }
        rc = ambre_block_write(d, ambre_block_offset(seg, ci), &b);
        if (rc != AMBRE_OK) {
            return rc;
        }
    }
    return AMBRE_OK;
}

/* --------------------------------------------------------- hand image ---- */

typedef struct {
    ambre_test_disk_t   td;
    ambre_alloc_table_t table;
    uint32_t            blk;
    uint64_t            seq;
    size_t              size;
} policy_image_t;

static inline bool policy_image_create(policy_image_t *im, uint32_t segments)
{
    size_t size = (size_t)AMBRE_FIRST_DATA_OFFSET +
                  (size_t)segments * AMBRE_SEGMENT_SIZE;
    uint8_t *mem = (uint8_t *)malloc(size);

    if (mem == NULL) {
        AMBRE_CHECK_MSG(false, "cannot allocate %u bytes", (unsigned)size);
        return false;
    }
    ambre_test_disk_init(&im->td, mem, (uint64_t)size);
    im->size = size;
    im->blk = 0u;
    im->seq = 1u;
    ambre_table_init(&im->table, POLICY_SESSION);
    /* Two commits: both copies end up valid with the same generation. */
    AMBRE_CHECK_OK(ambre_table_commit(&im->td.disk, &im->table));
    AMBRE_CHECK_OK(ambre_table_commit(&im->td.disk, &im->table));
    return true;
}

/* Commit the allocation table as it stands (two copies, valid CRC). */
static inline void policy_image_commit(policy_image_t *im)
{
    im->table.last_seq = im->seq - 1u;
    im->table.current_block = im->blk;
    AMBRE_CHECK_OK(ambre_table_commit(&im->td.disk, &im->table));
    AMBRE_CHECK_OK(ambre_table_commit(&im->td.disk, &im->table));
}

static inline void policy_image_put(policy_image_t *im, ambre_rec_type_t type,
                                    const void *payload, uint32_t len)
{
    ambre_block_t b;

    AMBRE_CHECK_OK(ambre_block_build(&b, type, im->seq++, POLICY_SESSION,
                                     POLICY_TS, payload, len));
    AMBRE_CHECK_OK(ambre_block_write(&im->td.disk,
                                     ambre_block_offset(0u, im->blk), &b));
    im->blk++;
}

static inline void policy_image_event(policy_image_t *im, uint32_t id,
                                      uint32_t start_blk, uint64_t seq_emerg,
                                      uint8_t state, uint8_t complete)
{
    ambre_ev_desc_t d;

    memset(&d, 0, sizeof(d));
    d.event_id = id;
    d.order = id;
    d.state = state;
    d.file_complete = complete;
    d.trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    d.start_block = start_blk;
    d.end_block = im->blk - 1u;
    d.trig_ts_us = POLICY_TS;
    d.seq_emerg = seq_emerg;
    d.seq_last = im->seq - 1u;
    AMBRE_CHECK_OK(ambre_table_add_event(&im->table, &d));
}

/* Compare the class A payload stored on disk with the injected snapshot. */
static inline void policy_check_emerg_at(const ambre_disk_t *d,
                                         const ambre_ev_desc_t *desc,
                                         uint64_t session_id, uint32_t id)
{
    static uint8_t payload[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    ambre_block_t head;
    ambre_emerg_t want;
    uint32_t len = 0u;

    AMBRE_CHECK_OK(ambre_read_record(d, desc->start_seg, desc->start_block,
                                     session_id, (int64_t)desc->seq_emerg, &head,
                                     payload, &len));
    AMBRE_CHECK_EQ_U64(len, sizeof(ambre_emerg_t));
    AMBRE_CHECK_EQ_U64(head.hdr.rec_type, AMBRE_REC_EMERG);
    policy_make_emerg(&want, id, 0u, id);
    AMBRE_CHECK_MEM_EQ(payload, &want, sizeof(want));
}

/* Recover a private copy of the image: no lost event, no phantom event. */
static inline void policy_check_scene(const policy_image_t *im,
                                      const uint32_t *ids, uint32_t count)
{
    ambre_test_disk_t probe;
    ambre_recovery_result_t res;
    uint8_t *copy = (uint8_t *)malloc(im->size);

    AMBRE_CHECK(copy != NULL);
    if (copy == NULL) {
        return;
    }
    /* ambre_test_disk_init() zeroes the buffer it is handed: copy afterwards. */
    ambre_test_disk_init(&probe, copy, (uint64_t)im->size);
    memcpy(copy, im->td.mem, im->size);
    AMBRE_CHECK_OK(ambre_recover(&probe.disk, &res));
    AMBRE_CHECK_EQ_U64(res.table.event_count, count);       /* no phantom */
    for (uint32_t i = 0u; i < count; i++) {
        const ambre_ev_desc_t *d =
            ambre_table_find_event_const(&res.table, ids[i]);

        AMBRE_CHECK_MSG(d != NULL, "event %u lost by recovery", ids[i]);
        if (d == NULL) {
            continue;
        }
        AMBRE_CHECK_EQ_U64(d->state, AMBRE_EV_SEALED);
        policy_check_emerg_at(&probe.disk, d, res.table.session_id, ids[i]);
    }
    free(copy);
}

/* US profile sizes (spec 12.7, 49 CFR 563: 20 s at 10 Hz, 20 data elements).
 * Derived, never guessed: 20 signals * 4-byte value * 200 points = 16 000
 * bytes of samples plus a 20-byte header. host/profiles/us_profile.py computes
 * the same numbers; the assertions below pin them on the C side. */
#define US_POINTS        200u   /* 20 s at 10 Hz                              */
#define US_SIGNALS       20u    /* data elements per snapshot (cahier 4.1)    */
#define US_VALUE_BYTES   4u     /* one int32 per signal and point             */
#define US_SAMPLES_BYTES (US_POINTS * US_SIGNALS * US_VALUE_BYTES)
#define US_HEADER_BYTES  20u    /* ids, flags, timestamps, point count        */
#define US_EMERG_BYTES   (US_HEADER_BYTES + US_SAMPLES_BYTES)
#define US_BLOCKS        ((US_EMERG_BYTES + AMBRE_BLOCK_PAYLOAD_MAX - 1u) / \
                          AMBRE_BLOCK_PAYLOAD_MAX)

/* Frozen struct layout at the US window: 32-byte header + 200 points of 20 B
 * (the ambre_a_point_t already packs every regulatory field). */
#define US_FROZEN_BYTES  (32u + US_POINTS * 20u)

/* Blocks a payload occupies, and whether it fits one block / one record. */
static inline uint32_t blocks_for(uint32_t bytes)
{
    return (bytes + AMBRE_BLOCK_PAYLOAD_MAX - 1u) / AMBRE_BLOCK_PAYLOAD_MAX;
}

static inline bool fits_one_block(uint32_t bytes)
{
    return bytes <= AMBRE_BLOCK_PAYLOAD_MAX;
}

static inline uint32_t largest_record_payload(void)
{
    return AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX;
}

static inline bool fits_one_record(uint32_t bytes)
{
    return bytes <= largest_record_payload();
}

#endif /* AMBRE_POLICY_FIXTURE_H */
