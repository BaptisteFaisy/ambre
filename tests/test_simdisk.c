/*
 * test_simdisk.c — Unit tests for the simulated raw partition (ambre_simdisk).
 *
 * Verifies write/read, op logging, marks, flush tracking, prefix replay,
 * snapshots with cut variants, overlapping writes, bounds checks, and
 * capacity exhaustion. Raw byte patterns only; does not depend on block codecs.
 */

#include "ambre_test.h"
#include "host/simdisk/ambre_simdisk.h"
#include <stdlib.h>
#include <string.h>

#define TEST_DISK_SIZE (64u * 1024u)

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

static void test_create_destroy(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    AMBRE_CHECK_EQ_U64(sd.size, TEST_DISK_SIZE);
    AMBRE_CHECK_EQ_U64(ambre_simdisk_op_count(&sd), 0);
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);
    AMBRE_CHECK(d != NULL);
    AMBRE_CHECK_EQ_U64(d->size, TEST_DISK_SIZE);
    AMBRE_CHECK_EQ_U64(d->sector_size, 512);
    ambre_simdisk_destroy(&sd);
}

static void test_write_read_roundtrip(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t wbuf[1024];
    uint8_t rbuf[1024];
    fill_pattern(wbuf, sizeof(wbuf), 0xAA);
    memset(rbuf, 0, sizeof(rbuf));

    AMBRE_CHECK_OK(d->write(d->ctx, 2048, wbuf, sizeof(wbuf)));
    AMBRE_CHECK_OK(d->read(d->ctx, 2048, rbuf, sizeof(rbuf)));
    AMBRE_CHECK_MEM_EQ(rbuf, wbuf, sizeof(wbuf));
    ambre_simdisk_destroy(&sd);
}

static void test_logging_exactness(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t w1[512];
    fill_pattern(w1, sizeof(w1), 0x11);
    AMBRE_CHECK_OK(d->write(d->ctx, 0, w1, sizeof(w1)));
    AMBRE_CHECK_OK(d->flush(d->ctx));
    d->mark(d->ctx, 10, 42, 99);

    AMBRE_CHECK_EQ_U64(ambre_simdisk_op_count(&sd), 3);

    const ambre_op_t *op0 = ambre_simdisk_op(&sd, 0);
    AMBRE_CHECK(op0 != NULL);
    AMBRE_CHECK_EQ_U64(op0->kind, AMBRE_OP_WRITE);
    AMBRE_CHECK_EQ_U64(op0->off, 0);
    AMBRE_CHECK_EQ_U64(op0->len, sizeof(w1));

    const ambre_op_t *op1 = ambre_simdisk_op(&sd, 1);
    AMBRE_CHECK(op1 != NULL);
    AMBRE_CHECK_EQ_U64(op1->kind, AMBRE_OP_FLUSH);

    const ambre_op_t *op2 = ambre_simdisk_op(&sd, 2);
    AMBRE_CHECK(op2 != NULL);
    AMBRE_CHECK_EQ_U64(op2->kind, AMBRE_OP_MARK);
    AMBRE_CHECK_EQ_U64(op2->mark_kind, 10);
    AMBRE_CHECK_EQ_U64(op2->mark_value, 42);
    AMBRE_CHECK_EQ_U64(op2->mark_aux, 99);
    ambre_simdisk_destroy(&sd);
}

static void test_find_mark_and_last_flush(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t dummy[512] = {0};
    d->mark(d->ctx, 1, 100, 0);                 /* op 0 */
    AMBRE_CHECK_OK(d->write(d->ctx, 0, dummy, 512)); /* op 1 */
    AMBRE_CHECK_OK(d->flush(d->ctx));           /* op 2 */
    d->mark(d->ctx, 1, 200, 0);                 /* op 3 */
    d->mark(d->ctx, 1, 100, 0);                 /* op 4 */
    AMBRE_CHECK_OK(d->flush(d->ctx));           /* op 5 */

    AMBRE_CHECK_EQ_I64(ambre_simdisk_find_mark(&sd, 1, 100), 4);
    AMBRE_CHECK_EQ_I64(ambre_simdisk_find_mark(&sd, 1, 200), 3);
    AMBRE_CHECK_EQ_I64(ambre_simdisk_find_mark(&sd, 1, 999), -1);

    AMBRE_CHECK_EQ_I64(ambre_simdisk_last_flush_before(&sd, 2), -1);
    AMBRE_CHECK_EQ_I64(ambre_simdisk_last_flush_before(&sd, 3), 2);
    AMBRE_CHECK_EQ_I64(ambre_simdisk_last_flush_before(&sd, 5), 2);
    AMBRE_CHECK_EQ_I64(ambre_simdisk_last_flush_before(&sd, 6), 5);
    ambre_simdisk_destroy(&sd);
}

static void test_prefix_and_idempotency(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t b1[512], b2[512], zero[512], readback[512];
    fill_pattern(b1, sizeof(b1), 0x10);
    fill_pattern(b2, sizeof(b2), 0x20);
    memset(zero, 0, sizeof(zero));

    AMBRE_CHECK_OK(d->write(d->ctx, 1024, b1, sizeof(b1))); /* op 0 */
    AMBRE_CHECK_OK(d->flush(d->ctx));                       /* op 1 */
    AMBRE_CHECK_OK(d->write(d->ctx, 2048, b2, sizeof(b2))); /* op 2 */

    /* Prefix 0: blank image */
    AMBRE_CHECK_OK(ambre_simdisk_apply_prefix(&sd, 0));
    AMBRE_CHECK_OK(d->read(d->ctx, 1024, readback, 512));
    AMBRE_CHECK_MEM_EQ(readback, zero, 512);

    /* Prefix 2 (includes op 0 write, op 1 flush): b1 visible, b2 absent */
    AMBRE_CHECK_OK(ambre_simdisk_apply_prefix(&sd, 2));
    AMBRE_CHECK_OK(d->read(d->ctx, 1024, readback, 512));
    AMBRE_CHECK_MEM_EQ(readback, b1, 512);
    AMBRE_CHECK_OK(d->read(d->ctx, 2048, readback, 512));
    AMBRE_CHECK_MEM_EQ(readback, zero, 512);

    /* Idempotency check: applying prefix 2 again yields the identical image */
    AMBRE_CHECK_OK(ambre_simdisk_apply_prefix(&sd, 2));
    AMBRE_CHECK_OK(d->read(d->ctx, 1024, readback, 512));
    AMBRE_CHECK_MEM_EQ(readback, b1, 512);

    /* Full prefix: both b1 and b2 visible */
    AMBRE_CHECK_OK(ambre_simdisk_apply_prefix(&sd, 3));
    AMBRE_CHECK_OK(d->read(d->ctx, 2048, readback, 512));
    AMBRE_CHECK_MEM_EQ(readback, b2, 512);
    ambre_simdisk_destroy(&sd);
}

static void test_snapshot_torn_write(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t w[3 * 512];
    fill_pattern(w, sizeof(w), 0x55);
    AMBRE_CHECK_OK(d->write(d->ctx, 0, w, sizeof(w)));

    uint8_t *snap = (uint8_t *)malloc(TEST_DISK_SIZE);
    AMBRE_CHECK(snap != NULL);

    /* Test torn write at each sector boundary (0, 512, 1024) */
    uint32_t cuts[3] = {0, 512, 1024};
    for (int i = 0; i < 3; i++) {
        ambre_cut_variant_t var;
        memset(&var, 0, sizeof(var));
        var.prefix = 1;
        var.torn_last_write = true;
        var.torn_bytes = cuts[i];

        AMBRE_CHECK_OK(ambre_simdisk_snapshot(&sd, 1, &var, snap, TEST_DISK_SIZE));
        if (cuts[i] > 0) {
            AMBRE_CHECK_MEM_EQ(snap, w, cuts[i]);
        }
        /* Bytes beyond torn_bytes must remain zero */
        for (size_t b = cuts[i]; b < sizeof(w); b++) {
            AMBRE_CHECK_EQ_U64(snap[b], 0);
        }
    }
    free(snap);
    ambre_simdisk_destroy(&sd);
}

static void test_snapshot_volatile_cache(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t w0[512], w1[512], w2[512], zero[512];
    fill_pattern(w0, sizeof(w0), 0xA0);
    fill_pattern(w1, sizeof(w1), 0xB0);
    fill_pattern(w2, sizeof(w2), 0xC0);
    memset(zero, 0, sizeof(zero));

    AMBRE_CHECK_OK(d->write(d->ctx, 0, w0, 512));    /* op 0: flushed write */
    AMBRE_CHECK_OK(d->flush(d->ctx));                /* op 1: FLUSH */
    AMBRE_CHECK_OK(d->write(d->ctx, 512, w1, 512));  /* op 2: unflushed 0 */
    AMBRE_CHECK_OK(d->write(d->ctx, 1024, w2, 512)); /* op 3: unflushed 1 */

    uint8_t *snap = (uint8_t *)malloc(TEST_DISK_SIZE);
    AMBRE_CHECK(snap != NULL);

    /* Case A: keep_mask = 0 -> both unflushed dropped */
    ambre_cut_variant_t v0 = { .drop_unflushed = true, .keep_mask = 0 };
    AMBRE_CHECK_OK(ambre_simdisk_snapshot(&sd, 4, &v0, snap, TEST_DISK_SIZE));
    AMBRE_CHECK_MEM_EQ(snap, w0, 512);
    AMBRE_CHECK_MEM_EQ(snap + 512, zero, 512);
    AMBRE_CHECK_MEM_EQ(snap + 1024, zero, 512);

    /* Case B: keep_mask = 0b01 -> unflushed 0 kept, 1 dropped */
    ambre_cut_variant_t v1 = { .drop_unflushed = true, .keep_mask = 1ULL };
    AMBRE_CHECK_OK(ambre_simdisk_snapshot(&sd, 4, &v1, snap, TEST_DISK_SIZE));
    AMBRE_CHECK_MEM_EQ(snap, w0, 512);
    AMBRE_CHECK_MEM_EQ(snap + 512, w1, 512);
    AMBRE_CHECK_MEM_EQ(snap + 1024, zero, 512);

    /* Case C: keep_mask = 0b11 -> both kept */
    ambre_cut_variant_t v2 = { .drop_unflushed = true, .keep_mask = 3ULL };
    AMBRE_CHECK_OK(ambre_simdisk_snapshot(&sd, 4, &v2, snap, TEST_DISK_SIZE));
    AMBRE_CHECK_MEM_EQ(snap + 512, w1, 512);
    AMBRE_CHECK_MEM_EQ(snap + 1024, w2, 512);

    free(snap);
    ambre_simdisk_destroy(&sd);
}

static void test_snapshot_corruption_and_tables(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t tA[512], tB[512], zero[512];
    fill_pattern(tA, sizeof(tA), 0x01);
    fill_pattern(tB, sizeof(tB), 0x02);
    memset(zero, 0, sizeof(zero));

    AMBRE_CHECK_OK(d->write(d->ctx, 0, tA, 512));
    AMBRE_CHECK_OK(d->write(d->ctx, 512, tB, 512));

    uint8_t *snap = (uint8_t *)malloc(TEST_DISK_SIZE);
    AMBRE_CHECK(snap != NULL);

    /* Bit flip test */
    ambre_cut_variant_t v_flip = {
        .corrupt_bit = true, .corrupt_off = 10, .corrupt_bit_index = 3
    };
    AMBRE_CHECK_OK(ambre_simdisk_snapshot(&sd, 2, &v_flip, snap, TEST_DISK_SIZE));
    AMBRE_CHECK_EQ_U64(snap[10], (uint8_t)(tA[10] ^ (1u << 3)));

    /* Table A wipe */
    ambre_cut_variant_t v_wA = { .wipe_table_a = true };
    AMBRE_CHECK_OK(ambre_simdisk_snapshot(&sd, 2, &v_wA, snap, TEST_DISK_SIZE));
    AMBRE_CHECK_MEM_EQ(snap, zero, 512);
    AMBRE_CHECK_MEM_EQ(snap + 512, tB, 512);

    /* Both tables wiped */
    ambre_cut_variant_t v_wBoth = { .wipe_both_tables = true };
    AMBRE_CHECK_OK(ambre_simdisk_snapshot(&sd, 2, &v_wBoth, snap, TEST_DISK_SIZE));
    AMBRE_CHECK_MEM_EQ(snap, zero, 512);
    AMBRE_CHECK_MEM_EQ(snap + 512, zero, 512);

    free(snap);
    ambre_simdisk_destroy(&sd);
}

static void test_overlapping_writes(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t buf1[100], buf2[100], readback[150];
    memset(buf1, 'A', sizeof(buf1));
    memset(buf2, 'B', sizeof(buf2));

    /* Write 100 bytes of 'A' at offset 10, then 100 bytes of 'B' at offset 60 */
    AMBRE_CHECK_OK(d->write(d->ctx, 10, buf1, sizeof(buf1)));
    AMBRE_CHECK_OK(d->write(d->ctx, 60, buf2, sizeof(buf2)));

    AMBRE_CHECK_OK(ambre_simdisk_apply_prefix(&sd, 2));
    AMBRE_CHECK_OK(d->read(d->ctx, 10, readback, 150));

    /* Bytes [10..59] must be 'A' (length 50) */
    for (int i = 0; i < 50; i++) {
        AMBRE_CHECK_EQ_U64(readback[i], 'A');
    }
    /* Bytes [60..159] must be 'B' (length 100) */
    for (int i = 50; i < 150; i++) {
        AMBRE_CHECK_EQ_U64(readback[i], 'B');
    }
    ambre_simdisk_destroy(&sd);
}

static void test_bounds_and_overflow(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t buf[64] = {0};
    /* Write completely out of bounds */
    AMBRE_CHECK(d->write(d->ctx, TEST_DISK_SIZE, buf, 1) != AMBRE_OK);
    /* Write overflowing size */
    AMBRE_CHECK(d->write(d->ctx, TEST_DISK_SIZE - 10, buf, 20) != AMBRE_OK);
    /* Read out of bounds */
    AMBRE_CHECK(d->read(d->ctx, TEST_DISK_SIZE, buf, 1) != AMBRE_OK);

    /* Write larger than MAX_DATA */
    uint8_t large[AMBRE_SIMDISK_MAX_DATA + 1];
    AMBRE_CHECK(d->write(d->ctx, 0, large, sizeof(large)) == AMBRE_ERR_TOO_BIG);

    /* Artificially saturate op capacity */
    sd.op_count = sd.op_cap;
    AMBRE_CHECK(d->write(d->ctx, 0, buf, 1) == AMBRE_ERR_IO);
    AMBRE_CHECK(d->flush(d->ctx) == AMBRE_ERR_IO);

    ambre_simdisk_destroy(&sd);
}

static void test_restore_image(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t *orig = (uint8_t *)malloc(TEST_DISK_SIZE);
    uint8_t *readback = (uint8_t *)malloc(TEST_DISK_SIZE);
    fill_pattern(orig, TEST_DISK_SIZE, 0x7E);

    ambre_simdisk_restore(&sd, orig);
    AMBRE_CHECK_OK(d->read(d->ctx, 0, readback, TEST_DISK_SIZE));
    AMBRE_CHECK_MEM_EQ(readback, orig, TEST_DISK_SIZE);

    free(orig);
    free(readback);
    ambre_simdisk_destroy(&sd);
}

static void test_read_logging_gated(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);
    uint8_t buf[512] = {0};

    /* log_reads defaults to false: reads are not logged */
    AMBRE_CHECK(sd.log_reads == false);
    AMBRE_CHECK_OK(d->read(d->ctx, 0, buf, sizeof(buf)));
    AMBRE_CHECK_EQ_U64(ambre_simdisk_op_count(&sd), 0);

    /* Enable read logging */
    sd.log_reads = true;
    AMBRE_CHECK_OK(d->read(d->ctx, 128, buf, 64));
    AMBRE_CHECK_EQ_U64(ambre_simdisk_op_count(&sd), 1);
    const ambre_op_t *op = ambre_simdisk_op(&sd, 0);
    AMBRE_CHECK(op != NULL);
    AMBRE_CHECK_EQ_U64(op->kind, AMBRE_OP_READ);
    AMBRE_CHECK_EQ_U64(op->off, 128);
    AMBRE_CHECK_EQ_U64(op->len, 64);

    /* READ ops are never replayed by apply_prefix */
    AMBRE_CHECK_OK(ambre_simdisk_apply_prefix(&sd, 1));
    ambre_simdisk_destroy(&sd);
}

static void test_write_payload_pool(void)
{
    ambre_simdisk_t sd;
    AMBRE_CHECK_OK(ambre_simdisk_create(&sd, TEST_DISK_SIZE));
    const ambre_disk_t *d = ambre_simdisk_disk(&sd);

    uint8_t w[256];
    fill_pattern(w, sizeof(w), 0x3C);
    AMBRE_CHECK_OK(d->write(d->ctx, 4096, w, sizeof(w)));

    const ambre_op_t *op = ambre_simdisk_op(&sd, 0);
    AMBRE_CHECK(op != NULL);
    AMBRE_CHECK_EQ_U64(op->data_id, 0);
    AMBRE_CHECK_EQ_U64(sd.data_len[op->data_id], sizeof(w));
    AMBRE_CHECK_MEM_EQ(sd.data_pool[op->data_id], w, sizeof(w));
    ambre_simdisk_destroy(&sd);
}

static const ambre_test_case_t cases[] = {
    { "create_destroy",             test_create_destroy },
    { "write_read_roundtrip",       test_write_read_roundtrip },
    { "logging_exactness",          test_logging_exactness },
    { "find_mark_and_last_flush",   test_find_mark_and_last_flush },
    { "prefix_and_idempotency",     test_prefix_and_idempotency },
    { "snapshot_torn_write",        test_snapshot_torn_write },
    { "snapshot_volatile_cache",    test_snapshot_volatile_cache },
    { "snapshot_corruption_tables", test_snapshot_corruption_and_tables },
    { "overlapping_writes",         test_overlapping_writes },
    { "bounds_and_overflow",        test_bounds_and_overflow },
    { "restore_image",              test_restore_image },
    { "read_logging_gated",         test_read_logging_gated },
    { "write_payload_pool",         test_write_payload_pool },
};

AMBRE_TEST_MAIN(cases)
