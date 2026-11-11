/*
 * edr_capture.c — Portable acquisition and crash trigger engine.
 *
 * Rules: files < 500 lines, functions < 50 lines, C11, no dynamic allocation
 * on trigger path (GEL-03).
 */

#include "edr_capture.h"
#include "../../target/edr_ipc.h"
#include "../../fmt/ambre_fmt.h"
#include <string.h>

#define DEFAULT_CONTINUE_PERIOD_US 10000u
#define AMBRE_REL_TIME_MAX_MS      65535u

bool ambre_capture_is_forbidden(uint16_t signal_id)
{
    return (signal_id >= 90u && signal_id <= 99u);
}

bool ambre_capture_is_whitelisted(uint16_t signal_id)
{
    if (signal_id >= 1u && signal_id <= 3u) return true;
    if (signal_id >= 10u && signal_id <= 13u) return true;
    if (signal_id >= 20u && signal_id <= 25u) return true;
    if (signal_id >= 40u && signal_id <= 43u) return true;
    return false;
}

uint32_t ambre_capture_signal_period_us(uint16_t signal_id)
{
    if (signal_id >= 1u && signal_id <= 3u) return 1000u;     /* 1 kHz */
    if (signal_id >= 10u && signal_id <= 25u) return 10000u; /* 100 Hz */
    if (signal_id >= 40u && signal_id <= 43u) return 100000u;/* 10 Hz */
    return 0u;
}

void ambre_capture_init(ambre_capture_t *cap, ambre_capture_profile_t prof,
                        uint8_t ignition_cycle)
{
    memset(cap, 0, sizeof(*cap));
    cap->profile = prof;
    cap->ignition_cycle = ignition_cycle;
    cap->continue_interval_us = DEFAULT_CONTINUE_PERIOD_US;
    cap->max_frames_per_10ms = AMBRE_CAPTURE_MAX_FRAMES_10MS;

    ambre_trig_cfg_t cfg;
    ambre_trig_cfg_default(&cfg);
    ambre_trig_init(&cap->trig_state, &cfg);
}

void ambre_capture_fill_emerg(const ambre_capture_ring_t *ring, ambre_emerg_t *e,
                              uint64_t t0_us, uint32_t event_id, uint8_t reason,
                              bool locked, uint8_t ignition_cycle)
{
    memset(e, 0, sizeof(*e));
    e->event_id = event_id;
    e->trig_reason = reason;
    e->locked = locked ? 1u : 0u;
    e->event_number = 1u;
    e->ignition_cycle = ignition_cycle;
    e->t0_ts_us = t0_us;
    e->trig_ts_us = t0_us;
    e->crash_order = event_id;

    uint32_t emitted = 0;
    for (uint32_t i = 0; i < ring->count; i++) {
        uint32_t idx = (ring->head - ring->count + i) % AMBRE_CAPTURE_POINTS_MAX;
        if (ring->pt_ts_us[idx] > t0_us) continue;
        uint64_t age_ms = (t0_us - ring->pt_ts_us[idx]) / 1000u;
        if (age_ms > AMBRE_CLASS_A_MAX_AGE_MS) continue;
        e->points[emitted] = ring->pts[idx];
        e->points[emitted].rel_time_ms =
            (uint16_t)(age_ms > AMBRE_REL_TIME_MAX_MS ? AMBRE_REL_TIME_MAX_MS : age_ms);
        emitted++;
    }
    e->point_count = (uint16_t)emitted;
}

static void ring_push_point(ambre_capture_ring_t *ring, const ambre_a_point_t *pt,
                            uint64_t ts_us)
{
    uint32_t slot = ring->head % AMBRE_CAPTURE_POINTS_MAX;
    ring->pts[slot] = *pt;
    ring->pt_ts_us[slot] = ts_us;
    ring->head++;
    if (ring->count < AMBRE_CAPTURE_POINTS_MAX) {
        ring->count++;
    }
}

static void decode_sample(ambre_a_point_t *st, const ambre_sample_t *s)
{
    switch (s->signal_id) {
    case 10: case 11: case 12: case 13:
        st->speed_kmh_x4 = (uint16_t)((s->value > 0 ? (uint64_t)s->value * 36u : 0u) / 2500u);
        break;
    case 20: st->steer_angle_deg_x2 = (int16_t)s->value; break;
    case 21: st->throttle_pct = (uint8_t)(s->value / 2); break;
    case 22: st->brake_active = (s->value != 0) ? 1u : 0u; break;
    case 23: st->yaw_rate_deg_s_x10 = (int16_t)s->value; break;
    case 25: st->engine_rpm = (uint16_t)s->value; break;
    case 41: st->belt_driver = (uint8_t)(s->value & 0x01u); break;
    case 42:
        st->airbag_lamp = (uint8_t)(s->value & 0x01u);
        st->abs_active = (uint8_t)((s->value >> 1) & 0x01u);
        st->esc_active = (uint8_t)((s->value >> 2) & 0x01u);
        break;
    default: break;
    }
}

static void update_class_b_state(ambre_capture_t *cap, const ambre_sample_t *s)
{
    if (s->signal_id < 64u) {
        cap->last_qualities[s->signal_id] = s->quality;
    }
    if (s->signal_id == 1u) {
        cap->last_accel_long_mm_s2 = s->value;
        cap->have_accel = true;
    } else if (s->signal_id == 2u) {
        cap->last_accel_lat_mm_s2 = s->value;
        cap->have_accel = true;
    } else {
        decode_sample(&cap->current_state, s);
    }
    cap->current_state.accel_long_mm_s2 = (int16_t)cap->last_accel_long_mm_s2;
    cap->current_state.accel_lat_mm_s2 = (int16_t)cap->last_accel_lat_mm_s2;
}

static void check_signal_gap(ambre_capture_t *cap, uint16_t sig_id, uint64_t ts_us,
                             ambre_capture_sink_fn sink, void *sink_ctx)
{
    if (sig_id >= 64u) return;
    uint32_t period_us = ambre_capture_signal_period_us(sig_id);
    if (period_us == 0u) return;

    if (cap->sig_have_ts[sig_id]) {
        uint64_t prev = cap->sig_last_ts_us[sig_id];
        if (ts_us > prev && (ts_us - prev) > (3u * (uint64_t)period_us)) {
            cap->stats.gaps_detected++;
            if (sink) {
                ambre_gap_t gap;
                gap.start_ts_us = prev;
                gap.end_ts_us = ts_us;
                gap.reason = sig_id;
                sink(sink_ctx, (uint8_t)AMBRE_MSG_GAP, &gap, sizeof(gap));
            }
        }
    }
    cap->sig_last_ts_us[sig_id] = ts_us;
    cap->sig_have_ts[sig_id] = true;
}

static void emit_trigger(ambre_capture_t *cap, uint64_t t0_us, uint64_t trig_ts_us,
                         uint8_t reason, bool locked,
                         ambre_capture_sink_fn sink, void *sink_ctx)
{
    cap->event_count++;
    cap->active_event_id = cap->event_count;
    cap->in_event = true;
    cap->active_t0_ts_us = t0_us;
    cap->active_trig_reason = reason;
    cap->active_locked = locked;
    cap->active_rel_time_ms = 0u;
    cap->last_continue_ts_us = trig_ts_us;
    cap->stats.triggers_fired++;

    if (sink) {
        ambre_emerg_t emerg;
        ambre_capture_fill_emerg(&cap->ring, &emerg, t0_us, cap->active_event_id,
                                 reason, locked, cap->ignition_cycle);
        emerg.trig_ts_us = trig_ts_us;
        sink(sink_ctx, (uint8_t)AMBRE_MSG_TRIGGER, &emerg, sizeof(emerg));
    }
}

static void emit_seal(ambre_capture_t *cap, const ambre_trig_out_t *out,
                      uint64_t ts_us, ambre_capture_sink_fn sink, void *sink_ctx)
{
    cap->in_event = false;
    cap->stats.seals_emitted++;

    if (sink) {
        ambre_seal_t seal;
        memset(&seal, 0, sizeof(seal));
        seal.event_id = cap->active_event_id;
        seal.file_complete = 1u;
        seal.seal_cause = (uint8_t)AMBRE_SEAL_NORMAL;
        seal.locked = (uint8_t)(out->locked || cap->active_locked ? 1u : 0u);
        seal.event_number = (uint8_t)out->event_number;
        seal.time_since_first_ms = out->time_since_first_ms;
        seal.max_dv_long_mm_s = (int32_t)out->max_dv_long_mm_s;
        seal.max_dv_long_at_ms = out->max_dv_long_at_ms;
        seal.max_dv_lat_mm_s = (int32_t)out->max_dv_lat_mm_s;
        seal.max_dv_lat_at_ms = out->max_dv_lat_at_ms;
        seal.restraint_deploy_ms = cap->restraint_deploy_ms;
        seal.vru_deploy_ms = cap->vru_deploy_ms;
        seal.ignition_cycle_at_read = cap->ignition_cycle;
        sink(sink_ctx, (uint8_t)AMBRE_MSG_SEAL, &seal, sizeof(seal));
    }
    ambre_trig_event_closed(&cap->trig_state, ts_us);
}

static void evaluate_trigger(ambre_capture_t *cap, uint64_t ts_us,
                             const ambre_trig_ext_t *ext,
                             ambre_capture_sink_fn sink, void *sink_ctx)
{
    ambre_trig_out_t out;
    ambre_trig_push(&cap->trig_state, ts_us, cap->last_accel_long_mm_s2,
                    cap->last_accel_lat_mm_s2, ext, &out);
    cap->last_trig_out = out;

    if (out.fired && !cap->in_event) {
        uint64_t t0 = out.t0_ts_us ? out.t0_ts_us : ts_us;
        emit_trigger(cap, t0, ts_us, out.reason, out.locked, sink, sink_ctx);
    }
    if (out.event_end && cap->in_event) {
        emit_seal(cap, &out, ts_us, sink, sink_ctx);
    }
}

static void maybe_record_a_point(ambre_capture_t *cap, uint64_t ts_us)
{
    if (cap->last_a_point_ts_us == 0u ||
        (ts_us >= cap->last_a_point_ts_us + AMBRE_CLASS_A_INTERVAL_US)) {
        ring_push_point(&cap->ring, &cap->current_state, ts_us);
        cap->last_a_point_ts_us = ts_us;
    }
}

static int check_overload(ambre_capture_t *cap, uint64_t rx_ts_us)
{
    if (cap->max_frames_per_10ms == 0u) return AMBRE_OK;
    if (cap->overload_win_start_us == 0u ||
        rx_ts_us >= cap->overload_win_start_us + AMBRE_CAPTURE_WIN_US) {
        cap->overload_win_start_us = rx_ts_us;
        cap->overload_win_frames = 1u;
        return AMBRE_OK;
    }
    cap->overload_win_frames++;
    if (cap->overload_win_frames > cap->max_frames_per_10ms) {
        cap->stats.frames_overload++;
        return AMBRE_ERR_FULL;
    }
    return AMBRE_OK;
}

static int validate_frame_header(ambre_capture_t *cap, const void *buf, size_t len,
                                 const ambre_ipc_hdr_t **hdr_out,
                                 const ambre_ipc_frame_body_t **body_out)
{
    if (len < (sizeof(ambre_ipc_hdr_t) + sizeof(ambre_ipc_frame_body_t))) {
        cap->stats.frames_malformed++;
        return AMBRE_ERR_PARAM;
    }
    if (len > AMBRE_IPC_FRAME_MAX) {
        cap->stats.frames_malformed++;
        return AMBRE_ERR_TOO_BIG;
    }
    const ambre_ipc_hdr_t *h = (const ambre_ipc_hdr_t *)buf;
    if (h->magic != AMBRE_IPC_MAGIC || h->version != AMBRE_IPC_VERSION ||
        h->msg_type != AMBRE_MSG_FRAME) {
        cap->stats.frames_malformed++;
        return AMBRE_ERR_BAD_MAGIC;
    }
    if ((size_t)h->body_len + sizeof(ambre_ipc_hdr_t) != len) {
        cap->stats.frames_malformed++;
        return AMBRE_ERR_TORN;
    }
    const ambre_ipc_frame_body_t *fb =
        (const ambre_ipc_frame_body_t *)((const uint8_t *)buf + sizeof(ambre_ipc_hdr_t));
    size_t samples_size = (size_t)fb->sample_count * sizeof(ambre_sample_t);
    if (sizeof(ambre_ipc_frame_body_t) + samples_size != h->body_len) {
        cap->stats.frames_malformed++;
        return AMBRE_ERR_PARAM;
    }
    *hdr_out = h;
    *body_out = fb;
    return AMBRE_OK;
}

static uint32_t count_accel_ticks(const ambre_sample_t *samples, uint32_t count)
{
    uint32_t ticks = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        if (samples[i].signal_id == 1u) ticks++;
    }
    return ticks;
}

static uint64_t compute_tick_step(const ambre_capture_t *cap, uint64_t rx_ts_us,
                                  uint32_t n_ticks)
{
    if (n_ticks <= 1u) return 0u;
    uint64_t step = 1000u;
    if (cap->have_last_frame && rx_ts_us > cap->last_frame_ts_us) {
        uint64_t avail = rx_ts_us - cap->last_frame_ts_us;
        if (avail < (uint64_t)n_ticks * 1000u) {
            step = avail / n_ticks;
        }
    } else if (rx_ts_us < (uint64_t)(n_ticks - 1u) * 1000u) {
        step = rx_ts_us / n_ticks;
    }
    return step;
}

static void process_frame_samples(ambre_capture_t *cap, const ambre_sample_t *samples,
                                  uint32_t count, uint64_t rx_ts_us,
                                  ambre_capture_sink_fn sink, void *sink_ctx)
{
    uint32_t n_ticks = count_accel_ticks(samples, count);
    uint64_t step = compute_tick_step(cap, rx_ts_us, n_ticks);
    uint64_t t_base = (n_ticks > 1u) ? (rx_ts_us - (uint64_t)(n_ticks - 1u) * step) : rx_ts_us;
    uint32_t tick_idx = 0u;
    bool accel_pending = false;
    bool accel_invalid = false;

    for (uint32_t i = 0u; i < count; i++) {
        cap->stats.samples_received++;
        const ambre_sample_t *s = &samples[i];
        if (ambre_capture_is_forbidden(s->signal_id)) {
            cap->stats.samples_forbidden++;
            continue;
        }
        if (!ambre_capture_is_whitelisted(s->signal_id)) {
            cap->stats.samples_rejected++;
            continue;
        }
        if (s->signal_id == 1u && accel_pending) {
            uint64_t cur_ts = t_base + (uint64_t)tick_idx * step;
            ambre_trig_ext_t ext = { .sample_invalid = accel_invalid };
            maybe_record_a_point(cap, cur_ts);
            evaluate_trigger(cap, cur_ts, accel_invalid ? &ext : NULL, sink, sink_ctx);
            tick_idx++;
            accel_pending = false;
            accel_invalid = false;
        }
        uint64_t cur_ts = (n_ticks > 1u) ? (t_base + (uint64_t)tick_idx * step) : rx_ts_us;
        check_signal_gap(cap, s->signal_id, cur_ts, sink, sink_ctx);
        update_class_b_state(cap, s);
        if (s->signal_id == 1u || s->signal_id == 2u) {
            accel_pending = true;
            if (s->quality != 0u) accel_invalid = true;
        }
    }
    maybe_record_a_point(cap, rx_ts_us);
    if (cap->have_accel) {
        ambre_trig_ext_t ext = { .sample_invalid = accel_invalid };
        evaluate_trigger(cap, rx_ts_us, accel_invalid ? &ext : NULL, sink, sink_ctx);
    }
}

int ambre_capture_push_frame(ambre_capture_t *cap, const void *buf, size_t len,
                             uint64_t rx_ts_us, ambre_capture_sink_fn sink,
                             void *sink_ctx)
{
    cap->stats.frames_received++;
    if (check_overload(cap, rx_ts_us) != AMBRE_OK) {
        return AMBRE_ERR_FULL;
    }
    const ambre_ipc_hdr_t *h = NULL;
    const ambre_ipc_frame_body_t *fb = NULL;
    int rc = validate_frame_header(cap, buf, len, &h, &fb);
    if (rc != AMBRE_OK) return rc;

    const ambre_sample_t *samples =
        (const ambre_sample_t *)((const uint8_t *)fb + sizeof(ambre_ipc_frame_body_t));
    process_frame_samples(cap, samples, fb->sample_count, rx_ts_us, sink, sink_ctx);

    cap->last_frame_ts_us = rx_ts_us;
    cap->have_last_frame = true;
    cap->stats.frames_accepted++;
    return AMBRE_OK;
}

int ambre_capture_push_external(ambre_capture_t *cap, uint64_t ts_us,
                                uint32_t signal_mask, uint32_t producer_id,
                                ambre_capture_sink_fn sink, void *sink_ctx)
{
    (void)producer_id;
    ambre_trig_ext_t ext;
    memset(&ext, 0, sizeof(ext));
    ext.restraint_deployed = (signal_mask & 0x01u) != 0;
    ext.vru_activated = (signal_mask & 0x02u) != 0;
    ext.emergency_brake = (signal_mask & 0x04u) != 0;
    ext.aeb_intervention = (signal_mask & 0x08u) != 0;
    ext.service_command = (signal_mask & 0x10u) != 0;

    if (ext.restraint_deployed && cap->in_event && cap->restraint_deploy_ms == 0u) {
        cap->restraint_deploy_ms =
            (uint16_t)((ts_us >= cap->active_t0_ts_us ? ts_us - cap->active_t0_ts_us : 0u) / 1000u);
    }
    if (ext.vru_activated && cap->in_event && cap->vru_deploy_ms == 0u) {
        cap->vru_deploy_ms =
            (uint16_t)((ts_us >= cap->active_t0_ts_us ? ts_us - cap->active_t0_ts_us : 0u) / 1000u);
    }
    evaluate_trigger(cap, ts_us, &ext, sink, sink_ctx);
    return AMBRE_OK;
}

int ambre_capture_tick(ambre_capture_t *cap, uint64_t now_us,
                       ambre_capture_sink_fn sink, void *sink_ctx)
{
    if (!cap->in_event) return AMBRE_OK;

    if (now_us >= cap->last_continue_ts_us + cap->continue_interval_us) {
        cap->last_continue_ts_us = now_us;
        cap->active_rel_time_ms =
            (uint16_t)((now_us >= cap->active_t0_ts_us ? now_us - cap->active_t0_ts_us : 0u) / 1000u);
        cap->stats.continues_emitted++;

        if (sink) {
            ambre_cont_t cont;
            memset(&cont, 0, sizeof(cont));
            cont.event_id = cap->active_event_id;
            cont.sample_count = 1u;
            cont.rel_time_ms = cap->active_rel_time_ms;
            cont.dv_long_mm_s = (int32_t)cap->last_trig_out.dv_since_t0_long_mm_s;
            cont.dv_lat_mm_s = (int32_t)cap->last_trig_out.dv_since_t0_lat_mm_s;
            cont.points[0] = cap->current_state;
            cont.points[0].rel_time_ms = cap->active_rel_time_ms;
            sink(sink_ctx, (uint8_t)AMBRE_MSG_CONTINUE, &cont, sizeof(cont));
        }
    }
    return AMBRE_OK;
}
