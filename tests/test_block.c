/*
 * test_block.c - Unit tests for block codec, validation, packing, offsets.
 */

#include "ambre_test.h"
#include "ambre_test_disk.h"

static uint8_t s_backing[AMBRE_FIRST_DATA_OFFSET + 8u * AMBRE_BLOCK_SIZE];

static void test_pack_unpack_roundtrip(void)
{
    ambre_block_t b1, b2;
    uint8_t raw[AMBRE_BLOCK_SIZE];
    uint8_t data[32];

    for (size_t i = 0u; i < sizeof(data); i++) {
        data[i] = (uint8_t)(0xA0u + i);
    }
    AMBRE_CHECK_OK(ambre_block_build(&b1, AMBRE_REC_DATA, 42u, 0xCAFEBABE1234ull,
                                     999000ull, data, sizeof(data)));
    AMBRE_CHECK_OK(ambre_block_validate(&b1));
    ambre_block_pack(&b1, raw);
    ambre_block_unpack(raw, &b2);
    AMBRE_CHECK_OK(ambre_block_validate(&b2));
    AMBRE_CHECK_EQ_U64(b2.hdr.seq, 42u);
    AMBRE_CHECK_EQ_U64(b2.hdr.session_id, 0xCAFEBABE1234ull);
    AMBRE_CHECK_EQ_U64(b2.hdr.first_ts_us, 999000ull);
    AMBRE_CHECK_EQ_U64(b2.hdr.payload_len, sizeof(data));
    AMBRE_CHECK_MEM_EQ(b2.payload, data, sizeof(data));
}

static void test_corruption_codes(void)
{
    ambre_block_t base, b;
    uint8_t data[16] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    AMBRE_CHECK_OK(ambre_block_build(&base, AMBRE_REC_GAP, 1u, 7u, 100ull,
                                     data, sizeof(data)));

    b = base;
    b.hdr.magic ^= 1u;
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&b), AMBRE_ERR_BAD_MAGIC);

    b = base;
    b.hdr.version = 2u;
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&b), AMBRE_ERR_BAD_VER);

    b = base;
    b.block_crc ^= 1u;
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&b), AMBRE_ERR_BAD_CRC);

    b = base;
    b.hdr.payload_crc ^= 1u;
    b.block_crc = ambre_crc32c(0u, &b, AMBRE_BLOCK_SIZE - AMBRE_BLOCK_CRC_SIZE);
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&b), AMBRE_ERR_BAD_CRC);
}

static void test_invalid_lengths_and_chunks(void)
{
    ambre_block_t base, b;
    uint8_t data[16] = { 9, 8, 7 };

    AMBRE_CHECK_OK(ambre_block_build(&base, AMBRE_REC_DATA, 2u, 7u, 200ull,
                                     data, sizeof(data)));

    b = base;
    b.hdr.payload_len = AMBRE_BLOCK_PAYLOAD_MAX + 1u;
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&b), AMBRE_ERR_TOO_BIG);

    b = base;
    b.hdr.chunk_count = AMBRE_REC_MAX_BLOCKS + 1u;
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&b), AMBRE_ERR_TOO_BIG);

    b = base;
    b.hdr.chunk_count = 0u;
    AMBRE_CHECK_EQ_I64(ambre_block_validate(&b), AMBRE_ERR_PARAM);

    AMBRE_CHECK_EQ_I64(ambre_block_build(&b, AMBRE_REC_DATA, 3u, 7u, 300ull,
                                         data, AMBRE_BLOCK_PAYLOAD_MAX + 1u),
                       AMBRE_ERR_TOO_BIG);
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_DATA, 3u, 7u, 300ull,
                                               0u, 0u, data, 16u, 16u),
                       AMBRE_ERR_PARAM);
}

static void test_multiblock_reassembly_and_disk_io(void)
{
    ambre_test_disk_t td;
    uint8_t src[1000];
    uint8_t dst[1000];
    uint32_t total = sizeof(src);
    uint16_t nchunks = 3u;
    uint32_t off_in_rec = 0u;

    ambre_test_disk_init(&td, s_backing, sizeof(s_backing));
    for (size_t i = 0u; i < sizeof(src); i++) {
        src[i] = (uint8_t)(i ^ (i >> 3));
    }
    for (uint16_t ci = 0u; ci < nchunks; ci++) {
        ambre_block_t blk;
        uint32_t rem = total - off_in_rec;
        uint32_t clen = (rem > AMBRE_BLOCK_PAYLOAD_MAX) ? AMBRE_BLOCK_PAYLOAD_MAX : rem;
        AMBRE_CHECK_OK(ambre_block_build_chunk(&blk, AMBRE_REC_DATA, 10u, 55u,
                                               5000ull, ci, nchunks,
                                               src + off_in_rec, clen, total));
        AMBRE_CHECK_EQ_U64(blk.hdr.flags, (ci > 0u) ? AMBRE_FL_CONTINUATION : 0u);
        AMBRE_CHECK_OK(ambre_block_write(&td.disk,
                                         ambre_block_offset(0u, ci), &blk));
        off_in_rec += clen;
    }

    off_in_rec = 0u;
    for (uint16_t ci = 0u; ci < nchunks; ci++) {
        ambre_block_t blk;
        AMBRE_CHECK_OK(ambre_block_read(&td.disk,
                                        ambre_block_offset(0u, ci), &blk));
        AMBRE_CHECK_OK(ambre_block_validate(&blk));
        AMBRE_CHECK_EQ_U64(blk.hdr.chunk_index, ci);
        memcpy(dst + off_in_rec, blk.payload, blk.hdr.payload_len);
        off_in_rec += blk.hdr.payload_len;
    }
    AMBRE_CHECK_EQ_U64(off_in_rec, total);
    AMBRE_CHECK_MEM_EQ(dst, src, total);
}

static void test_offset_segblk_roundtrip(void)
{
    uint32_t seg = 0u, blk = 0u;

    for (uint32_t s = 0u; s < AMBRE_SEGMENT_COUNT; s += 5u) {
        for (uint32_t k = 0u; k < AMBRE_BLOCKS_PER_SEGMENT; k += 1023u) {
            uint64_t off = ambre_block_offset(s, k);
            AMBRE_CHECK(ambre_offset_to_segblk(off, &seg, &blk));
            AMBRE_CHECK_EQ_U64(seg, s);
            AMBRE_CHECK_EQ_U64(blk, k);
        }
    }
    AMBRE_CHECK(!ambre_offset_to_segblk(0u, &seg, &blk));
    AMBRE_CHECK(!ambre_offset_to_segblk(AMBRE_TABLE_B_OFFSET, &seg, &blk));
    AMBRE_CHECK(!ambre_offset_to_segblk(AMBRE_FIRST_DATA_OFFSET + 1u, &seg, &blk));
    AMBRE_CHECK(!ambre_offset_to_segblk(AMBRE_JOURNAL_BYTES, &seg, &blk));
}

static const ambre_test_case_t cases[] = {
    { "block_pack_unpack_roundtrip",       test_pack_unpack_roundtrip },
    { "block_corruption_error_codes",      test_corruption_codes },
    { "block_invalid_lengths_and_chunks",  test_invalid_lengths_and_chunks },
    { "block_multiblock_reassembly_io",    test_multiblock_reassembly_and_disk_io },
    { "block_offset_segblk_roundtrip",     test_offset_segblk_roundtrip }
};

AMBRE_TEST_MAIN(cases)
