/*
 * test_coverage_gaps_edges.c - The last error and edge branches of fmt/.
 *
 * Spec 12.10 asks for more than 95% of the lines of the critical library
 * covered. After the functional suite and the three other gap suites, a
 * handful of lines stay out: the recovery paths that only trigger on a
 * self-describing image (a SEAL record whose event the table does not know,
 * or one the table already marked sealed), the I/O failures of recovery and
 * of the table commit, a segment whose chain breaks mid-scan, and the
 * segment-pool bookkeeping (a table that carries an empty slot inside
 * event_count, an open event pinning its segment, a zero segment limit).
 *
 * Every case here is an extra scenario: no functional test is modified or
 * weakened.
 */

#include "ambre_test.h"
#include "ambre_gap_disk.h"
#include "ambre_journal_internal.h"

#define EDGE_SESSION 0xED6Eu
#define ONE_SEG_DISK (AMBRE_FIRST_DATA_OFFSET + (size_t)AMBRE_SEGMENT_SIZE)
#define TWO_SEG_DISK (AMBRE_FIRST_DATA_OFFSET + 2u * (size_t)AMBRE_SEGMENT_SIZE)

static gap_disk_t g;
static uint8_t *g_mem;
static size_t g_mem_size;

static ambre_journal_cfg_t cfg_of(uint32_t limit)
{
    ambre_journal_cfg_t cfg;

    ambre_journal_cfg_default(&cfg);
    cfg.session_id = EDGE_SESSION;
    cfg.segment_limit = limit;
    return cfg;
}

/* Fresh formatted journal on a two-segment disk. */
static bool fresh_two_segments(ambre_journal_cfg_t *cfg, ambre_journal_t *j)
{
    if (!gap_setup(&g, &g_mem, &g_mem_size, TWO_SEG_DISK)) {
        return false;
    }
    *cfg = cfg_of(2u);
    if (ambre_journal_format(&g.disk, cfg) != AMBRE_OK) {
        return false;
    }
    return ambre_journal_open(&g.disk, cfg, false, j) == AMBRE_OK;
}

static void fill_emerg(ambre_emerg_t *em, uint32_t event_id, uint8_t locked)
{
    memset(em, 0, sizeof(*em));
    em->event_id = event_id;
    em->trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    em->locked = locked;
    em->event_number = 1u;
    em->ignition_cycle = 2u;
    em->t0_ts_us = 2000000u;
    em->trig_ts_us = 2000000u;
    em->crash_order = event_id;
    em->point_count = 1u;
}

/* Write one SEAL/CONT record straight on the disk at (seg, blk), bypassing
 * the journal: that is how a self-describing image looks after a cut. */
static int put_seal_record(uint32_t seg, uint32_t blk, uint64_t seq,
                           uint32_t event_id, uint8_t complete)
{
    ambre_block_t blk_buf;
    ambre_seal_t seal;

    memset(&seal, 0, sizeof(seal));
    seal.event_id = event_id;
    seal.file_complete = complete;
    seal.seal_cause = (uint8_t)AMBRE_SEAL_NORMAL;
    if (ambre_block_build(&blk_buf, AMBRE_REC_SEAL, seq, EDGE_SESSION,
                          3000000u, &seal, sizeof(seal)) != AMBRE_OK) {
        return -1;
    }
    return ambre_block_write(&g.disk, ambre_block_offset(seg, blk), &blk_buf);
}

/* Rewrite both table copies with the table the test wants to see. */
static int write_table(const ambre_alloc_table_t *t)
{
    uint8_t buf[AMBRE_BLOCK_SIZE];

    ambre_table_pack(t, buf);
    if (ambre_test_disk_write(&g.base, AMBRE_TABLE_A_OFFSET, buf,
                              AMBRE_BLOCK_SIZE) != 0) {
        return -1;
    }
    return ambre_test_disk_write(&g.base, AMBRE_TABLE_B_OFFSET, buf,
                                 AMBRE_BLOCK_SIZE);
}

/* A SEAL record for an event the allocation table does not know must be
 * ignored: recovery never invents an event (REP-05, invariant I3). */
static void seal_for_unknown_event_is_ignored(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_recovery_result_t res;

    if (!fresh_two_segments(&cfg, &j)) {
        return;
    }
    AMBRE_CHECK_OK(put_seal_record(0u, 0u, 1u, 77u, 1u));
    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    AMBRE_CHECK_EQ_U64(res.table.event_count, 0u);
    AMBRE_CHECK_MSG(ambre_table_find_event(&res.table, 77u) == NULL,
                    "a SEAL record alone must not create event 77");
    gap_teardown(&g_mem, &g_mem_size);
}

/* A second SEAL record for an event already sealed in the table must not
 * overwrite the descriptor: the table keeps the authoritative end state. */
static void seal_on_sealed_event_keeps_the_table(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_seal_t seal;
    ambre_recovery_result_t res;
    const ambre_ev_desc_t *desc;

    if (!fresh_two_segments(&cfg, &j)) {
        return;
    }
    fill_emerg(&em, 5u, 1u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    memset(&seal, 0, sizeof(seal));
    seal.event_id = 5u;
    seal.file_complete = 1u;
    seal.locked = 1u;
    AMBRE_CHECK_OK(ambre_journal_seal(&j, &seal));
    /* A stale duplicate SEAL further in the same segment. */
    AMBRE_CHECK_OK(put_seal_record(0u, 40u, 999u, 5u, 0u));
    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    desc = ambre_table_find_event_const(&res.table, 5u);
    AMBRE_CHECK_MSG(desc != NULL, "event 5 must stay in the table");
    if (desc != NULL) {
        AMBRE_CHECK_EQ_U64(desc->state, (uint64_t)AMBRE_EV_SEALED);
        AMBRE_CHECK_EQ_U64(desc->file_complete, 1u);
    }
    gap_teardown(&g_mem, &g_mem_size);
}

/* The SEAL write of the recovery itself can fail: recovery must then report
 * the I/O error instead of claiming a sealed event. */
static void recovery_reports_seal_write_failure(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_recovery_result_t res;
    int rc;

    if (!fresh_two_segments(&cfg, &j)) {
        return;
    }
    fill_emerg(&em, 9u, 0u);          /* left open: recovery must seal it */
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    g.fail_write = true;
    rc = ambre_recover(&g.disk, &res);
    AMBRE_CHECK_EQ_I64(rc, (int64_t)AMBRE_ERR_IO);
    g.fail_write = false;
    gap_teardown(&g_mem, &g_mem_size);
}

/* An unreadable partition is handled by design: an unreadable table copy is
 * simply unusable, so recovery rebuilds the table from the blocks instead of
 * returning garbage. It must still terminate and report the rebuild. */
static void recovery_survives_unreadable_table(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_recovery_result_t res;
    int rc;

    if (!fresh_two_segments(&cfg, &j)) {
        return;
    }
    g.base.fail_read = true;
    rc = ambre_recover(&g.disk, &res);
    AMBRE_CHECK_EQ_I64(rc, (int64_t)AMBRE_OK);
    AMBRE_CHECK_MSG(res.table_rebuilt,
                    "both copies unreadable: the table must be rebuilt");
    AMBRE_CHECK_MSG(ambre_table_validate(&res.table) == AMBRE_OK,
                    "the rebuilt table must validate");
    g.base.fail_read = false;
    gap_teardown(&g_mem, &g_mem_size);
}

/* A commit whose read-back of the pair fails still writes a copy: an
 * unreadable copy counts as older than any valid one (STO-04), so the
 * generation strictly increases on the media afterwards. */
static void table_commit_survives_read_failure(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_alloc_table_t table;
    ambre_alloc_table_t readback;
    int copy_used = -1;

    if (!fresh_two_segments(&cfg, &j)) {
        return;
    }
    table = j.table;
    table.generation += 4u;
    g.base.fail_read = true;
    AMBRE_CHECK_OK(ambre_table_commit(&g.disk, &table));
    g.base.fail_read = false;
    AMBRE_CHECK_OK(ambre_table_read_best(&g.disk, &readback, &copy_used));
    AMBRE_CHECK_EQ_U64(readback.generation, table.generation);
    gap_teardown(&g_mem, &g_mem_size);
}

/* A chain that breaks in the middle of a segment stops the scan there: the
 * blocks after the hole are not stitched onto the prefix (FORMAT.md 10). */
static void broken_chain_stops_the_scan(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_sample_t s;
    ambre_recovery_result_t res;
    ambre_scan_result_t scan;

    if (!fresh_two_segments(&cfg, &j)) {
        return;
    }
    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    AMBRE_CHECK_OK(ambre_journal_append_data(&j, 1000u, &s, 1u));
    fill_emerg(&em, 3u, 0u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    AMBRE_CHECK_OK(ambre_journal_append_data(&j, 2000u, &s, 1u));
    /* Destroy the third block: the chain must stop at block 2. */
    memset(g.base.mem + ambre_block_offset(0u, 2u), 0xFF, AMBRE_BLOCK_SIZE);
    AMBRE_CHECK_OK(ambre_scan_segment(&g.disk, EDGE_SESSION, 1u, 0u, 0u, &scan));
    AMBRE_CHECK_MSG(scan.next_block <= 2u,
                    "the scan went past the broken block (%u)", scan.next_block);
    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    gap_teardown(&g_mem, &g_mem_size);
}

/* An empty slot inside event_count is skipped by every segment query. */
static void empty_slot_inside_event_count_is_skipped(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_emerg_t em;
    ambre_alloc_table_t table;
    ambre_recovery_result_t res;
    uint32_t victim = 0u;

    if (!fresh_two_segments(&cfg, &j)) {
        return;
    }
    fill_emerg(&em, 4u, 1u);
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    table = j.table;
    /* Slot 0 holds the locked event; slot 1 is empty but counted. */
    memset(&table.events[1], 0, sizeof(table.events[1]));
    table.event_count = 2u;
    AMBRE_CHECK_OK(write_table(&table));
    AMBRE_CHECK_OK(ambre_recover(&g.disk, &res));
    AMBRE_CHECK_MSG(ambre_table_find_event(&res.table, 4u) != NULL,
                    "the locked event must survive recovery");
    /* pick_victim walks every segment: with all of them unavailable and no
     * reclaimable sealed event, there is no victim (STO-08). */
    for (uint32_t i = 0u; i < AMBRE_SEGMENT_COUNT; i++) {
        table.seg_states[i] = (uint8_t)AMBRE_SEG_LOCKED;
    }
    AMBRE_CHECK_EQ_I64(ambre_table_pick_victim(&table, &victim),
                       (int64_t)AMBRE_ERR_FULL);
    gap_teardown(&g_mem, &g_mem_size);
}

/* An open event, and a locked one, pin their segment: never recycled. */
static void pinned_event_segment_is_not_reclaimable(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    ambre_emerg_t em;
    uint32_t seg = 0u;

    if (!gap_setup(&g, &g_mem, &g_mem_size, ONE_SEG_DISK)) {
        return;
    }
    cfg = cfg_of(1u);                 /* one segment: the event fills it */
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    fill_emerg(&em, 6u, 0u);          /* open, not locked */
    AMBRE_CHECK_OK(ambre_journal_trigger(&j, &em));
    /* No FREE segment left, and the only candidate holds the open event. */
    j.table.seg_states[0] = (uint8_t)AMBRE_SEG_FROZEN;
    AMBRE_CHECK_EQ_I64(ambre_journal_pick_segment(&j, &seg),
                       (int64_t)AMBRE_ERR_FULL);
    AMBRE_CHECK_EQ_U64(j.stats.refused_no_space, 1u);
    /* Same picture with the event locked (SEC-03). */
    j.table.events[0].locked = 1u;
    AMBRE_CHECK_EQ_I64(ambre_journal_pick_segment(&j, &seg),
                       (int64_t)AMBRE_ERR_FULL);
    AMBRE_CHECK_EQ_U64(j.stats.refused_no_space, 2u);
    gap_teardown(&g_mem, &g_mem_size);
}

/* A zero or oversized segment limit falls back to the whole journal. */
static void zero_segment_limit_uses_the_whole_journal(void)
{
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    uint32_t seg = 0u;

    if (!gap_setup(&g, &g_mem, &g_mem_size, (size_t)AMBRE_JOURNAL_BYTES)) {
        return;
    }
    cfg = cfg_of(0u);
    AMBRE_CHECK_OK(ambre_journal_format(&g.disk, &cfg));
    AMBRE_CHECK_OK(ambre_journal_open(&g.disk, &cfg, false, &j));
    AMBRE_CHECK_EQ_U64(j.cfg.segment_limit, 0u);
    /* A zero limit must not divide by zero: it falls back to the whole
     * 24-segment journal, so a valid segment comes back. */
    AMBRE_CHECK_OK(ambre_journal_pick_segment(&j, &seg));
    AMBRE_CHECK_MSG(seg < AMBRE_SEGMENT_COUNT, "segment %u out of range", seg);
    gap_teardown(&g_mem, &g_mem_size);
}

static const ambre_test_case_t cases[] = {
    { "seal_for_unknown_event_is_ignored", seal_for_unknown_event_is_ignored },
    { "seal_on_sealed_event_keeps_the_table",
      seal_on_sealed_event_keeps_the_table },
    { "recovery_reports_seal_write_failure", recovery_reports_seal_write_failure },
    { "recovery_survives_unreadable_table",
      recovery_survives_unreadable_table },
    { "table_commit_survives_read_failure",
      table_commit_survives_read_failure },
    { "broken_chain_stops_the_scan", broken_chain_stops_the_scan },
    { "empty_slot_inside_event_count_is_skipped",
      empty_slot_inside_event_count_is_skipped },
    { "pinned_event_segment_is_not_reclaimable",
      pinned_event_segment_is_not_reclaimable },
    { "zero_segment_limit_uses_the_whole_journal",
      zero_segment_limit_uses_the_whole_journal },
};

AMBRE_TEST_MAIN(cases)
