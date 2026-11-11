/*
 * test_ipc.c — Wire protocol helpers (target/ipc/edr_ipc.c).
 *
 * Covers the frozen contract of target/edr_ipc.h: header round trip, the
 * three rejections (bad magic, unknown version, body above the ceiling), the
 * coherence between the announced body_len and the bytes actually received,
 * the exact header size (16 bytes) and the ACQ-01 boundary: a 4096-byte
 * message must pass exactly, a 4097-byte one must not.
 */

#include "ambre_test.h"

#include "edr_ipc.h"
#include "edr_ipc_ext.h"

#include <string.h>

/* A frame body large enough to reach the ceiling: 4096 - 16 = 4080. */
#define MAX_BODY ((size_t)AMBRE_IPC_MAX_BODY)

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0u; i < len; i++) {
        buf[i] = (uint8_t)(i * 31u + seed);
    }
}

/* ------------------------------------------------------------ the header -- */

static void test_header_roundtrip(void)
{
    ambre_ipc_hdr_t h;
    size_t total = ambre_ipc_header_init(&h, AMBRE_MSG_FRAME, 64u);

    AMBRE_CHECK_EQ_U64(total, AMBRE_IPC_HDR_SIZE + 64u);
    AMBRE_CHECK_EQ_U64(h.magic, AMBRE_IPC_MAGIC);
    AMBRE_CHECK_EQ_U64(h.version, AMBRE_IPC_VERSION);
    AMBRE_CHECK_EQ_U64(h.msg_type, AMBRE_MSG_FRAME);
    AMBRE_CHECK_EQ_U64(h.flags, 0u);
    AMBRE_CHECK_EQ_U64(h.reserved, 0u);
    AMBRE_CHECK_EQ_U64(h.body_len, 64u);
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_OK);
    AMBRE_CHECK_EQ_U64(ambre_ipc_frame_total(&h), total);
}

static void test_header_is_exactly_sixteen_bytes(void)
{
    ambre_ipc_hdr_t h;

    /* Also enforced at compile time by the _Static_assert in edr_ipc.h. */
    AMBRE_CHECK_EQ_U64(sizeof(ambre_ipc_hdr_t), 16u);
    AMBRE_CHECK_EQ_U64(sizeof(ambre_ipc_hdr_t), AMBRE_IPC_HDR_SIZE);
    AMBRE_CHECK_EQ_U64(ambre_ipc_header_init(&h, AMBRE_MSG_ACK, 0u),
                       AMBRE_IPC_HDR_SIZE);
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_OK);
}

static void test_every_message_type_survives_the_roundtrip(void)
{
    static const ambre_ipc_msg_t types[] = {
        AMBRE_MSG_FRAME, AMBRE_MSG_EXTERNAL, AMBRE_MSG_TRIGGER,
        AMBRE_MSG_CONTINUE, AMBRE_MSG_SEAL, AMBRE_MSG_GAP,
        AMBRE_MSG_EVENT_READY, AMBRE_MSG_STATUS, AMBRE_MSG_LIST,
        AMBRE_MSG_EXPORT, AMBRE_MSG_VERIFY, AMBRE_MSG_ACK, AMBRE_MSG_NAK,
        AMBRE_MSG_STATUS_REPLY, AMBRE_MSG_LIST_REPLY
    };
    ambre_ipc_hdr_t h;

    for (size_t i = 0u; i < sizeof(types) / sizeof(types[0]); i++) {
        AMBRE_CHECK_EQ_U64(ambre_ipc_header_init(&h, types[i], 8u),
                           AMBRE_IPC_HDR_SIZE + 8u);
        AMBRE_CHECK_EQ_U64(h.msg_type, (uint8_t)types[i]);
        AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_OK);
    }
}

/* ---------------------------------------------------------- the rejections - */

static void test_reject_bad_magic(void)
{
    ambre_ipc_hdr_t h;

    (void)ambre_ipc_header_init(&h, AMBRE_MSG_STATUS, 8u);
    h.magic = 0xDEADBEEFu;
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_BAD_MAGIC);

    h.magic = 0u;
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_BAD_MAGIC);
}

static void test_reject_unknown_version(void)
{
    ambre_ipc_hdr_t h;

    (void)ambre_ipc_header_init(&h, AMBRE_MSG_STATUS, 8u);
    h.version = (uint8_t)(AMBRE_IPC_VERSION + 1u);
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_BAD_VER);

    h.version = 0u;
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_BAD_VER);
}

static void test_reject_body_above_the_ceiling(void)
{
    ambre_ipc_hdr_t h;

    /* init refuses to announce a body that would break the 4096-byte frame. */
    AMBRE_CHECK_EQ_U64(ambre_ipc_header_init(&h, AMBRE_MSG_FRAME,
                                             AMBRE_IPC_MAX_BODY + 1u), 0u);
    AMBRE_CHECK_EQ_U64(ambre_ipc_header_init(&h, AMBRE_MSG_FRAME,
                                             AMBRE_IPC_FRAME_MAX), 0u);
    AMBRE_CHECK_EQ_U64(ambre_ipc_header_init(&h, AMBRE_MSG_FRAME, UINT32_MAX),
                       0u);

    /* and validate rejects such a header however it was produced. */
    (void)ambre_ipc_header_init(&h, AMBRE_MSG_FRAME, 0u);
    h.body_len = AMBRE_IPC_MAX_BODY + 1u;
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_TOO_BIG);
    h.body_len = AMBRE_IPC_FRAME_MAX;
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_TOO_BIG);
    h.body_len = UINT32_MAX;
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_TOO_BIG);
}

static void test_reject_null_header(void)
{
    AMBRE_CHECK_EQ_U64(ambre_ipc_header_init(NULL, AMBRE_MSG_ACK, 0u), 0u);
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(NULL), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_U64(ambre_ipc_frame_total(NULL), 0u);
    AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(NULL, AMBRE_IPC_HDR_SIZE),
                       AMBRE_ERR_PARAM);
}

/* A header alone cannot tell whether the datagram carried the announced
 * body: that is the receiver's length check, exercised here on purpose. */
static void test_reject_body_len_incoherent_with_the_buffer(void)
{
    uint8_t buf[AMBRE_IPC_HDR_SIZE + 32u];
    uint8_t body[32];
    ambre_ipc_hdr_t h;
    int built;

    fill_pattern(body, sizeof(body), 5u);
    built = ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_FRAME, body,
                            (uint32_t)sizeof(body));
    AMBRE_CHECK_EQ_I64(built, (int)(AMBRE_IPC_HDR_SIZE + 32u));
    AMBRE_CHECK_MEM_EQ(buf + AMBRE_IPC_HDR_SIZE, body, sizeof(body));

    /* The whole frame arrived: accepted. */
    AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(buf, (size_t)built), AMBRE_OK);

    /* Only the header arrived (truncated datagram): refused. */
    AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(buf, AMBRE_IPC_HDR_SIZE),
                       AMBRE_ERR_TORN);
    /* One body byte missing: refused. */
    AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(buf, (size_t)built - 1u),
                       AMBRE_ERR_TORN);
    /* Less than a header: refused. */
    AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(buf, AMBRE_IPC_HDR_SIZE - 1u),
                       AMBRE_ERR_TORN);

    /* One byte too many: refused. */
    {
        uint8_t big[AMBRE_IPC_HDR_SIZE + 33u];

        memcpy(big, buf, sizeof(buf));
        big[sizeof(buf)] = 0u;
        AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(big, sizeof(big)),
                           AMBRE_ERR_TOO_BIG);
    }

    /* A tampered body_len makes the same frame incoherent. */
    memcpy(&h, buf, sizeof(h));
    h.body_len = 4096u;
    AMBRE_CHECK(h.body_len + AMBRE_IPC_HDR_SIZE != (size_t)built);
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_ERR_TOO_BIG);
}

/* ------------------------------------------------------------- the builder - */

static void test_build_frame_message(void)
{
    uint8_t buf[AMBRE_IPC_HDR_SIZE + sizeof(ambre_ipc_frame_body_t)];
    ambre_ipc_frame_body_t body;
    ambre_ipc_frame_body_t back;
    ambre_ipc_hdr_t h;
    int built;

    body.first_ts_us = 1234567u;
    body.sample_count = 10u;
    body.producer_id = 3u;
    built = ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_FRAME, &body,
                            (uint32_t)sizeof(body));
    AMBRE_CHECK_EQ_I64(built, (int)sizeof(buf));

    memcpy(&h, buf, sizeof(h));
    AMBRE_CHECK_EQ_I64(ambre_ipc_header_validate(&h), AMBRE_OK);
    AMBRE_CHECK_EQ_U64(h.msg_type, AMBRE_MSG_FRAME);
    AMBRE_CHECK_EQ_U64(h.body_len, sizeof(body));
    memcpy(&back, buf + AMBRE_IPC_HDR_SIZE, sizeof(back));
    AMBRE_CHECK_EQ_U64(back.first_ts_us, body.first_ts_us);
    AMBRE_CHECK_EQ_U64(back.sample_count, body.sample_count);
    AMBRE_CHECK_EQ_U64(back.producer_id, body.producer_id);
}

static void test_build_carries_the_maximum_sample_batch(void)
{
    /* ACQ-01: the sample budget of edr_ipc.h must fit the frame ceiling. */
    uint8_t body[sizeof(ambre_ipc_frame_body_t) +
                 AMBRE_IPC_MAX_SAMPLES * sizeof(ambre_sample_t)];
    uint8_t buf[AMBRE_IPC_HDR_SIZE + sizeof(body)];
    int built;

    AMBRE_CHECK(sizeof(body) <= MAX_BODY);
    fill_pattern(body, sizeof(body), 7u);
    built = ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_FRAME, body,
                            (uint32_t)sizeof(body));
    AMBRE_CHECK_EQ_I64(built, (int)(AMBRE_IPC_HDR_SIZE + sizeof(body)));
    AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(buf, (size_t)built), AMBRE_OK);
    AMBRE_CHECK_MEM_EQ(buf + AMBRE_IPC_HDR_SIZE, body, sizeof(body));
}

static void test_build_rejects_bad_arguments(void)
{
    uint8_t buf[AMBRE_IPC_HDR_SIZE + 8u];
    uint8_t body[8];

    fill_pattern(body, sizeof(body), 1u);
    AMBRE_CHECK_EQ_I64(ambre_ipc_build(NULL, sizeof(buf), AMBRE_MSG_FRAME,
                                       body, 8u), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_FRAME,
                                       NULL, 8u), AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_ipc_build(buf, 4u, AMBRE_MSG_FRAME, body, 8u),
                       AMBRE_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_FRAME, body,
                                       AMBRE_IPC_MAX_BODY + 1u),
                       AMBRE_ERR_TOO_BIG);
    /* A body-less message is legal (a pulse on QNX, an ack on the PC). */
    AMBRE_CHECK_EQ_I64(ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_ACK, NULL,
                                       0u), (int)AMBRE_IPC_HDR_SIZE);
}

/* ------------------------------------------------- the ACQ-01 boundary ----- */

static void test_message_of_4096_bytes_passes_exactly(void)
{
    static uint8_t body[MAX_BODY];
    static uint8_t buf[AMBRE_IPC_FRAME_MAX];
    static uint8_t over[AMBRE_IPC_FRAME_MAX + 1u];
    int built;

    fill_pattern(body, sizeof(body), 42u);

    built = ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_FRAME, body,
                            (uint32_t)sizeof(body));
    AMBRE_CHECK_EQ_I64(built, (int)AMBRE_IPC_FRAME_MAX);   /* exactly 4096 */
    AMBRE_CHECK_EQ_I64(ambre_ipc_frame_check(buf, (size_t)built), AMBRE_OK);
    AMBRE_CHECK_MEM_EQ(buf + AMBRE_IPC_HDR_SIZE, body, sizeof(body));

    /* One byte more cannot be framed at all. */
    AMBRE_CHECK_EQ_I64(ambre_ipc_build(over, sizeof(over), AMBRE_MSG_FRAME,
                                       body, (uint32_t)MAX_BODY + 1u),
                       AMBRE_ERR_TOO_BIG);
    /* And a 4096-byte buffer cannot hold a 4096-byte frame plus one byte. */
    AMBRE_CHECK_EQ_I64(ambre_ipc_build(buf, sizeof(buf), AMBRE_MSG_FRAME, body,
                                       (uint32_t)MAX_BODY + 1u),
                       AMBRE_ERR_TOO_BIG);
}

static const ambre_test_case_t cases[] = {
    { "header round trip", test_header_roundtrip },
    { "header is exactly 16 bytes", test_header_is_exactly_sixteen_bytes },
    { "every message type round trips", test_every_message_type_survives_the_roundtrip },
    { "reject bad magic", test_reject_bad_magic },
    { "reject unknown version", test_reject_unknown_version },
    { "reject body above the ceiling", test_reject_body_above_the_ceiling },
    { "reject null header", test_reject_null_header },
    { "reject incoherent body_len", test_reject_body_len_incoherent_with_the_buffer },
    { "build a frame message", test_build_frame_message },
    { "build the maximum sample batch", test_build_carries_the_maximum_sample_batch },
    { "build rejects bad arguments", test_build_rejects_bad_arguments },
    { "4096-byte message passes exactly", test_message_of_4096_bytes_passes_exactly }
};

AMBRE_TEST_MAIN(cases)
