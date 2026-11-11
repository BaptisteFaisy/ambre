/*
 * test_table.c - Unit tests for allocation table codec, commit, victim choice.
 */

#include "ambre_test.h"
#include "ambre_test_disk.h"

static uint8_t s_backing[AMBRE_FIRST_DATA_OFFSET + 4u * AMBRE_BLOCK_SIZE];

static void write_raw_table(ambre_test_disk_t *td, uint64_t off,
                            const ambre_alloc_table_t *t)
{
    uint8_t buf[AMBRE_BLOCK_SIZE];
    ambre_table_pack(t, buf);
    AMBRE_CHECK_OK(td->disk.write(td->disk.ctx, off, buf, AMBRE_BLOCK_SIZE));
}

/* Recompute the table CRC after the test hand-edits a field (generation). */
static void table_seal_crc(ambre_alloc_table_t *t)
{
    uint8_t raw[AMBRE_BLOCK_SIZE];
    ambre_table_pack(t, raw);
    t->crc = ambre_crc32c(0u, raw, AMBRE_BLOCK_SIZE - AMBRE_BLOCK_CRC_SIZE);
}

static void test_init_and_validate(void)
{
    ambre_alloc_table_t t, bad;

    ambre_table_init(&t, 0x11223344ull);
    AMBRE_CHECK_OK(ambre_table_validate(&t));
    AMBRE_CHECK_EQ_U64(t.generation, 1u);
    AMBRE_CHECK_EQ_U64(t.seg_states[0], AMBRE_SEG_CURRENT);
    AMBRE_CHECK_EQ_U64(t.seg_states[1], AMBRE_SEG_FREE);

    bad = t;
    bad.crc ^= 0x80u;
    AMBRE_CHECK_EQ_I64(ambre_table_validate(&bad), AMBRE_ERR_BAD_CRC);

    bad = t;
    bad.magic ^= 1u;
    AMBRE_CHECK_EQ_I64(ambre_table_validate(&bad), AMBRE_ERR_BAD_MAGIC);

    bad = t;
    bad.version = 99u;
    AMBRE_CHECK_EQ_I64(ambre_table_validate(&bad), AMBRE_ERR_BAD_VER);
}

static void test_read_best_picks_highest_generation(void)
{
    ambre_test_disk_t td;
    ambre_alloc_table_t ta, tb, best;
    int copy_used = 99;

    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    AMBRE_CHECK_EQ_I64(ambre_table_read_best(&td.disk, &best, &copy_used),
                       AMBRE_ERR_BAD_CRC);
    AMBRE_CHECK_EQ_I64(copy_used, -1);

    ambre_table_init(&ta, 10u);
    ta.generation = 4u;
    table_seal_crc(&ta);
    write_raw_table(&td, AMBRE_TABLE_A_OFFSET, &ta);

    tb = ta;
    tb.generation = 9u;
    table_seal_crc(&tb);
    write_raw_table(&td, AMBRE_TABLE_B_OFFSET, &tb);

    AMBRE_CHECK_OK(ambre_table_read_best(&td.disk, &best, &copy_used));
    AMBRE_CHECK_EQ_I64(copy_used, 1);
    AMBRE_CHECK_EQ_U64(best.generation, 9u);

    /* Corrupt copy B -> falls back to copy A */
    td.mem[AMBRE_TABLE_B_OFFSET + 16u] ^= 0x01u;
    AMBRE_CHECK_OK(ambre_table_read_best(&td.disk, &best, &copy_used));
    AMBRE_CHECK_EQ_I64(copy_used, 0);
    AMBRE_CHECK_EQ_U64(best.generation, 4u);
}

static void test_commit_alternates_and_survives_corrupt_copy(void)
{
    ambre_test_disk_t td;
    ambre_alloc_table_t t, ra, rb, best;
    int copy_used = -1;
    uint8_t saved_a[AMBRE_BLOCK_SIZE];

    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    ambre_table_init(&t, 77u);
    write_raw_table(&td, AMBRE_TABLE_A_OFFSET, &t);

    /* A valid (gen=1), B invalid -> first commit writes B (gen=2), A intact */
    memcpy(saved_a, td.mem + AMBRE_TABLE_A_OFFSET, AMBRE_BLOCK_SIZE);
    AMBRE_CHECK_OK(ambre_table_commit(&td.disk, &t));
    AMBRE_CHECK_EQ_U64(td.last_write_off, AMBRE_TABLE_B_OFFSET);
    AMBRE_CHECK_MEM_EQ(td.mem + AMBRE_TABLE_A_OFFSET, saved_a, AMBRE_BLOCK_SIZE);
    AMBRE_CHECK_EQ_U64(t.generation, 2u);

    /* Now A=1, B=2 -> next commit overwrites A (gen=3), B stays gen=2 */
    AMBRE_CHECK_OK(ambre_table_commit(&td.disk, &t));
    AMBRE_CHECK_EQ_U64(td.last_write_off, AMBRE_TABLE_A_OFFSET);
    ambre_table_unpack(td.mem + AMBRE_TABLE_A_OFFSET, &ra);
    ambre_table_unpack(td.mem + AMBRE_TABLE_B_OFFSET, &rb);
    AMBRE_CHECK_EQ_U64(ra.generation, 3u);
    AMBRE_CHECK_EQ_U64(rb.generation, 2u);

    /* Corrupt A (gen=3), commit again -> writes A (gen=4), B (gen=2) intact */
    td.mem[AMBRE_TABLE_A_OFFSET + 20u] ^= 0xFFu;
    AMBRE_CHECK_OK(ambre_table_commit(&td.disk, &t));
    AMBRE_CHECK_EQ_U64(td.last_write_off, AMBRE_TABLE_A_OFFSET);
    AMBRE_CHECK_OK(ambre_table_read_best(&td.disk, &best, &copy_used));
    AMBRE_CHECK_EQ_I64(copy_used, 0);
    AMBRE_CHECK_EQ_U64(best.generation, 4u);
}

static ambre_ev_desc_t make_ev(uint32_t id, uint32_t order, uint32_t seg,
                               uint8_t state, uint8_t locked)
{
    ambre_ev_desc_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_id  = id;
    ev.order     = order;
    ev.state     = state;
    ev.locked    = locked;
    ev.start_seg = seg;
    ev.end_seg   = seg;
    ev.seq_emerg = id * 10u;
    return ev;
}

static void test_pick_victim_policy(void)
{
    ambre_alloc_table_t t;
    uint32_t seg = 99u;

    ambre_table_init(&t, 1u);
    /* 1. FREE preferred (seg 0 is CURRENT, seg 1 is FREE) */
    AMBRE_CHECK_OK(ambre_table_pick_victim(&t, &seg));
    AMBRE_CHECK_EQ_U64(seg, 1u);

    /* 2. Fill all with FROZEN, put ARCHIVED on seg 5 -> picks 5 */
    for (uint32_t s = 1u; s < AMBRE_SEGMENT_COUNT; s++) {
        t.seg_states[s] = (uint8_t)AMBRE_SEG_FROZEN;
    }
    t.seg_states[5] = (uint8_t)AMBRE_SEG_ARCHIVED;
    AMBRE_CHECK_OK(ambre_table_pick_victim(&t, &seg));
    AMBRE_CHECK_EQ_U64(seg, 5u);

    /* 3. No FREE/ARCHIVED: pick sealed+unlocked with oldest order */
    t.seg_states[5] = (uint8_t)AMBRE_SEG_FROZEN;
    t.seg_states[2] = (uint8_t)AMBRE_SEG_LOCKED;
    {
        ambre_ev_desc_t e1 = make_ev(1u, 10u, 2u, AMBRE_EV_SEALED, 1u); /* locked */
        ambre_ev_desc_t e2 = make_ev(2u, 20u, 4u, AMBRE_EV_SEALED, 0u); /* oldest unlocked */
        ambre_ev_desc_t e3 = make_ev(3u, 30u, 6u, AMBRE_EV_SEALED, 0u);
        ambre_ev_desc_t e4 = make_ev(4u, 5u,  7u, AMBRE_EV_OPEN,   0u); /* still open */
        AMBRE_CHECK_OK(ambre_table_add_event(&t, &e1));
        AMBRE_CHECK_OK(ambre_table_add_event(&t, &e2));
        AMBRE_CHECK_OK(ambre_table_add_event(&t, &e3));
        AMBRE_CHECK_OK(ambre_table_add_event(&t, &e4));
    }
    AMBRE_CHECK(ambre_table_find_event(&t, 2u) != NULL);
    AMBRE_CHECK_OK(ambre_table_pick_victim(&t, &seg));
    AMBRE_CHECK_EQ_U64(seg, 4u);

    /* 4. Lock all remaining reclaimable events -> AMBRE_ERR_FULL */
    ambre_table_find_event(&t, 2u)->locked = 1u;
    ambre_table_find_event(&t, 3u)->locked = 1u;
    t.seg_states[4] = (uint8_t)AMBRE_SEG_LOCKED;
    t.seg_states[6] = (uint8_t)AMBRE_SEG_LOCKED;
    AMBRE_CHECK_EQ_I64(ambre_table_pick_victim(&t, &seg), AMBRE_ERR_FULL);
    AMBRE_CHECK((t.flags & 0x02u) != 0u);
}

static const ambre_test_case_t cases[] = {
    { "table_init_and_validate",           test_init_and_validate },
    { "table_read_best_highest_gen",       test_read_best_picks_highest_generation },
    { "table_commit_alternates_corrupt",   test_commit_alternates_and_survives_corrupt_copy },
    { "table_pick_victim_policy",          test_pick_victim_policy }
};

AMBRE_TEST_MAIN(cases)
