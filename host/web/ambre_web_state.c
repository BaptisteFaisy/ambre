/*
 * ambre_web_state.c — One crash state, as the page shows it.
 *
 * The three steps are the explorer's own: ambre_explore_build_image() puts the
 * disk in the state the cut left it, ambre_explore_judge() runs the real
 * recovery and the real I1..I8 battery, and ambre_journal_read_event() reads
 * back what the black box holds. Nothing here re-implements a rule.
 */

#include "ambre_web_ctx.h"
#include "ambre_web_json.h"
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers -- */

typedef struct {
    ambre_disk_t disk;
    uint8_t     *img;
    uint64_t     size;
} buf_disk_t;

static int bd_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    buf_disk_t *b = (buf_disk_t *)ctx;
    if (!b || !buf || off + len > b->size || off + len < off) return AMBRE_ERR_IO;
    memcpy(buf, b->img + off, len);
    return AMBRE_OK;
}

static int bd_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    buf_disk_t *b = (buf_disk_t *)ctx;
    if (!b || !buf || off + len > b->size || off + len < off) return AMBRE_ERR_IO;
    memcpy(b->img + off, buf, len);
    return AMBRE_OK;
}

static int bd_flush(void *ctx)
{
    (void)ctx;
    return AMBRE_OK;
}

static void buf_disk_init(buf_disk_t *b, uint8_t *img, uint64_t size)
{
    memset(b, 0, sizeof(*b));
    b->img = img;
    b->size = size;
    b->disk.ctx = b;
    b->disk.size = size;
    b->disk.sector_size = AMBRE_BLOCK_SIZE;
    b->disk.read = bd_read;
    b->disk.write = bd_write;
    b->disk.flush = bd_flush;
}

static void fill_state(ambre_explore_state_t *st, uint32_t prefix, int variant,
                       uint32_t torn_bytes, uint64_t keep_mask,
                       uint64_t corrupt_off, uint32_t corrupt_bit,
                       int wipe_both_tables)
{
    memset(st, 0, sizeof(*st));
    st->prefix = prefix;
    st->variant_type = variant;
    st->variant.prefix = prefix;
    st->variant.torn_last_write = (variant == AMBRE_WEB_VAR_TORN);
    st->variant.torn_bytes = torn_bytes;
    st->variant.drop_unflushed = (variant == AMBRE_WEB_VAR_CACHE);
    st->variant.keep_mask = keep_mask;
    st->variant.corrupt_bit = (variant == AMBRE_WEB_VAR_BITFLIP);
    st->variant.corrupt_off = corrupt_off;
    st->variant.corrupt_bit_index = (uint8_t)corrupt_bit;
    st->variant.wipe_table_a = (variant == AMBRE_WEB_VAR_TABLE) && !wipe_both_tables;
    st->variant.wipe_both_tables = (variant == AMBRE_WEB_VAR_TABLE) && wipe_both_tables;
}

static const char *invariant_name(uint32_t bit)
{
    switch (bit) {
    case AMBRE_I1_TERMINATES: return "I1";
    case AMBRE_I2_ACQUIRED:   return "I2";
    case AMBRE_I3_NO_PHANTOM: return "I3";
    case AMBRE_I4_SAMPLES:    return "I4";
    case AMBRE_I5_RECENCY:    return "I5";
    case AMBRE_I6_LOCKED:     return "I6";
    case AMBRE_I7_COMPLETE:   return "I7";
    case AMBRE_I8_REPEATABLE: return "I8";
    default:                  return "?";
    }
}

/* ------------------------------------------------------------------ image -- */

/* 0 = untouched, 1..5 = a valid record of that type, -1 = written but invalid
 * (torn, flipped, stale): the page colours blocks from this code. */
static int block_code(const uint8_t *img, uint64_t off)
{
    ambre_block_t b;
    uint32_t magic = 0u;

    memcpy(&magic, img + off, sizeof(magic));
    if (magic != AMBRE_MAGIC_BLOCK) {
        for (uint32_t k = 0; k < AMBRE_BLOCK_SIZE; k++) {
            if (img[off + k] != 0u) return -1;
        }
        return 0;
    }
    ambre_block_unpack(img + off, &b);
    return (ambre_block_validate(&b) == AMBRE_OK) ? b.hdr.rec_type : -1;
}

static int table_code(const uint8_t *img, uint64_t off)
{
    ambre_alloc_table_t t;

    for (uint32_t k = 0; k < AMBRE_BLOCK_SIZE; k++) {
        if (img[off + k] != 0u) break;
        if (k == AMBRE_BLOCK_SIZE - 1u) return 0;   /* never written */
    }
    ambre_table_unpack(img + off, &t);
    return (ambre_table_validate(&t) == AMBRE_OK) ? 1 : -1;
}

static uint64_t table_gen(const uint8_t *img, uint64_t off)
{
    ambre_alloc_table_t t;

    ambre_table_unpack(img + off, &t);
    return (ambre_table_validate(&t) == AMBRE_OK) ? t.generation : 0u;
}

/* One run of consecutive blocks sharing a code. */
static void map_run(ambre_json_t *j, bool *first, uint32_t seg, uint32_t blk,
                    uint32_t len, int code)
{
    char tmp[64];

    if (!len) return;
    snprintf(tmp, sizeof(tmp), "%s[%u,%u,%u,%d]", *first ? "" : ",",
             seg, blk, len, code);
    ambre_json_raw(j, tmp);
    *first = false;
}

/* The block map of one image, run-length encoded: one entry per consecutive
 * run of blocks sharing a code. A journal of thousands of DATA blocks stays a
 * few hundred entries. */
static void image_runs(ambre_json_t *j, const uint8_t *img, uint64_t size,
                       uint32_t segment_limit)
{
    bool first = true;
    int run = 0, code = 0;
    uint32_t run_seg = 0, run_blk = 0, run_len = 0;

    ambre_json_raw(j, "[");
    for (uint32_t seg = 0; seg < segment_limit; seg++) {
        for (uint32_t blk = 0; blk < AMBRE_BLOCKS_PER_SEGMENT; blk++) {
            uint64_t off = ambre_block_offset(seg, blk);
            if (off + AMBRE_BLOCK_SIZE > size) break;
            code = block_code(img, off);
            if (run_len && code == run) { run_len++; continue; }
            map_run(j, &first, run_seg, run_blk, run_len, run);
            run = code;
            run_seg = seg;
            run_blk = blk;
            run_len = 1u;
        }
    }
    map_run(j, &first, run_seg, run_blk, run_len, run);
    ambre_json_raw(j, "]");
}

static void image_fragment(ambre_json_t *out, const char *key,
                           const uint8_t *img, uint64_t size,
                           uint32_t segment_limit)
{
    ambre_json_t j;

    ambre_json_init(&j);
    ambre_json_raw(&j, "\"");
    ambre_json_raw(&j, key);
    ambre_json_raw(&j, "\":{");
    ambre_json_kv_i64(&j, "ta", table_code(img, AMBRE_TABLE_A_OFFSET));
    ambre_json_kv_i64(&j, "tb", table_code(img, AMBRE_TABLE_B_OFFSET));
    ambre_json_kv_u64(&j, "ga", table_gen(img, AMBRE_TABLE_A_OFFSET));
    ambre_json_kv_u64(&j, "gb", table_gen(img, AMBRE_TABLE_B_OFFSET));
    ambre_json_raw(&j, ",\"runs\":");
    image_runs(&j, img, size, segment_limit);
    ambre_json_raw(&j, "}");
    ambre_json_embed(out, &j);
    ambre_json_free(&j);
}

/* ------------------------------------------------------------------ event -- */

void ambre_web_points_json(ambre_json_t *j, const ambre_emerg_t *e)
{
    char tmp[256];
    uint32_t n = e->point_count > 21u ? 21u : e->point_count;

    ambre_json_raw(j, "[");
    for (uint32_t i = 0; i < n; i++) {
        const ambre_a_point_t *p = &e->points[i];
        snprintf(tmp, sizeof(tmp), "%s[%u,%.2f,%u,%u,%u,%.1f,%d,%d,%u,%u,%u,%u]",
                 i ? "," : "", p->rel_time_ms, p->speed_kmh_x4 / 4.0,
                 p->throttle_pct, p->brake_active, p->engine_rpm,
                 p->steer_angle_deg_x2 / 2.0, p->accel_long_mm_s2,
                 p->accel_lat_mm_s2, p->belt_driver, p->airbag_lamp,
                 p->abs_active, p->esc_active);
        ambre_json_raw(j, tmp);
    }
    ambre_json_raw(j, "]");
}

static void event_emerg(ambre_json_t *j, const ambre_emerg_t *e)
{
    ambre_json_raw(j, "\"emerg\":{");
    ambre_json_kv_u64(j, "point_count", e->point_count);
    ambre_json_kv_u64(j, "t0_ts_us", e->t0_ts_us);
    ambre_json_kv_u64(j, "trig_ts_us", e->trig_ts_us);
    ambre_json_kv_u64(j, "trig_reason", e->trig_reason);
    ambre_json_kv_u64(j, "locked", e->locked);
    ambre_json_kv_u64(j, "event_number", e->event_number);
    ambre_json_raw(j, ",\"points\":");
    ambre_web_points_json(j, e);
    ambre_json_raw(j, "}");
}

static void event_seal(ambre_json_t *j, const ambre_seal_t *s)
{
    ambre_json_raw(j, "\"seal\":{");
    ambre_json_kv_u64(j, "file_complete", s->file_complete);
    ambre_json_kv_u64(j, "cause", s->seal_cause);
    ambre_json_kv_u64(j, "locked", s->locked);
    ambre_json_kv_u64(j, "time_since_first_ms", s->time_since_first_ms);
    ambre_json_kv_i64(j, "max_dv_long_mm_s", s->max_dv_long_mm_s);
    ambre_json_kv_i64(j, "max_dv_lat_mm_s", s->max_dv_lat_mm_s);
    ambre_json_kv_u64(j, "restraint_deploy_ms", s->restraint_deploy_ms);
    ambre_json_raw(j, "}");
}

/* What the recovered journal holds for one event, read through the real
 * read path (ambre_journal_read_event) — the same one edrctl uses. */
static void event_json(ambre_json_t *out, const ambre_journal_t *jr,
                       const ambre_ev_desc_t *ev)
{
    ambre_event_view_t view;
    ambre_json_t j;

    memset(&view, 0, sizeof(view));
    (void)ambre_journal_read_event(jr, ev->event_id, &view);
    ambre_json_init(&j);
    ambre_json_raw(&j, "{");
    ambre_json_kv_u64(&j, "event_id", ev->event_id);
    ambre_json_kv_u64(&j, "state", ev->state);
    ambre_json_kv_u64(&j, "locked", ev->locked);
    ambre_json_kv_u64(&j, "file_complete", ev->file_complete);
    ambre_json_kv_u64(&j, "trig_reason", ev->trig_reason);
    ambre_json_kv_u64(&j, "start_seg", ev->start_seg);
    ambre_json_kv_u64(&j, "start_block", ev->start_block);
    ambre_json_kv_u64(&j, "end_block", ev->end_block);
    ambre_json_kv_u64(&j, "seq_emerg", ev->seq_emerg);
    ambre_json_kv_u64(&j, "seq_last", ev->seq_last);
    ambre_json_kv_bool(&j, "have_emerg", view.have_emerg);
    ambre_json_kv_bool(&j, "have_seal", view.have_seal);
    ambre_json_kv_u64(&j, "cont_count", view.cont_count);
    ambre_json_kv_u64(&j, "data_blocks", view.data_blocks);
    ambre_json_kv_u64(&j, "last_ts_us", view.last_ts_us);
    if (view.have_emerg) {
        ambre_json_raw(&j, ",");
        event_emerg(&j, &view.emerg);
    }
    if (view.have_seal) {
        ambre_json_raw(&j, ",");
        event_seal(&j, &view.seal);
    }
    ambre_json_raw(&j, "}");
    ambre_json_embed(out, &j);
    ambre_json_free(&j);
}

/* The events of the recovered image, read with the real read path. */
static void events_fragment(ambre_json_t *out, const ambre_web_ctx_t *ctx,
                            const ambre_recovery_result_t *rec)
{
    buf_disk_t bd;
    ambre_journal_t jr;
    ambre_journal_cfg_t jcfg;
    ambre_alloc_table_t tbl;
    ambre_json_t j;
    int copy_used = -1;

    buf_disk_init(&bd, ctx->after_img, ctx->size);
    ambre_json_init(&j);
    ambre_json_raw(&j, "\"events\":[");
    if (ambre_table_read_best(&bd.disk, &tbl, &copy_used) == AMBRE_OK) {
        ambre_journal_cfg_default(&jcfg);
        jcfg.segment_limit = ctx->cfg.segment_limit;
        jcfg.session_id = rec->table.session_id;
        if (ambre_journal_open(&bd.disk, &jcfg, false, &jr) == AMBRE_OK) {
            for (uint32_t i = 0;
                 i < rec->table.event_count && i < AMBRE_MAX_EVENTS; i++) {
                const ambre_ev_desc_t *ev = &rec->table.events[i];
                if (ev->state == AMBRE_EV_EMPTY) continue;
                event_json(&j, &jr, ev);      /* embed() writes the commas */
            }
        }
    }
    ambre_json_raw(&j, "]");
    ambre_json_embed(out, &j);
    ambre_json_free(&j);
}

/* ------------------------------------------------------------- fragments -- */

static void violations_fragment(ambre_json_t *out, uint32_t mask)
{
    ambre_json_t j;
    bool first = true;

    ambre_json_init(&j);
    ambre_json_raw(&j, "\"violations\":[");
    for (uint32_t b = 0; b < AMBRE_INVARIANT_COUNT; b++) {
        if (!(mask & (1u << b))) continue;
        if (!first) ambre_json_ch(&j, ',');
        ambre_json_str(&j, invariant_name(b));
        first = false;
    }
    ambre_json_raw(&j, "]");
    ambre_json_embed(out, &j);
    ambre_json_free(&j);
}

static void recovery_fragment(ambre_json_t *out, const ambre_recovery_result_t *rec)
{
    ambre_json_t j;

    ambre_json_init(&j);
    ambre_json_raw(&j, "\"recovery\":{");
    ambre_json_kv_u64(&j, "scanned_blocks", rec->scanned_blocks);
    ambre_json_kv_u64(&j, "events_before", rec->events_before);
    ambre_json_kv_u64(&j, "events_after", rec->events_after);
    ambre_json_kv_u64(&j, "sealed_incomplete", rec->sealed_incomplete);
    ambre_json_kv_u64(&j, "torn_records", rec->torn_records);
    ambre_json_kv_u64(&j, "table_commits", rec->table_commits);
    ambre_json_kv_bool(&j, "table_rebuilt", rec->table_rebuilt);
    ambre_json_kv_str(&j, "diag", rec->diag);
    ambre_json_raw(&j, "}");
    ambre_json_embed(out, &j);
    ambre_json_free(&j);
}

/* Run the state: crash image in ctx->crash_img, recovered image in
 * ctx->after_img. Returns the explorer's own verdict (1 = violated). */
static int run_state(ambre_web_ctx_t *ctx, const ambre_explore_state_t *st,
                     ambre_invariant_report_t *rep, ambre_recovery_result_t *rec)
{
    const ambre_disk_t *rd;

    if (ambre_explore_build_image(&ctx->ws, &ctx->sd, st) != 0) return -1;
    memcpy(ctx->crash_img, ctx->ws.work.image, (size_t)ctx->size);
    int rc = ambre_explore_judge(&ctx->ws, &ctx->truth, st, &ctx->sd, rep, rec);
    rd = ambre_explore_recovered_disk(&ctx->ws, st);
    if (rd && rd->ctx) {
        const ambre_memdisk_t *md = (const ambre_memdisk_t *)rd->ctx;
        memcpy(ctx->after_img, md->image, (size_t)ctx->size);
    }
    return rc;
}

/* The scalar part of a state answer: what was asked, and the verdict. */
static void state_scalars(ambre_json_t *j, const ambre_web_ctx_t *ctx,
                          uint32_t prefix, int variant, int rc,
                          const ambre_invariant_report_t *rep,
                          uint32_t torn_bytes, uint64_t keep_mask,
                          uint64_t corrupt_off, uint32_t corrupt_bit,
                          int wipe_both_tables)
{
    ambre_json_kv_u64(j, "prefix", prefix);
    ambre_json_kv_str(j, "variant", ambre_web_variant_name(variant));
    ambre_json_kv_i64(j, "variant_id", variant);
    ambre_json_kv_str(j, "note", ambre_web_variant_note(variant));
    ambre_json_kv_i64(j, "rc", rc);
    ambre_json_kv_bool(j, "ok", rc == 0 && rep->violated_mask == 0);
    ambre_json_kv_u64(j, "violated_mask", rep->violated_mask);
    ambre_json_kv_str(j, "detail", rep->detail);
    ambre_json_kv_u64(j, "torn_bytes", torn_bytes);
    ambre_json_kv_u64(j, "keep_mask", keep_mask);
    ambre_json_kv_u64(j, "corrupt_off", corrupt_off);
    ambre_json_kv_u64(j, "corrupt_bit", corrupt_bit);
    ambre_json_kv_bool(j, "wipe_both_tables", wipe_both_tables != 0);
    ambre_json_kv_u64(j, "acquired_before_cut",
                      ambre_explore_acquired_before(&ctx->truth, prefix));
}

const char *ambre_web_state_json(uint32_t prefix, int variant,
                                  uint32_t torn_bytes, uint64_t keep_mask,
                                  uint64_t corrupt_off, uint32_t corrupt_bit,
                                  int wipe_both_tables)
{
    ambre_web_ctx_t *ctx = ambre_web_ctx();
    ambre_explore_state_t st;
    ambre_invariant_report_t rep;
    ambre_recovery_result_t rec;
    ambre_json_t j;
    int rc;

    if (!ctx->ready) return "{\"error\":\"not ready\"}";
    if (prefix > ambre_simdisk_op_count(&ctx->sd)) {
        return "{\"error\":\"prefix beyond the end of the trace\"}";
    }
    if (variant < 0 || variant > AMBRE_WEB_VAR_NESTED) {
        return "{\"error\":\"unknown variant\"}";
    }
    memset(&rep, 0, sizeof(rep));
    memset(&rec, 0, sizeof(rec));
    fill_state(&st, prefix, variant, torn_bytes, keep_mask, corrupt_off,
               corrupt_bit, wipe_both_tables);
    rc = run_state(ctx, &st, &rep, &rec);

    ambre_json_init(&j);
    ambre_json_raw(&j, "{");
    state_scalars(&j, ctx, prefix, variant, rc, &rep, torn_bytes, keep_mask,
                  corrupt_off, corrupt_bit, wipe_both_tables);
    violations_fragment(&j, rep.violated_mask);
    recovery_fragment(&j, &rec);
    events_fragment(&j, ctx, &rec);
    image_fragment(&j, "image", ctx->crash_img, ctx->size, ctx->cfg.segment_limit);
    image_fragment(&j, "after", ctx->after_img, ctx->size, ctx->cfg.segment_limit);
    ambre_json_raw(&j, "}");
    const char *out = ambre_json_publish(&j);
    ambre_json_free(&j);
    return out;
}
