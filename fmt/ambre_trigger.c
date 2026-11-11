/*
 * ambre_trigger.c — Crash trigger / T0 / end-of-event engine (UN R160 5.3).
 *
 * Portable C11, fixed memory, bounded time (DEC-07): no allocation, no
 * recursion, ring buffers plus monotonic min/max deques over prefix sums.
 * Every per-sample cost is O(1) amortised.
 *
 * ARITHMETIC CONTRACT A1..A9 (fmt/ambre_trigger.h is normative; the Python
 * twin host/verify/ambre_trigger.py must reproduce these decisions bit for
 * bit, DEC-12):
 *
 *  A1. Timestamps are monotonic microseconds (uint64).
 *  A2. Accelerations are int32 mm/s^2, cumulative delta-v is int64 mm/s.
 *  A3. Rectangle rule on the CURRENT sample:
 *          dt_us = ts[i] - ts[i-1] clamped to [0, AMBRE_TRIG_DT_MAX_US]
 *          dv_cum += idiv_trunc((int64)accel[i] * dt_us, 1000000)
 *      idiv_trunc truncates toward zero (C `/`), never floor.
 *  A4. A window holds the ring entries with ts >= ts_now - win_ms*1000
 *      (inclusive); window_delta = max(dv) - min(dv) over them.
 *  A5. Trigger, mode WINDOW (default): first sample where the 150 ms window
 *      delta exceeds dv_trigger_mm_s on the long or the lat axis. Mode
 *      ANCHORED: the excursion is measured from the last sample where delta-v
 *      was at rest, and only counts within win_trigger_ms of that anchor.
 *  A6. T0 (DEC-05) is the FIRST sample where the 20 ms long-axis window delta
 *      (or the 5 ms lat-axis one) exceeds dv_t0_mm_s. T0 is therefore
 *      retroactive: it is latched before the trigger fires, and released once
 *      both windows fall back to <= dv_t0 outside an event. For an external
 *      signal, T0 = the signal timestamp.
 *  A7. End of event (DEC-06): first in-event sample where the 20 ms long-axis
 *      window delta is <= dv_t0_mm_s (never on the sample that fires).
 *  A8. Thresholds: mm_s = idiv_trunc(mkmh * 5, 18) => 2222 / 6944 / 222.
 *  A9. Strict (>) to trigger, non-strict (<=) to end.
 */

#include "ambre_trigger.h"
#include "ambre_trigger_window.h"
#include <string.h>

#define AMBRE_TRIG_OK 0
#define RING_SLOTS ((uint16_t)AMBRE_TRIG_RING_SLOTS)

/* --------------------------------------------------------------- scalars -- */

static int64_t i64_abs(int64_t v)
{
    return (v < 0) ? -v : v;
}

static int64_t idiv_trunc(int64_t num, int64_t den)
{
    return num / den;                       /* A3: truncation toward zero */
}

static int64_t integrate(int32_t accel, uint32_t dt_us)
{
    return idiv_trunc((int64_t)accel * (int64_t)dt_us, 1000000LL);
}

static uint32_t clamp_dt(uint64_t now, uint64_t prev, bool have_prev)
{
    if (!have_prev) return 0;
    uint64_t raw = (now >= prev) ? (now - prev) : 0;
    return (raw > AMBRE_TRIG_DT_MAX_US) ? AMBRE_TRIG_DT_MAX_US : (uint32_t)raw;
}

/* Milliseconds since T0, clamped to the uint16 range of the SEAL payload. */
static uint16_t clamp_ms(uint64_t ts, uint64_t t0)
{
    uint64_t ms = (ts >= t0) ? ((ts - t0) / 1000ULL) : 0;
    return (ms > 65535ULL) ? (uint16_t)65535 : (uint16_t)ms;
}


/* ------------------------------------------------------------ configuration - */

static int32_t mkmh_to_mm_s(int32_t mkmh)
{
    return (int32_t)idiv_trunc((int64_t)mkmh * 5LL, 18LL);   /* A8 */
}

void ambre_trig_cfg_default(ambre_trig_cfg_t *cfg)
{
    cfg->mode = AMBRE_TRIG_MODE_WINDOW;
    cfg->dv_trigger_mm_s = mkmh_to_mm_s(8000);      /* 2222 mm/s = 8 km/h  */
    cfg->dv_lock_mm_s = mkmh_to_mm_s(25000);        /* 6944 mm/s = 25 km/h */
    cfg->dv_t0_mm_s = mkmh_to_mm_s(800);            /* 222 mm/s = 0.8 km/h */
    cfg->win_trigger_ms = 150;
    cfg->win_t0_long_ms = 20;
    cfg->win_t0_lat_ms = 5;
    cfg->has_front_restraint = 1;
    cfg->same_crash_window_ms = 5000;               /* DEC-09: 5 s         */
}

void ambre_trig_init(ambre_trig_state_t *st, const ambre_trig_cfg_t *cfg)
{
    memset(st, 0, sizeof(*st));
    st->cfg = *cfg;
}

/* --------------------------------------------------------------- decisions - */

/* A6: latch/release T0. t0_ts_us == 0 means "no onset latched"; A1 guarantees
 * microsecond timestamps, and the first acquired sample is always > 0. */
static void latch_t0(ambre_trig_state_t *st, uint64_t ts_us,
                     int64_t w_t0_long, int64_t w_t0_lat)
{
    if (st->in_event) return;

    bool active = (w_t0_long > st->cfg.dv_t0_mm_s) || (w_t0_lat > st->cfg.dv_t0_mm_s);
    if (!active) {
        st->t0_ts_us = 0;
        return;
    }
    if (st->t0_ts_us == 0) {
        st->t0_ts_us = ts_us;
        st->dv_at_t0_long = st->dv_long;
        st->dv_at_t0_lat = st->dv_lat;
    }
}

/* DEC-03 / DEC-10 / DEC-11: external signals win over the delta-v rules. */
static bool external_trigger(const ambre_trig_ext_t *ext, uint8_t *reason, bool *locks)
{
    if (!ext) return false;
    if (ext->restraint_deployed) {
        *reason = AMBRE_TRIG_RESTRAINT;  *locks = true;  return true;
    }
    if (ext->vru_activated) {
        *reason = AMBRE_TRIG_VRU;        *locks = true;  return true;
    }
    if (ext->emergency_brake) {
        *reason = AMBRE_TRIG_EMERG_BRAKE; *locks = false; return true;
    }
    if (ext->aeb_intervention) {
        *reason = AMBRE_TRIG_AEB;        *locks = false; return true;
    }
    if (ext->service_command) {
        *reason = AMBRE_TRIG_SERVICE;    *locks = false; return true;
    }
    return false;
}

/* A5 ANCHORED: excursion since the last sample where delta-v was at rest. */
static void anchored_excursion(ambre_trig_state_t *st, uint64_t ts_us,
                               int64_t *ex_long, int64_t *ex_lat)
{
    int64_t d_long = st->dv_long - st->anchor_dv_long;
    int64_t d_lat = st->dv_lat - st->anchor_dv_lat;

    if (!st->have_anchor || (d_long == 0 && d_lat == 0)) {
        st->have_anchor = true;
        st->anchor_ts_us = ts_us;
        st->anchor_dv_long = st->dv_long;
        st->anchor_dv_lat = st->dv_lat;
        *ex_long = 0;
        *ex_lat = 0;
        return;
    }

    uint64_t elapsed = (ts_us >= st->anchor_ts_us) ? (ts_us - st->anchor_ts_us) : 0;
    if (elapsed <= (uint64_t)st->cfg.win_trigger_ms * 1000ULL) {
        *ex_long = i64_abs(d_long);
        *ex_lat = i64_abs(d_lat);
    } else {
        *ex_long = 0;
        *ex_lat = 0;
    }
}

/* DEC-01 / DEC-02 / DEC-04 on the excursion of the active mode (A9 strict). */
static bool dynamic_trigger(const ambre_trig_state_t *st, int64_t ex_long,
                            int64_t ex_lat, uint8_t *reason, bool *locks)
{
    bool lock_on = (st->cfg.has_front_restraint == 0);

    if (lock_on && ex_long > st->cfg.dv_lock_mm_s) {
        *reason = AMBRE_TRIG_DV25_LOCK; *locks = true;  return true;
    }
    if (ex_long > st->cfg.dv_trigger_mm_s) {
        *reason = AMBRE_TRIG_DV_LONG;   *locks = false; return true;
    }
    if (ex_lat > st->cfg.dv_trigger_mm_s) {
        *reason = AMBRE_TRIG_DV_LAT;    *locks = false; return true;
    }
    return false;
}

/* DEC-09: events whose T0 are less than same_crash_window_ms apart are grouped. */
static void assign_group(ambre_trig_state_t *st)
{
    uint64_t win = (uint64_t)st->cfg.same_crash_window_ms * 1000ULL;
    uint64_t t0 = st->t0_ts_us;

    if (st->have_crash_group && t0 >= st->crash_first_t0_us &&
        (t0 - st->crash_first_t0_us) < win) {
        st->event_number++;
    } else {
        st->crash_first_t0_us = t0;
        st->have_crash_group = true;
        st->event_number = 1;
    }
}

static void register_fire(ambre_trig_state_t *st, uint64_t ts_us, bool locks,
                          bool force_t0)
{
    if (force_t0 || st->t0_ts_us == 0) {
        st->t0_ts_us = ts_us;
        st->dv_at_t0_long = st->dv_long;
        st->dv_at_t0_lat = st->dv_lat;
    }
    st->in_event = true;
    if (locks) st->locked = true;

    st->max_dv_long = 0;
    st->max_dv_lat = 0;
    st->max_dv_long_at_ms = 0;
    st->max_dv_lat_at_ms = 0;

    assign_group(st);
    st->events_total++;
}

/* GEL-05 extrema, DEC-04 lock elevation and DEC-06 end of event. */
static bool advance_event(ambre_trig_state_t *st, uint64_t ts_us, int64_t w_long,
                          bool allow_end, uint8_t *lock_reason)
{
    int64_t d_long = i64_abs(st->dv_long - st->dv_at_t0_long);
    int64_t d_lat = i64_abs(st->dv_lat - st->dv_at_t0_lat);

    if (d_long > st->max_dv_long) {
        st->max_dv_long = d_long;
        st->max_dv_long_at_ms = clamp_ms(ts_us, st->t0_ts_us);
    }
    if (d_lat > st->max_dv_lat) {
        st->max_dv_lat = d_lat;
        st->max_dv_lat_at_ms = clamp_ms(ts_us, st->t0_ts_us);
    }

    *lock_reason = AMBRE_TRIG_NONE;
    if (st->cfg.has_front_restraint == 0 && !st->locked &&
        w_long > st->cfg.dv_lock_mm_s) {
        st->locked = true;
        *lock_reason = AMBRE_TRIG_DV25_LOCK;
    }

    if (!allow_end) return false;

    int64_t w_end = ambre_trig_axis_window_delta(ambre_trig_axis_long(st), ts_us, st->cfg.win_t0_long_ms);
    if (w_end <= st->cfg.dv_t0_mm_s) {              /* A9: non-strict */
        st->in_event = false;
        st->have_anchor = false;
        /* The latched T0 stays readable for this sample's decision and is
         * released by the caller once the output has been produced (A6). */
        return true;
    }
    return false;
}

static void fill_out(const ambre_trig_state_t *st, uint64_t ts_us, bool fired,
                     bool event_end, uint8_t reason, int64_t w_long, int64_t w_lat,
                     ambre_trig_out_t *out)
{
    bool ctx = st->in_event || event_end;

    out->fired = fired;
    out->event_end = event_end;
    out->locked = st->locked;
    out->reason = reason;
    out->dv_long_mm_s = st->dv_long;
    out->dv_lat_mm_s = st->dv_lat;
    out->win_long_mm_s = w_long;
    out->win_lat_mm_s = w_lat;

    if (!ctx) return;

    out->t0_ts_us = st->t0_ts_us;
    out->event_number = st->event_number;
    /* DEC-09: time elapsed since the first T0 of the current crash group. */
    if (st->have_crash_group && ts_us >= st->crash_first_t0_us) {
        out->time_since_first_ms =
            (uint32_t)((ts_us - st->crash_first_t0_us) / 1000ULL);
    }
    out->dv_since_t0_long_mm_s = st->dv_long - st->dv_at_t0_long;
    out->dv_since_t0_lat_mm_s = st->dv_lat - st->dv_at_t0_lat;
    out->max_dv_long_mm_s = st->max_dv_long;
    out->max_dv_lat_mm_s = st->max_dv_lat;
    out->max_dv_long_at_ms = st->max_dv_long_at_ms;
    out->max_dv_lat_at_ms = st->max_dv_lat_at_ms;
}

/* Integrate the current sample into the cumulative delta-v (A3, DEC-13):
 * the clock always advances, an invalid sample is never summed. */
static void integrate_sample(ambre_trig_state_t *st, uint64_t ts_us,
                              int32_t accel_long_mm_s2, int32_t accel_lat_mm_s2,
                              const ambre_trig_ext_t *ext)
{
    uint32_t dt_us = clamp_dt(ts_us, st->last_ts_us, st->have_last);
    st->last_ts_us = ts_us;
    st->have_last = true;

    bool invalid = (ext != NULL) && ext->sample_invalid;
    if (invalid || dt_us == 0) return;

    st->dv_long += integrate(accel_long_mm_s2, dt_us);
    st->dv_lat += integrate(accel_lat_mm_s2, dt_us);
}

/* Push the sample into both axis rings, trim them, and read the four
 * window deltas used by the decisions (A4). */
static void read_windows(ambre_trig_state_t *st, uint64_t ts_us,
                         int64_t *w_long, int64_t *w_lat,
                         int64_t *w_t0_long, int64_t *w_t0_lat)
{
    ambre_trig_axis_push(ambre_trig_axis_long(st), ts_us, st->dv_long);
    ambre_trig_axis_push(ambre_trig_axis_lat(st), ts_us, st->dv_lat);

    uint16_t widest = st->cfg.win_trigger_ms;
    if (st->cfg.win_t0_long_ms > widest) widest = st->cfg.win_t0_long_ms;
    if (st->cfg.win_t0_lat_ms > widest) widest = st->cfg.win_t0_lat_ms;
    ambre_trig_axis_trim(ambre_trig_axis_long(st), ts_us, widest);
    ambre_trig_axis_trim(ambre_trig_axis_lat(st), ts_us, widest);

    *w_long = ambre_trig_axis_window_delta(ambre_trig_axis_long(st), ts_us,
                                            st->cfg.win_trigger_ms);
    *w_lat = ambre_trig_axis_window_delta(ambre_trig_axis_lat(st), ts_us,
                                           st->cfg.win_trigger_ms);
    *w_t0_long = ambre_trig_axis_window_delta(ambre_trig_axis_long(st), ts_us,
                                               st->cfg.win_t0_long_ms);
    *w_t0_lat = ambre_trig_axis_window_delta(ambre_trig_axis_lat(st), ts_us,
                                              st->cfg.win_t0_lat_ms);
}

/* ------------------------------------------------------------------ public - */

int ambre_trig_push(ambre_trig_state_t *st, uint64_t ts_us,
                    int32_t accel_long_mm_s2, int32_t accel_lat_mm_s2,
                    const ambre_trig_ext_t *ext, ambre_trig_out_t *out)
{
    memset(out, 0, sizeof(*out));

    integrate_sample(st, ts_us, accel_long_mm_s2, accel_lat_mm_s2, ext);

    int64_t w_long, w_lat, w_t0_long, w_t0_lat;
    read_windows(st, ts_us, &w_long, &w_lat, &w_t0_long, &w_t0_lat);

    latch_t0(st, ts_us, w_t0_long, w_t0_lat);       /* A6 */

    bool fired = false;
    uint8_t reason = AMBRE_TRIG_NONE;

    if (!st->in_event) {
        bool locks = false;
        bool force_t0 = external_trigger(ext, &reason, &locks);
        fired = force_t0;

        if (!fired) {
            int64_t ex_long = w_long;
            int64_t ex_lat = w_lat;
            if (st->cfg.mode == AMBRE_TRIG_MODE_ANCHORED) {
                anchored_excursion(st, ts_us, &ex_long, &ex_lat);
            }
            fired = dynamic_trigger(st, ex_long, ex_lat, &reason, &locks);
        }
        if (fired) register_fire(st, ts_us, locks, force_t0);
    }

    bool event_end = false;
    if (st->in_event) {
        uint8_t lock_reason = AMBRE_TRIG_NONE;
        event_end = advance_event(st, ts_us, w_long, !fired, &lock_reason);
        if (lock_reason != AMBRE_TRIG_NONE && !fired) reason = lock_reason;
    }

    fill_out(st, ts_us, fired, event_end, reason, w_long, w_lat, out);
    if (event_end) st->t0_ts_us = 0;    /* A6: release after the decision */

    return AMBRE_TRIG_OK;
}

void ambre_trig_event_closed(ambre_trig_state_t *st, uint64_t ts_us)
{
    st->last_event_end_ts_us = ts_us;
    st->in_event = false;
    st->t0_ts_us = 0;
    st->have_anchor = false;

    if (st->have_crash_group) {
        uint64_t win = (uint64_t)st->cfg.same_crash_window_ms * 1000ULL;
        if (ts_us >= st->crash_first_t0_us && (ts_us - st->crash_first_t0_us) >= win) {
            st->have_crash_group = false;
        }
    }
}
