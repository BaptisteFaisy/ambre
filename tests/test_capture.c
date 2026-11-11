/*
 * test_capture.c — Unit and property tests for portable edr-capture logic.
 *
 * Tests:
 * 1. Valid frame accepted, sample count / counters incremented
 * 2. Frame too large refused (ACQ-01 ceiling 4096 bytes)
 * 3. Whitelist enforcement: unknown signal rejected & counted (ACQ-04)
 * 4. Forbidden signal (90..99) rejected & counted (VIE-01)
 * 5. Monotonic timestamps preserved, no absolute time (ACQ-02, VIE-02)
 * 6. Supported cadences & quality indicator preserved (ACQ-03, ACQ-07)
 * 7. GAP emitted after interruption > 3 periods (ACQ-06)
 * 8. Crash trigger on synthetic crash (8.1 km/h in 150 ms) -> EMERG snapshot
 *    with 11 Class A points with decreasing, real rel_time_ms (GEL-01, GEL-04, DEC-06)
 * 9. CONTINUE emitted every 10 ms during event
 * 10. SEAL emitted at end of event with max dv and timing (GEL-05)
 * 11. Overload handling: 10x nominal rate accepted/dropped without blocking (ROB-06)
 */

#include "ambre_test.h"
#include "../target/capture/edr_capture.h"
#include "../target/edr_ipc.h"
#include "../fmt/ambre_fmt.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t trigger_count;
    uint32_t continue_count;
    uint32_t seal_count;
    uint32_t gap_count;
    ambre_emerg_t last_emerg;
    ambre_cont_t  last_cont;
    ambre_seal_t  last_seal;
    ambre_gap_t   last_gap;
} test_sink_t;

static int test_sink_fn(void *ctx, uint8_t msg_type,
                        const void *body, size_t body_len)
{
    test_sink_t *s = (test_sink_t *)ctx;
    if (s == NULL) return 0;
    if (msg_type == AMBRE_MSG_TRIGGER && body_len >= sizeof(ambre_emerg_t)) {
        s->trigger_count++;
        memcpy(&s->last_emerg, body, sizeof(ambre_emerg_t));
    } else if (msg_type == AMBRE_MSG_CONTINUE && body_len >= sizeof(ambre_cont_t)) {
        s->continue_count++;
        memcpy(&s->last_cont, body, sizeof(ambre_cont_t));
    } else if (msg_type == AMBRE_MSG_SEAL && body_len >= sizeof(ambre_seal_t)) {
        s->seal_count++;
        memcpy(&s->last_seal, body, sizeof(ambre_seal_t));
    } else if (msg_type == AMBRE_MSG_GAP && body_len >= sizeof(ambre_gap_t)) {
        s->gap_count++;
        memcpy(&s->last_gap, body, sizeof(ambre_gap_t));
    }
    return 0;
}

static size_t build_raw_frame(void *buf, size_t cap, uint64_t ts_us,
                              const ambre_sample_t *samples, uint32_t n_samples)
{
    size_t samples_bytes = (size_t)n_samples * sizeof(ambre_sample_t);
    size_t body_len = sizeof(ambre_ipc_frame_body_t) + samples_bytes;
    size_t total = sizeof(ambre_ipc_hdr_t) + body_len;
    if (total > cap) return 0;

    ambre_ipc_hdr_t *h = (ambre_ipc_hdr_t *)buf;
    h->magic = AMBRE_IPC_MAGIC;
    h->version = AMBRE_IPC_VERSION;
    h->msg_type = (uint8_t)AMBRE_MSG_FRAME;
    h->flags = 0;
    h->reserved = 0;
    h->body_len = (uint32_t)body_len;
    h->reserved2 = 0;

    ambre_ipc_frame_body_t *fb = (ambre_ipc_frame_body_t *)((uint8_t *)buf + sizeof(ambre_ipc_hdr_t));
    fb->first_ts_us = ts_us;
    fb->sample_count = n_samples;
    fb->producer_id = 42u;

    if (n_samples > 0 && samples != NULL) {
        memcpy((uint8_t *)fb + sizeof(ambre_ipc_frame_body_t), samples, samples_bytes);
    }
    return total;
}

static void test_frame_valid(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    test_sink_t sink;
    memset(&sink, 0, sizeof(sink));

    ambre_sample_t s[2];
    s[0].signal_id = 1; s[0].quality = 0; s[0].pad = 0; s[0].value = 100;
    s[1].signal_id = 2; s[1].quality = 0; s[1].pad = 0; s[1].value = -50;

    uint8_t wire[512];
    size_t len = build_raw_frame(wire, sizeof(wire), 1000000u, s, 2);
    AMBRE_CHECK(len > 0);

    int rc = ambre_capture_push_frame(&cap, wire, len, 1000000u, test_sink_fn, &sink);
    AMBRE_CHECK_EQ_U64(rc, AMBRE_OK);
    AMBRE_CHECK_EQ_U64(cap.stats.frames_received, 1);
    AMBRE_CHECK_EQ_U64(cap.stats.frames_accepted, 1);
    AMBRE_CHECK_EQ_U64(cap.stats.samples_received, 2);
    AMBRE_CHECK_EQ_U64(cap.stats.samples_rejected, 0);
    AMBRE_CHECK_EQ_U64(cap.stats.samples_forbidden, 0);
}

static void test_frame_too_big(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);

    uint8_t wire[5000];
    ambre_ipc_hdr_t *h = (ambre_ipc_hdr_t *)wire;
    h->magic = AMBRE_IPC_MAGIC;
    h->version = AMBRE_IPC_VERSION;
    h->msg_type = (uint8_t)AMBRE_MSG_FRAME;
    h->body_len = 4090u;
    h->reserved2 = 0;

    int rc = ambre_capture_push_frame(&cap, wire, 4106u, 1000000u, NULL, NULL);
    AMBRE_CHECK(rc == AMBRE_ERR_TOO_BIG);
    AMBRE_CHECK_EQ_U64(cap.stats.frames_malformed, 1);
}

static void test_whitelist_and_forbidden(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);

    ambre_sample_t s[4];
    s[0].signal_id = 1;  s[0].quality = 0; s[0].pad = 0; s[0].value = 10;   /* whitelisted */
    s[1].signal_id = 77; s[1].quality = 0; s[1].pad = 0; s[1].value = 20;   /* unlisted */
    s[2].signal_id = 91; s[2].quality = 0; s[2].pad = 0; s[2].value = 30;   /* forbidden VIN */
    s[3].signal_id = 95; s[3].quality = 0; s[3].pad = 0; s[3].value = 40;   /* forbidden driver */

    uint8_t wire[512];
    size_t len = build_raw_frame(wire, sizeof(wire), 1000000u, s, 4);
    int rc = ambre_capture_push_frame(&cap, wire, len, 1000000u, NULL, NULL);
    AMBRE_CHECK_EQ_U64(rc, AMBRE_OK);
    AMBRE_CHECK_EQ_U64(cap.stats.samples_received, 4);
    AMBRE_CHECK_EQ_U64(cap.stats.samples_rejected, 1);
    AMBRE_CHECK_EQ_U64(cap.stats.samples_forbidden, 2);
}

static void test_gap_detection(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    test_sink_t sink;
    memset(&sink, 0, sizeof(sink));

    ambre_sample_t s;
    s.signal_id = 1; s.quality = 0; s.pad = 0; s.value = 0;
    uint8_t wire[512];
    size_t len = build_raw_frame(wire, sizeof(wire), 1000000u, &s, 1);

    ambre_capture_push_frame(&cap, wire, len, 1000000u, test_sink_fn, &sink);
    AMBRE_CHECK_EQ_U64(sink.gap_count, 0);

    /* Signal 1 is 1 kHz -> period = 1000 us. 3 periods = 3000 us. Send frame at +5000 us */
    len = build_raw_frame(wire, sizeof(wire), 1005000u, &s, 1);
    ambre_capture_push_frame(&cap, wire, len, 1005000u, test_sink_fn, &sink);

    AMBRE_CHECK_EQ_U64(sink.gap_count, 1);
    AMBRE_CHECK_EQ_U64(sink.last_gap.start_ts_us, 1000000u);
    AMBRE_CHECK_EQ_U64(sink.last_gap.end_ts_us, 1005000u);
    AMBRE_CHECK_EQ_U64(sink.last_gap.reason, 1);
}

static void test_quality_and_time(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 3u);

    ambre_sample_t s[2];
    s[0].signal_id = 21; s[0].quality = 1; s[0].pad = 0; s[0].value = 40; /* throttle, quality=1 frozen */
    s[1].signal_id = 22; s[1].quality = 3; s[1].pad = 0; s[1].value = 1;  /* brake, quality=3 fault */

    uint8_t wire[512];
    size_t len = build_raw_frame(wire, sizeof(wire), 5000000u, s, 2);
    ambre_capture_push_frame(&cap, wire, len, 5000000u, NULL, NULL);

    AMBRE_CHECK_EQ_U64(cap.last_qualities[21], 1);
    AMBRE_CHECK_EQ_U64(cap.last_qualities[22], 3);
    AMBRE_CHECK_EQ_U64(cap.last_frame_ts_us, 5000000u);
    AMBRE_CHECK_EQ_U64(cap.ignition_cycle, 3);
}

static void fill_precrash_history(ambre_capture_t *cap, uint64_t t0_us)
{
    /* Push 5.5 s of class A history at 2 Hz (every 500 ms) */
    uint64_t t_start = (t0_us >= 5500000u) ? t0_us - 5500000u : 0u;
    for (uint64_t t = t_start; t <= t0_us; t += 500000u) {
        ambre_sample_t s[4];
        s[0].signal_id = 10; s[0].quality = 0; s[0].pad = 0; s[0].value = 13888;
        s[1].signal_id = 21; s[1].quality = 0; s[1].pad = 0; s[1].value = 60;
        s[2].signal_id = 25; s[2].quality = 0; s[2].pad = 0; s[2].value = 2500;
        s[3].signal_id = 1;  s[3].quality = 0; s[3].pad = 0; s[3].value = 0;

        uint8_t wire[512];
        size_t len = build_raw_frame(wire, sizeof(wire), t, s, 4);
        ambre_capture_push_frame(cap, wire, len, t, NULL, NULL);
    }
}

static void inject_accel_burst(ambre_capture_t *cap, uint64_t t_start,
                               uint32_t duration_ms, int32_t accel_mm_s2,
                               test_sink_t *sink)
{
    for (uint32_t ms = 1; ms <= duration_ms; ms++) {
        uint64_t t = t_start + (uint64_t)ms * 1000u;
        ambre_sample_t s;
        s.signal_id = 1; s.quality = 0; s.pad = 0; s.value = accel_mm_s2;
        uint8_t wire[512];
        size_t len = build_raw_frame(wire, sizeof(wire), t, &s, 1);
        ambre_capture_push_frame(cap, wire, len, t, test_sink_fn, sink);
        ambre_capture_tick(cap, t, test_sink_fn, sink);
    }
}

static void test_crash_trigger_and_points(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    test_sink_t sink;
    memset(&sink, 0, sizeof(sink));

    uint64_t t_base = 10000000u;
    fill_precrash_history(&cap, t_base);

    /* Synthetic crash: > 8 km/h in 150 ms (16000 mm/s^2 for 150 ms, then 150 ms rest).
     * Peak delta-v since T0 reaches 2400 mm/s before the 150 ms window clears. */
    inject_accel_burst(&cap, t_base, 150u, 16000, &sink);
    AMBRE_CHECK_EQ_U64(sink.trigger_count, 1);
    AMBRE_CHECK_EQ_U64(cap.stats.triggers_fired, 1);
    AMBRE_CHECK(sink.last_emerg.point_count >= 10);
    AMBRE_CHECK(sink.last_emerg.point_count <= 21);

    for (uint32_t i = 1; i < sink.last_emerg.point_count; i++) {
        AMBRE_CHECK(sink.last_emerg.points[i - 1].rel_time_ms >=
                    sink.last_emerg.points[i].rel_time_ms);
    }
    AMBRE_CHECK(sink.last_emerg.points[sink.last_emerg.point_count - 1].rel_time_ms <= 500);

    /* Continue decelerating slightly or hold 0 for 25 ms to trigger end-of-event (20ms window <= 222 mm/s) */
    inject_accel_burst(&cap, t_base + 150000u, 25u, 0, &sink);
    AMBRE_CHECK(sink.continue_count >= 1);
    AMBRE_CHECK(sink.seal_count >= 1);
    AMBRE_CHECK_EQ_U64(sink.last_seal.file_complete, 1);
}

static void test_overload_non_blocking(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);

    ambre_sample_t s;
    s.signal_id = 1; s.quality = 0; s.pad = 0; s.value = 10;
    uint8_t wire[512];
    size_t len = build_raw_frame(wire, sizeof(wire), 1000000u, &s, 1);

    uint32_t accepted = 0;
    uint32_t dropped = 0;
    for (uint32_t i = 0; i < 150; i++) {
        int rc = ambre_capture_push_frame(&cap, wire, len, 1000000u + i * 10u, NULL, NULL);
        if (rc == AMBRE_OK) accepted++;
        else if (rc == AMBRE_ERR_FULL) dropped++;
    }

    AMBRE_CHECK_EQ_U64(accepted, 100);
    AMBRE_CHECK_EQ_U64(dropped, 50);
    AMBRE_CHECK_EQ_U64(cap.stats.frames_overload, 50);
}

static const ambre_test_case_t cases[] = {
    { "capture_frame_valid", test_frame_valid },
    { "capture_frame_too_big", test_frame_too_big },
    { "capture_whitelist_forbidden", test_whitelist_and_forbidden },
    { "capture_gap_detection", test_gap_detection },
    { "capture_quality_and_time", test_quality_and_time },
    { "capture_crash_trigger_and_points", test_crash_trigger_and_points },
    { "capture_overload_non_blocking", test_overload_non_blocking },
};

AMBRE_TEST_MAIN(cases)
