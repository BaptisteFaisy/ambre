/*
 * test_coverage_gaps.c - Branch coverage of the block, record and table codec.
 *
 * Spec 12.10 asks for more than 95% of the lines of fmt/ covered by the test
 * suites. The functional suites (test_block, test_table, ...) exercise the
 * nominal paths; this file adds the branches they cannot reach: parameter
 * checks, I/O failures, forged blocks and torn records. No existing test is
 * modified or weakened - these are extra scenarios only.
 *
 * Companion suites: test_coverage_gaps_scan.c (scan + recovery) and
 * test_coverage_gaps_journal.c (journal engine + trigger).
 */

#include "ambre_test.h"
#include "ambre_gap_disk.h"

#define GAP_SESSION 0x5E5Au
#define SMALL_DISK  (AMBRE_FIRST_DATA_OFFSET + 64u * AMBRE_BLOCK_SIZE)
/* One full segment: the geometry checks need the last block of a segment. */
#define SEG_DISK    (AMBRE_FIRST_DATA_OFFSET + (size_t)AMBRE_SEGMENT_SIZE)

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

/* ------------------------------------------------------- block codec gaps - */

static void test_build_chunk_rejects_bad_parameters(void)
{
    ambre_block_t b;
    uint8_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(NULL, AMBRE_REC_DATA, 1u, 1u, 1u,
                                               0u, 1u, data, 8u, 8u),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_DATA, 1u, 1u, 1u,
                                               0u, AMBRE_REC_MAX_BLOCKS + 1u,
                                               data, 8u, 8u),
                       AMBRE_ERR_TOO_BIG);
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_DATA, 1u, 1u, 1u,
                                               1u, 1u, data, 8u, 8u),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_DATA, 1u, 1u, 1u,
                                               0u, 1u, data,
                                               AMBRE_BLOCK_PAYLOAD_MAX + 1u, 8u),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_DATA, 1u, 1u, 1u,
                                               0u, 1u, data, 8u,
                                               AMBRE_BLOCK_PAYLOAD_MAX + 1u),
                       AMBRE_ERR_TOO_BIG);
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_DATA, 1u, 1u, 1u,
                                               0u, 1u, NULL, 8u, 8u),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_block_build(NULL, AMBRE_REC_DATA, 1u, 1u, 1u,
                                         data, 8u), AMBRE_ERR_PARAM);
}

static void test_validate_and_pack_reject_null(void)
{
    ambre_block_t b, forged;
    uint8_t raw[AMBRE_BLOCK_SIZE];
    uint8_t data[4] = { 7, 7, 7, 7 };

    AMBRE_CHECK_EQ_I64(ambre_block_validate(NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_OK(ambre_block_build(&b, AMBRE_REC_DATA, 5u, GAP_SESSION, 10u,
                                     data, sizeof(data)));
    forged = b;
    forged.hdr.chunk_index = 3u;           /* >= chunk_count */
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&forged), AMBRE_ERR_PARAM);

    ambre_block_pack(NULL, raw);           /* must not crash */
    ambre_block_pack(&b, NULL);
    ambre_block_unpack(NULL, &forged);
    ambre_block_unpack(raw, NULL);
    AMBRE_CHECK_OK(ambre_block_validate(&b));
}

static void test_block_io_rejects_bad_arguments(void)
{
    ambre_block_t b;
    uint8_t data[4] = { 1, 2, 3, 4 };
    ambre_disk_t tiny;

    if (!setup(SMALL_DISK)) {
        return;
    }
    AMBRE_CHECK_OK(ambre_block_build(&b, AMBRE_REC_DATA, 1u, GAP_SESSION, 1u,
                                     data, sizeof(data)));
    AMBRE_CHECK_EQ_I64(ambre_block_write(NULL, 0u, &b), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_block_write(&g.disk, 0u, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_block_read(NULL, 0u, &b), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_block_read(&g.disk, 0u, NULL), AMBRE_ERR_PARAM);

    /* A disk too small for one block, and an offset past its end. */
    tiny = g.disk;
    tiny.size = AMBRE_BLOCK_SIZE - 1u;
    AMBRE_CHECK_EQ_I64(ambre_block_write(&tiny, 0u, &b), AMBRE_ERR_IO);
    AMBRE_CHECK_EQ_I64(ambre_block_read(&tiny, 0u, &b), AMBRE_ERR_IO);
    AMBRE_CHECK_EQ_I64(ambre_block_write(&g.disk, g.disk.size, &b), AMBRE_ERR_IO);
    AMBRE_CHECK_EQ_I64(ambre_block_read(&g.disk, g.disk.size, &b), AMBRE_ERR_IO);

    /* The vtable itself fails. */
    g.fail_write = true;
    AMBRE_CHECK_EQ_I64(ambre_block_write(&g.disk, 0u, &b), AMBRE_ERR_IO);
    g.fail_write = false;
    g.base.fail_read = true;
    AMBRE_CHECK_EQ_I64(ambre_block_read(&g.disk, 0u, &b), AMBRE_ERR_IO);
    g.base.fail_read = false;
    teardown();
}

static void test_offset_to_segblk_rejects_outside_data(void)
{
    uint32_t seg = 0u;
    uint32_t blk = 0u;

    AMBRE_CHECK(!ambre_offset_to_segblk(AMBRE_FIRST_DATA_OFFSET - 1u, &seg, &blk));
    AMBRE_CHECK(!ambre_offset_to_segblk(AMBRE_JOURNAL_BYTES, &seg, &blk));
    AMBRE_CHECK(!ambre_offset_to_segblk(AMBRE_FIRST_DATA_OFFSET + 1u, &seg, &blk));
    AMBRE_CHECK(ambre_offset_to_segblk(AMBRE_FIRST_DATA_OFFSET, &seg, &blk));
    AMBRE_CHECK_EQ_U64(seg, 0u);
    AMBRE_CHECK_EQ_U64(blk, 0u);
    AMBRE_CHECK(ambre_offset_to_segblk(AMBRE_FIRST_DATA_OFFSET, NULL, NULL));
}

/* ------------------------------------------------------ record level gaps - */

static void test_read_record_reassembles_two_chunks(void)
{
    ambre_block_t head, tail, read;
    uint8_t payload[AMBRE_BLOCK_PAYLOAD_MAX + 16u];
    uint8_t out[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    uint32_t len = 0u;

    if (!setup(SMALL_DISK)) {
        return;
    }
    for (size_t i = 0u; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i * 7u + 1u);
    }
    AMBRE_CHECK_OK(ambre_block_build_chunk(&head, AMBRE_REC_DATA, 11u,
                                           GAP_SESSION, 1000u, 0u, 2u, payload,
                                           AMBRE_BLOCK_PAYLOAD_MAX,
                                           (uint32_t)sizeof(payload)));
    AMBRE_CHECK_OK(ambre_block_build_chunk(&tail, AMBRE_REC_DATA, 11u,
                                           GAP_SESSION, 1000u, 1u, 2u,
                                           payload + AMBRE_BLOCK_PAYLOAD_MAX,
                                           (uint32_t)sizeof(payload) -
                                           AMBRE_BLOCK_PAYLOAD_MAX,
                                           (uint32_t)sizeof(payload)));
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 0u, &head));
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 1u, &tail));

    AMBRE_CHECK_OK(ambre_read_record(&g.disk, 0u, 0u, GAP_SESSION, 11, &read,
                                     out, &len));
    AMBRE_CHECK_EQ_U64(len, sizeof(payload));
    AMBRE_CHECK_MEM_EQ(out, payload, sizeof(payload));
    AMBRE_CHECK_EQ_U64(read.hdr.chunk_count, 2u);
    teardown();
}

static void test_read_record_rejects_torn_continuation(void)
{
    ambre_block_t head, tail;
    uint8_t payload[AMBRE_BLOCK_PAYLOAD_MAX + 4u];
    uint8_t out[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    uint32_t len = 0u;

    if (!setup(SMALL_DISK)) {
        return;
    }
    memset(payload, 0x3Cu, sizeof(payload));
    AMBRE_CHECK_OK(ambre_block_build_chunk(&head, AMBRE_REC_DATA, 21u,
                                           GAP_SESSION, 5u, 0u, 2u, payload,
                                           AMBRE_BLOCK_PAYLOAD_MAX,
                                           (uint32_t)sizeof(payload)));
    AMBRE_CHECK_OK(ambre_block_build_chunk(&tail, AMBRE_REC_DATA, 21u,
                                           GAP_SESSION, 5u, 1u, 2u,
                                           payload + AMBRE_BLOCK_PAYLOAD_MAX, 4u,
                                           (uint32_t)sizeof(payload)));
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 0u, &head));

    /* The continuation is missing: the record is torn (REP-05). */
    AMBRE_CHECK_EQ_I64(ambre_read_record(&g.disk, 0u, 0u, GAP_SESSION, 21, NULL,
                                         out, &len), AMBRE_ERR_TORN);
    /* The continuation exists but belongs to another session. */
    tail.hdr.session_id = GAP_SESSION + 1u;
    gap_reseal(&tail);
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 1u, &tail));
    AMBRE_CHECK_EQ_I64(ambre_read_record(&g.disk, 0u, 0u, GAP_SESSION, 21, NULL,
                                         out, &len), AMBRE_ERR_TORN);
    /* Right session, wrong chunk index. */
    tail.hdr.session_id = GAP_SESSION;
    tail.hdr.chunk_index = 0u;
    gap_reseal(&tail);
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 1u, &tail));
    AMBRE_CHECK_EQ_I64(ambre_read_record(&g.disk, 0u, 0u, GAP_SESSION, 21, NULL,
                                         out, &len), AMBRE_ERR_TORN);
    /* Right geometry, but the declared total does not match the chunks. */
    tail.hdr.chunk_index = 1u;
    tail.hdr.total_len = (uint32_t)sizeof(payload) + 1u;
    head.hdr.total_len = tail.hdr.total_len;
    gap_reseal(&tail);
    gap_reseal(&head);
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 0u, &head));
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 1u, &tail));
    AMBRE_CHECK_EQ_I64(ambre_read_record(&g.disk, 0u, 0u, GAP_SESSION, 21, NULL,
                                         out, &len), AMBRE_ERR_TORN);
    teardown();
}

static void test_read_record_checks_arguments_and_geometry(void)
{
    ambre_block_t b;
    uint8_t data[4] = { 1, 2, 3, 4 };

    if (!setup(SEG_DISK)) {
        return;
    }
    AMBRE_CHECK_EQ_I64(ambre_read_record(NULL, 0u, 0u, 0u, -1, NULL, NULL, NULL),
                       AMBRE_ERR_PARAM);

    /* A record that would run past the end of the segment. */
    AMBRE_CHECK_OK(ambre_block_build_chunk(&b, AMBRE_REC_DATA, 31u, GAP_SESSION,
                                           1u, 0u, 2u, data, 4u, 8u));
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, AMBRE_BLOCKS_PER_SEGMENT - 1u, &b));
    AMBRE_CHECK_EQ_I64(ambre_read_record(&g.disk, 0u, AMBRE_BLOCKS_PER_SEGMENT - 1u,
                                         GAP_SESSION, 31, NULL, NULL, NULL),
                       AMBRE_ERR_TORN);

    /* A stale session and a stale sequence are reported as such. */
    AMBRE_CHECK_OK(gap_write_block(&g, 0u, 0u, &b));
    AMBRE_CHECK_EQ_I64(ambre_read_record(&g.disk, 0u, 0u, GAP_SESSION + 1u, 31,
                                         NULL, NULL, NULL), AMBRE_ERR_STALE);
    AMBRE_CHECK_EQ_I64(ambre_read_record(&g.disk, 0u, 0u, GAP_SESSION, 32,
                                         NULL, NULL, NULL), AMBRE_ERR_STALE);
    teardown();
}

/* -------------------------------------------------------- table codec gaps - */

static void test_table_helpers_reject_null(void)
{
    ambre_alloc_table_t t;
    uint8_t raw[AMBRE_BLOCK_SIZE];

    ambre_table_init(NULL, 1u);
    AMBRE_CHECK_EQ_I64(ambre_table_validate(NULL), AMBRE_ERR_PARAM);
    ambre_table_pack(NULL, raw);
    ambre_table_pack(&t, NULL);
    ambre_table_unpack(NULL, &t);
    ambre_table_unpack(raw, NULL);
    AMBRE_CHECK(ambre_table_find_event(NULL, 1u) == NULL);
    AMBRE_CHECK(ambre_table_find_event_const(NULL, 1u) == NULL);
    AMBRE_CHECK_EQ_I64(ambre_table_add_event(NULL, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_table_pick_victim(NULL, NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_table_commit(NULL, &t), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_table_commit(&g.disk, NULL), AMBRE_ERR_PARAM);
}

static void test_table_validate_rejects_out_of_range_fields(void)
{
    ambre_alloc_table_t t;

    ambre_table_init(&t, GAP_SESSION);
    t.event_count = AMBRE_MAX_EVENTS + 1u;
    AMBRE_CHECK_EQ_I64(ambre_table_validate(&t), AMBRE_ERR_PARAM);

    ambre_table_init(&t, GAP_SESSION);
    t.current_seg = AMBRE_SEGMENT_COUNT;
    AMBRE_CHECK_EQ_I64(ambre_table_validate(&t), AMBRE_ERR_PARAM);

    ambre_table_init(&t, GAP_SESSION);
    t.current_block = AMBRE_BLOCKS_PER_SEGMENT;
    AMBRE_CHECK_EQ_I64(ambre_table_validate(&t), AMBRE_ERR_PARAM);
}

static void test_table_read_best_reports_io_errors(void)
{
    ambre_alloc_table_t t;
    ambre_disk_t small;
    int copy = -2;

    if (!setup(SMALL_DISK)) {
        return;
    }
    AMBRE_CHECK_EQ_I64(ambre_table_read_best(&g.disk, NULL, &copy), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(copy, -1);
    AMBRE_CHECK_EQ_I64(ambre_table_read_best(NULL, &t, &copy), AMBRE_ERR_PARAM);

    small = g.disk;
    small.size = AMBRE_FIRST_DATA_OFFSET - 1u;
    AMBRE_CHECK_EQ_I64(ambre_table_read_best(&small, &t, &copy), AMBRE_ERR_IO);
    AMBRE_CHECK_EQ_I64(copy, -1);

    g.base.fail_read = true;
    AMBRE_CHECK_EQ_I64(ambre_table_read_best(&g.disk, &t, &copy), AMBRE_ERR_BAD_CRC);
    g.base.fail_read = false;
    teardown();
}

static void test_table_commit_reports_write_errors(void)
{
    ambre_alloc_table_t t;
    ambre_disk_t no_write;

    if (!setup(SMALL_DISK)) {
        return;
    }
    ambre_table_init(&t, GAP_SESSION);
    AMBRE_CHECK_OK(ambre_table_commit(&g.disk, &t));

    g.fail_write = true;
    AMBRE_CHECK_EQ_I64(ambre_table_commit(&g.disk, &t), AMBRE_ERR_IO);
    g.fail_write = false;

    no_write = g.disk;
    no_write.write = NULL;
    AMBRE_CHECK_EQ_I64(ambre_table_commit(&no_write, &t), AMBRE_ERR_PARAM);
    teardown();
}

static void test_table_add_event_updates_in_place(void)
{
    ambre_alloc_table_t t;
    ambre_ev_desc_t ev;
    const ambre_ev_desc_t *found;

    ambre_table_init(&t, GAP_SESSION);
    memset(&ev, 0, sizeof(ev));
    ev.event_id = 3u;
    ev.state = (uint8_t)AMBRE_EV_OPEN;
    ev.start_seg = 1u;
    AMBRE_CHECK_OK(ambre_table_add_event(&t, &ev));
    AMBRE_CHECK_EQ_U64(t.event_count, 1u);

    ev.state = (uint8_t)AMBRE_EV_SEALED;
    ev.file_complete = 1u;
    AMBRE_CHECK_OK(ambre_table_add_event(&t, &ev));
    AMBRE_CHECK_EQ_U64(t.event_count, 1u);
    found = ambre_table_find_event_const(&t, 3u);
    AMBRE_CHECK(found != NULL);
    if (found != NULL) {
        AMBRE_CHECK_EQ_U64(found->state, (uint64_t)AMBRE_EV_SEALED);
        AMBRE_CHECK_EQ_U64(found->file_complete, 1u);
    }
    AMBRE_CHECK(ambre_table_find_event_const(&t, 4u) == NULL);
}

static const ambre_test_case_t cases[] = {
    { "build_chunk_rejects_bad_parameters", test_build_chunk_rejects_bad_parameters },
    { "validate_and_pack_reject_null", test_validate_and_pack_reject_null },
    { "block_io_rejects_bad_arguments", test_block_io_rejects_bad_arguments },
    { "offset_to_segblk_rejects_outside_data", test_offset_to_segblk_rejects_outside_data },
    { "read_record_reassembles_two_chunks", test_read_record_reassembles_two_chunks },
    { "read_record_rejects_torn_continuation", test_read_record_rejects_torn_continuation },
    { "read_record_checks_arguments_and_geometry", test_read_record_checks_arguments_and_geometry },
    { "table_helpers_reject_null", test_table_helpers_reject_null },
    { "table_validate_rejects_out_of_range_fields", test_table_validate_rejects_out_of_range_fields },
    { "table_read_best_reports_io_errors", test_table_read_best_reports_io_errors },
    { "table_commit_reports_write_errors", test_table_commit_reports_write_errors },
    { "table_add_event_updates_in_place", test_table_add_event_updates_in_place },
};

AMBRE_TEST_MAIN(cases)
