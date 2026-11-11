/*
 * test_coverage_gaps_scan.c - Branch coverage of the scanner and of recovery.
 *
 * Spec 12.10 asks for more than 95% of the lines of fmt/ covered. This file
 * reaches the branches of ambre_fmt_scan.c and ambre_fmt_recovery.c that the
 * functional suites do not: the rebuild fallbacks (REP-02), the table merge
 * limits, the I/O failures of the seal and commit steps, and the "never move
 * the pointer backwards" rule (STO-03).
 *
 * No existing test is modified or weakened: these are extra scenarios only.
 */

#include "ambre_test.h"
#include "ambre_gap_disk.h"

#define GAP_SESSION 0x7B7Bu
#define TWO_SEG_DISK (AMBRE_FIRST_DATA_OFFSET + 2u * (size_t)AMBRE_SEGMENT_SIZE)
#define ONE_SEG_DISK (AMBRE_FIRST_DATA_OFFSET + (size_t)AMBRE_SEGMENT_SIZE)

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

/* --------------------------------------------------------- scanner gaps --- */

static void test_scan_segment_rejects_bad_arguments(void)
{
    ambre_scan_result_t res;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_EQ_I64(ambre_scan_segment(NULL, 0u, 0u, 0u, 0u, &res),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_scan_segment(&g.disk, 0u, 0u, 0u, 0u, NULL),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_scan_segment(&g.disk, 0u, 0u,
                                          AMBRE_SEGMENT_COUNT, 0u, &res),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_scan_segment(&g.disk, 0u, 0u, 0u,
                                          AMBRE_BLOCKS_PER_SEGMENT + 1u, &res),
                       AMBRE_ERR_PARAM);
    teardown();
}

static void test_rebuild_rejects_bad_arguments(void)
{
    ambre_alloc_table_t t;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_EQ_I64(ambre_rebuild_table(NULL, &t), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_rebuild_table(&g.disk, NULL), AMBRE_ERR_PARAM);

    /* A disk too small to hold one block after the two table copies. */
    g.disk.size = AMBRE_FIRST_DATA_OFFSET + AMBRE_BLOCK_SIZE - 1u;
    AMBRE_CHECK_EQ_I64(ambre_rebuild_table(&g.disk, &t), AMBRE_ERR_IO);
    teardown();
}

static void test_rebuild_clamps_the_segment_count(void)
{
    ambre_alloc_table_t t;

    /* A disk smaller than one segment: the rebuild uses one partial segment. */
    if (!setup(AMBRE_FIRST_DATA_OFFSET + 64u * AMBRE_BLOCK_SIZE)) {
        return;
    }
    AMBRE_CHECK_OK(ambre_rebuild_table(&g.disk, &t));
    AMBRE_CHECK_EQ_U64(t.current_seg, 0u);
    teardown();

    /* A disk larger than the journal: the segment count is clamped to 24. */
    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    g.disk.size = AMBRE_FIRST_DATA_OFFSET + 40u * (uint64_t)AMBRE_SEGMENT_SIZE;
    AMBRE_CHECK_OK(ambre_rebuild_table(&g.disk, &t));
    AMBRE_CHECK_EQ_U64(t.current_seg, 0u);
    teardown();
}

static void test_rebuild_ignores_a_truncated_emerg(void)
{
    ambre_alloc_table_t t;
    uint8_t tiny[4] = { 1, 2, 3, 4 };

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    /* An EMERG whose payload is shorter than the structure: it cannot be
     * decoded, so no event may be invented from it (REP-05). */
    gap_put_record(&g, 0u, 0u, AMBRE_REC_EMERG, 0u, GAP_SESSION, 100u, tiny,
                   sizeof(tiny));
    AMBRE_CHECK_OK(ambre_rebuild_table(&g.disk, &t));
    AMBRE_CHECK_EQ_U64(t.event_count, 0u);
    teardown();
}

static void test_rebuild_follows_a_continuation_record(void)
{
    ambre_alloc_table_t t;
    ambre_emerg_t em;
    ambre_cont_t cont;
    const ambre_ev_desc_t *ev;

    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    gap_make_emerg(&em, 7u, 0u);
    gap_make_cont(&cont, 7u, 40u);
    /* The event starts in segment 0 and continues in segment 1. */
    gap_put_record(&g, 0u, 0u, AMBRE_REC_EMERG, 0u, GAP_SESSION, 100u, &em,
                   (uint32_t)sizeof(em));
    gap_put_record(&g, 1u, 0u, AMBRE_REC_CONT, 1u, GAP_SESSION, 200u, &cont,
                   (uint32_t)sizeof(cont));

    AMBRE_CHECK_OK(ambre_rebuild_table(&g.disk, &t));
    AMBRE_CHECK_EQ_U64(t.event_count, 1u);
    ev = ambre_table_find_event_const(&t, 7u);
    AMBRE_CHECK(ev != NULL);
    if (ev != NULL) {
        AMBRE_CHECK_EQ_U64(ev->start_seg, 0u);
        AMBRE_CHECK_EQ_U64(ev->end_seg, 1u);
        AMBRE_CHECK_EQ_U64(ev->seq_last, 1u);
    }
    teardown();
}

static void test_rebuild_picks_a_free_segment_then_reports_full(void)
{
    ambre_alloc_table_t t;

    /* One full segment, one empty one: the rebuild starts in the empty one. */
    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    gap_fill_segment(&g, 0u, AMBRE_BLOCKS_PER_SEGMENT, GAP_SESSION);
    AMBRE_CHECK_OK(ambre_rebuild_table(&g.disk, &t));
    AMBRE_CHECK_EQ_U64(t.current_seg, 1u);
    AMBRE_CHECK_EQ_U64(t.current_block, 0u);
    teardown();

    /* Every segment full: the table reports "memory full". */
    if (!setup(TWO_SEG_DISK)) {
        return;
    }
    gap_fill_segment(&g, 0u, AMBRE_BLOCKS_PER_SEGMENT, GAP_SESSION);
    gap_fill_segment(&g, 1u, AMBRE_BLOCKS_PER_SEGMENT, GAP_SESSION);
    AMBRE_CHECK_OK(ambre_rebuild_table(&g.disk, &t));
    AMBRE_CHECK_EQ_U64(t.current_seg, 1u);
    AMBRE_CHECK_EQ_U64(t.current_block, AMBRE_BLOCKS_PER_SEGMENT - 1u);
    AMBRE_CHECK((t.flags & 0x02u) != 0u);
    teardown();
}

/* -------------------------------------------------------- recovery gaps --- */

static void test_recover_rejects_null_arguments(void)
{
    ambre_recovery_result_t res;

    AMBRE_CHECK_EQ_I64(ambre_recover(NULL, &res), AMBRE_ERR_PARAM);
    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_EQ_I64(ambre_recover(&g.disk, NULL), AMBRE_ERR_PARAM);
    teardown();
}

static void test_recover_on_a_disk_too_small_to_rebuild(void)
{
    ambre_recovery_result_t res;

    if (!setup(AMBRE_FIRST_DATA_OFFSET + 64u)) {
        return;
    }
    /* Neither table copy is usable and the rebuild cannot run either (the disk
     * is smaller than one block of data): the recovery falls back to an empty
     * table instead of inventing anything, and still reports success. */
    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    AMBRE_CHECK(res.table_rebuilt);
    AMBRE_CHECK_EQ_U64(res.events_after, 0u);
    teardown();
}

static void test_recover_ignores_duplicate_and_extra_emerg_records(void)
{
    ambre_recovery_result_t res;
    ambre_emerg_t em;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    gap_make_emerg(&em, 5u, 0u);
    /* The same event id over and over: one event, and the scan stops at 16
     * records per segment (bounded work, invariant I1). */
    for (uint32_t i = 0u; i < 20u; i++) {
        gap_put_record(&g, 0u, i, AMBRE_REC_EMERG, i, GAP_SESSION, 100u + i,
                       &em, (uint32_t)sizeof(em));
    }
    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    AMBRE_CHECK_EQ_U64(res.events_after, 1u);
    teardown();
}

static void test_recover_refuses_to_invent_a_ninth_event(void)
{
    ambre_recovery_result_t res;
    ambre_alloc_table_t table;
    ambre_emerg_t em;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    /* The table already holds the eight event slots. */
    ambre_table_init(&table, GAP_SESSION);
    table.event_count = AMBRE_MAX_EVENTS;
    for (uint32_t i = 0u; i < AMBRE_MAX_EVENTS; i++) {
        gap_make_desc(&table.events[i], i, 0u, (uint8_t)AMBRE_EV_SEALED, 0u);
    }
    table.seg_states[0] = (uint8_t)AMBRE_SEG_CURRENT;
    gap_put_table(&g, &table);

    gap_make_emerg(&em, 99u, 0u);
    gap_put_record(&g, 0u, 0u, AMBRE_REC_EMERG, 0u, GAP_SESSION, 100u, &em,
                   (uint32_t)sizeof(em));

    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    AMBRE_CHECK_EQ_U64(res.events_after, AMBRE_MAX_EVENTS);
    teardown();
}

static void test_recover_reports_a_failed_seal_and_a_failed_commit(void)
{
    ambre_recovery_result_t res;
    ambre_alloc_table_t table;
    ambre_ev_desc_t ev;

    /* An open event on the disk: the recovery must seal it (REP-03). */
    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    ambre_table_init(&table, GAP_SESSION);
    gap_make_desc(&ev, 1u, 0u, (uint8_t)AMBRE_EV_OPEN, 0u);
    table.events[0] = ev;
    table.event_count = 1u;
    gap_put_table(&g, &table);

    g.fail_write = true;                /* the seal record cannot be written */
    AMBRE_CHECK_EQ_I64(ambre_recover(&g.disk, &res), AMBRE_ERR_IO);
    g.fail_write = false;
    teardown();

    /* The seal succeeds but the table commit fails. */
    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    ambre_table_init(&table, GAP_SESSION);
    gap_make_desc(&ev, 1u, 0u, (uint8_t)AMBRE_EV_OPEN, 0u);
    table.events[0] = ev;
    table.event_count = 1u;
    gap_put_table(&g, &table);

    g.fail_table_write = true;          /* only the table copies fail */
    AMBRE_CHECK_EQ_I64(ambre_recover(&g.disk, &res), AMBRE_ERR_IO);
    g.fail_table_write = false;
    teardown();
}

static void test_recover_never_moves_the_write_pointer_backwards(void)
{
    ambre_recovery_result_t res;
    ambre_alloc_table_t table;

    if (!setup(ONE_SEG_DISK)) {
        return;
    }
    /* The table says the writer is at segment 1 block 100, but the disk only
     * holds ten blocks in segment 0: the pointer must not go back (STO-03). */
    ambre_table_init(&table, GAP_SESSION);
    table.current_seg = 1u;
    table.current_block = 100u;
    table.seg_states[0] = (uint8_t)AMBRE_SEG_ARCHIVED;
    table.seg_states[1] = (uint8_t)AMBRE_SEG_CURRENT;
    gap_put_table(&g, &table);
    gap_fill_segment(&g, 0u, 10u, GAP_SESSION);

    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    AMBRE_CHECK_EQ_U64(res.table.current_seg, 1u);
    AMBRE_CHECK_EQ_U64(res.table.current_block, 100u);
    teardown();
}

static const ambre_test_case_t cases[] = {
    { "scan_segment_rejects_bad_arguments", test_scan_segment_rejects_bad_arguments },
    { "rebuild_rejects_bad_arguments", test_rebuild_rejects_bad_arguments },
    { "rebuild_clamps_the_segment_count", test_rebuild_clamps_the_segment_count },
    { "rebuild_ignores_a_truncated_emerg", test_rebuild_ignores_a_truncated_emerg },
    { "rebuild_follows_a_continuation_record", test_rebuild_follows_a_continuation_record },
    { "rebuild_picks_a_free_segment_then_reports_full",
      test_rebuild_picks_a_free_segment_then_reports_full },
    { "recover_rejects_null_arguments", test_recover_rejects_null_arguments },
    { "recover_on_a_disk_too_small_to_rebuild", test_recover_on_a_disk_too_small_to_rebuild },
    { "recover_ignores_duplicate_and_extra_emerg_records",
      test_recover_ignores_duplicate_and_extra_emerg_records },
    { "recover_refuses_to_invent_a_ninth_event", test_recover_refuses_to_invent_a_ninth_event },
    { "recover_reports_a_failed_seal_and_a_failed_commit",
      test_recover_reports_a_failed_seal_and_a_failed_commit },
    { "recover_never_moves_the_write_pointer_backwards",
      test_recover_never_moves_the_write_pointer_backwards },
};

AMBRE_TEST_MAIN(cases)
