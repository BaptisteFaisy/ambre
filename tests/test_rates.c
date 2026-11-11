/*
 * test_rates.c — dedicated per-rate test for ACQ-03 (close-out, week 5).
 *
 * ACQ-03: supported cadences are 1 kHz (accelerations), 100 Hz (dynamic
 * signals) and 10 Hz (states), and the signal list is fixed at build time
 * (docs/FORMAT.md §5). This suite pins:
 *
 *  1. the build-time table itself: every id 0..1023 (plus spot ids above)
 *     is classified exactly as docs/FORMAT.md §5 says — 17 whitelisted
 *     signals, three rate bands (1 kHz: ids 1..3; 100 Hz: ids 10..13 and
 *     20..25; 10 Hz: ids 40..43), forbidden band 90..99, everything else
 *     rejected;
 *  2. the sampling behaviour at each rate: a producer sending one sample
 *     per period for one simulated second is counted exactly (1000 / 100 /
 *     10 samples) and raises no GAP;
 *  3. the GAP threshold per rate: an interruption of exactly three periods
 *     is still nominal, anything above three periods emits one GAP record
 *     naming the interrupted signal (ACQ-06 boundary, per rate);
 *  4. mixed cadences: the three rates interleaved in one capture instance
 *     track their gaps independently — stopping only the 10 Hz signal
 *     raises exactly one GAP, for that signal.
 *
 * No QNX dependency: this is pure portable capture logic on the PC backend.
 */

#include "ambre_test.h"
#include "../target/capture/edr_capture.h"
#include "../target/edr_ipc.h"
#include "../fmt/ambre_fmt.h"
#include <string.h>

/* ------------------------------------------------------------ wire build -- */

/* Build a one-message frame with the frozen wire helper (single place). */
static size_t build_frame(void *buf, size_t cap, uint64_t ts_us,
                          uint16_t signal_id, uint32_t value)
{
    ambre_sample_t s;
    size_t body_len = sizeof(ambre_ipc_frame_body_t) + sizeof(s);
    ambre_ipc_hdr_t *h = (ambre_ipc_hdr_t *)buf;
    ambre_ipc_frame_body_t *fb;
    size_t total;

    if (sizeof(ambre_ipc_hdr_t) + body_len > cap) return 0;
    total = ambre_ipc_header_init(h, AMBRE_MSG_FRAME, (uint32_t)body_len);
    if (total == 0) return 0;
    fb = (ambre_ipc_frame_body_t *)((uint8_t *)buf + sizeof(*h));
    fb->first_ts_us = ts_us;
    fb->sample_count = 1u;
    fb->producer_id = 7u;
    s.signal_id = signal_id;
    s.quality = 0u;
    s.pad = 0u;
    s.value = (int32_t)value;
    memcpy((uint8_t *)fb + sizeof(*fb), &s, sizeof(s));
    return total;
}

/* Send one single-sample frame; fail the test on any refusal. */
static void send_sample(ambre_capture_t *cap, uint64_t ts_us,
                        uint16_t signal_id, uint32_t value)
{
    uint8_t wire[128];
    size_t len = build_frame(wire, sizeof(wire), ts_us, signal_id, value);
    AMBRE_CHECK(len > 0);
    AMBRE_CHECK_EQ_U64(ambre_capture_push_frame(cap, wire, len, ts_us,
                                                NULL, NULL), AMBRE_OK);
}

/* ------------------------------------------------------------------ sink -- */

typedef struct {
    uint32_t gap_count;
    uint32_t last_reason;
} gap_sink_t;

static int gap_sink_fn(void *ctx, uint8_t msg_type,
                       const void *body, size_t body_len)
{
    gap_sink_t *g = (gap_sink_t *)ctx;
    if (msg_type == (uint8_t)AMBRE_MSG_GAP && body_len >= sizeof(ambre_gap_t)) {
        const ambre_gap_t *rec = (const ambre_gap_t *)body;
        g->gap_count++;
        g->last_reason = rec->reason;
    }
    return 0;
}

/* ------------------------------------------------- 1. the fixed table ------ */

static bool expected_whitelist(uint32_t id)
{
    if (id >= 1u && id <= 3u) return true;      /* accelerations, 1 kHz    */
    if (id >= 10u && id <= 13u) return true;    /* wheel speeds, 100 Hz    */
    if (id >= 20u && id <= 25u) return true;    /* dynamic signals, 100 Hz */
    if (id >= 40u && id <= 43u) return true;    /* states, 10 Hz           */
    return false;
}

static uint32_t expected_period_us(uint32_t id)
{
    if (id >= 1u && id <= 3u) return 1000u;
    if (expected_whitelist(id) && id >= 10u && id <= 25u) return 10000u;
    if (id >= 40u && id <= 43u) return 100000u;
    return 0u;
}

static void check_one_id(uint32_t id, uint32_t *whitelisted,
                         uint32_t *b1khz, uint32_t *b100hz, uint32_t *b10hz)
{
    bool wl = ambre_capture_is_whitelisted((uint16_t)id);
    uint32_t period = ambre_capture_signal_period_us((uint16_t)id);
    AMBRE_CHECK_MSG(wl == expected_whitelist(id),
                    "id %u: whitelist %d, expected %d",
                    id, (int)wl, (int)expected_whitelist(id));
    if (wl) {
        AMBRE_CHECK_EQ_U64(period, expected_period_us(id));
        (*whitelisted)++;
        if (period == 1000u) (*b1khz)++;
        if (period == 10000u) (*b100hz)++;
        if (period == 100000u) (*b10hz)++;
    }
    AMBRE_CHECK(ambre_capture_is_forbidden((uint16_t)id) ==
                (id >= 90u && id <= 99u));
}

static void test_table_fixed_at_build_time(void)
{
    uint32_t whitelisted = 0u, b1khz = 0u, b100hz = 0u, b10hz = 0u;

    for (uint32_t id = 0u; id <= 1023u; id++) {
        check_one_id(id, &whitelisted, &b1khz, &b100hz, &b10hz);
    }
    /* Spot-check far ids: nothing above the table may sneak in. */
    check_one_id(1024u, &whitelisted, &b1khz, &b100hz, &b10hz);
    check_one_id(4096u, &whitelisted, &b1khz, &b100hz, &b10hz);
    check_one_id(65535u, &whitelisted, &b1khz, &b100hz, &b10hz);

    /* docs/FORMAT.md §5: exactly 17 signals, split 3 / 10 / 4 over the
     * three supported rates 1 kHz / 100 Hz / 10 Hz. */
    AMBRE_CHECK_EQ_U64(whitelisted, 17u);
    AMBRE_CHECK_EQ_U64(b1khz, 3u);
    AMBRE_CHECK_EQ_U64(b100hz, 10u);
    AMBRE_CHECK_EQ_U64(b10hz, 4u);
}

/* ------------------------------------- 2+3. one rate at a time, with GAP --- */

/* Drive `count` samples of `id` at its nominal period from `t0_us`, then
 * verify the counters; `count` is chosen so the run lasts one second. */
static void drive_one_second(ambre_capture_t *cap, uint16_t id,
                             uint64_t t0_us, uint32_t count)
{
    uint32_t period = ambre_capture_signal_period_us(id);
    AMBRE_CHECK(period > 0u);
    for (uint32_t i = 0u; i < count; i++) {
        send_sample(cap, t0_us + (uint64_t)i * period, id, 100u + i);
    }
    AMBRE_CHECK_EQ_U64(cap->stats.samples_received, count);
    AMBRE_CHECK_EQ_U64(cap->stats.samples_rejected, 0u);
    AMBRE_CHECK_EQ_U64(cap->stats.gaps_detected, 0u);
}

/* Exactly three periods: still nominal. Above three: one GAP naming `id`. */
static void check_gap_boundary(ambre_capture_t *cap, uint16_t id,
                               uint64_t last_us)
{
    uint64_t period = ambre_capture_signal_period_us(id);
    uint64_t t_edge = last_us + 3u * period;
    uint64_t t_gap = t_edge + 3u * period + 1u;
    gap_sink_t sink;
    uint8_t wire[128];
    size_t len;

    memset(&sink, 0, sizeof(sink));
    send_sample(cap, t_edge, id, 7u);               /* == 3 periods: no gap */
    AMBRE_CHECK_EQ_U64(cap->stats.gaps_detected, 0u);

    len = build_frame(wire, sizeof(wire), t_gap, id, 8u);
    AMBRE_CHECK(len > 0);
    AMBRE_CHECK_EQ_U64(ambre_capture_push_frame(cap, wire, len, t_gap,
                                                gap_sink_fn, &sink), AMBRE_OK);
    AMBRE_CHECK_EQ_U64(cap->stats.gaps_detected, 1u);
    AMBRE_CHECK_EQ_U64(sink.gap_count, 1u);
    AMBRE_CHECK_EQ_U64(sink.last_reason, id);
}

static void test_rate_1khz(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    drive_one_second(&cap, 1u, 1000000u, 1000u);   /* 1 kHz x 1 s = 1000 */
    check_gap_boundary(&cap, 1u, 1000000u + 999u * 1000u);
}

static void test_rate_100hz(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    drive_one_second(&cap, 10u, 1000000u, 100u);  /* 100 Hz x 1 s = 100 */
    check_gap_boundary(&cap, 10u, 1000000u + 99u * 10000u);
}

static void test_rate_10hz(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    drive_one_second(&cap, 40u, 1000000u, 10u);   /* 10 Hz x 1 s = 10   */
    check_gap_boundary(&cap, 40u, 1000000u + 9u * 100000u);
}

/* --------------------------------------------------- 4. mixed cadences ---- */

static void test_mixed_rates_independent_gaps(void)
{
    ambre_capture_t cap;
    gap_sink_t sink;
    uint64_t t0 = 1000000u;

    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    memset(&sink, 0, sizeof(sink));

    /* One simulated second: sig 1 every ms, sig 10 every 10 ms, sig 40
     * every 100 ms — 1000 + 100 + 10 samples, no gap. */
    for (uint32_t ms = 0u; ms <= 1000u; ms++) {
        uint64_t ts = t0 + (uint64_t)ms * 1000u;
        uint8_t wire[128];
        size_t len = build_frame(wire, sizeof(wire), ts, 1u, 50u);
        AMBRE_CHECK(len > 0);
        ambre_capture_push_frame(&cap, wire, len, ts, gap_sink_fn, &sink);
        if (ms % 10u == 0u) send_sample(&cap, ts, 10u, 60u);
        if (ms % 100u == 0u) send_sample(&cap, ts, 40u, 70u);
    }
    AMBRE_CHECK_EQ_U64(cap.stats.samples_received, 1001u + 101u + 11u);
    AMBRE_CHECK_EQ_U64(sink.gap_count, 0u);

    /* Keep the 1 kHz and 100 Hz signals alive, starve the 10 Hz one for
     * 500 ms, then feed it again: exactly one GAP, naming signal 40. */
    for (uint32_t ms = 1001u; ms < 1500u; ms++) {
        uint64_t ts = t0 + (uint64_t)ms * 1000u;
        send_sample(&cap, ts, 1u, 50u);
        if (ms % 10u == 0u) send_sample(&cap, ts, 10u, 60u);
    }
    {   /* the starving signal returns at +500 ms: 5 periods > 3 (GAP) */
        uint64_t ts = t0 + 1500u * 1000u;
        uint8_t wire[128];
        size_t len = build_frame(wire, sizeof(wire), ts, 40u, 70u);
        AMBRE_CHECK(len > 0);
        AMBRE_CHECK_EQ_U64(ambre_capture_push_frame(&cap, wire, len, ts,
                                                    gap_sink_fn, &sink),
                           AMBRE_OK);
    }
    AMBRE_CHECK_EQ_U64(sink.gap_count, 1u);
    AMBRE_CHECK_EQ_U64(sink.last_reason, 40u);
    AMBRE_CHECK_EQ_U64(cap.stats.gaps_detected, 1u);
}

/* A non-whitelisted id is rejected and counted at every rate slot (ACQ-04
 * cross-check inside the per-rate suite): ids 14..19 sit inside the 100 Hz
 * period band but outside the fixed list. */
static void test_off_list_ids_never_sample(void)
{
    ambre_capture_t cap;
    ambre_capture_init(&cap, AMBRE_PROFILE_EU, 1u);
    for (uint16_t id = 4u; id <= 9u; id++) send_sample(&cap, 1000000u, id, 1u);
    for (uint16_t id = 14u; id <= 19u; id++) send_sample(&cap, 1001000u, id, 1u);
    AMBRE_CHECK_EQ_U64(cap.stats.samples_rejected, 12u);
    AMBRE_CHECK_EQ_U64(cap.stats.gaps_detected, 0u);
}

static const ambre_test_case_t cases[] = {
    { "rates_table_fixed_at_build_time", test_table_fixed_at_build_time },
    { "rate_1khz_cadence_and_gap", test_rate_1khz },
    { "rate_100hz_cadence_and_gap", test_rate_100hz },
    { "rate_10hz_cadence_and_gap", test_rate_10hz },
    { "rates_mixed_independent_gaps", test_mixed_rates_independent_gaps },
    { "rates_off_list_ids_rejected", test_off_list_ids_never_sample },
};

AMBRE_TEST_MAIN(cases)
