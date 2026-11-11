/*
 * test_scan.c - Unit tests for segment scanning and full-disk table rebuild.
 */

#include "ambre_test.h"
#include "ambre_test_disk.h"

/* 3 segments: table (1 KiB) + 3 * 2 MiB */
static uint8_t s_backing[AMBRE_FIRST_DATA_OFFSET + 3u * AMBRE_SEGMENT_SIZE];

static void write_data_block(ambre_test_disk_t *td, uint32_t seg, uint32_t blk,
                             uint64_t seq, uint64_t sess, uint64_t ts)
{
    ambre_block_t b;
    ambre_sample_t smp = { 1u, 0u, 0u, 1234 };
    AMBRE_CHECK_OK(ambre_block_build(&b, AMBRE_REC_DATA, seq, sess, ts,
                                     &smp, sizeof(smp)));
    AMBRE_CHECK_OK(ambre_block_write(&td->disk, ambre_block_offset(seg, blk), &b));
}

static void write_emerg_block(ambre_test_disk_t *td, uint32_t seg, uint32_t blk,
                              uint64_t seq, uint64_t sess, uint32_t ev_id,
                              uint32_t order, uint8_t locked, uint64_t ts)
{
    ambre_emerg_t em;
    ambre_block_t b;
    memset(&em, 0, sizeof(em));
    em.event_id    = ev_id;
    em.trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    em.locked      = locked;
    em.crash_order = order;
    em.trig_ts_us  = ts;
    AMBRE_CHECK_OK(ambre_block_build(&b, AMBRE_REC_EMERG, seq, sess, ts,
                                     &em, sizeof(em)));
    AMBRE_CHECK_OK(ambre_block_write(&td->disk, ambre_block_offset(seg, blk), &b));
}

static void write_seal_block(ambre_test_disk_t *td, uint32_t seg, uint32_t blk,
                             uint64_t seq, uint64_t sess, uint32_t ev_id,
                             uint8_t complete, uint8_t locked, uint64_t ts)
{
    ambre_seal_t sl;
    ambre_block_t b;
    memset(&sl, 0, sizeof(sl));
    sl.event_id      = ev_id;
    sl.file_complete = complete;
    sl.seal_cause    = (uint8_t)AMBRE_SEAL_NORMAL;
    sl.locked        = locked;
    AMBRE_CHECK_OK(ambre_block_build(&b, AMBRE_REC_SEAL, seq, sess, ts,
                                     &sl, sizeof(sl)));
    AMBRE_CHECK_OK(ambre_block_write(&td->disk, ambre_block_offset(seg, blk), &b));
}

static void test_scan_valid_and_emerg(void)
{
    ambre_test_disk_t td;
    ambre_scan_result_t res;

    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    write_data_block(&td, 0u, 0u, 1u, 900u, 1000ull);
    write_data_block(&td, 0u, 1u, 2u, 900u, 2000ull);
    write_emerg_block(&td, 0u, 2u, 3u, 900u, 7u, 1u, 0u, 3000ull);
    write_data_block(&td, 0u, 3u, 4u, 900u, 4000ull);

    AMBRE_CHECK_OK(ambre_scan_segment(&td.disk, 900u, 1u, 0u, 0u, &res));
    AMBRE_CHECK_EQ_U64(res.valid_blocks, 4u);
    AMBRE_CHECK_EQ_U64(res.last_seq, 4u);
    AMBRE_CHECK_EQ_U64(res.last_ts_us, 4000ull);
    AMBRE_CHECK_EQ_U64(res.next_block, 4u);
    AMBRE_CHECK(res.hit_emerg);
    AMBRE_CHECK_EQ_U64(res.emerg_seq, 3u);
    AMBRE_CHECK_EQ_U64(res.emerg_block, 2u);
}

static void test_scan_stops_on_crc_seq_session(void)
{
    ambre_test_disk_t td;
    ambre_scan_result_t res;

    /* Bad CRC at block 2 */
    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    write_data_block(&td, 0u, 0u, 10u, 55u, 100ull);
    write_data_block(&td, 0u, 1u, 11u, 55u, 200ull);
    write_data_block(&td, 0u, 2u, 12u, 55u, 300ull);
    td.mem[ambre_block_offset(0u, 2u) + 64u] ^= 0x04u;
    AMBRE_CHECK_OK(ambre_scan_segment(&td.disk, 55u, 10u, 0u, 0u, &res));
    AMBRE_CHECK_EQ_U64(res.valid_blocks, 2u);
    AMBRE_CHECK_EQ_U64(res.next_block, 2u);

    /* Sequence gap at block 2 (seq=15 instead of 12) */
    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    write_data_block(&td, 0u, 0u, 10u, 55u, 100ull);
    write_data_block(&td, 0u, 1u, 11u, 55u, 200ull);
    write_data_block(&td, 0u, 2u, 15u, 55u, 300ull);
    AMBRE_CHECK_OK(ambre_scan_segment(&td.disk, 55u, 10u, 0u, 0u, &res));
    AMBRE_CHECK_EQ_U64(res.valid_blocks, 2u);
    AMBRE_CHECK_EQ_U64(res.last_seq, 11u);

    /* Different session at block 1 */
    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    write_data_block(&td, 0u, 0u, 10u, 55u, 100ull);
    write_data_block(&td, 0u, 1u, 11u, 99u, 200ull);
    AMBRE_CHECK_OK(ambre_scan_segment(&td.disk, 55u, 10u, 0u, 0u, &res));
    AMBRE_CHECK_EQ_U64(res.valid_blocks, 1u);
    AMBRE_CHECK_EQ_U64(res.next_block, 1u);
}

static void test_rebuild_table_exact_events(void)
{
    ambre_test_disk_t td;
    ambre_alloc_table_t rebuilt;
    ambre_ev_desc_t *e1, *e2;

    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    /* Segment 0: sealed locked event #1 (seq 1..3) */
    write_data_block(&td, 0u, 0u, 1u, 42u, 1000ull);
    write_emerg_block(&td, 0u, 1u, 2u, 42u, 1u, 1u, 1u, 2000ull);
    write_seal_block(&td, 0u, 2u, 3u, 42u, 1u, 1u, 1u, 3000ull);

    /* Segment 1: open event #2 (seq 4..5), plus trailing garbage at block 2 */
    write_emerg_block(&td, 1u, 0u, 4u, 42u, 2u, 2u, 0u, 4000ull);
    write_data_block(&td, 1u, 1u, 5u, 42u, 5000ull);
    write_emerg_block(&td, 1u, 2u, 99u, 42u, 999u, 99u, 0u, 9999ull); /* bogus seq */

    AMBRE_CHECK_OK(ambre_rebuild_table(&td.disk, &rebuilt));
    AMBRE_CHECK_OK(ambre_table_validate(&rebuilt));
    AMBRE_CHECK_EQ_U64(rebuilt.event_count, 2u);
    AMBRE_CHECK_EQ_U64(rebuilt.sealed_count, 1u);
    AMBRE_CHECK_EQ_U64(rebuilt.last_seq, 5u);
    AMBRE_CHECK_EQ_U64(rebuilt.current_seg, 1u);
    AMBRE_CHECK_EQ_U64(rebuilt.current_block, 2u);

    e1 = ambre_table_find_event(&rebuilt, 1u);
    e2 = ambre_table_find_event(&rebuilt, 2u);
    AMBRE_CHECK(e1 != NULL && e2 != NULL);
    AMBRE_CHECK(ambre_table_find_event(&rebuilt, 999u) == NULL);
    AMBRE_CHECK_EQ_U64(e1->state, AMBRE_EV_SEALED);
    AMBRE_CHECK_EQ_U64(e1->locked, 1u);
    AMBRE_CHECK_EQ_U64(e1->file_complete, 1u);
    AMBRE_CHECK_EQ_U64(e2->state, AMBRE_EV_OPEN);
    AMBRE_CHECK_EQ_U64(rebuilt.seg_states[0], AMBRE_SEG_LOCKED);
    AMBRE_CHECK_EQ_U64(rebuilt.seg_states[2], AMBRE_SEG_FREE);
}

static const ambre_test_case_t cases[] = {
    { "scan_valid_chain_and_emerg",       test_scan_valid_and_emerg },
    { "scan_stops_on_crc_seq_session",    test_scan_stops_on_crc_seq_session },
    { "scan_rebuild_table_exact_events",  test_rebuild_table_exact_events }
};

AMBRE_TEST_MAIN(cases)
