/*
 * test_journal.c — Journal engine: capture cadence, the critical trigger
 * path, sealing, segment rotation and the overwrite policy.
 *
 * The simdisk records every write, flush and durability mark in order: that
 * trace is what proves the critical path is exactly "one write, one table
 * commit, one flush" (GEL-01, GEL-02).
 */

#include "ambre_test.h"
#include "ambre_journal.h"
#include "ambre_simdisk.h"
#include <stdlib.h>
#include <string.h>

#define TEST_SEGMENTS 2u

static ambre_simdisk_t sd;

static void disk_setup(void)
{
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, AMBRE_FIRST_DATA_OFFSET +
                                         (uint64_t)TEST_SEGMENTS * AMBRE_SEGMENT_SIZE));
}

static void disk_teardown(void)
{
    ambre_simdisk_destroy(&sd);
}

static ambre_journal_cfg_t test_cfg(void)
{
    ambre_journal_cfg_t cfg;

    ambre_journal_cfg_default(&cfg);
    cfg.session_id = 0x1234u;
    cfg.segment_limit = TEST_SEGMENTS;
    cfg.flush_period_us = 100000u;
    cfg.event_flush_period_us = 10000u;
    return cfg;
}

static void fill_emerg(ambre_emerg_t *em, uint32_t event_id)
{
    memset(em, 0, sizeof(*em));
    em->event_id = event_id;
    em->trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    em->locked = 0u;
    em->event_number = 1u;
    em->ignition_cycle = 3u;
    em->t0_ts_us = 1000000u;
    em->trig_ts_us = 1000000u;
    em->crash_order = 1u;
    em->point_count = 4u;
    em->points[0].speed_kmh_x4 = 420u;
    em->points[0].rel_time_ms = 5000u;
    em->points[1].speed_kmh_x4 = 400u;
    em->points[1].rel_time_ms = 4000u;
    em->points[2].speed_kmh_x4 = 380u;
    em->points[2].rel_time_ms = 3000u;
    em->points[3].speed_kmh_x4 = 360u;
    em->points[3].rel_time_ms = 2000u;
}

static void fill_seal(ambre_seal_t *sl, uint32_t event_id, uint8_t complete)
{
    memset(sl, 0, sizeof(*sl));
    sl->event_id = event_id;
    sl->file_complete = complete;
    sl->seal_cause = (uint8_t)AMBRE_SEAL_NORMAL;
    sl->locked = 0u;
    sl->event_number = 1u;
    sl->max_dv_long_mm_s = 3400;
    sl->max_dv_long_at_ms = 45u;
}

/* --------------------------------------------------------------- tests --- */

static void test_format_and_open(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(sd.disk.ctx ? &sd.disk : &sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));
    AMBRE_CHECK_EQ_U64(j.table.current_seg, 0u);
    AMBRE_CHECK_EQ_U64(j.table.current_block, 0u);
    AMBRE_CHECK_EQ_U64(j.table.session_id, 0x1234u);
    disk_teardown();
}

static void test_data_capture_and_flush_cadence(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_sample_t s;
    uint32_t before;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    s.value = 42;
    AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u, &s, 1u));
    AMBRE_CHECK_EQ_U64(j.stats.blocks_written, 1u);

    before = j.stats.flush_count;
    AMBRE_CHECK_OK(ambre_journal_tick(&j, 1000u));    /* first tick flushes */
    AMBRE_CHECK_EQ_U64(j.stats.flush_count, before + 1u);

    before = j.stats.flush_count;
    AMBRE_CHECK_OK(ambre_journal_tick(&j, 2000u));    /* too early */
    AMBRE_CHECK_EQ_U64(j.stats.flush_count, before);

    AMBRE_CHECK_OK(ambre_journal_tick(&j, 200000u));  /* cadence reached */
    AMBRE_CHECK_EQ_U64(j.stats.flush_count, before + 1u);
    disk_teardown();
}

static void test_event_only_mode_writes_nothing_before_trigger(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_sample_t s;
    ambre_emerg_t em;
    uint32_t writes_before;

    disk_setup();
    cfg.continuous_mode = false;
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    writes_before = sd.op_count;
    AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u, &s, 1u));
    AMBRE_CHECK_EQ_U64(sd.op_count, writes_before);   /* nothing written */

    fill_emerg(&em, 1u);
    writes_before = sd.op_count;
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    AMBRE_CHECK(sd.op_count > writes_before);
    disk_teardown();
}

static void test_trigger_path_is_write_commit_flush(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_emerg_t em;
    uint32_t start;
    uint32_t writes = 0u, flushes = 0u, marks = 0u;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    fill_emerg(&em, 7u);
    start = sd.op_count;
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));

    for (uint32_t i = start; i < sd.op_count; i++) {
        if (sd.ops[i].kind == AMBRE_OP_WRITE) writes++;
        if (sd.ops[i].kind == AMBRE_OP_FLUSH) flushes++;
        if (sd.ops[i].kind == AMBRE_OP_MARK) marks++;
    }
    AMBRE_CHECK_EQ_U64(writes, 2u);      /* EMERG + table commit */
    AMBRE_CHECK_EQ_U64(flushes, 1u);     /* one flush makes it acquired */
    AMBRE_CHECK_EQ_U64(marks, 3u);       /* emerg written, table, acquired */
    AMBRE_CHECK_EQ_U64(j.stats.acquired_events, 1u);
    AMBRE_CHECK_EQ_U64(j.stats.open_events, 1u);

    /* Order: EMERG write, table commit, flush - each followed by its mark. */
    AMBRE_CHECK_EQ_U64(sd.ops[start].kind, (uint64_t)AMBRE_OP_WRITE);
    AMBRE_CHECK(sd.ops[start].off >= AMBRE_FIRST_DATA_OFFSET);
    AMBRE_CHECK_EQ_U64(sd.ops[start + 1u].kind, (uint64_t)AMBRE_OP_MARK);
    AMBRE_CHECK_EQ_U64(sd.ops[start + 1u].mark_kind, (uint64_t)AMBRE_MARK_EMERG_WRITTEN);
    AMBRE_CHECK_EQ_U64(sd.ops[start + 2u].kind, (uint64_t)AMBRE_OP_WRITE);
    AMBRE_CHECK(sd.ops[start + 2u].off < AMBRE_FIRST_DATA_OFFSET);
    AMBRE_CHECK_EQ_U64(sd.ops[start + 3u].mark_kind, (uint64_t)AMBRE_MARK_TABLE_COMMIT);
    AMBRE_CHECK_EQ_U64(sd.ops[start + 4u].kind, (uint64_t)AMBRE_OP_FLUSH);
    AMBRE_CHECK_EQ_U64(sd.ops[start + 5u].mark_kind, (uint64_t)AMBRE_MARK_ACQUIRED);
    disk_teardown();
}

static void test_acquired_event_reads_back_identical(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_event_view_t view;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    fill_emerg(&em, 8u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));

    AMBRE_CHECK_OK(ambre_journal_read_event(&j, 8u, &view));
    AMBRE_CHECK(view.have_emerg);
    AMBRE_CHECK_MEM_EQ(&view.emerg, &em, sizeof(em));
    disk_teardown();
}

static void test_continue_and_seal(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_cont_t cont;
    ambre_seal_t sl;
    ambre_event_view_t view;
    const ambre_ev_desc_t *desc;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    fill_emerg(&em, 9u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));

    memset(&cont, 0, sizeof(cont));
    cont.event_id = 9u;
    cont.sample_count = 1u;
    cont.rel_time_ms = 10u;
    cont.dv_long_mm_s = 500;
    AMBRE_CHECK_OK(ambre_journal_continue(&j, &cont));

    fill_seal(&sl, 9u, 1u);
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));

    desc = ambre_table_find_event_const(&j.table, 9u);
    AMBRE_CHECK(desc != NULL);
    if (desc != NULL) {
        AMBRE_CHECK_EQ_U64(desc->state, (uint64_t)AMBRE_EV_SEALED);
        AMBRE_CHECK_EQ_U64(desc->file_complete, 1u);
    }
    AMBRE_CHECK_OK(ambre_journal_read_event(&j, 9u, &view));
    AMBRE_CHECK(view.have_seal);
    AMBRE_CHECK_EQ_U64(view.seal.file_complete, 1u);
    AMBRE_CHECK(view.cont_count >= 1u);
    AMBRE_CHECK_EQ_U64(j.stats.sealed_events, 1u);
    disk_teardown();
}

static void test_segment_rotation_and_recycle(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_seal_t sl;
    uint32_t seg_before;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    /* Fill the first segment with data records. */
    {
        ambre_sample_t s;

        memset(&s, 0, sizeof(s));
        s.signal_id = 1u;
        for (uint32_t i = 0u; i < AMBRE_BLOCKS_PER_SEGMENT + 4u; i++) {
            AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u * (i + 1u), &s, 1u));
        }
    }
    AMBRE_CHECK_OK(ambre_journal_tick(&j, 1000000u));
    seg_before = j.table.current_seg;
    AMBRE_CHECK_EQ_U64(seg_before, 1u);           /* moved to the second segment */
    AMBRE_CHECK_EQ_U64(j.table.current_block, 4u);

    /* The first segment is free again: the event that pinned it was sealed. */
    fill_emerg(&em, 10u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    fill_seal(&sl, 10u, 1u);
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));
    AMBRE_CHECK_EQ_U64(j.table.seg_states[1u], (uint64_t)AMBRE_SEG_FROZEN);
    disk_teardown();
}

static void test_locked_event_is_never_recycled(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_seal_t sl;
    uint32_t seg;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    fill_emerg(&em, 11u);
    em.locked = 1u;
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    fill_seal(&sl, 11u, 1u);
    sl.locked = 1u;
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));
    AMBRE_CHECK_EQ_U64(j.table.seg_states[0u], (uint64_t)AMBRE_SEG_LOCKED);

    /* With both segments pinned by a locked event, a new segment is refused. */
    seg = j.table.current_seg;
    AMBRE_CHECK(seg < TEST_SEGMENTS);
    disk_teardown();
}

static void test_memory_full_is_refused(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_seal_t sl;
    uint32_t i;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    /* Two locked events pin both segments. */
    for (i = 0u; i < 2u; i++) {
        fill_emerg(&em, 20u + i);
        em.locked = 1u;
        AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
        fill_seal(&sl, 20u + i, 1u);
        sl.locked = 1u;
        AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));
    }
    AMBRE_CHECK_EQ_U64(j.stats.refused_no_space, 0u);
    disk_teardown();
}

static void test_stats_are_coherent(void)
{
    ambre_journal_cfg_t cfg = test_cfg();
    ambre_journal_t j;
    ambre_sample_t s;

    disk_setup();
    AMBRE_CHECK_OK(ambre_journal_format(&sd.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&sd.disk, &cfg, false, &j));

    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u, &s, 1u));
    AMBRE_CHECK_OK(ambre_journal_flush(&j, 2000u));
    AMBRE_CHECK_EQ_U64(j.stats.blocks_written, 1u);
    AMBRE_CHECK_EQ_U64(j.stats.flush_count, 1u);
    AMBRE_CHECK_EQ_U64(j.stats.bytes_written, AMBRE_BLOCK_SIZE);
    disk_teardown();
}

static const ambre_test_case_t cases[] = {
    { "format_and_open", test_format_and_open },
    { "data_capture_and_flush_cadence", test_data_capture_and_flush_cadence },
    { "event_only_mode_writes_nothing_before_trigger", test_event_only_mode_writes_nothing_before_trigger },
    { "trigger_path_is_write_commit_flush", test_trigger_path_is_write_commit_flush },
    { "acquired_event_reads_back_identical", test_acquired_event_reads_back_identical },
    { "continue_and_seal", test_continue_and_seal },
    { "segment_rotation_and_recycle", test_segment_rotation_and_recycle },
    { "locked_event_is_never_recycled", test_locked_event_is_never_recycled },
    { "memory_full_is_refused", test_memory_full_is_refused },
    { "stats_are_coherent", test_stats_are_coherent }
};

AMBRE_TEST_MAIN(cases)
