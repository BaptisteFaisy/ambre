/*
 * test_port_posix.c — POSIX backend of the portability layer.
 *
 * What is proven here, on the PC:
 *   - the clock is monotonic and the sleep never returns too early;
 *   - a regular file stands in for the raw partition: write/read round trip,
 *     flush, zeroes past the written area, out-of-bounds access refused;
 *   - the flush probe says plainly that it cannot prove the flush order
 *     (AMBRE_PORT_ERR_UNSUPPORTED) instead of pretending (essay E1);
 *   - the transport carries a message and its ack, measures the round trip,
 *     times out when nobody answers, and notices a closed connection without
 *     any polling (QNX-05);
 *   - the access-control decision table (SEC-01);
 *   - the backend reports itself honestly.
 *
 * The fork/pipe plumbing is shared with the other port tests through
 * tests/ambre_test_port.h. Temporary files and sockets live in /tmp and are
 * removed; the forked servers are always reaped, so nothing is left behind.
 */

#include "ambre_test.h"

#include "ambre_test_port.h"

#include <string.h>

#define SHORT_TIMEOUT_US 200000u    /* 200 ms: used to measure a timeout  */
#define FRAME_BYTES       64u        /* payload of the test frames        */

/* --------------------------------------------------------------- identity -- */

static void test_backend_identifies_itself(void)
{
    AMBRE_CHECK(ambre_port_backend_name() != NULL);
    AMBRE_CHECK(strcmp(ambre_port_backend_name(), "posix") == 0);
    AMBRE_CHECK_EQ_I64(ambre_port_backend(), AMBRE_PORT_BACKEND_POSIX);
    AMBRE_CHECK(ambre_port_backend() != AMBRE_PORT_BACKEND_QNX);
}

/* ------------------------------------------------------------------ clock -- */

static void test_clock_is_monotonic(void)
{
    uint64_t t0 = ambre_port_now_us();
    uint64_t t1 = ambre_port_now_us();

    AMBRE_CHECK(t0 > 0u);
    AMBRE_CHECK(t1 >= t0);
    AMBRE_CHECK(ambre_port_sleep_us(1000u) > 0u);
    AMBRE_CHECK(ambre_port_now_us() > t1);
}

/* ACQ-02 on this backend: the monotonic clock must resolve 100 us or
 * better. Take the smallest positive step observed between adjacent reads;
 * preemption can only inflate a step, so the minimum bounds the real
 * granularity. The same property on the QNX target clock stays
 * BLOCKED(no QNX SDP) — see docs/VALIDATION.md ACQ-02. */
static void test_clock_resolution_at_most_100us(void)
{
    uint64_t min_step = UINT64_MAX;
    uint64_t prev = ambre_port_now_us();

    for (int i = 0; i < 200000 && min_step > 1u; i++) {
        uint64_t now = ambre_port_now_us();
        if (now > prev && (now - prev) < min_step) {
            min_step = now - prev;
        }
        prev = now;
    }
    AMBRE_CHECK(min_step < UINT64_MAX);          /* the clock did advance */
    AMBRE_CHECK_MSG(min_step <= 100u,
                    "clock granularity %" PRIu64 " us > 100 us", min_step);
}

static void test_sleep_waits_at_least_the_requested_time(void)
{
    uint64_t asked = 5000u;                 /* 5 ms */
    uint64_t t0 = ambre_port_now_us();
    uint64_t slept = ambre_port_sleep_us(asked);
    uint64_t elapsed = ambre_port_now_us() - t0;

    AMBRE_CHECK(slept >= asked);
    AMBRE_CHECK(elapsed >= asked);
    AMBRE_CHECK(slept < asked * 100u);       /* and not absurdly long */
}

/* ------------------------------------------------------------------- disk -- */

static void test_disk_write_read_roundtrip(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_disk_t *d = NULL;
    const ambre_disk_t *vt;
    uint8_t payload[AMBRE_BLOCK_SIZE];
    uint8_t back[AMBRE_BLOCK_SIZE];

    ambre_test_tmp_path(path, sizeof(path), "disk.img");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open(path, 4u * AMBRE_BLOCK_SIZE, &d),
                       AMBRE_PORT_OK);
    if (d == NULL) {
        return;
    }
    vt = ambre_port_disk_vtable(d);
    AMBRE_CHECK(vt != NULL);
    if (vt == NULL) {
        ambre_port_disk_close(d);
        (void)unlink(path);
        return;
    }
    AMBRE_CHECK_EQ_U64(vt->size, 4u * AMBRE_BLOCK_SIZE);
    AMBRE_CHECK_EQ_U64(vt->sector_size, AMBRE_BLOCK_SIZE);
    AMBRE_CHECK(vt->mark == NULL);           /* marks are for the explorer */

    memset(payload, 0xA5u, sizeof(payload));
    AMBRE_CHECK_EQ_I64(vt->write(vt->ctx, 0u, payload, sizeof(payload)), 0);
    AMBRE_CHECK_EQ_I64(vt->flush(vt->ctx), 0);
    memset(back, 0, sizeof(back));
    AMBRE_CHECK_EQ_I64(vt->read(vt->ctx, 0u, back, sizeof(back)), 0);
    AMBRE_CHECK_MEM_EQ(back, payload, sizeof(payload));

    /* A second, different pattern further in the file. */
    memset(payload, 0x5Au, sizeof(payload));
    AMBRE_CHECK_EQ_I64(vt->write(vt->ctx, AMBRE_BLOCK_SIZE, payload,
                                 sizeof(payload)), 0);
    AMBRE_CHECK_EQ_I64(vt->flush(vt->ctx), 0);
    AMBRE_CHECK_EQ_I64(vt->read(vt->ctx, AMBRE_BLOCK_SIZE, back, sizeof(back)),
                       0);
    AMBRE_CHECK_MEM_EQ(back, payload, sizeof(payload));

    ambre_port_disk_close(d);
    (void)unlink(path);
}

static void test_disk_data_survives_the_flush(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_disk_t *d = NULL;
    const ambre_disk_t *vt;
    uint8_t payload[256];
    uint8_t back[256];

    ambre_test_tmp_path(path, sizeof(path), "flush.img");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open(path, 4096u, &d), AMBRE_PORT_OK);
    if (d == NULL) {
        return;
    }
    vt = ambre_port_disk_vtable(d);
    memset(payload, 0x3Cu, sizeof(payload));
    AMBRE_CHECK_EQ_I64(vt->write(vt->ctx, 512u, payload, sizeof(payload)), 0);
    AMBRE_CHECK_EQ_I64(vt->flush(vt->ctx), 0);
    ambre_port_disk_close(d);                /* close flushes too */

    /* Reopen: the bytes are still there, the flush did its job. */
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open(path, 4096u, &d), AMBRE_PORT_OK);
    if (d == NULL) {
        (void)unlink(path);
        return;
    }
    vt = ambre_port_disk_vtable(d);
    memset(back, 0, sizeof(back));
    AMBRE_CHECK_EQ_I64(vt->read(vt->ctx, 512u, back, sizeof(back)), 0);
    AMBRE_CHECK_MEM_EQ(back, payload, sizeof(payload));
    ambre_port_disk_close(d);
    (void)unlink(path);
}

static void test_disk_reads_zeroes_past_the_written_area(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_disk_t *d = NULL;
    const ambre_disk_t *vt;
    uint8_t zeros[AMBRE_BLOCK_SIZE];
    bool clean = true;
    size_t i;

    ambre_test_tmp_path(path, sizeof(path), "zero.img");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open(path, 4u * AMBRE_BLOCK_SIZE, &d),
                       AMBRE_PORT_OK);
    if (d == NULL) {
        return;
    }
    vt = ambre_port_disk_vtable(d);
    memset(zeros, 0xFFu, sizeof(zeros));
    AMBRE_CHECK_EQ_I64(vt->read(vt->ctx, 0u, zeros, sizeof(zeros)), 0);
    for (i = 0u; i < sizeof(zeros); i++) {
        if (zeros[i] != 0u) {
            clean = false;
            break;
        }
    }
    AMBRE_CHECK_MSG(clean, "byte %u past the written area is not zero",
                    (unsigned)i);
    ambre_port_disk_close(d);
    (void)unlink(path);
}

static void test_disk_refuses_out_of_bounds_access(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_disk_t *d = NULL;
    const ambre_disk_t *vt;
    uint8_t payload[512];
    uint8_t back[512];

    ambre_test_tmp_path(path, sizeof(path), "bounds.img");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open(path, 4096u, &d), AMBRE_PORT_OK);
    if (d == NULL) {
        return;
    }
    vt = ambre_port_disk_vtable(d);
    memset(payload, 0x11u, sizeof(payload));

    /* One byte past the end. */
    AMBRE_CHECK_EQ_I64(vt->write(vt->ctx, 4096u, payload, 1u), -1);
    AMBRE_CHECK_EQ_I64(vt->read(vt->ctx, 4096u, back, 1u), -1);
    /* A write that would cross the end. */
    AMBRE_CHECK_EQ_I64(vt->write(vt->ctx, 4000u, payload, sizeof(payload)), -1);
    AMBRE_CHECK_EQ_I64(vt->read(vt->ctx, 4000u, back, sizeof(back)), -1);
    /* Inside the partition it still works. */
    AMBRE_CHECK_EQ_I64(vt->write(vt->ctx, 4000u, payload, 96u), 0);

    ambre_port_disk_close(d);
    (void)unlink(path);
}

static void test_disk_open_rejects_bad_parameters(void)
{
    ambre_port_disk_t *d = NULL;

    AMBRE_CHECK_EQ_I64(ambre_port_disk_open(NULL, 4096u, &d),
                       AMBRE_PORT_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open("/tmp/ambre-test-none.img", 0u, &d),
                       AMBRE_PORT_ERR_PARAM);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open("/tmp/ambre-test-none.img", 4096u,
                                            NULL), AMBRE_PORT_ERR_PARAM);
}

static void test_flush_probe_is_honest(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_disk_t *d = NULL;
    bool flush_seen = true;
    uint64_t flush_ns = 0u;

    ambre_test_tmp_path(path, sizeof(path), "probe.img");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_open(path, 4096u, &d), AMBRE_PORT_OK);
    if (d == NULL) {
        return;
    }
    /* The POSIX backend cannot prove that the flush order reached a device:
     * it says so instead of reporting a fake "yes" (essay E1). */
    AMBRE_CHECK_EQ_I64(ambre_port_disk_flush_probe(d, &flush_seen, &flush_ns),
                       AMBRE_PORT_ERR_UNSUPPORTED);
    AMBRE_CHECK(!flush_seen);
    AMBRE_CHECK_EQ_I64(ambre_port_disk_flush_probe(NULL, &flush_seen, &flush_ns),
                       AMBRE_PORT_ERR_PARAM);
    ambre_port_disk_close(d);
    (void)unlink(path);
}

/* -------------------------------------------------------------- transport -- */

static void test_transport_roundtrip_with_ack(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_conn_t *conn = NULL;
    uint8_t payload[FRAME_BYTES];
    uint64_t worst_latency = 0u;
    int ready = -1;
    pid_t pid;

    ambre_test_tmp_path(path, sizeof(path), "rt.sock");
    (void)unlink(path);
    pid = ambre_test_spawn_echo_server(path, 3, FRAME_BYTES, &ready);
    AMBRE_CHECK(pid > 0);
    if (pid <= 0) {
        return;
    }
    AMBRE_CHECK(ambre_test_wait_ready(ready, 3000));
    close(ready);
    AMBRE_CHECK_EQ_I64(ambre_port_conn_open(path, &conn), AMBRE_PORT_OK);
    if (conn == NULL) {
        (void)ambre_test_reap(pid);
        (void)unlink(path);
        return;
    }
    for (int i = 0; i < 3; i++) {
        uint64_t latency = 0u;

        memset(payload, i + 1, sizeof(payload));
        AMBRE_CHECK_EQ_I64(ambre_port_conn_send(conn, payload, sizeof(payload),
                                                AMBRE_TEST_CLIENT_US, &latency),
                           AMBRE_PORT_OK);
        if (latency > worst_latency) {
            worst_latency = latency;
        }
    }
    /* PERF-05 and trial E4: the round trip is measured on the producer side. */
    AMBRE_CHECK_MSG(worst_latency > 0u, "round trip measured as 0 ns");
    ambre_port_conn_close(conn);
    AMBRE_CHECK_EQ_I64(ambre_test_reap(pid), 0);
    (void)unlink(path);
}

static void test_transport_detects_a_closed_connection(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_chan_t *chan = NULL;
    uint8_t buf[512];
    uint32_t sender = 0u;
    uint32_t dead;
    int got;
    pid_t pid;

    ambre_test_tmp_path(path, sizeof(path), "dc.sock");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_chan_open(path, &chan), AMBRE_PORT_OK);
    if (chan == NULL) {
        return;
    }
    AMBRE_CHECK_EQ_U64(ambre_port_chan_poll_disconnect(chan), 0u);
    pid = ambre_test_spawn_dying_client(path, FRAME_BYTES);
    AMBRE_CHECK(pid > 0);
    if (pid <= 0) {
        ambre_port_chan_close(chan);
        (void)unlink(path);
        return;
    }
    got = ambre_port_chan_recv(chan, buf, sizeof(buf), AMBRE_TEST_SERVER_US,
                                &sender);
    AMBRE_CHECK_EQ_I64(got, (int)FRAME_BYTES);
    AMBRE_CHECK(sender != 0u);
    AMBRE_CHECK_EQ_I64(ambre_port_chan_reply(chan, sender, buf, 4u),
                       AMBRE_PORT_OK);

    /* The producer is gone: the transport notices without any polling, so the
     * next receive simply times out but records the departure. */
    AMBRE_CHECK_EQ_I64(ambre_port_chan_recv(chan, buf, sizeof(buf),
                                             SHORT_TIMEOUT_US, NULL),
                       AMBRE_PORT_ERR_TIMEOUT);
    dead = ambre_port_chan_poll_disconnect(chan);
    AMBRE_CHECK_EQ_U64(dead, sender);
    AMBRE_CHECK_EQ_U64(ambre_port_chan_poll_disconnect(chan), 0u);

    AMBRE_CHECK_EQ_I64(ambre_test_reap(pid), 0);
    ambre_port_chan_close(chan);
    (void)unlink(path);
}

/* Shared assertion: the wait really lasted (at least 3/4 of) the timeout. */
static void check_waited_at_least(uint64_t t0, uint64_t timeout_us)
{
    uint64_t elapsed = ambre_port_now_us() - t0;

    AMBRE_CHECK_MSG(elapsed >= (timeout_us * 3u) / 4u,
                    "timeout returned after %llu us, expected >= %llu us",
                    (unsigned long long)elapsed, (unsigned long long)timeout_us);
}

/* A client whose server never answers must give up after the timeout. */
static void test_transport_client_timeout_is_respected(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_chan_t *chan = NULL;
    ambre_port_conn_t *conn = NULL;
    uint8_t payload[FRAME_BYTES];
    uint64_t t0;

    ambre_test_tmp_path(path, sizeof(path), "to.sock");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_chan_open(path, &chan), AMBRE_PORT_OK);
    if (chan == NULL) {
        return;
    }
    AMBRE_CHECK_EQ_I64(ambre_port_conn_open(path, &conn), AMBRE_PORT_OK);
    if (conn != NULL) {
        t0 = ambre_port_now_us();
        AMBRE_CHECK_EQ_I64(ambre_port_conn_send(conn, payload, sizeof(payload),
                                                SHORT_TIMEOUT_US, NULL),
                           AMBRE_PORT_ERR_TIMEOUT);
        check_waited_at_least(t0, SHORT_TIMEOUT_US);
        ambre_port_conn_close(conn);
    }
    ambre_port_chan_close(chan);
    (void)unlink(path);
}

/* A channel nobody ever connects to times out as well. */
static void test_transport_idle_channel_timeout_is_respected(void)
{
    char path[AMBRE_TEST_PATH_MAX];
    ambre_port_chan_t *chan = NULL;
    uint8_t payload[FRAME_BYTES];
    uint64_t t0;

    ambre_test_tmp_path(path, sizeof(path), "idle.sock");
    (void)unlink(path);
    AMBRE_CHECK_EQ_I64(ambre_port_chan_open(path, &chan), AMBRE_PORT_OK);
    if (chan == NULL) {
        return;
    }
    t0 = ambre_port_now_us();
    AMBRE_CHECK_EQ_I64(ambre_port_chan_recv(chan, payload, sizeof(payload),
                                            SHORT_TIMEOUT_US, NULL),
                       AMBRE_PORT_ERR_TIMEOUT);
    check_waited_at_least(t0, SHORT_TIMEOUT_US);
    ambre_port_chan_close(chan);
    (void)unlink(path);
}

/* --------------------------------------------------------- access control -- */

typedef struct {
    uint32_t          peer;
    ambre_access_op_t op;
    int               expected;
} acl_row_t;

/* SEC-01: the decision table of image/ambre.sec, portable half. */
static const acl_row_t acl_rows[] = {
    { AMBRE_PEER_PRODUCER, AMBRE_OP_WRITE_INPUT,  AMBRE_PORT_OK       },
    { AMBRE_PEER_PRODUCER, AMBRE_OP_READ_STATUS,  AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_PRODUCER, AMBRE_OP_READ_EVENTS,  AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_PRODUCER, AMBRE_OP_RAW_DISK,     AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_SERVICE,  AMBRE_OP_READ_STATUS,  AMBRE_PORT_OK       },
    { AMBRE_PEER_SERVICE,  AMBRE_OP_READ_EVENTS,  AMBRE_PORT_OK       },
    { AMBRE_PEER_SERVICE,  AMBRE_OP_WRITE_INPUT,  AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_SERVICE,  AMBRE_OP_RAW_DISK,     AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_JOURNAL,  AMBRE_OP_RAW_DISK,     AMBRE_PORT_OK       },
    { AMBRE_PEER_JOURNAL,  AMBRE_OP_WRITE_INPUT,  AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_JOURNAL,  AMBRE_OP_READ_STATUS,  AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_CAPTURE,  AMBRE_OP_RAW_DISK,     AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_CAPTURE,  AMBRE_OP_WRITE_INPUT,  AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_ARCHIVE,  AMBRE_OP_READ_EVENTS,  AMBRE_PORT_OK       },
    { AMBRE_PEER_ARCHIVE,  AMBRE_OP_RAW_DISK,     AMBRE_PORT_ERR_PERM },
    { AMBRE_PEER_UNKNOWN,  AMBRE_OP_READ_STATUS,  AMBRE_PORT_ERR_PERM }
};

static void test_access_control_sec01(void)
{
    for (size_t i = 0u; i < sizeof(acl_rows) / sizeof(acl_rows[0]); i++) {
        int rc = ambre_port_access_allowed(acl_rows[i].peer, acl_rows[i].op);

        AMBRE_CHECK_MSG(rc == acl_rows[i].expected,
                        "peer %u op %d -> %d, expected %d",
                        (unsigned)acl_rows[i].peer, (int)acl_rows[i].op, rc,
                        acl_rows[i].expected);
    }
}

static const ambre_test_case_t cases[] = {
    { "backend identifies itself", test_backend_identifies_itself },
    { "clock is monotonic", test_clock_is_monotonic },
    { "clock resolution is at most 100 us", test_clock_resolution_at_most_100us },
    { "sleep waits at least the requested time", test_sleep_waits_at_least_the_requested_time },
    { "disk write/read round trip", test_disk_write_read_roundtrip },
    { "disk data survives the flush", test_disk_data_survives_the_flush },
    { "disk reads zeroes past the written area", test_disk_reads_zeroes_past_the_written_area },
    { "disk refuses out-of-bounds access", test_disk_refuses_out_of_bounds_access },
    { "disk open rejects bad parameters", test_disk_open_rejects_bad_parameters },
    { "flush probe is honest", test_flush_probe_is_honest },
    { "transport round trip with ack", test_transport_roundtrip_with_ack },
    { "transport detects a closed connection", test_transport_detects_a_closed_connection },
    { "transport client timeout is respected", test_transport_client_timeout_is_respected },
    { "transport idle channel timeout", test_transport_idle_channel_timeout_is_respected },
    { "access control (SEC-01)", test_access_control_sec01 }
};

AMBRE_TEST_MAIN(cases)
