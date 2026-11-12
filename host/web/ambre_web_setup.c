/*
 * ambre_web_setup.c — Run lifetime: build the simdisk, record the ground
 * truth and the op trace, then answer questions about them.
 *
 * Everything here calls the same functions the PC explorer calls
 * (ambre_explore_record_truth, ambre_simdisk_op): the browser sees the real
 * trace of the real journal, not a simplified model of it.
 */

#include "ambre_web_ctx.h"
#include "ambre_web_json.h"
#include <stdlib.h>
#include <string.h>

static ambre_web_ctx_t g_ctx;

ambre_web_ctx_t *ambre_web_ctx(void)
{
    return &g_ctx;
}

const char *ambre_web_variant_name(int variant)
{
    switch (variant) {
    case AMBRE_WEB_VAR_PREFIX:  return "prefix";
    case AMBRE_WEB_VAR_TORN:    return "torn";
    case AMBRE_WEB_VAR_CACHE:   return "cache";
    case AMBRE_WEB_VAR_BITFLIP: return "bitflip";
    case AMBRE_WEB_VAR_TABLE:   return "table";
    case AMBRE_WEB_VAR_NESTED:  return "nested";
    default:                    return "unknown";
    }
}

const char *ambre_web_variant_note(int variant)
{
    switch (variant) {
    case AMBRE_WEB_VAR_PREFIX:
        return "Plain cut: every write before the cut reached the disk, "
               "nothing after it did.";
    case AMBRE_WEB_VAR_TORN:
        return "Torn write: the last write of the prefix only reached the disk "
               "up to torn_bytes, a whole number of 512-byte sectors.";
    case AMBRE_WEB_VAR_CACHE:
        return "Volatile disk cache: the writes not yet flushed when power died "
               "may or may not have reached the media, one by one.";
    case AMBRE_WEB_VAR_BITFLIP:
        return "Bit flip: one bit of the data area is inverted, as if the media "
               "had decayed (ROB-07).";
    case AMBRE_WEB_VAR_TABLE:
        return "Allocation table destroyed: copy A alone, or both copies, are "
               "zeroed (REP-02).";
    case AMBRE_WEB_VAR_NESTED:
        return "Cut during recovery: the power dies again halfway through the "
               "recovery's own writes (I8).";
    default:
        return "";
    }
}

static void ctx_free(ambre_web_ctx_t *c)
{
    if (c->ws.plain) ambre_explore_ws_free(&c->ws);
    if (c->truth.samples) ambre_truth_free(&c->truth);
    if (c->sd.image) ambre_simdisk_destroy(&c->sd);
    if (c->q.items) ambre_state_queue_free(&c->q);
    free(c->crash_img);
    free(c->after_img);
    memset(c, 0, sizeof(*c));
}

int ambre_web_setup(const char *scenario_path, uint64_t seed,
                    uint32_t duration_ms, uint32_t crash_at_ms,
                    uint32_t segment_limit)
{
    ambre_web_ctx_t *c = ambre_web_ctx();
    ctx_free(c);

    ambre_explore_cfg_default(&c->cfg);
    snprintf(c->scenario, sizeof(c->scenario), "%s",
             (scenario_path && scenario_path[0]) ? scenario_path : "(synthetic)");
    c->cfg.scenario_path = (scenario_path && scenario_path[0]) ? c->scenario : NULL;
    c->cfg.seed = seed;
    if (duration_ms) c->cfg.duration_ms = duration_ms;
    if (crash_at_ms) c->cfg.crash_at_ms = crash_at_ms;
    if (segment_limit > 0u && segment_limit <= AMBRE_SEGMENT_COUNT) {
        c->cfg.segment_limit = segment_limit;
    }
    c->size = AMBRE_FIRST_DATA_OFFSET +
              (uint64_t)c->cfg.segment_limit * AMBRE_SEGMENT_SIZE;

    if (ambre_simdisk_create(&c->sd, c->size) != AMBRE_OK) return AMBRE_ERR_IO;
    if (ambre_explore_record_truth(&c->cfg, &c->sd, &c->truth) != AMBRE_OK) {
        return AMBRE_ERR_IO;
    }
    if (ambre_explore_ws_init(&c->ws, c->size) != AMBRE_OK) return AMBRE_ERR_IO;
    c->crash_img = (uint8_t *)calloc(1, (size_t)c->size);
    c->after_img = (uint8_t *)calloc(1, (size_t)c->size);
    if (!c->crash_img || !c->after_img) return AMBRE_ERR_IO;
    c->ready = true;
    return AMBRE_OK;
}

void ambre_web_teardown(void)
{
    ctx_free(ambre_web_ctx());
}

/* One ground-truth event: what was injected and when it became durable. */
static void info_event(ambre_json_t *j, const ambre_truth_event_t *te)
{
    ambre_json_t ev;
    ambre_json_init(&ev);
    ambre_json_raw(&ev, "{");
    ambre_json_kv_u64(&ev, "event_id", te->event_id);
    ambre_json_kv_bool(&ev, "triggered", te->triggered);
    ambre_json_kv_bool(&ev, "sealed", te->sealed);
    ambre_json_kv_bool(&ev, "locked", te->locked);
    ambre_json_kv_bool(&ev, "file_complete", te->file_complete);
    ambre_json_kv_i64(&ev, "emerg_op", te->emerg_op);
    ambre_json_kv_i64(&ev, "acquired_op", te->acquired_op);
    ambre_json_kv_i64(&ev, "sealed_op", te->sealed_op);
    ambre_json_kv_i64(&ev, "sealed_flush_op", te->sealed_flush_op);
    ambre_json_kv_u64(&ev, "t0_ts_us", te->t0_ts_us);
    ambre_json_kv_u64(&ev, "trig_ts_us", te->trig_ts_us);
    ambre_json_kv_u64(&ev, "trig_reason", te->emerg.trig_reason);
    ambre_json_kv_u64(&ev, "point_count", te->emerg.point_count);
    ambre_json_raw(&ev, ",\"points\":");
    ambre_web_points_json(&ev, &te->emerg);   /* byte-exact injected class A */
    ambre_json_raw(&ev, "}");
    ambre_json_embed(j, &ev);
    ambre_json_free(&ev);
}

/* The injected class B stream, one entry per millisecond: [ts_us, a_long,
 * a_lat] in mm/s^2. The page draws the road and the acceleration curve from
 * it, so the animation shows the data that was really injected. */
const char *ambre_web_samples_json(uint32_t from, uint32_t count)
{
    ambre_web_ctx_t *c = ambre_web_ctx();
    uint32_t total = c->truth.sample_count / 2u;   /* two signals per sample */
    uint32_t end = from + count;
    ambre_json_t j;
    char tmp[128];

    if (end > total || end < from) end = total;
    ambre_json_init(&j);
    snprintf(tmp, sizeof(tmp), "{\"from\":%u,\"count\":%u,\"total\":%u,\"samples\":[",
             from, end > from ? end - from : 0u, total);
    ambre_json_raw(&j, tmp);
    for (uint32_t i = from; i < end; i++) {
        const ambre_sample_t *s = &c->truth.samples[i * 2u];
        snprintf(tmp, sizeof(tmp), "%s[%llu,%d,%d]", i > from ? "," : "",
                 (unsigned long long)c->truth.sample_ts_us[i * 2u],
                 s->value, (s + 1)->value);
        ambre_json_raw(&j, tmp);
    }
    ambre_json_raw(&j, "]}");
    const char *out = ambre_json_publish(&j);
    ambre_json_free(&j);
    return out;
}

const char *ambre_web_info_json(void)
{
    ambre_web_ctx_t *c = ambre_web_ctx();
    ambre_json_t j;
    ambre_json_init(&j);
    ambre_json_raw(&j, "{");
    ambre_json_kv_bool(&j, "ready", c->ready);
    ambre_json_kv_str(&j, "scenario", c->scenario);
    ambre_json_kv_u64(&j, "op_count", ambre_simdisk_op_count(&c->sd));
    ambre_json_kv_u64(&j, "disk_size", c->size);
    ambre_json_kv_u64(&j, "segment_limit", c->cfg.segment_limit);
    ambre_json_kv_u64(&j, "segment_size", AMBRE_SEGMENT_SIZE);
    ambre_json_kv_u64(&j, "block_size", AMBRE_BLOCK_SIZE);
    ambre_json_kv_u64(&j, "blocks_per_segment", AMBRE_BLOCKS_PER_SEGMENT);
    ambre_json_kv_u64(&j, "duration_ms", c->cfg.duration_ms);
    ambre_json_kv_u64(&j, "crash_at_ms", c->cfg.crash_at_ms);
    ambre_json_kv_u64(&j, "seed", c->cfg.seed);
    ambre_json_kv_u64(&j, "sample_count", c->truth.sample_count);
    ambre_json_kv_u64(&j, "last_ts_us", c->truth.last_ts_us);
    ambre_json_kv_u64(&j, "max_write_bytes", AMBRE_SIMDISK_MAX_DATA);
    ambre_json_kv_u64(&j, "max_ops", AMBRE_SIMDISK_MAX_OPS);
    ambre_json_raw(&j, ",\"events\":[");
    for (uint32_t i = 0; i < c->truth.event_count; i++) {
        info_event(&j, &c->truth.events[i]);   /* embed() writes the commas */
    }
    ambre_json_raw(&j, "]}");
    return ambre_json_publish(&j);
}

/* Metadata of a logged write: the block header is still intact in the trace
 * (the torn variants truncate it only when a state is materialised), so the
 * page gets a real clock and a real record type for every write. */
static void ops_block_meta(ambre_json_t *j, const ambre_simdisk_t *sd,
                           const ambre_op_t *op)
{
    const uint8_t *raw = sd->data_pool[op->data_id];
    ambre_block_t b;
    uint32_t magic = 0u;
    char tmp[96];

    memcpy(&magic, raw, sizeof(magic));
    if (magic == AMBRE_MAGIC_TABLE) {
        ambre_json_raw(j, ",0,9,0,0,1");
        return;
    }
    if (magic != AMBRE_MAGIC_BLOCK) {
        ambre_json_raw(j, ",0,0,0,0,0");
        return;
    }
    ambre_block_unpack(raw, &b);
    snprintf(tmp, sizeof(tmp), ",%llu,%u,%llu,%u,%u",
             (unsigned long long)b.hdr.first_ts_us, b.hdr.rec_type,
             (unsigned long long)b.hdr.seq, b.hdr.chunk_index, b.hdr.chunk_count);
    ambre_json_raw(j, tmp);
}

/* One op of the trace, compact: the page draws thousands of them.
 * [kind, off, len] then either the mark fields or the block metadata. */
static void ops_one(ambre_json_t *j, const ambre_simdisk_t *sd,
                    const ambre_op_t *op)
{
    char tmp[96];

    ambre_json_raw(j, op->kind == AMBRE_OP_WRITE ? "[1" :
                      op->kind == AMBRE_OP_FLUSH ? "[2" : "[3");
    snprintf(tmp, sizeof(tmp), ",%llu,%u", (unsigned long long)op->off, op->len);
    ambre_json_raw(j, tmp);
    if (op->kind == AMBRE_OP_MARK) {
        snprintf(tmp, sizeof(tmp), ",%u,%llu,%llu", op->mark_kind,
                 (unsigned long long)op->mark_value,
                 (unsigned long long)op->mark_aux);
        ambre_json_raw(j, tmp);
    } else if (op->kind == AMBRE_OP_WRITE && op->len >= AMBRE_BLOCK_SIZE) {
        ops_block_meta(j, sd, op);
    }
    ambre_json_raw(j, "]");
}

const char *ambre_web_trace_json(void)
{
    ambre_web_ctx_t *c = ambre_web_ctx();
    uint32_t total = ambre_simdisk_op_count(&c->sd);
    uint32_t writes = 0u, flushes = 0u, marks = 0u, longest = 0u;
    uint64_t bytes = 0u;
    ambre_json_t j;

    for (uint32_t i = 0; i < total; i++) {
        const ambre_op_t *op = ambre_simdisk_op(&c->sd, i);
        if (op->kind == AMBRE_OP_WRITE) {
            writes++;
            bytes += op->len;
            if (op->len > longest) longest = op->len;
        } else if (op->kind == AMBRE_OP_FLUSH) {
            flushes++;
        } else {
            marks++;
        }
    }
    ambre_json_init(&j);
    ambre_json_raw(&j, "{");
    ambre_json_kv_u64(&j, "op_count", total);
    ambre_json_kv_u64(&j, "writes", writes);
    ambre_json_kv_u64(&j, "flushes", flushes);
    ambre_json_kv_u64(&j, "marks", marks);
    ambre_json_kv_u64(&j, "longest_write", longest);
    ambre_json_kv_u64(&j, "bytes_written", bytes);
    ambre_json_kv_u64(&j, "max_ops_capacity", AMBRE_SIMDISK_MAX_OPS);
    ambre_json_kv_u64(&j, "max_write_capacity", AMBRE_SIMDISK_MAX_DATA);
    ambre_json_raw(&j, "}");
    const char *out = ambre_json_publish(&j);
    ambre_json_free(&j);
    return out;
}

const char *ambre_web_ops_json(uint32_t from, uint32_t count)
{
    ambre_web_ctx_t *c = ambre_web_ctx();
    uint32_t total = ambre_simdisk_op_count(&c->sd);
    uint32_t end = from + count;
    ambre_json_t j;
    ambre_json_init(&j);

    if (end > total || end < from) end = total;
    ambre_json_raw(&j, "{\"from\":");
    char tmp[48];
    snprintf(tmp, sizeof(tmp), "%u,\"count\":%u,\"total\":%u,\"ops\":[",
             from, end > from ? end - from : 0u, total);
    ambre_json_raw(&j, tmp);
    for (uint32_t i = from; i < end; i++) {
        if (i > from) ambre_json_ch(&j, ',');
        ops_one(&j, &c->sd, ambre_simdisk_op(&c->sd, i));
    }
    ambre_json_raw(&j, "]}");
    return ambre_json_publish(&j);
}
