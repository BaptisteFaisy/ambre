/*
 * ambre_web_sweep.c — Run the whole exploration inside the page.
 *
 * Same state space as the PC explorer (ambre_explore_gen_states) and same
 * counters (ambre_explore_merge_stats), so a browser sweep and `ambre-explore
 * --report` describe an exploration the same way. The only difference is that
 * a browser has one thread: the page calls sweep_step() in small batches and
 * keeps drawing between them.
 */

#define _POSIX_C_SOURCE 200809L   /* clock_gettime */

#include "ambre_web_ctx.h"
#include "ambre_web_json.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void apply_variants(ambre_explore_cfg_t *cfg, uint32_t variants)
{
    cfg->variant_torn = (variants & AMBRE_WEB_SWEEP_TORN) != 0u;
    cfg->variant_volatile_cache = (variants & AMBRE_WEB_SWEEP_CACHE) != 0u;
    cfg->variant_bitflip = (variants & AMBRE_WEB_SWEEP_BITFLIP) != 0u;
    cfg->variant_table_wipe = (variants & AMBRE_WEB_SWEEP_TABLE) != 0u;
    cfg->nested_recovery = (variants & AMBRE_WEB_SWEEP_NESTED) != 0u;
}

static void states_by_variant_fragment(ambre_json_t *out,
                                       const ambre_state_queue_t *q)
{
    static const char *names[6] = { "prefix", "torn", "cache", "bitflip",
                                    "table", "nested" };
    uint64_t count[6] = { 0 };
    ambre_json_t j;
    char tmp[48];

    for (size_t i = 0; i < q->count; i++) {
        int v = q->items[i].variant_type;
        if (v >= 0 && v < 6) count[v]++;
    }
    ambre_json_init(&j);
    ambre_json_raw(&j, "\"planned\":{");
    for (uint32_t i = 0; i < 6u; i++) {
        snprintf(tmp, sizeof(tmp), "%s\"%s\":%llu", i ? "," : "", names[i],
                 (unsigned long long)count[i]);
        ambre_json_raw(&j, tmp);
    }
    ambre_json_raw(&j, "}");
    ambre_json_embed(out, &j);
    ambre_json_free(&j);
}

const char *ambre_web_sweep_start_json(uint32_t max_states, uint32_t variants)
{
    ambre_web_ctx_t *ctx = ambre_web_ctx();
    ambre_json_t j;

    if (!ctx->ready) return "{\"error\":\"not ready\"}";
    if (ctx->q.items) ambre_state_queue_free(&ctx->q);
    apply_variants(&ctx->cfg, variants);
    ctx->cfg.max_states = max_states;
    ctx->cfg.jobs = 1u;                     /* one thread in a browser */
    if (ambre_explore_gen_states(&ctx->sd, &ctx->cfg, &ctx->q) != AMBRE_OK) {
        return "{\"error\":\"could not build the state space\"}";
    }
    memset(&ctx->stats, 0, sizeof(ctx->stats));
    ctx->sweep_done = 0u;
    ctx->sweep_start = now_seconds();

    ambre_json_init(&j);
    ambre_json_raw(&j, "{");
    ambre_json_kv_u64(&j, "total", ctx->q.count);
    ambre_json_kv_bool(&j, "truncated", max_states != 0u &&
                                        ctx->q.count >= max_states);
    ambre_json_kv_u64(&j, "max_states", max_states);
    states_by_variant_fragment(&j, &ctx->q);
    ambre_json_raw(&j, "}");
    const char *out = ambre_json_publish(&j);
    ambre_json_free(&j);
    return out;
}

/* Counters of the exploration, in the shape ambre-explore --report writes. */
static void stats_fragment(ambre_json_t *out, const ambre_explore_stats_t *st)
{
    static const char *inv[AMBRE_INVARIANT_COUNT] = {
        "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8"
    };
    static const char *names[6] = { "prefix", "torn", "cache", "bitflip",
                                    "table", "nested" };
    ambre_json_t j;
    char tmp[64];

    ambre_json_init(&j);
    ambre_json_raw(&j, "\"states_by_variant\":{");
    for (uint32_t i = 0; i < 6u; i++) {
        snprintf(tmp, sizeof(tmp), "%s\"%s\":%llu", i ? "," : "", names[i],
                 (unsigned long long)st->states_by_variant[i]);
        ambre_json_raw(&j, tmp);
    }
    ambre_json_raw(&j, "},\"violations_by_invariant\":{");
    for (uint32_t i = 0; i < AMBRE_INVARIANT_COUNT; i++) {
        snprintf(tmp, sizeof(tmp), "%s\"%s\":%llu", i ? "," : "", inv[i],
                 (unsigned long long)st->per_invariant[i]);
        ambre_json_raw(&j, tmp);
    }
    ambre_json_raw(&j, "}");
    ambre_json_embed(out, &j);
    ambre_json_free(&j);
}

/* One state of the sweep, counted exactly like the PC explorer counts it. */
static void sweep_one(ambre_web_ctx_t *ctx, const ambre_explore_state_t *st)
{
    ambre_invariant_report_t rep;
    ambre_recovery_result_t rec;

    memset(&rep, 0, sizeof(rep));
    memset(&rec, 0, sizeof(rec));
    if (ambre_explore_build_image(&ctx->ws, &ctx->sd, st) != 0) return;
    int rc = ambre_explore_judge(&ctx->ws, &ctx->truth, st, &ctx->sd, &rep, &rec);
    if (rc < 0) return;
    ctx->stats.states_explored++;
    ctx->stats.recoveries_run++;
    ctx->stats.events_acquired += ambre_explore_acquired_before(&ctx->truth,
                                                                st->prefix);
    if (st->variant_type >= 0 && st->variant_type < 6) {
        ctx->stats.states_by_variant[st->variant_type]++;
    }
    if (rc > 0) {
        ctx->stats.violations++;
        if (rep.violated_mask & (1u << AMBRE_I3_NO_PHANTOM)) {
            ctx->stats.events_phantom++;
        }
        for (uint32_t k = 0; k < AMBRE_INVARIANT_COUNT; k++) {
            if (rep.violated_mask & (1u << k)) ctx->stats.per_invariant[k]++;
        }
        if (ctx->stats.first_violation[0] == '\0') {
            snprintf(ctx->stats.first_violation,
                     sizeof(ctx->stats.first_violation),
                     "prefix=%u variant=%d: %s", st->prefix, st->variant_type,
                     rep.detail);
        }
    }
}

const char *ambre_web_sweep_step_json(uint32_t states)
{
    ambre_web_ctx_t *ctx = ambre_web_ctx();
    ambre_json_t j;
    uint32_t done = 0;

    if (!ctx->ready) return "{\"error\":\"not ready\"}";
    while (done < states && ctx->sweep_done < ctx->q.count) {
        sweep_one(ctx, &ctx->q.items[ctx->sweep_done++]);
        done++;
    }
    ctx->stats.seconds = now_seconds() - ctx->sweep_start;
    if (ctx->stats.seconds <= 0.0) ctx->stats.seconds = 0.001;

    ambre_json_init(&j);
    ambre_json_raw(&j, "{");
    ambre_json_kv_u64(&j, "done", ctx->sweep_done);
    ambre_json_kv_u64(&j, "total", ctx->q.count);
    ambre_json_kv_bool(&j, "finished", ctx->sweep_done >= ctx->q.count);
    ambre_json_kv_u64(&j, "states_explored", ctx->stats.states_explored);
    ambre_json_kv_u64(&j, "violations", ctx->stats.violations);
    ambre_json_kv_u64(&j, "recoveries_run", ctx->stats.recoveries_run);
    ambre_json_kv_u64(&j, "events_acquired", ctx->stats.events_acquired);
    ambre_json_kv_u64(&j, "events_phantom", ctx->stats.events_phantom);
    ambre_json_kv_dbl(&j, "seconds", ctx->stats.seconds);
    ambre_json_kv_dbl(&j, "states_per_second",
                      ctx->stats.states_explored / ctx->stats.seconds);
    ambre_json_kv_str(&j, "first_violation", ctx->stats.first_violation);
    stats_fragment(&j, &ctx->stats);
    ambre_json_raw(&j, "}");
    const char *out = ambre_json_publish(&j);
    ambre_json_free(&j);
    return out;
}
