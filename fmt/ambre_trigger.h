/*
 * ambre_trigger.h — Crash trigger / T0 / end-of-event engine (UN R160 5.3).
 *
 * Portable C11, fixed memory, bounded time (DEC-07): no allocation, no
 * recursion, only ring buffers and monotonic min/max queues over prefix sums.
 *
 * ARITHMETIC CONTRACT — the Python twin (host/verify/ambre_trigger.py) must
 * reproduce these rules bit for bit (DEC-12):
 *
 *  A1. All timestamps are monotonic microseconds (uint64). No absolute time.
 *  A2. Accelerations are int32 in mm/s^2. Delta-v is int64 in mm/s.
 *  A3. Integration per sample i (i > 0), rectangle rule on the CURRENT sample:
 *          dt_us = ts[i] - ts[i-1], clamped to [0, AMBRE_TRIG_DT_MAX_US]
 *          dv_cum += idiv_trunc((int64)accel[i] * dt_us, 1000000)
 *      idiv_trunc() truncates toward zero (C semantics). Python must NOT use
 *      floor division: use ambre_idiv_trunc() semantics from ambre_fmt.h.
 *  A4. A window over an axis is the set of ring entries with
 *          ts >= ts_now - (window_ms * 1000)      (inclusive)
 *      window_delta = max(dv over window) - min(dv over window).
 *  A5. Trigger (DEC-01/02), default mode AMBRE_TRIG_MODE_WINDOW:
 *          fires on the first sample where window_delta over 150 ms
 *          exceeds dv_trigger_mm_s on the long or the lat axis.
 *      Mode AMBRE_TRIG_MODE_ANCHORED: the excursion is anchored at the sample
 *      where |dv_cum - dv_anchor| first became non-zero; it fires only if the
 *      threshold is exceeded at most win_trigger_ms after that anchor. This
 *      mode exists so that DEC-08 case 3 ("8.1 km/h reached in 151 ms does
 *      not trigger") is testable on a monotonic ramp; WINDOW is the default
 *      and the regulation-faithful reading ("exists a sub-window <= 150 ms").
 *  A6. T0 (DEC-05): first sample where window_delta exceeds dv_t0_mm_s over
 *      win_t0_long_ms (20 ms) on the long axis, or over win_t0_lat_ms (5 ms)
 *      on the lat axis. For an external-signal trigger, T0 = signal timestamp.
 *  A7. End of event (DEC-06): while in event, first sample where the 20 ms
 *      long-axis window_delta is <= dv_t0_mm_s.
 *  A8. Thresholds in mm/s are derived from milli-km/h by
 *          mm_s = mkmh * 5 / 18      (integer division, truncating)
 *      so 8 km/h -> 2222, 0.8 km/h -> 222, 25 km/h -> 6944.
 *  A9. Comparisons are strict for triggering (>), non-strict for ending (<=).
 */

#ifndef AMBRE_TRIGGER_H
#define AMBRE_TRIGGER_H

#include "ambre_types.h"

#define AMBRE_TRIG_MODE_WINDOW   0u
#define AMBRE_TRIG_MODE_ANCHORED 1u

#define AMBRE_TRIG_DT_MAX_US     10000u   /* gaps above 10 ms clamp (ACQ-06) */
#define AMBRE_TRIG_RING_SLOTS    192u     /* > 150 ms at 1 kHz               */

#define AMBRE_MKMH_TO_MM_S(mkmh) ((int32_t)(((int64_t)(mkmh) * 5) / 18))

#define AMBRE_DV_TRIGGER_MM_S    AMBRE_MKMH_TO_MM_S(8000)   /* 2222 = 8 km/h */
#define AMBRE_DV_LOCK_MM_S       AMBRE_MKMH_TO_MM_S(25000)  /* 6944 = 25 km/h*/
#define AMBRE_DV_T0_MM_S         AMBRE_MKMH_TO_MM_S(800)    /*  222 = 0.8 km/h*/

typedef struct {
    uint8_t  mode;                    /* AMBRE_TRIG_MODE_*                   */
    int32_t  dv_trigger_mm_s;
    int32_t  dv_lock_mm_s;
    int32_t  dv_t0_mm_s;
    uint16_t win_trigger_ms;          /* 150                                 */
    uint16_t win_t0_long_ms;          /* 20                                  */
    uint16_t win_t0_lat_ms;           /* 5                                   */
    uint8_t  has_front_restraint;     /* 1 = vehicle has one (DEC-04)        */
    uint32_t same_crash_window_ms;    /* 5000 (DEC-09)                       */
} ambre_trig_cfg_t;

typedef struct {
    uint64_t ts_us;
    int64_t  dv;                      /* cumulative delta-v, mm/s            */
} ambre_trig_slot_t;

/* Monotonic deque over the ring: stores ring indices. */
typedef struct {
    uint16_t idx[AMBRE_TRIG_RING_SLOTS];
    uint16_t head;
    uint16_t count;
} ambre_trig_deque_t;

typedef struct {
    ambre_trig_slot_t slots[AMBRE_TRIG_RING_SLOTS];
    uint16_t head;                    /* next write position                 */
    uint16_t count;
} ambre_trig_ring_t;

typedef struct {
    ambre_trig_cfg_t cfg;
    uint64_t last_ts_us;
    bool     have_last;
    int64_t  dv_long;                 /* cumulative, mm/s                    */
    int64_t  dv_lat;
    ambre_trig_ring_t ring_long, ring_lat;
    ambre_trig_deque_t max_long, min_long, max_lat, min_lat;
    bool     in_event;
    bool     locked;
    uint64_t t0_ts_us;
    uint64_t anchor_ts_us;            /* ANCHORED mode only                  */
    int64_t  anchor_dv_long, anchor_dv_lat;
    bool     have_anchor;
    uint64_t crash_first_t0_us;       /* first T0 of the current crash group */
    bool     have_crash_group;
    uint32_t event_number;            /* index inside the crash group        */
    uint32_t events_total;
    int64_t  dv_at_t0_long, dv_at_t0_lat;
    int64_t  max_dv_long, max_dv_lat; /* since T0                            */
    uint16_t max_dv_long_at_ms, max_dv_lat_at_ms;
    uint64_t last_event_end_ts_us;
} ambre_trig_state_t;

typedef struct {
    bool     restraint_deployed;      /* DEC-03, locks                       */
    bool     vru_activated;           /* DEC-10, locks                       */
    bool     emergency_brake;         /* DEC-11, never locks                 */
    bool     aeb_intervention;        /* DEC-11, never locks                 */
    bool     service_command;         /* DEC-11, never locks                 */
    bool     sample_invalid;          /* DEC-13: excluded from delta-v       */
} ambre_trig_ext_t;

typedef struct {
    bool     fired;                   /* a trigger decision on this sample   */
    bool     event_end;               /* end-of-event condition on this sample*/
    bool     locked;                  /* event must be locked                */
    uint8_t  reason;                  /* ambre_trig_reason_t                 */
    uint64_t t0_ts_us;
    uint32_t event_number;
    uint32_t time_since_first_ms;
    int64_t  dv_long_mm_s;            /* cumulative since boot               */
    int64_t  dv_lat_mm_s;
    int64_t  dv_since_t0_long_mm_s;
    int64_t  dv_since_t0_lat_mm_s;
    int64_t  max_dv_long_mm_s;
    int64_t  max_dv_lat_mm_s;
    uint16_t max_dv_long_at_ms;
    uint16_t max_dv_lat_at_ms;
    int64_t  win_long_mm_s;           /* diagnostics: window deltas          */
    int64_t  win_lat_mm_s;
} ambre_trig_out_t;

void ambre_trig_cfg_default(ambre_trig_cfg_t *cfg);
void ambre_trig_init(ambre_trig_state_t *st, const ambre_trig_cfg_t *cfg);

/* Push one acceleration sample (1 kHz nominal). ext may be NULL.
 * Returns AMBRE_OK; out is always filled. Bounded time, no allocation. */
int ambre_trig_push(ambre_trig_state_t *st, uint64_t ts_us,
                    int32_t accel_long_mm_s2, int32_t accel_lat_mm_s2,
                    const ambre_trig_ext_t *ext, ambre_trig_out_t *out);

/* Notify that a triggered event has been fully sealed: closes the event and
 * lets the engine start a new crash group after same_crash_window_ms. */
void ambre_trig_event_closed(ambre_trig_state_t *st, uint64_t ts_us);

#endif /* AMBRE_TRIGGER_H */
