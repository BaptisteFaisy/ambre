/*
 * ambre_explore.c — Scenario recording (ground truth + op trace) and the
 * exploration driver (spec 7.3).
 *
 * record_truth() runs the journal once against the simdisk with no cut. That
 * single run yields both the op trace (every write/flush/mark, in order) and
 * the ground truth: what was injected, what was triggered, when each event
 * became acquired and sealed. Every later crash state is judged against it.
 */

#include "ambre_explore_internal.h"
#include "../../fmt/ambre_trigger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

/* Class A ring: 5 s at 2 Hz = 10 points, plus the point at T0 (spec 6.1). */
#define AMBRE_REC_MAX_POINTS 11u
#define AMBRE_POINT_PERIOD_US 500000u   /* class A at 2 Hz */
#define AMBRE_POINT_MAX_AGE_MS 5500u    /* 5 s window + sampling phase margin */
#define AMBRE_REL_TIME_MAX_MS 65535u    /* rel_time_ms is a uint16 */
#define AMBRE_DATA_EVERY_US 1000u       /* one DATA record per ms */

void ambre_explore_cfg_default(ambre_explore_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->seed = 1234;
    cfg->segment_limit = 4;
    cfg->flush_period_us = 100000;
    cfg->event_flush_period_us = 10000;
    cfg->duration_ms = 1500;
    cfg->crash_at_ms = 500;
    cfg->max_states = 0;
    cfg->jobs = 0;
    cfg->variant_torn = true;
    cfg->variant_volatile_cache = true;
    cfg->variant_bitflip = true;
    cfg->variant_table_wipe = true;
    cfg->nested_recovery = false;
}

void ambre_truth_free(ambre_truth_t *truth)
{
    if (!truth) return;
    free(truth->samples);
    free(truth->sample_ts_us);
    truth->samples = NULL;
    truth->sample_ts_us = NULL;
    truth->sample_count = 0;
}

/* ------------------------------------------------------------- recording -- */

typedef struct {
    const ambre_scenario_t *sc;
    ambre_truth_t           *truth;
    ambre_journal_t         *j;
    ambre_simdisk_t         *sd;
    ambre_a_point_t          pts[AMBRE_REC_MAX_POINTS];
    uint64_t                 pt_ts_us[AMBRE_REC_MAX_POINTS]; /* real sample time */
    uint32_t                 pt_head;
    uint32_t                 pt_count;
    uint32_t                 next_pt_us;
    uint32_t                 sample_cap;
    uint32_t                 events_open;
} ambre_rec_ctx_t;

static void push_point(ambre_rec_ctx_t *c, const ambre_scen_sample_t *s)
{
    uint32_t slot = c->pt_head % AMBRE_REC_MAX_POINTS;
    ambre_a_point_t *p = &c->pts[slot];
    memset(p, 0, sizeof(*p));
    p->speed_kmh_x4 = s->speed_kmh_x4;
    p->throttle_pct = s->throttle_pct;
    p->brake_active = s->brake_active;
    p->engine_rpm = s->engine_rpm;
    p->steer_angle_deg_x2 = s->steer_deg_x2;
    p->accel_long_mm_s2 = (int16_t)s->a_long_mm_s2;
    p->accel_lat_mm_s2 = (int16_t)s->a_lat_mm_s2;
    p->rel_time_ms = 0;
    c->pt_ts_us[slot] = s->ts_us;
    c->pt_head++;
    if (c->pt_count < AMBRE_REC_MAX_POINTS) c->pt_count++;
}

static void store_samples(ambre_rec_ctx_t *c, uint64_t ts, const ambre_sample_t *s,
                          uint32_t n)
{
    if (c->truth->sample_count + n > c->sample_cap) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = c->truth->sample_count++;
        c->truth->samples[k] = s[i];
        c->truth->sample_ts_us[k] = ts;
    }
}

/* Build the EMERG snapshot from the class A points still in the ring.
 * rel_time_ms is the REAL age of each point against T0 (spec 6.1): the ring
 * keeps the sample timestamp of every point, so no nominal grid is assumed.
 * Points older than the 5 s window (+ sampling phase margin) are dropped and
 * point_count only reflects the points actually emitted. */
static void fill_emerg(ambre_rec_ctx_t *c, ambre_emerg_t *e, uint64_t ts,
                       uint8_t reason, bool locked)
{
    memset(e, 0, sizeof(*e));
    e->event_id = c->truth->event_count;   /* already incremented */
    e->trig_reason = reason;
    e->locked = locked ? 1u : 0u;
    e->event_number = 1;
    e->t0_ts_us = ts;
    e->trig_ts_us = ts;
    e->crash_order = e->event_id;
    uint32_t emitted = 0;
    for (uint32_t i = 0; i < c->pt_count; i++) {
        uint32_t idx = (c->pt_head - c->pt_count + i) % AMBRE_REC_MAX_POINTS;
        uint64_t age_ms = (ts - c->pt_ts_us[idx]) / 1000u;
        if (age_ms > AMBRE_POINT_MAX_AGE_MS) continue;
        e->points[emitted] = c->pts[idx];
        e->points[emitted].rel_time_ms =
            (uint16_t)(age_ms > AMBRE_REL_TIME_MAX_MS ? AMBRE_REL_TIME_MAX_MS
                                                      : age_ms);
        emitted++;
    }
    e->point_count = (uint16_t)emitted;
}

static void do_trigger(ambre_rec_ctx_t *c, const ambre_trig_out_t *out,
                       uint64_t ts)
{
    if (c->truth->event_count >= AMBRE_TRUTH_MAX_EVENTS) return;
    ambre_truth_event_t *te = &c->truth->events[c->truth->event_count++];
    memset(te, 0, sizeof(*te));
    te->triggered = true;
    te->locked = out->locked;
    te->file_complete = true;
    te->t0_ts_us = out->t0_ts_us;
    te->trig_ts_us = ts;
    te->first_sample_ts_us = ts;
    te->last_sample_ts_us = ts;
    fill_emerg(c, &te->emerg, ts, out->reason, out->locked);
    te->event_id = te->emerg.event_id;
    te->order = te->event_id;
    te->emerg_op = (long)ambre_simdisk_op_count(c->sd);
    if (ambre_journal_trigger(c->j, &te->emerg) != AMBRE_OK) {
        te->triggered = false;
        c->truth->event_count--;
        return;
    }
    te->acquired_op = ambre_simdisk_find_mark(c->sd, AMBRE_MARK_ACQUIRED,
                                              te->event_id);
    c->truth->triggered_count++;
    c->events_open++;
}

static void do_seal(ambre_rec_ctx_t *c, ambre_truth_event_t *te, uint64_t ts)
{
    ambre_seal_t seal;
    memset(&seal, 0, sizeof(seal));
    seal.event_id = te->event_id;
    seal.file_complete = 1;
    seal.seal_cause = AMBRE_SEAL_NORMAL;
    seal.locked = te->locked ? 1u : 0u;
    seal.event_number = 1;
    seal.time_since_first_ms = (uint32_t)((ts - te->t0_ts_us) / 1000u);
    te->seal = seal;
    te->sealed = true;
    /* "Sealed before the cut" means the SEAL record reached the disk: that is
     * the first op of the seal sequence, not the durability mark that follows
     * the flush. The recovery reads the record, so the ground truth must use
     * the same instant (invariant I7). */
    te->sealed_op = (long)ambre_simdisk_op_count(c->sd);
    te->sealed_flush_op = -1;
    if (ambre_journal_seal(c->j, &seal) == AMBRE_OK) {
        te->sealed_flush_op = ambre_simdisk_find_mark(c->sd, AMBRE_MARK_SEALED,
                                                      te->event_id);
        if (c->events_open) c->events_open--;
    } else {
        te->sealed_op = -1;
    }
}

static ambre_truth_event_t *open_event(ambre_rec_ctx_t *c)
{
    for (uint32_t i = 0; i < c->truth->event_count; i++) {
        ambre_truth_event_t *te = &c->truth->events[i];
        if (te->triggered && !te->sealed) return te;
    }
    return NULL;
}

/* One millisecond of scenario: data capture, trigger evaluation, sealing. */
static void step_scenario(ambre_rec_ctx_t *c, ambre_trig_state_t *trig,
                          uint32_t i)
{
    const ambre_scen_sample_t *s = &c->sc->s[i];
    ambre_sample_t b[2];
    memset(b, 0, sizeof(b));
    b[0].signal_id = 1;
    b[0].value = s->a_long_mm_s2;
    b[1].signal_id = 2;
    b[1].value = s->a_lat_mm_s2;
    store_samples(c, s->ts_us, b, 2u);
    if ((i % 10u) == 0u) {
        (void)ambre_journal_append_data(c->j, s->ts_us, b, 2u);
    }
    if (s->ts_us >= c->next_pt_us) {
        push_point(c, s);
        c->next_pt_us += AMBRE_POINT_PERIOD_US;
    }
    ambre_trig_ext_t ext;
    memset(&ext, 0, sizeof(ext));
    ext.restraint_deployed = (s->ext_flags & 0x01u) != 0;
    ext.vru_activated = (s->ext_flags & 0x02u) != 0;
    ext.emergency_brake = (s->ext_flags & 0x04u) != 0;
    ext.aeb_intervention = (s->ext_flags & 0x08u) != 0;
    ext.service_command = (s->ext_flags & 0x10u) != 0;
    ext.sample_invalid = (s->ext_flags & 0x20u) != 0;
    ambre_trig_out_t out;
    (void)ambre_trig_push(trig, s->ts_us, s->a_long_mm_s2, s->a_lat_mm_s2,
                          &ext, &out);
    if (out.fired) do_trigger(c, &out, s->ts_us);
    ambre_truth_event_t *te = open_event(c);
    if (te) {
        te->last_sample_ts_us = s->ts_us;
        if (out.event_end) {
            do_seal(c, te, s->ts_us);
            ambre_trig_event_closed(trig, s->ts_us);
        }
    }
    (void)ambre_journal_tick(c->j, s->ts_us);
    c->truth->last_ts_us = s->ts_us;
}

int ambre_explore_record_truth(const ambre_explore_cfg_t *cfg,
                               ambre_simdisk_t *sd, ambre_truth_t *truth)
{
    memset(truth, 0, sizeof(*truth));
    ambre_scenario_t sc;
    if (ambre_scenario_load(cfg, &sc) != AMBRE_OK) return AMBRE_ERR_IO;

    ambre_journal_cfg_t jcfg;
    ambre_journal_cfg_default(&jcfg);
    jcfg.segment_limit = cfg->segment_limit;
    jcfg.flush_period_us = cfg->flush_period_us;
    jcfg.event_flush_period_us = cfg->event_flush_period_us;
    jcfg.session_id = cfg->seed ^ 0xA5A5A5A5u;
    jcfg.continuous_mode = true;

    int rc = AMBRE_ERR_IO;
    uint32_t sample_cap = sc.count * 2u + 8u;
    truth->samples = (ambre_sample_t *)calloc(sample_cap, sizeof(ambre_sample_t));
    truth->sample_ts_us = (uint64_t *)calloc(sample_cap, sizeof(uint64_t));
    ambre_journal_t j;
    ambre_trig_state_t trig;
    ambre_trig_cfg_t tcfg;
    ambre_trig_cfg_default(&tcfg);
    ambre_trig_init(&trig, &tcfg);
    if (!truth->samples || !truth->sample_ts_us) goto out;
    if (ambre_journal_format(&sd->disk, &jcfg) != AMBRE_OK) goto out;
    if (ambre_journal_open(&sd->disk, &jcfg, false, &j) != AMBRE_OK) goto out;

    ambre_rec_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.sc = &sc;
    ctx.truth = truth;
    ctx.j = &j;
    ctx.sd = sd;
    ctx.sample_cap = sample_cap;
    for (uint32_t i = 0; i < sc.count; i++) step_scenario(&ctx, &trig, i);
    ambre_truth_event_t *te = open_event(&ctx);
    if (te) do_seal(&ctx, te, truth->last_ts_us);
    (void)ambre_journal_flush(&j, truth->last_ts_us);
    rc = AMBRE_OK;
out:
    ambre_scenario_free(&sc);
    if (rc != AMBRE_OK) ambre_truth_free(truth);
    return rc;
}
