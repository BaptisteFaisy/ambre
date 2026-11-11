/*
 * edr_capture.h — Portable acquisition and crash trigger engine (ACQ-01..09).
 *
 * Rules: files < 500 lines, functions < 50 lines, C11, no dynamic allocation
 * on trigger path (GEL-03).
 */

#ifndef EDR_CAPTURE_H
#define EDR_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../fmt/ambre_types.h"
#include "../../fmt/ambre_trigger.h"

#define AMBRE_CAPTURE_POINTS_MAX  21u
#define AMBRE_CLASS_A_INTERVAL_US 500000u /* 2 Hz = 500 ms */
#define AMBRE_CLASS_A_MAX_AGE_MS  5500u   /* 5 s window + margin */
#define AMBRE_CAPTURE_MAX_FRAMES_10MS 100u /* ROB-06: 10x nominal 1 frame/10ms */
#define AMBRE_CAPTURE_WIN_US      10000u  /* 10 ms frame window (ACQ-01) */

typedef enum {
    AMBRE_PROFILE_EU = 0,
    AMBRE_PROFILE_US = 1
} ambre_capture_profile_t;

typedef struct {
    uint64_t frames_received;
    uint64_t frames_accepted;
    uint64_t frames_malformed;      /* SEC-05 */
    uint64_t frames_overload;       /* ROB-06 */
    uint64_t samples_received;
    uint64_t samples_rejected;      /* ACQ-04 */
    uint64_t samples_forbidden;     /* VIE-01 (ids 90..99) */
    uint64_t gaps_detected;         /* ACQ-06 */
    uint64_t triggers_fired;
    uint64_t continues_emitted;
    uint64_t seals_emitted;
} ambre_capture_stats_t;

typedef struct {
    ambre_a_point_t pts[AMBRE_CAPTURE_POINTS_MAX];
    uint64_t        pt_ts_us[AMBRE_CAPTURE_POINTS_MAX];
    uint32_t        head;
    uint32_t        count;
} ambre_capture_ring_t;

typedef struct {
    ambre_capture_profile_t profile;
    uint8_t                 ignition_cycle;       /* VIE-02 */
    uint32_t                event_count;
    uint64_t                last_frame_ts_us;
    uint64_t                last_a_point_ts_us;
    uint64_t                last_continue_ts_us;
    uint32_t                continue_interval_us; /* 10 ms (GEL-04) */
    uint64_t                overload_win_start_us;
    uint32_t                overload_win_frames;
    uint32_t                max_frames_per_10ms;  /* ROB-06 */
    bool                    have_last_frame;

    ambre_a_point_t         current_state;
    int32_t                 last_accel_long_mm_s2;
    int32_t                 last_accel_lat_mm_s2;
    uint8_t                 last_qualities[64];   /* ACQ-07 per signal_id */
    bool                    have_accel;

    uint64_t                sig_last_ts_us[64];
    bool                    sig_have_ts[64];

    ambre_trig_state_t      trig_state;
    ambre_trig_out_t        last_trig_out;
    ambre_capture_ring_t    ring;

    bool                    in_event;
    uint32_t                active_event_id;
    uint64_t                active_t0_ts_us;
    uint8_t                 active_trig_reason;
    bool                    active_locked;
    uint16_t                active_rel_time_ms;
    uint16_t                restraint_deploy_ms;
    uint16_t                vru_deploy_ms;

    ambre_capture_stats_t   stats;
} ambre_capture_t;

typedef int (*ambre_capture_sink_fn)(void *ctx, uint8_t msg_type,
                                     const void *body, size_t body_len);

void ambre_capture_init(ambre_capture_t *cap, ambre_capture_profile_t prof,
                        uint8_t ignition_cycle);

bool ambre_capture_is_whitelisted(uint16_t signal_id);
bool ambre_capture_is_forbidden(uint16_t signal_id);
uint32_t ambre_capture_signal_period_us(uint16_t signal_id);

int ambre_capture_push_frame(ambre_capture_t *cap, const void *buf, size_t len,
                             uint64_t rx_ts_us, ambre_capture_sink_fn sink,
                             void *sink_ctx);

int ambre_capture_push_external(ambre_capture_t *cap, uint64_t ts_us,
                                uint32_t signal_mask, uint32_t producer_id,
                                ambre_capture_sink_fn sink, void *sink_ctx);

int ambre_capture_tick(ambre_capture_t *cap, uint64_t now_us,
                       ambre_capture_sink_fn sink, void *sink_ctx);

void ambre_capture_fill_emerg(const ambre_capture_ring_t *ring, ambre_emerg_t *e,
                              uint64_t t0_us, uint32_t event_id, uint8_t reason,
                              bool locked, uint8_t ignition_cycle);

#endif /* EDR_CAPTURE_H */
