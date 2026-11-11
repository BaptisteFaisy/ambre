/*
 * ambre_gap_disk.h - Shared fixture of the coverage-gap suites (spec 12.10).
 *
 * An in-memory disk (the frozen tests/ambre_test_disk.h) wrapped with fault
 * injection: reads, writes and flushes can be made to fail, either everywhere
 * or only on the two allocation-table copies. That is what the error branches
 * of fmt/ need, and no functional suite exercises them.
 *
 * Header-only and static inline so each test binary links without extra
 * objects (the same pattern as ambre_test_disk.h). The three gap suites share
 * this one implementation: no duplicated fixture logic.
 */

#ifndef AMBRE_GAP_DISK_H
#define AMBRE_GAP_DISK_H

#include "ambre_fmt.h"
#include "ambre_test.h"
#include "ambre_test_disk.h"
#include <stdlib.h>

typedef struct {
    ambre_test_disk_t base;      /* memory, size and the base fail flags */
    bool         fail_flush;     /* flush() returns an error             */
    bool         fail_table_write; /* only the two table copies fail     */
    bool         fail_write;     /* every write fails                    */
    ambre_disk_t disk;           /* the vtable handed to the library     */
} gap_disk_t;

static inline int gap_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    return ambre_test_disk_read(&((gap_disk_t *)ctx)->base, off, buf, len);
}

static inline int gap_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    gap_disk_t *gd = (gap_disk_t *)ctx;

    if (gd->fail_write) {
        return -1;
    }
    if (gd->fail_table_write && off < AMBRE_FIRST_DATA_OFFSET) {
        return -1;
    }
    return ambre_test_disk_write(&gd->base, off, buf, len);
}

static inline int gap_flush(void *ctx)
{
    return ((gap_disk_t *)ctx)->fail_flush ? -1 : 0;
}

static inline void gap_clear_faults(gap_disk_t *gd)
{
    gd->fail_flush = false;
    gd->fail_table_write = false;
    gd->fail_write = false;
}

/* (Re)allocate the backing store and install the fault-injecting vtable.
 * Returns false when the allocation failed (the caller must then stop). */
static inline bool gap_setup(gap_disk_t *gd, uint8_t **mem, size_t *mem_size,
                             size_t size)
{
    if (*mem_size < size) {
        free(*mem);
        *mem = (uint8_t *)malloc(size);
        *mem_size = (*mem != NULL) ? size : 0u;
    }
    if (*mem == NULL) {
        AMBRE_CHECK_MSG(false, "cannot allocate %u bytes", (unsigned)size);
        return false;
    }
    ambre_test_disk_init(&gd->base, *mem, (uint64_t)size);
    gap_clear_faults(gd);
    gd->disk = gd->base.disk;
    gd->disk.ctx = gd;
    gd->disk.read = gap_read;
    gd->disk.write = gap_write;
    gd->disk.flush = gap_flush;
    return true;
}

static inline void gap_teardown(uint8_t **mem, size_t *mem_size)
{
    free(*mem);
    *mem = NULL;
    *mem_size = 0u;
}

/* Recompute both CRCs after hand-editing a block header. The library refuses
 * to build an invalid block, so a forged one is the only way to reach the
 * torn-record and bad-CRC branches. */
static inline void gap_reseal(ambre_block_t *b)
{
    b->hdr.payload_crc = ambre_crc32c(0u, b->payload, b->hdr.payload_len);
    b->block_crc = ambre_crc32c(0u, b, AMBRE_BLOCK_SIZE - AMBRE_BLOCK_CRC_SIZE);
}

/* Write one (possibly forged) block at (seg, blk). */
static inline int gap_write_block(gap_disk_t *gd, uint32_t seg, uint32_t blk,
                                  const ambre_block_t *b)
{
    return ambre_block_write(&gd->disk, ambre_block_offset(seg, blk), b);
}

/* Build and write one valid single-block record; returns its seq. */
static inline uint64_t gap_put_record(gap_disk_t *gd, uint32_t seg, uint32_t blk,
                                      ambre_rec_type_t type, uint64_t seq,
                                      uint64_t session, uint64_t ts_us,
                                      const void *payload, uint32_t len)
{
    ambre_block_t b;

    AMBRE_CHECK_OK(ambre_block_build(&b, type, seq, session, ts_us, payload, len));
    AMBRE_CHECK_OK(gap_write_block(gd, seg, blk, &b));
    return seq;
}

/* Put a hand-built table on disk through the public commit path, so both
 * copies carry a valid CRC and a monotonic generation (STO-04). Committing
 * twice writes copy A then copy B, exactly like two journal updates would. */
static inline void gap_put_table(gap_disk_t *gd, ambre_alloc_table_t *t)
{
    AMBRE_CHECK_OK(ambre_table_commit(&gd->disk, t));
    AMBRE_CHECK_OK(ambre_table_commit(&gd->disk, t));
}

/* Fill `count` blocks of a segment with valid single-block DATA records, so
 * the scanners see a complete chain (seq 0..count-1 of one session). */
static inline void gap_fill_segment(gap_disk_t *gd, uint32_t seg, uint32_t count,
                                    uint64_t session)
{
    ambre_block_t b;
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    for (uint32_t blk = 0u; blk < count; blk++) {
        AMBRE_CHECK_OK(ambre_block_build(&b, AMBRE_REC_DATA, (uint64_t)blk,
                                         session, 1000u + blk, data,
                                         sizeof(data)));
        AMBRE_CHECK_OK(gap_write_block(gd, seg, blk, &b));
    }
}

/* Minimal but valid class A payloads, shared by the gap suites. */
static inline void gap_make_emerg(ambre_emerg_t *em, uint32_t event_id,
                                  uint8_t locked)
{
    memset(em, 0, sizeof(*em));
    em->event_id = event_id;
    em->trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    em->locked = locked;
    em->event_number = 1u;
    em->ignition_cycle = 3u;
    em->t0_ts_us = 500000u;
    em->trig_ts_us = 500000u;
    em->crash_order = event_id;
    em->point_count = 1u;
    em->points[0].speed_kmh_x4 = 200u;
    em->points[0].rel_time_ms = 5000u;
}

static inline void gap_make_cont(ambre_cont_t *cont, uint32_t event_id,
                                 uint16_t rel_time_ms)
{
    memset(cont, 0, sizeof(*cont));
    cont->event_id = event_id;
    cont->sample_count = 1u;
    cont->rel_time_ms = rel_time_ms;
    cont->dv_long_mm_s = 1000;
    cont->points[0].speed_kmh_x4 = 180u;
}

static inline void gap_make_seal(ambre_seal_t *seal, uint32_t event_id,
                                 uint8_t complete, uint8_t locked)
{
    memset(seal, 0, sizeof(*seal));
    seal->event_id = event_id;
    seal->file_complete = complete;
    seal->seal_cause = (uint8_t)AMBRE_SEAL_NORMAL;
    seal->locked = locked;
    seal->event_number = 1u;
    seal->max_dv_long_mm_s = 2500;
    seal->ignition_cycle_at_read = 3u;
}

static inline void gap_make_desc(ambre_ev_desc_t *ev, uint32_t event_id,
                                 uint32_t seg, uint8_t state, uint8_t locked)
{
    memset(ev, 0, sizeof(*ev));
    ev->event_id = event_id;
    ev->order = event_id;
    ev->state = state;
    ev->locked = locked;
    ev->start_seg = seg;
    ev->start_block = 0u;
    ev->end_seg = seg;
    ev->end_block = 0u;
    ev->trig_ts_us = 500000u;
    ev->seq_emerg = 0u;
    ev->seq_last = 0u;
}

#endif /* AMBRE_GAP_DISK_H */
