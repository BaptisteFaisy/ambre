/*
 * test_policy_us.c — Record size and multi-block mechanism of the US profile.
 *
 * Spec 12.7 asks for a selectable American profile (49 CFR Part 563), which
 * keeps 20 s of pre-crash history at 10 Hz instead of 5 s at 2 Hz (cahier
 * des charges, section 2 and 6.2). This file measures what such a class A
 * snapshot weighs and proves how it travels through the frozen format.
 *
 * Companion files, written by the same author and never duplicated here:
 *   host/profiles/us_profile.py   the same figures, in Python
 *   tests/test_profile_us_py.py   the Python proof
 *   tests/ambre_policy_fixture.h  shared fixture (sizes, chunked write)
 */

#include "ambre_test.h"
#include "ambre_policy_fixture.h"
#include <string.h>

/* ---------------------------------------------- US profile record size ---- */

/* 49 CFR 563 keeps 20 s at 10 Hz instead of 5 s at 2 Hz (spec 12.7, 6.2).
 *
 * The frozen layout cannot carry the window: `ambre_emerg_t` is a 32-byte
 * header plus `points[21]` of 20 bytes, so 200 points would need 4 032 bytes
 * (9 blocks) while the struct sizes 452 bytes for 21 points, i.e. 2.1 s of US
 * data. fmt/ambre_types.h is frozen for this task, so the limit is measured
 * and documented here instead of being patched.
 *
 * Two encodings are measured, both honest:
 *   - the frozen struct layout: 32 B + 200 x 20 B = 4 032 B -> 9 blocks;
 *   - the generic sizing of the cahier (section 4.1: "~16 Ko for 20 signals"):
 *     20 B header + 200 x 20 signals x 4 B = 16 020 B -> 35 blocks.
 * Neither fits one 512-byte block; only the first fits one legal record. */
static void test_us_profile_record_size(void)
{
    _Static_assert(US_SAMPLES_BYTES == 16000u, "20 signals * 4 B * 200 points");
    _Static_assert(US_EMERG_BYTES == 16020u, "16 000 B of samples + 20 B header");
    _Static_assert(US_BLOCKS == 35u, "16 020 B needs 35 blocks of 460 B");

    /* What the frozen format holds. */
    AMBRE_CHECK_EQ_U64(AMBRE_BLOCK_PAYLOAD_MAX, 460u);
    AMBRE_CHECK_EQ_U64(AMBRE_REC_MAX_BLOCKS, 16u);
    AMBRE_CHECK_EQ_U64(sizeof(ambre_emerg_t), 452u);
    AMBRE_CHECK_EQ_U64(sizeof(ambre_a_point_t), 20u);
    AMBRE_CHECK_EQ_U64(sizeof(((ambre_emerg_t *)0)->points) /
                       sizeof(ambre_a_point_t), 21u);

    /* Frozen layout at the US window: 32 + 200 * 20 = 4 032 B. */
    AMBRE_CHECK_EQ_U64(32u + US_POINTS * sizeof(ambre_a_point_t), 4032u);
    AMBRE_CHECK(US_FROZEN_BYTES <= largest_record_payload());
    AMBRE_CHECK_EQ_U64(blocks_for(US_FROZEN_BYTES), 9u);
    /* Generic sizing of the cahier: 16 020 B, beyond every legal record. */
    AMBRE_CHECK_EQ_U64(blocks_for(US_EMERG_BYTES), US_BLOCKS);
    AMBRE_CHECK(US_EMERG_BYTES > largest_record_payload());

    /* Verdict, stated as assertions so a regression breaks the build. */
    AMBRE_CHECK(!fits_one_block(US_FROZEN_BYTES));   /* 4 032 > 460  */
    AMBRE_CHECK(!fits_one_block(US_EMERG_BYTES));    /* 16 020 > 460 */
    AMBRE_CHECK(fits_one_record(US_FROZEN_BYTES));   /* 4 032 <= 7 360 */
    AMBRE_CHECK(!fits_one_record(US_EMERG_BYTES));   /* 16 020 > 7 360 */
    AMBRE_CHECK(US_POINTS * sizeof(ambre_a_point_t) >
                sizeof(((ambre_emerg_t *)0)->points));
}

/* One round trip: write `bytes` of class A payload as consecutive chunks, then
 * read it back with ambre_read_record and compare byte for byte. */
static void check_chunk_roundtrip(ambre_test_disk_t *td, uint32_t seq,
                                  const uint8_t *payload, uint32_t bytes)
{
    static uint8_t readback[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    const uint16_t chunks = (uint16_t)blocks_for(bytes);
    ambre_block_t b;
    uint32_t len = 0u;

    AMBRE_CHECK_OK(ambre_block_build_chunk(&b, AMBRE_REC_EMERG, seq,
                                           POLICY_SESSION, POLICY_TS, 0u, chunks,
                                           payload, AMBRE_BLOCK_PAYLOAD_MAX,
                                           bytes));
    AMBRE_CHECK_EQ_U64(b.hdr.chunk_count, chunks);
    AMBRE_CHECK_EQ_U64(b.hdr.total_len, bytes);
    AMBRE_CHECK_OK(policy_write_chunked(&td->disk, 0u, AMBRE_REC_EMERG, seq,
                                        POLICY_SESSION, POLICY_TS, payload, bytes,
                                        chunks));
    AMBRE_CHECK_OK(ambre_read_record(&td->disk, 0u, 0u, POLICY_SESSION,
                                     (int64_t)seq, &b, readback, &len));
    AMBRE_CHECK_EQ_U64(len, bytes);
    AMBRE_CHECK_EQ_U64(b.hdr.chunk_count, chunks);
    AMBRE_CHECK_MEM_EQ(readback, payload, bytes);
}

/* The mechanism, demonstrated at the two sizes that matter. */
static void test_us_profile_multiblock(void)
{
    static uint8_t disk_mem[AMBRE_FIRST_DATA_OFFSET + AMBRE_SEGMENT_SIZE];
    static uint8_t payload[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_PAYLOAD_MAX];
    ambre_test_disk_t td;
    ambre_block_t b;

    for (uint32_t i = 0u; i < largest_record_payload(); i++) {
        payload[i] = (uint8_t)((i * 7u + 3u) & 0xFFu);
    }
    ambre_test_disk_init(&td, disk_mem, sizeof(disk_mem));

    /* 4 032 B of class A data: 9 blocks, one contiguous record. */
    check_chunk_roundtrip(&td, 1u, payload, US_FROZEN_BYTES);
    /* The 16-block ceiling (7 360 B) is reached and read back exactly. */
    check_chunk_roundtrip(&td, 2u, payload, largest_record_payload());

    /* Beyond the ceiling the codec refuses instead of truncating. */
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_EMERG, 3u,
                                               POLICY_SESSION, POLICY_TS, 0u,
                                               (uint16_t)US_BLOCKS, payload,
                                               AMBRE_BLOCK_PAYLOAD_MAX,
                                               US_EMERG_BYTES),
                       AMBRE_ERR_TOO_BIG);
    AMBRE_CHECK_EQ_I64(ambre_block_build_chunk(&b, AMBRE_REC_EMERG, 3u,
                                               POLICY_SESSION, POLICY_TS, 0u,
                                               (uint16_t)(AMBRE_REC_MAX_BLOCKS + 1u),
                                               payload, AMBRE_BLOCK_PAYLOAD_MAX,
                                               largest_record_payload() +
                                               AMBRE_BLOCK_PAYLOAD_MAX),
                       AMBRE_ERR_TOO_BIG);
}

/* ---------------------------------------------------------------- cases --- */

static const ambre_test_case_t cases[] = {
    { "us_profile_record_size",             test_us_profile_record_size },
    { "us_profile_multiblock",              test_us_profile_multiblock }
};

AMBRE_TEST_MAIN(cases)
