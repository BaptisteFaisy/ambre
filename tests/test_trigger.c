/*
 * test_trigger.c — Unit tests for the crash trigger engine (DEC-01..DEC-13).
 */

#include "ambre_test.h"
#include "ambre_trigger.h"
#include <string.h>

static void push_constant_accel(ambre_trig_state_t *st, uint64_t start_ts_us,
                                int32_t a_long, int32_t a_lat,
                                uint32_t count, uint32_t dt_us)
{
    uint64_t ts = start_ts_us;
    for (uint32_t i = 0; i < count; i++) {
        ambre_trig_out_t out;
        ambre_trig_push(st, ts, a_long, a_lat, NULL, &out);
        ts += dt_us;
    }
}

static void test_dec08_case1_79kmh_150ms_no_trigger(void)
{
    /* DEC-08 case 1: ~7.9 km/h over 150 ms => no trigger.
     * accel 14629 mm/s2 at 1 ms cadence: per-interval dv = trunc(14629*1000/1e6)
     * = 14 mm/s (A3 truncates toward zero). After 150 intervals dv = 2100 mm/s
     * (the nominal 7.9 km/h = 2194 mm/s loses 0.629 mm/s/ms to truncation).
     * 2100 < 2222 threshold => no trigger, strictly below (A9). This uses the
     * exact same construction as host/verify/gen_vectors.py case 1. */
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    push_constant_accel(&st, 1000, 14629, 0, 150, 1000);
    ambre_trig_out_t out;
    ambre_trig_push(&st, 151000, 0, 0, NULL, &out);
    AMBRE_CHECK(!out.fired && !st.in_event);
}

static void test_dec08_case2_81kmh_150ms_triggers(void)
{
    /* DEC-08 case 2: ~8.1 km/h over 150 ms => triggers.
     * accel 15000 mm/s2 at 1 ms cadence: per-interval dv = 15 mm/s exactly.
     * dv crosses 2222 at interval 149 (2235 mm/s) => fires there, with T0 at
     * that sample (DEC-05, A9 strict >). Nominal 8.1 km/h = 2250 mm/s.
     * Same construction as gen_vectors.py case 2. */
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    bool fired = false;
    uint64_t ts = 1000;
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 15000, 0, NULL, &out);
        if (out.fired) fired = true;
    }
    AMBRE_CHECK(fired && st.in_event);
}

/* DEC-08 case 3 driver: 152 ramp samples at 14000 mm/s2 then one 137000 spike.
 * Returns true if any sample fired; *last holds the final decision. */
static bool run_slow_ramp(uint8_t mode, ambre_trig_state_t *st,
                          ambre_trig_out_t *last)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    cfg.mode = mode;
    ambre_trig_init(st, &cfg);

    bool fired = false;
    uint64_t ts = 1000;
    for (int i = 0; i < 152; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(st, ts, 14000, 0, NULL, &out);
        if (out.fired) fired = true;
    }
    ambre_trig_push(st, ts, 137000, 0, NULL, last);
    return fired || last->fired;
}

static void test_dec08_case3_81kmh_151ms_no_trigger_anchored(void)
{
    /* DEC-08 case 3: ~8.1 km/h reached only in 151 ms must NOT trigger. This
     * case is the reason ANCHORED mode exists.
     *
     * WHY ANCHORED IS NECESSARY: the regulation-faithful WINDOW reading ("there
     * exists a sub-window of <= 150 ms whose delta exceeds the threshold") still
     * fires on a slow ramp, because once the ramp is long enough some 150 ms
     * sub-window does exceed the threshold. ANCHORED instead measures the
     * excursion from the sample where delta-v was last at rest and only counts
     * it within win_trigger_ms (150 ms) of that anchor, so a ramp that needs
     * 151 ms is correctly rejected.
     *
     * CONSTRUCTION: 1 ms cadence, 152 samples at 14000 mm/s2 => per-interval dv
     * = trunc(14.0) = 14 mm/s, so dv(151) = 2114 mm/s (excursion 2100 from the
     * anchor, below 2222). Sample 152 then adds 137 mm/s, bringing dv to
     * 2251 mm/s: an excursion of 2237 mm/s, genuinely above the threshold, but
     * reached 151 ms after the anchor. ANCHORED must not fire; WINDOW does fire
     * because its 150 ms window then holds dv from 28 to 2251 (delta 2223).
     * Both assertions are checked below, side by side. */
    ambre_trig_state_t st;
    ambre_trig_out_t last;
    bool fired_anchored = run_slow_ramp(AMBRE_TRIG_MODE_ANCHORED, &st, &last);

    /* Sanity: the threshold really was exceeded by the final sample. */
    AMBRE_CHECK(last.dv_long_mm_s > AMBRE_DV_TRIGGER_MM_S);
    AMBRE_CHECK(!fired_anchored && !st.in_event);

    ambre_trig_state_t st_w;
    bool fired_window = run_slow_ramp(AMBRE_TRIG_MODE_WINDOW, &st_w, &last);
    AMBRE_CHECK(fired_window && st_w.in_event);
}

static void test_dec02_lateral_trigger(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    bool fired = false;
    uint8_t reason = AMBRE_TRIG_NONE;
    uint64_t ts = 1000;
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 0, 15000, NULL, &out);
        if (out.fired) { fired = true; reason = out.reason; }
    }
    AMBRE_CHECK(fired);
    AMBRE_CHECK_EQ_U64(reason, AMBRE_TRIG_DV_LAT);
}

static void test_dec04_lock_with_front_restraint(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    cfg.has_front_restraint = 1;
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    bool fired = false;
    uint64_t ts = 1000;
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 100000, 0, NULL, &out);
        if (out.fired) fired = true;
    }
    AMBRE_CHECK(fired && !st.locked);
}

static void test_dec04_lock_without_front_restraint(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    cfg.has_front_restraint = 0;
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    bool fired = false;
    uint64_t ts = 1000;
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 100000, 0, NULL, &out);
        if (out.fired) fired = true;
    }
    AMBRE_CHECK(fired && st.locked);
}

static void test_dec03_external_restraint_locks(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    push_constant_accel(&st, 1000, 1000, 0, 10, 1000);
    ambre_trig_ext_t ext = { .restraint_deployed = true };
    ambre_trig_out_t out;
    uint64_t sig_ts = 20000;
    ambre_trig_push(&st, sig_ts, 0, 0, &ext, &out);
    AMBRE_CHECK(out.fired && out.locked);
    AMBRE_CHECK_EQ_U64(out.reason, AMBRE_TRIG_RESTRAINT);
    AMBRE_CHECK_EQ_U64(out.t0_ts_us, sig_ts);
}

static void test_dec10_vru_locks(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    ambre_trig_ext_t ext = { .vru_activated = true };
    ambre_trig_out_t out;
    ambre_trig_push(&st, 10000, 0, 0, &ext, &out);
    AMBRE_CHECK(out.fired && out.locked);
    AMBRE_CHECK_EQ_U64(out.reason, AMBRE_TRIG_VRU);
}

static void test_dec11_off_profile_never_locks(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;

    ambre_trig_init(&st, &cfg);
    ambre_trig_ext_t e1 = { .emergency_brake = true };
    ambre_trig_out_t o1;
    ambre_trig_push(&st, 10000, 0, 0, &e1, &o1);
    AMBRE_CHECK(o1.fired && !o1.locked);
    AMBRE_CHECK_EQ_U64(o1.reason, AMBRE_TRIG_EMERG_BRAKE);

    ambre_trig_init(&st, &cfg);
    ambre_trig_ext_t e2 = { .aeb_intervention = true };
    ambre_trig_out_t o2;
    ambre_trig_push(&st, 10000, 0, 0, &e2, &o2);
    AMBRE_CHECK(o2.fired && !o2.locked);
    AMBRE_CHECK_EQ_U64(o2.reason, AMBRE_TRIG_AEB);

    ambre_trig_init(&st, &cfg);
    ambre_trig_ext_t e3 = { .service_command = true };
    ambre_trig_out_t o3;
    ambre_trig_push(&st, 10000, 0, 0, &e3, &o3);
    AMBRE_CHECK(o3.fired && !o3.locked);
    AMBRE_CHECK_EQ_U64(o3.reason, AMBRE_TRIG_SERVICE);
}

static void test_dec05_t0_detection(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    bool fired = false;
    uint64_t t0 = 0, ts = 1000;
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 15000, 0, NULL, &out);
        if (out.fired && !fired) { fired = true; t0 = out.t0_ts_us; }
    }
    AMBRE_CHECK(fired && t0 > 0);
}

static void test_dec06_end_of_event(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    uint64_t ts = 1000;
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 15000, 0, NULL, &out);
    }
    AMBRE_CHECK(st.in_event);

    bool ended = false;
    for (int i = 0; i < 100; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 0, 0, NULL, &out);
        if (out.event_end) ended = true;
    }
    AMBRE_CHECK(ended && !st.in_event);
}

static void test_dec09_same_crash_grouping(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    uint64_t ts = 1000;
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 15000, 0, NULL, &out);
    }
    for (int i = 0; i < 100; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 0, 0, NULL, &out);
    }
    ambre_trig_event_closed(&st, ts);

    ts += 1000000; /* 1 s later, inside 5 s grouping window */
    for (int i = 0; i < 150; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 15000, 0, NULL, &out);
        if (out.fired) {
            AMBRE_CHECK_EQ_U64(out.event_number, 2);
            AMBRE_CHECK(out.time_since_first_ms > 0);
        }
    }
}

static void test_dec13_invalid_sample_excluded(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    push_constant_accel(&st, 1000, 10000, 0, 10, 1000);
    ambre_trig_ext_t ext = { .sample_invalid = true };
    ambre_trig_out_t out;
    ambre_trig_push(&st, 12000, 1000000, 0, &ext, &out);
    int64_t dv_before = out.dv_long_mm_s;
    ambre_trig_push(&st, 13000, 10000, 0, NULL, &out);
    int64_t dv_diff = out.dv_long_mm_s - dv_before;
    AMBRE_CHECK(dv_diff < 20); /* ~10 mm/s, invalid 1000000 ignored */
}

static void test_fixed_memory_size(void)
{
    size_t sz = sizeof(ambre_trig_state_t);
    printf("sizeof(ambre_trig_state_t) = %zu bytes\n", sz);
    AMBRE_CHECK(sz < 16384);
}

static void test_fixed_memory_no_overflow_200k(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t st;
    ambre_trig_init(&st, &cfg);
    uint64_t ts = 1000;
    for (int i = 0; i < 200000; i++, ts += 1000) {
        ambre_trig_out_t out;
        ambre_trig_push(&st, ts, 1000, 500, NULL, &out);
        if (i % 50000 == 0) {
            AMBRE_CHECK(st.ring_long.count <= AMBRE_TRIG_RING_SLOTS);
        }
    }
    AMBRE_CHECK(st.ring_long.count <= AMBRE_TRIG_RING_SLOTS);
}

static void test_determinism(void)
{
    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_state_t s1, s2;
    ambre_trig_init(&s1, &cfg);
    ambre_trig_init(&s2, &cfg);
    ambre_trig_out_t o1[100], o2[100];
    uint64_t ts = 1000;
    for (int i = 0; i < 100; i++, ts += 1000) {
        ambre_trig_push(&s1, ts, 15000, 0, NULL, &o1[i]);
        ambre_trig_push(&s2, ts, 15000, 0, NULL, &o2[i]);
    }
    AMBRE_CHECK_MEM_EQ(o1, o2, sizeof(o1));
}

static const ambre_test_case_t cases[] = {
    { "dec08_case1_79kmh_150ms_no_trigger", test_dec08_case1_79kmh_150ms_no_trigger },
    { "dec08_case2_81kmh_150ms_triggers", test_dec08_case2_81kmh_150ms_triggers },
    { "dec08_case3_81kmh_151ms_no_trigger_anchored", test_dec08_case3_81kmh_151ms_no_trigger_anchored },
    { "dec02_lateral_trigger", test_dec02_lateral_trigger },
    { "dec04_lock_with_front_restraint", test_dec04_lock_with_front_restraint },
    { "dec04_lock_without_front_restraint", test_dec04_lock_without_front_restraint },
    { "dec03_external_restraint_locks", test_dec03_external_restraint_locks },
    { "dec10_vru_locks", test_dec10_vru_locks },
    { "dec11_off_profile_never_locks", test_dec11_off_profile_never_locks },
    { "dec05_t0_detection", test_dec05_t0_detection },
    { "dec06_end_of_event", test_dec06_end_of_event },
    { "dec09_same_crash_grouping", test_dec09_same_crash_grouping },
    { "dec13_invalid_sample_excluded", test_dec13_invalid_sample_excluded },
    { "fixed_memory_size", test_fixed_memory_size },
    { "fixed_memory_no_overflow_200k", test_fixed_memory_no_overflow_200k },
    { "determinism", test_determinism }
};

AMBRE_TEST_MAIN(cases)
