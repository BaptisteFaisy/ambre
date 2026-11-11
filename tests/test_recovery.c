/*
 * test_recovery.c — Recovery after a power cut (REP-01..REP-05, I1..I8).
 *
 * The scenarios are built by hand on an in-memory fake disk (no journal
 * engine needed): that keeps the test focused on what recovery does with the
 * bytes it finds, whatever wrote them.
 */

#include "ambre_test.h"
#include "ambre_fmt.h"
#include "ambre_test_disk.h"
#include <stdlib.h>

#define TEST_SESSION 0xA5A5u
#define TEST_SEG     0u
#define TEST_BLOCK   4u

/* ------------------------------------------------------------- helpers --- */

static void disk_reset(ambre_test_disk_t *td)
{
    uint8_t *mem = (uint8_t *)malloc(AMBRE_JOURNAL_BYTES);

    AMBRE_CHECK(mem != NULL);
    if (mem == NULL) {
        return;
    }
    memset(mem, 0, AMBRE_JOURNAL_BYTES);
    free(td->mem);
    td->mem = mem;
    td->size = AMBRE_JOURNAL_BYTES;
    td->write_count = 0u;
    td->fail_read = false;
    td->fail_write = false;
    ambre_test_disk_init(td, mem, AMBRE_JOURNAL_BYTES);
}

static void write_table_copies(ambre_test_disk_t *td, const ambre_alloc_table_t *t)
{
    uint8_t buf[AMBRE_BLOCK_SIZE];

    ambre_table_pack(t, buf);
    AMBRE_CHECK_OK(td->disk.write(td->disk.ctx, AMBRE_TABLE_A_OFFSET, buf, sizeof(buf)));
    AMBRE_CHECK_OK(td->disk.write(td->disk.ctx, AMBRE_TABLE_B_OFFSET, buf, sizeof(buf)));
}

static uint64_t write_block(ambre_test_disk_t *td, uint32_t seg, uint32_t blk,
                             uint8_t type, uint64_t seq, uint64_t ts_us,
                             const void *payload, uint32_t len)
{
    ambre_block_t b;
    uint64_t off = ambre_block_offset(seg, blk);

    AMBRE_CHECK_OK(ambre_block_build(&b, (ambre_rec_type_t)type, seq, TEST_SESSION,
                                      ts_us, payload, len));
    AMBRE_CHECK_OK(ambre_block_write(&td->disk, off, &b));
    return seq;
}

static void fill_emerg(ambre_emerg_t *em, uint32_t event_id, uint32_t order)
{
    memset(em, 0, sizeof(*em));
    em->event_id = event_id;
    em->trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    em->locked = 0u;
    em->event_number = 1u;
    em->ignition_cycle = 7u;
    em->t0_ts_us = 1000000u;
    em->trig_ts_us = 1000000u;
    em->crash_order = order;
    em->point_count = 3u;
    em->points[0].speed_kmh_x4 = 400u;
    em->points[0].rel_time_ms = 5000u;
    em->points[1].speed_kmh_x4 = 380u;
    em->points[1].rel_time_ms = 4500u;
    em->points[2].speed_kmh_x4 = 360u;
    em->points[2].rel_time_ms = 4000u;
}

static void fill_seal(ambre_seal_t *sl, uint32_t event_id, uint8_t complete)
{
    memset(sl, 0, sizeof(*sl));
    sl->event_id = event_id;
    sl->file_complete = complete;
    sl->seal_cause = (uint8_t)AMBRE_SEAL_NORMAL;
    sl->locked = 0u;
    sl->event_number = 1u;
    sl->max_dv_long_mm_s = 3500;
    sl->max_dv_long_at_ms = 40u;
}

/* Write the class B data blocks of a scenario. */
static uint32_t write_data_blocks(ambre_test_disk_t *td, uint64_t *seq,
                                  uint32_t blk)
{
    ambre_sample_t s;

    memset(&s, 0, sizeof(s));
    s.signal_id = 1u;
    s.value = 1234;
    write_block(td, TEST_SEG, blk++, AMBRE_REC_DATA, (*seq)++, 500u, &s, sizeof(s));
    s.value = 1235;
    write_block(td, TEST_SEG, blk++, AMBRE_REC_DATA, (*seq)++, 1500u, &s, sizeof(s));
    return blk;
}

/* Commit a table that knows about one event. */
static void commit_table_with_event(ambre_test_disk_t *td, uint32_t event_id,
                                    bool with_seal, uint32_t blk, uint64_t seq,
                                    uint64_t t0_ts_us)
{
    ambre_alloc_table_t t;
    ambre_ev_desc_t desc;

    ambre_table_init(&t, TEST_SESSION);
    t.current_seg = TEST_SEG;
    memset(&desc, 0, sizeof(desc));
    desc.event_id = event_id;
    desc.order = 1u;
    desc.state = (uint8_t)AMBRE_EV_OPEN;
    desc.file_complete = (uint8_t)(with_seal ? 1u : 0u);
    desc.trig_reason = (uint8_t)AMBRE_TRIG_DV_LONG;
    desc.start_seg = TEST_SEG;
    desc.start_block = TEST_BLOCK + 2u;
    desc.end_seg = TEST_SEG;
    desc.end_block = desc.start_block;
    desc.trig_ts_us = t0_ts_us;
    desc.seq_emerg = seq - 1u;
    desc.seq_last = seq - 1u;
    AMBRE_CHECK_OK(ambre_table_add_event(&t, &desc));
    t.last_seq = seq - 1u;
    t.current_block = blk;
    write_table_copies(td, &t);
}

/* Build a journal image: table + data blocks + EMERG (+ optional SEAL). */
static void build_journal(ambre_test_disk_t *td, uint32_t event_id,
                          bool with_seal, bool commit_table, bool with_data)
{
    ambre_alloc_table_t t;
    ambre_emerg_t em;
    ambre_seal_t sl;
    uint64_t seq = 1u;
    uint32_t blk = TEST_BLOCK;

    ambre_table_init(&t, TEST_SESSION);
    t.current_seg = TEST_SEG;
    t.last_seq = seq;
    write_table_copies(td, &t);

    if (with_data) {
        blk = write_data_blocks(td, &seq, blk);
    }
    fill_emerg(&em, event_id, 1u);
    write_block(td, TEST_SEG, blk++, AMBRE_REC_EMERG, seq++, em.t0_ts_us, &em,
                sizeof(em));
    if (with_seal) {
        fill_seal(&sl, event_id, 1u);
        write_block(td, TEST_SEG, blk++, AMBRE_REC_SEAL, seq++, 1100000u, &sl,
                    sizeof(sl));
    }
    if (commit_table) {
        commit_table_with_event(td, event_id, with_seal, blk, seq, em.t0_ts_us);
    }
}

/* --------------------------------------------------------------- tests --- */

static void test_empty_partition(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    AMBRE_CHECK_EQ_U64(res.events_after, 0u);
    AMBRE_CHECK_EQ_U64(res.sealed_incomplete, 0u);
    free(td.mem);
}

static void test_acquired_event_survives(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;
    ambre_ev_desc_t *d;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    build_journal(&td, 42u, false, true, true);

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    d = ambre_table_find_event(&res.table, 42u);
    AMBRE_CHECK(d != NULL);
    if (d != NULL) {
        AMBRE_CHECK_EQ_U64(d->state, (uint64_t)AMBRE_EV_SEALED);
        AMBRE_CHECK_EQ_U64(d->file_complete, 0u);   /* power cut: incomplete */
        AMBRE_CHECK_EQ_U64(d->trig_reason, (uint64_t)AMBRE_TRIG_DV_LONG);
    }
    AMBRE_CHECK_EQ_U64(res.sealed_incomplete, 1u);
    free(td.mem);
}

static void test_sealed_event_keeps_complete_flag(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;
    ambre_ev_desc_t *d;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    build_journal(&td, 43u, true, true, true);

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    d = ambre_table_find_event(&res.table, 43u);
    AMBRE_CHECK(d != NULL);
    if (d != NULL) {
        AMBRE_CHECK_EQ_U64(d->file_complete, 1u);   /* sealed before the cut */
    }
    AMBRE_CHECK_EQ_U64(res.sealed_incomplete, 0u);
    free(td.mem);
}

static void test_event_found_without_table_entry(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;
    ambre_ev_desc_t *d;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    build_journal(&td, 44u, false, false, true);   /* table never updated */

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    d = ambre_table_find_event(&res.table, 44u);
    AMBRE_CHECK(d != NULL);                        /* self-describing format */
    if (d != NULL) {
        AMBRE_CHECK_EQ_U64(d->file_complete, 0u);
    }
    free(td.mem);
}

static void test_torn_emerg_is_dropped(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    build_journal(&td, 45u, false, false, true);

    /* Corrupt the EMERG block: its CRC no longer matches. */
    td.mem[ambre_block_offset(TEST_SEG, TEST_BLOCK + 2u) + 60u] ^= 0x40u;

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    AMBRE_CHECK(ambre_table_find_event(&res.table, 45u) == NULL);  /* no phantom */
    AMBRE_CHECK_EQ_U64(res.events_after, 0u);
    free(td.mem);
}

static void test_both_tables_destroyed(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;
    ambre_ev_desc_t *d;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    build_journal(&td, 46u, false, true, true);

    memset(td.mem + AMBRE_TABLE_A_OFFSET, 0xFF, AMBRE_BLOCK_SIZE);
    memset(td.mem + AMBRE_TABLE_B_OFFSET, 0xFF, AMBRE_BLOCK_SIZE);

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    AMBRE_CHECK(res.table_rebuilt);
    d = ambre_table_find_event(&res.table, 46u);
    AMBRE_CHECK(d != NULL);
    free(td.mem);
}

static void test_bitflip_discards_one_block(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;
    ambre_ev_desc_t *d;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    build_journal(&td, 47u, false, true, true);

    /* Flip a bit inside the first DATA block: that block is dropped, the
     * event that follows must still be found (ROB-07). */
    td.mem[ambre_block_offset(TEST_SEG, TEST_BLOCK) + 100u] ^= 0x01u;

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    d = ambre_table_find_event(&res.table, 47u);
    AMBRE_CHECK(d != NULL);
    free(td.mem);
}

static void test_recovery_is_idempotent(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t r1, r2;
    ambre_ev_desc_t *d1, *d2;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    build_journal(&td, 48u, false, true, true);

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &r1));
    AMBRE_CHECK_OK(ambre_recover(&td.disk, &r2));

    d1 = ambre_table_find_event(&r1.table, 48u);
    d2 = ambre_table_find_event(&r2.table, 48u);
    AMBRE_CHECK(d1 != NULL && d2 != NULL);
    if (d1 != NULL && d2 != NULL) {
        AMBRE_CHECK_EQ_U64(d1->state, d2->state);
        AMBRE_CHECK_EQ_U64(d1->file_complete, d2->file_complete);
        AMBRE_CHECK_EQ_U64(d1->seq_last, d2->seq_last);
    }
    AMBRE_CHECK_EQ_U64(r2.sealed_incomplete, 0u);   /* nothing left open */
    AMBRE_CHECK_EQ_U64(r2.table_commits, 0u);       /* nothing changed */
    free(td.mem);
}

static void test_noise_partition_terminates(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;
    uint32_t seed = 12345u;
    uint32_t i;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    for (i = 0u; i < 4000u; i++) {
        seed = seed * 1103515245u + 12345u;
        td.mem[(seed >> 8) % (uint32_t)(AMBRE_JOURNAL_BYTES - 512u)] =
            (uint8_t)(seed >> 16);
    }
    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    free(td.mem);
}

static void test_locked_event_is_kept(void)
{
    ambre_test_disk_t td;
    ambre_recovery_result_t res;
    ambre_emerg_t em;
    ambre_alloc_table_t t;
    ambre_ev_desc_t *d;

    memset(&td, 0, sizeof(td));
    disk_reset(&td);
    ambre_table_init(&t, TEST_SESSION);
    write_table_copies(&td, &t);

    fill_emerg(&em, 49u, 1u);
    em.locked = 1u;
    write_block(&td, TEST_SEG, TEST_BLOCK, AMBRE_REC_EMERG, 1u, em.t0_ts_us,
                &em, sizeof(em));

    AMBRE_CHECK_OK(ambre_recover(&td.disk, &res));
    d = ambre_table_find_event(&res.table, 49u);
    AMBRE_CHECK(d != NULL);
    if (d != NULL) {
        AMBRE_CHECK_EQ_U64(d->locked, 1u);
        AMBRE_CHECK_EQ_U64(res.table.seg_states[TEST_SEG], (uint64_t)AMBRE_SEG_LOCKED);
    }
    free(td.mem);
}

static const ambre_test_case_t cases[] = {
    { "empty_partition", test_empty_partition },
    { "acquired_event_survives", test_acquired_event_survives },
    { "sealed_event_keeps_complete_flag", test_sealed_event_keeps_complete_flag },
    { "event_found_without_table_entry", test_event_found_without_table_entry },
    { "torn_emerg_is_dropped", test_torn_emerg_is_dropped },
    { "both_tables_destroyed", test_both_tables_destroyed },
    { "bitflip_discards_one_block", test_bitflip_discards_one_block },
    { "recovery_is_idempotent", test_recovery_is_idempotent },
    { "noise_partition_terminates", test_noise_partition_terminates },
    { "locked_event_is_kept", test_locked_event_is_kept }
};

AMBRE_TEST_MAIN(cases)
