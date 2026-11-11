/*
 * test_policy.c — Week 4 proofs: multi-event capacity, overwrite policy,
 * refusal when full, locked-event protection, crash grouping, write metering,
 * the archive gate, multi-event recovery and the US profile record size.
 *
 * Requirements covered: STO-05, STO-06, STO-08, STO-09, SEC-03, DEC-09 and
 * GEL-06 (journal side). The US profile (spec 12.7) is proved next door, in
 * tests/test_policy_us.c, with the same fixture.
 *
 * Everything runs against the frozen library (fmt/) plus a disk: the
 * in-memory fake of tests/ambre_test_disk.h for the journal scenarios,
 * host/simdisk for the write-metering cross-check (its operation log is the
 * ground truth), and hand-built images for the recovery sweep. The shared
 * fixture lives in tests/ambre_policy_fixture.h — no logic is duplicated
 * here, and no existing test file is modified.
 */

#include "ambre_test.h"
#include "ambre_policy_fixture.h"
#include "ambre_journal.h"
#include "ambre_journal_internal.h"   /* pick_segment: the recycling path */
#include "ambre_trigger.h"
#include "ambre_simdisk.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- STO-05 ----- */

/* Six events pinned at the same time: the journal target of STO-05. */
static void test_sto05_six_frozen_events(void)
{
    policy_fx_t fx;

    if (!policy_create(&fx, 6u)) {
        return;
    }
    for (uint32_t id = 1u; id <= 6u; id++) {
        if (id > 1u) {
            AMBRE_CHECK_OK(policy_fill_segment(&fx.j));
        }
        AMBRE_CHECK_OK(policy_event(&fx.j, id, 0u, id));
    }
    for (uint32_t s = 0u; s < 6u; s++) {
        AMBRE_CHECK_EQ_U64(fx.j.table.seg_states[s], AMBRE_SEG_FROZEN);
    }
    AMBRE_CHECK_EQ_U64(fx.j.table.event_count, 6u);
    AMBRE_CHECK_EQ_U64(fx.j.stats.sealed_events, 6u);
    for (uint32_t id = 1u; id <= 6u; id++) {
        policy_expect_intact(&fx.j, id, 0u, id);
    }
    policy_destroy(&fx);
}

/* ------------------------------------------------------------- STO-06 ----- */

/* Four events, four segments, four reuses: eviction is oldest first, and only
 * the oldest one is lost on each rotation. */
static void test_sto06_eviction_is_oldest_first(void)
{
    policy_fx_t fx;

    if (!policy_create(&fx, 4u)) {
        return;
    }
    for (uint32_t id = 1u; id <= 4u; id++) {
        if (id > 1u) {
            AMBRE_CHECK_OK(policy_fill_segment(&fx.j));
        }
        AMBRE_CHECK_OK(policy_event(&fx.j, id, 0u, id));
        AMBRE_CHECK_EQ_U64(fx.j.table.events[id - 1u].start_seg, id - 1u);
    }
    for (uint32_t id = 5u; id <= 8u; id++) {
        AMBRE_CHECK_OK(policy_fill_segment(&fx.j));
        AMBRE_CHECK_OK(policy_event(&fx.j, id, 0u, id));
        AMBRE_CHECK_EQ_U64(fx.j.table.events[id - 1u].start_seg,
                           (id - 1u) % 4u);
        policy_expect_recycled(&fx.j, id - 4u);       /* the oldest one only */
        for (uint32_t keep = id - 3u; keep <= id; keep++) {
            policy_expect_intact(&fx.j, keep, 0u, keep);
        }
    }
    AMBRE_CHECK_EQ_U64(fx.j.stats.refused_no_space, 0u); /* recycling worked */
    policy_destroy(&fx);
}

/* ------------------------------------------------------------- STO-08 ----- */

/* Every slot locked: the write and the trigger are refused, the table says
 * "memory full" and the refusal is counted (STO-08). */
static void test_sto08_memory_full_refusal(void)
{
    policy_fx_t fx;
    uint32_t before;

    if (!policy_create(&fx, 2u)) {
        return;
    }
    AMBRE_CHECK_OK(policy_event(&fx.j, 1u, 1u, 1u));
    AMBRE_CHECK_OK(policy_fill_segment(&fx.j));
    AMBRE_CHECK_OK(policy_event(&fx.j, 2u, 1u, 2u));
    /* Leaving a segment through rotation re-marks it FROZEN, but a segment
     * holding a locked event is never FREE again: the descriptor flag
     * `locked` is the authority of the overwrite policy (FORMAT.md sec. 9). */
    AMBRE_CHECK(fx.j.table.seg_states[0] != (uint8_t)AMBRE_SEG_FREE);
    AMBRE_CHECK(fx.j.table.seg_states[1] != (uint8_t)AMBRE_SEG_FREE);
    AMBRE_CHECK_EQ_U64(fx.j.table.events[0].locked, 1u);
    AMBRE_CHECK_EQ_U64(fx.j.table.events[1].locked, 1u);
    AMBRE_CHECK_EQ_U64(fx.j.stats.refused_no_space, 0u);
    AMBRE_CHECK_EQ_U64(fx.j.table.flags & 0x02u, 0u);

    AMBRE_CHECK_EQ_I64(policy_fill_segment(&fx.j), AMBRE_ERR_FULL);
    AMBRE_CHECK_EQ_U64(fx.j.stats.refused_no_space, 1u);
    AMBRE_CHECK_EQ_U64(fx.j.table.flags & 0x02u, 0x02u);   /* memory full */

    before = fx.j.stats.refused_no_space;
    {
        ambre_emerg_t em;

        policy_make_emerg(&em, 3u, 0u, 3u);
        AMBRE_CHECK_EQ_I64(ambre_journal_trigger(&fx.j, &em), AMBRE_ERR_FULL);
    }
    AMBRE_CHECK_EQ_U64(fx.j.stats.refused_no_space, before + 1u);
    AMBRE_CHECK_EQ_U64(fx.j.stats.open_events, 0u);

    /* The two locked events are untouched by the refusals. */
    policy_expect_intact(&fx.j, 1u, 1u, 1u);
    policy_expect_intact(&fx.j, 2u, 1u, 2u);
    policy_destroy(&fx);
}

/* ------------------------------------------------------------- SEC-03 ----- */

/* No API path erases a locked event or reuses its segment: the locked segment
 * is snapshotted, then compared byte for byte once seven more events have
 * cycled through the rest of the journal (SEC-03, invariant I6). */
static void test_sec03_locked_event_is_protected(void)
{
    policy_fx_t fx;
    uint64_t off = ambre_block_offset(0u, 0u);
    size_t span = (size_t)AMBRE_SEGMENT_SIZE;
    uint8_t *snapshot;

    if (!policy_create(&fx, 3u)) {
        return;
    }
    snapshot = (uint8_t *)malloc(span);
    AMBRE_CHECK(snapshot != NULL);
    if (snapshot == NULL) {
        policy_destroy(&fx);
        return;
    }
    AMBRE_CHECK_OK(policy_event(&fx.j, 1u, 1u, 1u));
    AMBRE_CHECK_OK(policy_fill_segment(&fx.j));            /* leave segment 0 */
    AMBRE_CHECK(fx.j.table.seg_states[0] != (uint8_t)AMBRE_SEG_FREE);
    memcpy(snapshot, fx.td.mem + off, span);

    for (uint32_t id = 2u; id <= 8u; id++) {
        const ambre_ev_desc_t *d;

        AMBRE_CHECK_OK(policy_fill_segment(&fx.j));
        AMBRE_CHECK_OK(policy_event(&fx.j, id, 0u, id));
        d = ambre_table_find_event_const(&fx.j.table, id);
        AMBRE_CHECK_MSG(d != NULL && d->start_seg != 0u,
                        "event %u reused the locked segment", id);
        AMBRE_CHECK(fx.j.table.seg_states[0] != (uint8_t)AMBRE_SEG_FREE);
    }
    AMBRE_CHECK_MEM_EQ(fx.td.mem + off, snapshot, span);
    AMBRE_CHECK_EQ_U64(fx.j.table.events[0].locked, 1u);
    AMBRE_CHECK_EQ_U64(fx.j.table.events[0].file_complete, 1u);
    AMBRE_CHECK_EQ_U64(fx.j.table.events[0].state, AMBRE_EV_SEALED);
    policy_expect_intact(&fx.j, 1u, 1u, 1u);

    /* And the segment picker — the only path that can recycle a segment —
     * never returns the locked one, once per rotation the journal asked. */
    for (uint32_t call = 0u; call < 16u; call++) {
        uint32_t picked = 0u;

        if (ambre_journal_pick_segment(&fx.j, &picked) == AMBRE_OK) {
            AMBRE_CHECK_MSG(picked != 0u, "pick_segment returned the locked "
                                          "segment 0 on call %u", call);
        }
    }
    free(snapshot);
    policy_destroy(&fx);
}

/* ------------------------------------------------------------- DEC-09 ----- */

/* Fire an event through an external signal: A6 says T0 is exactly the signal
 * timestamp, so the grouping arithmetic can be checked to the millisecond. */
static void fire_external(ambre_trig_state_t *st, uint64_t ts_us,
                          ambre_trig_out_t *fired)
{
    ambre_trig_ext_t ext;

    memset(&ext, 0, sizeof(ext));
    ext.emergency_brake = true;         /* off-profile, never locks (DEC-11) */
    ambre_trig_push(st, ts_us, 0, 0, &ext, fired);
}

/* Push zero acceleration until the end of the event; returns the next slot. */
static uint64_t end_event(ambre_trig_state_t *st, uint64_t ts_us)
{
    for (uint32_t i = 0u; i < 400u; i++) {
        ambre_trig_out_t out;

        ambre_trig_push(st, ts_us, 0, 0, NULL, &out);
        if (out.event_end) {
            ambre_trig_event_closed(st, ts_us);
            return ts_us + 1000u;
        }
        ts_us += 1000u;
    }
    return ts_us;
}

/* Two T0 less than 5 s apart belong to one accident (number 2, elapsed time
 * recorded); beyond 5 s the group expires and a new accident starts at 1. */
static void test_dec09_crash_grouping(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_state_t st;
    ambre_trig_out_t first, second, third;
    uint64_t t0;

    ambre_trig_cfg_default(&cfg);
    ambre_trig_init(&st, &cfg);

    t0 = POLICY_TS;
    fire_external(&st, t0, &first);
    AMBRE_CHECK(first.fired);
    AMBRE_CHECK_EQ_U64(first.t0_ts_us, t0);
    AMBRE_CHECK_EQ_U64(first.event_number, 1u);
    AMBRE_CHECK_EQ_U64(first.time_since_first_ms, 0u);
    (void)end_event(&st, t0 + 100000u);

    fire_external(&st, t0 + 2000000u, &second);    /* exactly 2 s later */
    AMBRE_CHECK(second.fired);
    AMBRE_CHECK_EQ_U64(second.t0_ts_us, t0 + 2000000u);
    AMBRE_CHECK_EQ_U64(second.event_number, 2u);
    AMBRE_CHECK_EQ_U64(second.time_since_first_ms, 2000u);
    (void)end_event(&st, second.t0_ts_us + 100000u);

    fire_external(&st, t0 + 8000000u, &third);     /* beyond the 5 s window */
    AMBRE_CHECK(third.fired);
    AMBRE_CHECK_EQ_U64(third.event_number, 1u);
    AMBRE_CHECK_EQ_U64(third.time_since_first_ms, 0u);
}

/* The event number and the elapsed time are also recorded on disk. */
static void test_dec09_recorded_on_disk(void)
{
    policy_fx_t fx;
    ambre_emerg_t em;
    ambre_seal_t sl;
    ambre_event_view_t view;

    if (!policy_create(&fx, 2u)) {
        return;
    }
    policy_make_emerg(&em, 7u, 0u, 7u);
    em.event_number = 2u;                 /* second event of the accident */
    em.t0_ts_us = POLICY_TS + 2000000u;
    AMBRE_CHECK_OK(ambre_journal_trigger(&fx.j, &em));
    policy_make_seal(&sl, 7u, 0u, 1u);
    sl.event_number = 2u;
    sl.time_since_first_ms = 2000u;
    AMBRE_CHECK_OK(ambre_journal_seal(&fx.j, &sl));

    memset(&view, 0, sizeof(view));
    AMBRE_CHECK_OK(ambre_journal_read_event(&fx.j, 7u, &view));
    AMBRE_CHECK(view.have_seal);
    AMBRE_CHECK_EQ_U64(view.seal.event_number, 2u);
    AMBRE_CHECK_EQ_U64(view.seal.time_since_first_ms, 2000u);
    policy_destroy(&fx);
}

/* ------------------------------------------------------------- STO-09 ----- */

/* Written volume and flush count are exposed by counters and match the
 * operation log of the simulated disk, block for block (STO-09).
 *
 * Documented boundary: the counters meter the RECORD path (capture volume).
 * Allocation-table updates (STO-04, one sector per commit, always the older
 * copy) are counted separately, because they are format overhead, not capture
 * volume; the test accounts for both so nothing is hidden. */
static void test_sto09_write_metering(void)
{
    ambre_simdisk_t sd;
    ambre_journal_cfg_t cfg = policy_cfg(2u);
    ambre_journal_t j;
    ambre_sample_t s;
    ambre_emerg_t em;
    ambre_seal_t sl;
    policy_meter_t m;
    uint32_t base;

    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, AMBRE_FIRST_DATA_OFFSET +
                                       2ull * AMBRE_SEGMENT_SIZE));
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));
    base = ambre_simdisk_op_count(&sd);

    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    for (uint32_t i = 0u; i < 8u; i++) {
        AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u * i, &s, 1u));
    }
    AMBRE_CHECK_OK(ambre_journal_flush(&j, 5000u));
    policy_make_emerg(&em, 1u, 0u, 1u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    policy_make_seal(&sl, 1u, 0u, 1u);
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));

    policy_meter(&sd, base, &m);
    AMBRE_CHECK_EQ_U64(m.record_blocks, 10u);    /* 8 DATA, EMERG, SEAL */
    AMBRE_CHECK_EQ_U64(m.table_bytes, 2u * AMBRE_BLOCK_SIZE); /* 2 commits */
    AMBRE_CHECK(m.flushes > 0u);
    AMBRE_CHECK_EQ_U64(ambre_journal_stats(&j)->bytes_written, m.record_bytes);
    AMBRE_CHECK_EQ_U64(ambre_journal_stats(&j)->flush_count, m.flushes);
    AMBRE_CHECK_EQ_U64(ambre_journal_stats(&j)->blocks_written, m.record_blocks);
    AMBRE_CHECK_EQ_U64(ambre_journal_stats(&j)->bytes_written,
                       ambre_journal_stats(&j)->blocks_written * AMBRE_BLOCK_SIZE);
    AMBRE_CHECK(ambre_journal_stats(&j) == &j.stats);   /* exposed, not copied */
    ambre_simdisk_destroy(&sd);
}

/* ------------------------------------------------------------- GEL-06 ----- */

/* While an event is open the journal refuses a second one, and archiving is
 * read-only: a read-back writes not a single byte (GEL-06). */
static void test_gel06_no_archive_while_open(void)
{
    policy_fx_t fx;
    ambre_emerg_t em;
    ambre_event_view_t view;
    uint64_t writes;

    if (!policy_create(&fx, 2u)) {
        return;
    }
    policy_make_emerg(&em, 1u, 0u, 1u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&fx.j, &em));
    AMBRE_CHECK_EQ_U64(fx.j.stats.open_events, 1u);

    AMBRE_CHECK_EQ_I64(ambre_journal_trigger(&fx.j, &em), AMBRE_ERR_STATE);
    AMBRE_CHECK_EQ_U64(fx.j.stats.refused_busy, 1u);
    AMBRE_CHECK_EQ_U64(fx.j.stats.open_events, 1u);

    writes = fx.td.write_count;
    memset(&view, 0, sizeof(view));
    AMBRE_CHECK_OK(ambre_journal_read_event(&fx.j, 1u, &view));
    AMBRE_CHECK(view.have_emerg);
    AMBRE_CHECK_EQ_U64(fx.td.write_count, writes);   /* archive: no write */

    {
        ambre_seal_t sl;

        policy_make_seal(&sl, 1u, 0u, 1u);
        AMBRE_CHECK_OK(ambre_journal_seal(&fx.j, &sl));
    }
    AMBRE_CHECK_EQ_U64(fx.j.stats.open_events, 0u);   /* the gate reopens */
    policy_destroy(&fx);
}

/* ------------------------------------------- multi-event + power cut ------ */

/* Every intermediate state of a 3-event sequence survives recovery: an
 * acquired event is never lost, an event that was never triggered never
 * appears (REP-05, invariants I2 and I3). The image is built by hand so the
 * cut can happen before the EMERG block, after it, after the table commit and
 * after the seal, for each event. */
static void test_multievent_powercut_states(void)
{
    policy_image_t im;
    uint32_t ids[3] = { 1u, 2u, 3u };
    ambre_emerg_t em;
    ambre_seal_t sl;

    if (!policy_image_create(&im, 2u)) {
        return;
    }
    policy_check_scene(&im, ids, 0u);                 /* virgin partition */
    for (uint32_t id = 1u; id <= 3u; id++) {
        uint32_t start_blk = im.blk;
        uint64_t seq_emerg = im.seq;

        policy_make_emerg(&em, id, 0u, id);
        policy_image_put(&im, AMBRE_REC_EMERG, &em, (uint32_t)sizeof(em));
        policy_check_scene(&im, ids, id);              /* EMERG on disk only */
        policy_image_event(&im, id, start_blk, seq_emerg,
                           (uint8_t)AMBRE_EV_OPEN, 0u);
        policy_image_commit(&im);
        policy_check_scene(&im, ids, id);              /* acquired */
        policy_make_seal(&sl, id, 0u, 1u);
        policy_image_put(&im, AMBRE_REC_SEAL, &sl, (uint32_t)sizeof(sl));
        policy_image_event(&im, id, start_blk, seq_emerg,
                           (uint8_t)AMBRE_EV_SEALED, 1u);
        policy_check_scene(&im, ids, id);              /* SEAL, not committed */
        policy_image_commit(&im);
        policy_check_scene(&im, ids, id);              /* sealed and committed */
    }
    free(im.td.mem);
}

/* ---------------------------------------------------------------- cases --- */

static const ambre_test_case_t cases[] = {
    { "sto05_six_frozen_events",            test_sto05_six_frozen_events },
    { "sto06_eviction_is_oldest_first",     test_sto06_eviction_is_oldest_first },
    { "sto08_memory_full_refusal",          test_sto08_memory_full_refusal },
    { "sec03_locked_event_is_protected",    test_sec03_locked_event_is_protected },
    { "dec09_crash_grouping",               test_dec09_crash_grouping },
    { "dec09_recorded_on_disk",             test_dec09_recorded_on_disk },
    { "sto09_write_metering",               test_sto09_write_metering },
    { "gel06_no_archive_while_open",        test_gel06_no_archive_while_open },
    { "multievent_powercut_states",         test_multievent_powercut_states },
};

AMBRE_TEST_MAIN(cases)
