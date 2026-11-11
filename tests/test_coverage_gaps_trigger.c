/*
 * test_coverage_gaps_trigger.c - Branch coverage of the trigger engine.
 *
 * Spec 12.10 asks for more than 95% of the lines of fmt/ covered. This file
 * reaches the branches of ambre_trigger.c and ambre_trigger_window.c that the
 * regulation cases of test_trigger.c do not: the 25 km/h lock decision at the
 * very first firing sample (DEC-04), the lateral maximum tracked during an
 * event (GEL-05), the release of an old crash group (DEC-09), the drop of a
 * deque slot that the ring is about to overwrite, and an empty window.
 *
 * No existing test is modified or weakened: these are extra scenarios only.
 */

#include "ambre_test.h"
#include "ambre_trigger.h"
#include "ambre_trigger_window.h"
#include <string.h>

/* 1000 g at a 1 ms cadence: one sample alone crosses the 25 km/h lock
 * threshold, so the decision is taken on the first firing sample. */
#define HUGE_ACCEL 10000000
#define BIG_ACCEL   1000000

static void init_default(ambre_trig_state_t *st)
{
    ambre_trig_cfg_t cfg;

    ambre_trig_cfg_default(&cfg);
    ambre_trig_init(st, &cfg);
}

/* The default configuration declares a front restraint; DEC-04 only locks on
 * the delta-v rule when the vehicle has none. */
static void init_without_restraint(ambre_trig_state_t *st)
{
    ambre_trig_cfg_t cfg;

    ambre_trig_cfg_default(&cfg);
    cfg.has_front_restraint = 0u;
    ambre_trig_init(st, &cfg);
}

static void test_lock_decision_on_the_first_firing_sample(void)
{
    ambre_trig_state_t st;
    ambre_trig_out_t out;

    init_without_restraint(&st);
    AMBRE_CHECK_EQ_U64(st.cfg.has_front_restraint, 0u);

    /* The first sample only starts the clock (no delta yet). */
    AMBRE_CHECK_OK(ambre_trig_push(&st, 1000u, 0, 0, NULL, &out));
    AMBRE_CHECK(!out.fired);

    /* One sample above 25 km/h of delta-v: DEC-04 fires and locks at once. */
    AMBRE_CHECK_OK(ambre_trig_push(&st, 2000u, HUGE_ACCEL, 0, NULL, &out));
    AMBRE_CHECK(out.fired);
    AMBRE_CHECK_EQ_U64(out.reason, (uint64_t)AMBRE_TRIG_DV25_LOCK);
    AMBRE_CHECK(out.locked);
    AMBRE_CHECK(st.locked);
    AMBRE_CHECK(st.in_event);
}

static void test_lateral_maximum_is_tracked_during_the_event(void)
{
    ambre_trig_state_t st;
    ambre_trig_out_t out;

    init_default(&st);
    AMBRE_CHECK_OK(ambre_trig_push(&st, 1000u, 0, 0, NULL, &out));
    AMBRE_CHECK_OK(ambre_trig_push(&st, 2000u, HUGE_ACCEL, 0, NULL, &out));
    AMBRE_CHECK(out.fired);

    /* After T0 the vehicle also slides sideways: the lateral maximum and its
     * instant must be reported (GEL-05). */
    AMBRE_CHECK_OK(ambre_trig_push(&st, 3000u, 0, BIG_ACCEL, NULL, &out));
    AMBRE_CHECK_EQ_I64(out.max_dv_lat_mm_s, 1000);
    AMBRE_CHECK_EQ_U64(out.max_dv_lat_at_ms, 1u);   /* 1 ms after T0 */
    AMBRE_CHECK(st.in_event);
}

static void test_event_closed_releases_an_old_crash_group(void)
{
    ambre_trig_state_t st;
    ambre_trig_out_t out;
    uint64_t t0;

    init_default(&st);
    AMBRE_CHECK_OK(ambre_trig_push(&st, 1000u, 0, 0, NULL, &out));
    AMBRE_CHECK_OK(ambre_trig_push(&st, 2000u, HUGE_ACCEL, 0, NULL, &out));
    AMBRE_CHECK(out.fired);
    t0 = out.t0_ts_us;
    AMBRE_CHECK_EQ_U64(out.event_number, 1u);

    /* Closed inside the same-crash window: the group survives (DEC-09). */
    ambre_trig_event_closed(&st, t0 + 1000u);
    AMBRE_CHECK(st.have_crash_group);

    /* Closed after the window: the group is released, the next event starts a
     * new numbering. */
    ambre_trig_event_closed(&st, t0 + (uint64_t)st.cfg.same_crash_window_ms * 1000u);
    AMBRE_CHECK(!st.have_crash_group);
    AMBRE_CHECK_EQ_U64(st.last_event_end_ts_us,
                       t0 + (uint64_t)st.cfg.same_crash_window_ms * 1000u);
}

static void test_deque_drops_the_slot_the_ring_overwrites(void)
{
    ambre_trig_state_t st;
    ambre_trig_axis_t ax;
    uint64_t last_ts = 0u;

    init_default(&st);
    ax = ambre_trig_axis_long(&st);

    /* A strictly increasing delta-v keeps every slot in the minimum deque and
     * only the newest one in the maximum deque. */
    for (uint16_t i = 0u; i < AMBRE_TRIG_RING_SLOTS; i++) {
        last_ts = 1000u * (uint64_t)i;
        ambre_trig_axis_push(ax, last_ts, (int64_t)i);
    }
    AMBRE_CHECK_EQ_U64(ax.ring->count, AMBRE_TRIG_RING_SLOTS);
    AMBRE_CHECK_EQ_U64(ax.dmin->count, AMBRE_TRIG_RING_SLOTS);
    AMBRE_CHECK_EQ_U64(ax.dmax->count, 1u);

    /* One more push overwrites the oldest slot: the deque must drop it. */
    ambre_trig_axis_push(ax, 1000u * AMBRE_TRIG_RING_SLOTS,
                         (int64_t)AMBRE_TRIG_RING_SLOTS);
    AMBRE_CHECK_EQ_U64(ax.dmin->count, AMBRE_TRIG_RING_SLOTS);
    AMBRE_CHECK_EQ_U64(ambre_trig_dq_front(ax.dmin), 1u);
    AMBRE_CHECK_EQ_U64(ambre_trig_dq_front(ax.dmax), 0u);
    /* The window still answers with the right extremes. */
    AMBRE_CHECK_EQ_I64(ambre_trig_axis_window_delta(ax,
                                                    1000u * AMBRE_TRIG_RING_SLOTS,
                                                    150u), 150);
}

static void test_window_delta_is_zero_when_nothing_is_recent(void)
{
    ambre_trig_state_t st;
    ambre_trig_axis_t ax;
    uint16_t pos = 0xFFFFu;

    init_default(&st);
    ax = ambre_trig_axis_lat(&st);
    for (uint16_t i = 0u; i < 5u; i++) {
        ambre_trig_axis_push(ax, 1000u * (uint64_t)i, (int64_t)i);
    }
    /* Every candidate is older than the window: no extreme can be reported. */
    AMBRE_CHECK(!ambre_trig_dq_first_since(ax.dmin, ax.ring, 9995000u, &pos));
    AMBRE_CHECK_EQ_I64(ambre_trig_axis_window_delta(ax, 10000000u, 5u), 0);
    /* The cutoff helper clamps at zero instead of wrapping. */
    AMBRE_CHECK_EQ_U64(ambre_trig_window_cutoff(1000u, 5u), 0u);
}

static const ambre_test_case_t cases[] = {
    { "lock_decision_on_the_first_firing_sample", test_lock_decision_on_the_first_firing_sample },
    { "lateral_maximum_is_tracked_during_the_event", test_lateral_maximum_is_tracked_during_the_event },
    { "event_closed_releases_an_old_crash_group", test_event_closed_releases_an_old_crash_group },
    { "deque_drops_the_slot_the_ring_overwrites", test_deque_drops_the_slot_the_ring_overwrites },
    { "window_delta_is_zero_when_nothing_is_recent", test_window_delta_is_zero_when_nothing_is_recent },
};

AMBRE_TEST_MAIN(cases)
