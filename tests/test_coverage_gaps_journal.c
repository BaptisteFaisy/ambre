/*
 * test_coverage_gaps_journal.c - Branch coverage of the journal engine.
 *
 * Spec 12.10 asks for more than 95% of the lines of fmt/ covered. This file
 * reaches the branches of ambre_journal_core.c and ambre_journal_event.c that
 * the functional suite does not: the I/O failures of every step (write, table
 * commit, flush), the state refusals (GEL-06), the segment recycling and the
 * "memory full" refusal (STO-08).
 *
 * No existing test is modified or weakened: these are extra scenarios only.
 */

#include "ambre_test.h"
#include "ambre_gap_disk.h"
#include "ambre_journal_internal.h"

#define GAP_SESSION 0x9C9Cu
#define ONE_SEG_DISK (AMBRE_FIRST_DATA_OFFSET + (size_t)AMBRE_SEGMENT_SIZE)
#define TWO_SEG_DISK (AMBRE_FIRST_DATA_OFFSET + 2u * (size_t)AMBRE_SEGMENT_SIZE)
#define FULL_DISK    ((size_t)AMBRE_JOURNAL_BYTES)

static gap_disk_t g;
static uint8_t *g_mem;
static size_t g_mem_size;

static bool setup(size_t size)
{
    return gap_setup(&g, &g_mem, &g_mem_size, size);
}

static void teardown(void)
{
    gap_teardown(&g_mem, &g_mem_size);
}

static ambre_journal_cfg_t cfg_of(uint32_t limit, bool continuous)
{
    ambre_journal_cfg_t cfg;

    ambre_journal_cfg_default(&cfg);
    cfg.session_id = GAP_SESSION;
    cfg.segment_limit = limit;
    cfg.continuous_mode = continuous;
    return cfg;
}

/* ------------------------------------------------------- argument checks -- */

static void test_lifecycle_rejects_null_arguments(void)
{
    ambre_journal_cfg_t cfg = cfg_of(1u, true);
    ambre_journal_t j;
    ambre_event_view_t view;
    ambre_emerg_t em;
    ambre_cont_t cont;
    ambre_seal_t sl;

    gap_make_emerg(&em, 1u, 0u);
    memset(&cont, 0, sizeof(cont));
    gap_make_seal(&sl, 1u, 1u, 0u);

    AMBRE_CHECK_EQ_I64(ambre_journal_format(NULL, &cfg), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_format(&g.disk, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_open(NULL, &cfg, false, &j), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_open(&g.disk, NULL, false, &j), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_open(&g.disk, &cfg, false, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_append_data(NULL, 1u, NULL, 1u), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_append_gap(NULL, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_flush(NULL, 1u), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_tick(NULL, 1u), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_trigger(NULL, &em), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_trigger(&j, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_continue(NULL, &cont), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_seal(NULL, &sl), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_read_event(NULL, 1u, &view), AMBRE_ERR_PARAM);
    AMBRE_CHECK(ambre_journal_table(NULL) == NULL);
    AMBRE_CHECK(ambre_journal_stats(NULL) == NULL);
    AMBRE_CHECK(ambre_journal_recovery(NULL) == NULL);
}

/* ------------------------------------------------------------- lifecycle -- */

static void test_format_clamps_an_oversized_segment_limit(void)
{
    ambre_journal_cfg_t cfg = cfg_of(AMBRE_SEGMENT_COUNT + 76u, true);
    ambre_journal_t j;

    if (!setup(FULL_DISK)) {
        return;
    }
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    AMBRE_CHECK_EQ_U64(j.table.current_seg, 0u);
    teardown();
}

static void test_format_reports_write_errors(void)
{
    ambre_journal_cfg_t cfg = cfg_of(1u, true);

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    g.fail_write = true;                /* the segments cannot be zeroed */
    AMBRE_CHECK_EQ_I64(ambre_journal_format(&g.disk, &cfg), AMBRE_ERR_IO);
    g.fail_write = false;

    g.fail_table_write = true;          /* the two table copies cannot be written */
    AMBRE_CHECK_EQ_I64(ambre_journal_format(&g.disk, &cfg), AMBRE_ERR_IO);
    g.fail_table_write = false;
    teardown();
}

static void test_open_runs_recovery_or_formats_a_broken_partition(void)
{
    ambre_journal_cfg_t cfg = cfg_of(2u, true);
    ambre_journal_t j;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, true, &j));
    AMBRE_CHECK(j.recovered);
    AMBRE_CHECK(ambre_journal_recovery(&j) != NULL);
    AMBRE_CHECK(ambre_journal_table(&j) != NULL);
    AMBRE_CHECK(ambre_journal_stats(&j) != NULL);
    teardown();

    /* A partition whose two table copies are unusable: opening formats it. */
    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    memset(g_mem, 0xABu, AMBRE_FIRST_DATA_OFFSET);
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    AMBRE_CHECK(!j.recovered);
    AMBRE_CHECK_EQ_U64(j.table.current_seg, 0u);
    teardown();
}

/* --------------------------------------------------------------- capture -- */

static void test_append_data_and_gap_paths(void)
{
    ambre_journal_cfg_t cfg = cfg_of(2u, true);
    ambre_journal_t j;
    ambre_sample_t sample;
    ambre_gap_t gap;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    memset(&sample, 0, sizeof(sample));
    sample.signal_id = 1u;
    sample.value = 1234;
    memset(&gap, 0, sizeof(gap));
    gap.start_ts_us = 100u;
    gap.end_ts_us = 200u;
    gap.reason = 1u;

    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u, &sample, 1u));
    AMBRE_CHECK_OK(ambre_journal_append_gap(&j, &gap));
    AMBRE_CHECK_EQ_I64(ambre_journal_append_data(&j, 2000u, NULL, 1u), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_append_data(&j, 2000u, &sample, 0u), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_journal_append_gap(&j, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_U64(j.stats.blocks_written, 2u);

    g.fail_write = true;                /* the record cannot leave */
    AMBRE_CHECK_EQ_I64(ambre_journal_append_data(&j, 3000u, &sample, 1u), AMBRE_ERR_IO);
    AMBRE_CHECK_EQ_I64(ambre_journal_append_gap(&j, &gap), AMBRE_ERR_IO);
    g.fail_write = false;
    teardown();
}

static void test_write_record_geometry(void)
{
    ambre_journal_cfg_t cfg = cfg_of(2u, true);
    ambre_journal_t j;
    uint8_t big[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX + 8u];
    uint64_t seq = 0u;
    uint32_t blocks = 0u;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    memset(big, 0x5Au, sizeof(big));
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));

    /* Bigger than one record: refused before anything is staged. */
    AMBRE_CHECK_EQ_I64(ambre_journal_write_record(&j, (uint8_t)AMBRE_REC_DATA,
                                                  1u, big, (uint32_t)sizeof(big),
                                                  &seq, &blocks),
                       AMBRE_ERR_TOO_BIG);
    /* Two chunks: the staging loop splits the payload. */
    AMBRE_CHECK_OK(ambre_journal_write_record(&j, (uint8_t)AMBRE_REC_DATA, 2u,
                                              big, AMBRE_BLOCK_PAYLOAD_MAX + 10u,
                                              &seq, &blocks));
    AMBRE_CHECK_EQ_U64(blocks, 2u);
    ambre_journal_advance(&j, blocks, seq);
    /* An empty payload still occupies one block. */
    AMBRE_CHECK_OK(ambre_journal_write_record(&j, (uint8_t)AMBRE_REC_DATA, 3u,
                                              big, 0u, &seq, &blocks));
    AMBRE_CHECK_EQ_U64(blocks, 1u);
    ambre_journal_advance(&j, blocks, seq);
    AMBRE_CHECK_EQ_U64(seq, j.next_seq - 1u);
    AMBRE_CHECK_EQ_U64(j.table.current_block, 3u);

    g.fail_write = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_write_record(&j, (uint8_t)AMBRE_REC_DATA,
                                                  4u, big, 8u, &seq, &blocks),
                       AMBRE_ERR_IO);
    g.fail_write = false;
    teardown();
}

static void test_event_only_mode_writes_nothing(void)
{
    ambre_journal_cfg_t cfg = cfg_of(1u, false);
    ambre_journal_t j;
    ambre_sample_t sample;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    memset(&sample, 0, sizeof(sample));
    sample.signal_id = 2u;
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u, &sample, 1u));
    AMBRE_CHECK_EQ_U64(j.stats.blocks_written, 0u);
    AMBRE_CHECK_OK(ambre_journal_tick(&j, 5000u));   /* no flush in this mode */
    AMBRE_CHECK_EQ_U64(j.stats.flush_count, 0u);
    teardown();
}

/* ----------------------------------------------------------------- flush -- */

static void test_flush_reports_io_errors(void)
{
    ambre_journal_cfg_t cfg = cfg_of(1u, true);
    ambre_journal_t j;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    g.fail_flush = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_flush(&j, 1000u), AMBRE_ERR_IO);
    g.fail_flush = false;
    AMBRE_CHECK_OK(ambre_journal_flush(&j, 2000u));
    AMBRE_CHECK_EQ_U64(j.stats.flush_count, 1u);
    AMBRE_CHECK_EQ_U64(j.last_flush_ts_us, 2000u);
    teardown();
}

static void test_tick_refuses_when_no_segment_is_reclaimable(void)
{
    ambre_journal_cfg_t cfg = cfg_of(1u, true);
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_seal_t sl;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));

    /* One locked event pins the only segment of the journal. */
    gap_make_emerg(&em, 1u, 1u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    gap_make_seal(&sl, 1u, 1u, 1u);
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));
    AMBRE_CHECK_EQ_U64(j.table.seg_states[0], (uint64_t)AMBRE_SEG_LOCKED);

    /* The segment is full: the rotation finds nothing to recycle (STO-08). */
    j.table.current_block = AMBRE_BLOCKS_PER_SEGMENT;
    AMBRE_CHECK_EQ_I64(ambre_journal_tick(&j, 9000u), AMBRE_ERR_FULL);
    AMBRE_CHECK_EQ_U64(j.stats.refused_no_space, 1u);
    AMBRE_CHECK((j.table.flags & 0x02u) != 0u);
    teardown();
}

/* -------------------------------------------------------------- rotation -- */

static void test_rotation_recycles_a_sealed_segment(void)
{
    ambre_journal_cfg_t cfg = cfg_of(2u, true);
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_seal_t sl;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));

    /* Event 1 in segment 0, sealed and unlocked. */
    gap_make_emerg(&em, 1u, 0u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    gap_make_seal(&sl, 1u, 1u, 0u);
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));

    /* Segment 0 is full: the rotation moves to the free segment 1 and freezes
     * the segment that still holds event 1. */
    j.table.current_block = AMBRE_BLOCKS_PER_SEGMENT;
    AMBRE_CHECK_OK(ambre_journal_tick(&j, 10000u));
    AMBRE_CHECK_EQ_U64(j.table.current_seg, 1u);
    AMBRE_CHECK_EQ_U64(j.table.seg_states[0], (uint64_t)AMBRE_SEG_FROZEN);

    /* Event 2 in segment 1, sealed as well: both segments are reclaimable. */
    gap_make_emerg(&em, 2u, 0u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    gap_make_seal(&sl, 2u, 1u, 0u);
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));

    /* Segment 1 is full: segment 0 is recycled and its event is marked as
     * overwritten (file_complete = 0). */
    j.table.current_block = AMBRE_BLOCKS_PER_SEGMENT;
    AMBRE_CHECK_OK(ambre_journal_tick(&j, 20000u));
    AMBRE_CHECK_EQ_U64(j.table.current_seg, 0u);
    AMBRE_CHECK_EQ_U64(j.table.current_block, 0u);
    teardown();
}

/* ------------------------------------------------------------ event path -- */

static void test_event_path_reports_state_and_io_errors(void)
{
    ambre_journal_cfg_t cfg = cfg_of(2u, true);
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_cont_t cont;
    ambre_seal_t sl;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    gap_make_emerg(&em, 1u, 0u);
    memset(&cont, 0, sizeof(cont));
    cont.event_id = 1u;
    gap_make_seal(&sl, 1u, 1u, 0u);

    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));

    /* Nothing is open yet. */
    AMBRE_CHECK_EQ_I64(ambre_journal_continue(&j, &cont), AMBRE_ERR_STATE);
    AMBRE_CHECK_EQ_I64(ambre_journal_seal(&j, &sl), AMBRE_ERR_STATE);

    /* The record cannot be written, then the flush is refused. */
    g.fail_write = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_trigger(&j, &em), AMBRE_ERR_IO);
    g.fail_write = false;
    g.fail_flush = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_trigger(&j, &em), AMBRE_ERR_IO);
    g.fail_flush = false;
    AMBRE_CHECK_EQ_U64(j.stats.open_events, 0u);

    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    AMBRE_CHECK_EQ_U64(j.stats.open_events, 1u);
    /* One event at a time (GEL-06). */
    AMBRE_CHECK_EQ_I64(ambre_journal_trigger(&j, &em), AMBRE_ERR_STATE);
    AMBRE_CHECK_EQ_U64(j.stats.refused_busy, 1u);
    teardown();
}

static void test_continuation_and_seal_report_io_errors(void)
{
    ambre_journal_cfg_t cfg = cfg_of(2u, true);
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_cont_t cont;
    ambre_seal_t sl;
    ambre_event_view_t view;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    gap_make_emerg(&em, 1u, 0u);
    memset(&cont, 0, sizeof(cont));
    cont.event_id = 1u;
    gap_make_seal(&sl, 1u, 1u, 0u);
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));

    g.fail_write = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_continue(&j, &cont), AMBRE_ERR_IO);
    g.fail_write = false;
    g.fail_flush = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_continue(&j, &cont), AMBRE_ERR_IO);
    g.fail_flush = false;
    AMBRE_CHECK_OK(ambre_journal_continue(&j, &cont));

    g.fail_write = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_seal(&j, &sl), AMBRE_ERR_IO);
    g.fail_write = false;
    g.fail_table_write = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_seal(&j, &sl), AMBRE_ERR_IO);
    g.fail_table_write = false;
    g.fail_flush = true;
    AMBRE_CHECK_EQ_I64(ambre_journal_seal(&j, &sl), AMBRE_ERR_IO);
    g.fail_flush = false;
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &sl));

    AMBRE_CHECK_OK(ambre_journal_read_event(&j, 1u, &view));
    AMBRE_CHECK(view.have_emerg);
    AMBRE_CHECK(view.have_seal);
    /* The continuation whose flush was refused is still on the disk: the
     * record was written before the flush failed, so it reads back. */
    AMBRE_CHECK_EQ_U64(view.cont_count, 2u);
    AMBRE_CHECK_EQ_I64(ambre_journal_read_event(&j, 77u, &view), AMBRE_ERR_STATE);
    AMBRE_CHECK_EQ_I64(ambre_journal_read_event(&j, 1u, NULL), AMBRE_ERR_PARAM);
    teardown();
}

static void test_read_event_skips_a_record_of_the_wrong_type(void)
{
    ambre_journal_cfg_t cfg = cfg_of(2u, true);
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_event_view_t view;
    ambre_block_t forged;
    const ambre_ev_desc_t *desc;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    gap_make_emerg(&em, 3u, 0u);
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));

    /* Forge the EMERG block into a DATA record: the read back must refuse to
     * report a snapshot it cannot prove (EXP-01). */
    desc = ambre_table_find_event_const(&j.table, 3u);
    AMBRE_CHECK(desc != NULL);
    if (desc == NULL) {
        teardown();
        return;
    }
    AMBRE_CHECK_OK(ambre_block_read(&g.disk,
                                    ambre_block_offset(desc->start_seg,
                                                       desc->start_block),
                                    &forged));
    forged.hdr.rec_type = (uint8_t)AMBRE_REC_DATA;
    gap_reseal(&forged);
    AMBRE_CHECK_OK(gap_write_block(&g, desc->start_seg, desc->start_block,
                                   &forged));

    AMBRE_CHECK_OK(ambre_journal_read_event(&j, 3u, &view));
    AMBRE_CHECK(!view.have_emerg);
    teardown();
}

static const ambre_test_case_t cases[] = {
    { "lifecycle_rejects_null_arguments", test_lifecycle_rejects_null_arguments },
    { "format_clamps_an_oversized_segment_limit", test_format_clamps_an_oversized_segment_limit },
    { "format_reports_write_errors", test_format_reports_write_errors },
    { "open_runs_recovery_or_formats_a_broken_partition",
      test_open_runs_recovery_or_formats_a_broken_partition },
    { "append_data_and_gap_paths", test_append_data_and_gap_paths },
    { "write_record_geometry", test_write_record_geometry },
    { "event_only_mode_writes_nothing", test_event_only_mode_writes_nothing },
    { "flush_reports_io_errors", test_flush_reports_io_errors },
    { "tick_refuses_when_no_segment_is_reclaimable", test_tick_refuses_when_no_segment_is_reclaimable },
    { "rotation_recycles_a_sealed_segment", test_rotation_recycles_a_sealed_segment },
    { "event_path_reports_state_and_io_errors", test_event_path_reports_state_and_io_errors },
    { "continuation_and_seal_report_io_errors", test_continuation_and_seal_report_io_errors },
    { "read_event_skips_a_record_of_the_wrong_type", test_read_event_skips_a_record_of_the_wrong_type },
};

AMBRE_TEST_MAIN(cases)
