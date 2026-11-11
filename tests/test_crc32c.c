/*
 * test_crc32c.c - Unit tests for CRC-32C Castagnoli.
 */

#include "ambre_test.h"
#include "ambre_crc32c.h"

static void test_check_value(void)
{
    static const uint8_t vec[] = "123456789";
    uint32_t crc = ambre_crc32c(0u, vec, 9u);
    AMBRE_CHECK_EQ_U64(crc, 0xE3069283u);
}

static void test_empty_buffer(void)
{
    uint8_t dummy = 0u;
    AMBRE_CHECK_EQ_U64(ambre_crc32c(0u, &dummy, 0u), 0x00000000u);
    AMBRE_CHECK_EQ_U64(ambre_crc32c(0x12345678u, &dummy, 0u), 0x12345678u);
}

static void test_incremental_equals_oneshot(void)
{
    static const uint8_t msg[] = "Ambre EDR QNX 8.0 Castagnoli CRC check";
    size_t len = sizeof(msg) - 1u;
    uint32_t oneshot = ambre_crc32c(0u, msg, len);
    uint32_t inc = 0u;

    inc = ambre_crc32c(inc, msg, 7u);
    inc = ambre_crc32c(inc, msg + 7u, 11u);
    inc = ambre_crc32c(inc, msg + 18u, len - 18u);
    AMBRE_CHECK_EQ_U64(inc, oneshot);

    uint32_t bytewise = 0u;
    for (size_t i = 0u; i < len; i++) {
        bytewise = ambre_crc32c(bytewise, msg + i, 1u);
    }
    AMBRE_CHECK_EQ_U64(bytewise, oneshot);
}

static void test_single_bit_flip_detected(void)
{
    uint8_t buf[64];
    uint32_t baseline;

    for (size_t i = 0u; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 13u + 5u);
    }
    baseline = ambre_crc32c(0u, buf, sizeof(buf));

    for (size_t byte_idx = 0u; byte_idx < sizeof(buf); byte_idx += 7u) {
        for (unsigned bit = 0u; bit < 8u; bit++) {
            buf[byte_idx] ^= (uint8_t)(1u << bit);
            AMBRE_CHECK(ambre_crc32c(0u, buf, sizeof(buf)) != baseline);
            buf[byte_idx] ^= (uint8_t)(1u << bit);
        }
    }
}

static const ambre_test_case_t cases[] = {
    { "crc32c_check_value",             test_check_value },
    { "crc32c_empty_buffer",            test_empty_buffer },
    { "crc32c_incremental_vs_oneshot",  test_incremental_equals_oneshot },
    { "crc32c_single_bit_flip",         test_single_bit_flip_detected }
};

AMBRE_TEST_MAIN(cases)
