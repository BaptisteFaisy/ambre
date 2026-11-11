/*
 * fuzz_gen.c — Image families for the journal fuzzer (spec 12.10).
 *
 * Deterministic by construction: every byte and every choice comes from a
 * splitmix64 stream seeded with mix(seed, index). Regenerating an image from
 * its (seed, index) pair gives the exact same bytes, which is how violating
 * images are saved and replayed as regression tests (spec 7.7).
 */

#include "fuzz_gen.h"
#include <stdlib.h>
#include <string.h>

static const char *const STRATEGY_NAMES[FUZZ_S_COUNT] = {
    "noise", "sparse", "partial", "torn-record", "bad-table", "bitflip", "mix"
};

const char *fuzz_strategy_name(int strategy)
{
    if (strategy < 0 || strategy >= FUZZ_S_COUNT) {
        return "unknown";
    }
    return STRATEGY_NAMES[strategy];
}

int fuzz_gen_ws_init(fuzz_gen_ws_t *ws)
{
    memset(ws, 0, sizeof(*ws));
    if (ambre_memdisk_init(&ws->scratch, AMBRE_JOURNAL_BYTES) != AMBRE_OK) {
        return -1;
    }
    ws->base = (uint8_t *)calloc(1, (size_t)AMBRE_JOURNAL_BYTES);
    if (ws->base == NULL) {
        fuzz_gen_ws_free(ws);
        return -1;
    }
    ws->base_size = AMBRE_JOURNAL_BYTES;
    return 0;
}

void fuzz_gen_ws_free(fuzz_gen_ws_t *ws)
{
    ambre_memdisk_destroy(&ws->scratch);
    free(ws->base);
    ws->base = NULL;
}

/* ------------------------------------------------------------ primitives - */

/* Fill [off, off+len) with PRNG bytes. */
static void fill_noise(uint8_t *image, uint64_t off, uint64_t len,
                       fuzz_rng *rng)
{
    for (uint64_t i = 0u; i + 8u <= len; i += 8u) {
        uint64_t v = fuzz_rng_next(rng);
        memcpy(image + off + i, &v, sizeof(v));
    }
    for (uint64_t i = len - (len % 8u); i < len; i++) {
        image[off + i] = (uint8_t)fuzz_rng_next(rng);
    }
}

static void corrupt_at(uint8_t *image, uint64_t off, uint64_t len,
                       fuzz_rng *rng)
{
    if (fuzz_rng_below(rng, 2u) == 0u) {
        memset(image + off, 0, (size_t)len);      /* lost write   */
    } else {
        fill_noise(image, off, len, rng);         /* media garbage */
    }
}

/* ------------------------------------------------------- valid journal --- */

static void fill_emerg(ambre_emerg_t *em, uint32_t id, uint32_t order,
                       fuzz_rng *rng)
{
    memset(em, 0, sizeof(*em));
    em->event_id      = id;
    em->trig_reason   = (uint8_t)AMBRE_TRIG_DV_LONG;
    em->event_number  = 1u;
    em->ignition_cycle = 3u;
    em->t0_ts_us      = 5000000u;
    em->trig_ts_us    = 5000000u;
    em->crash_order   = order;
    em->point_count   = 4u;
    for (uint32_t i = 0u; i < em->point_count; i++) {
        em->points[i].speed_kmh_x4 = (uint16_t)(200u + fuzz_rng_below(rng, 200u));
        em->points[i].rel_time_ms  = (uint16_t)(5000u - 500u * i);
        em->points[i].accel_long_mm_s2 = (int16_t)fuzz_rng_below(rng, 20000u);
    }
}

static void fill_cont(ambre_cont_t *c, uint32_t id, uint32_t k, fuzz_rng *rng)
{
    memset(c, 0, sizeof(*c));
    c->event_id     = id;
    c->sample_count = 1u;
    c->rel_time_ms  = (uint16_t)(10u * (k + 1u));
    c->dv_long_mm_s = (int32_t)fuzz_rng_below(rng, 9000u);
    c->dv_lat_mm_s  = (int32_t)fuzz_rng_below(rng, 2000u);
    c->points[0].rel_time_ms = c->rel_time_ms;
}

static void fill_seal(ambre_seal_t *sl, uint32_t id, fuzz_rng *rng)
{
    memset(sl, 0, sizeof(*sl));
    sl->event_id       = id;
    sl->file_complete  = 1u;
    sl->seal_cause     = (uint8_t)AMBRE_SEAL_NORMAL;
    sl->event_number   = 1u;
    sl->max_dv_long_mm_s = (int32_t)(4000 + fuzz_rng_below(rng, 4000u));
    sl->max_dv_long_at_ms = (uint16_t)(30u + fuzz_rng_below(rng, 60u));
}

/* Class B stream: small single-block records plus one multi-block record, so
 * the torn-record strategy always has a continuation block to corrupt. */
static void build_class_b(ambre_journal_t *j, uint64_t *ts, fuzz_rng *rng)
{
    ambre_sample_t s;

    for (uint32_t i = 0u; i < 40u; i++) {
        s.signal_id = (uint16_t)(1u + (i % 8u));
        s.quality   = 0u;
        s.value     = (int32_t)fuzz_rng_next(rng);
        ambre_journal_append_data(j, *ts, &s, 1u);
        *ts += 1000u;
    }
    ambre_sample_t big[120];                      /* 960 B => 3 blocks */
    for (uint32_t i = 0u; i < 120u; i++) {
        big[i].signal_id = (uint16_t)(1u + (i % 8u));
        big[i].quality   = 0u;
        big[i].value     = (int32_t)fuzz_rng_next(rng);
    }
    ambre_journal_append_data(j, *ts, big, 120u);
    *ts += 1000u;
    ambre_journal_flush(j, *ts);
}

/* Two events: #1 sealed and locked, #2 left open (as after a power cut). */
static void build_events(ambre_journal_t *j, uint64_t *ts, fuzz_rng *rng)
{
    ambre_emerg_t em;
    ambre_cont_t c;
    ambre_seal_t sl;

    fill_emerg(&em, 1u, 1u, rng);
    em.locked = 1u;
    ambre_journal_trigger(j, &em);
    for (uint32_t k = 0u; k < 3u; k++) {
        fill_cont(&c, 1u, k, rng);
        ambre_journal_continue(j, &c);
    }
    fill_seal(&sl, 1u, rng);
    sl.locked = 1u;
    ambre_journal_seal(j, &sl);
    fill_emerg(&em, 2u, 2u, rng);
    ambre_journal_trigger(j, &em);
    fill_cont(&c, 2u, 0u, rng);
    ambre_journal_continue(j, &c);
    *ts += 100000u;
    ambre_journal_tick(j, *ts);
}

/* Build a realistic journal on the scratch disk; its write log (started
 * after the format) is what the cut strategies replay. */
static void build_valid_journal(fuzz_gen_ws_t *ws, fuzz_rng *rng)
{
    ambre_memdisk_t *sc = &ws->scratch;
    ambre_journal_cfg_t cfg;
    ambre_journal_t j;
    uint64_t ts = 100000u;

    ambre_memdisk_reset(sc);
    sc->log_ops = false;
    ambre_journal_cfg_default(&cfg);
    cfg.session_id = 0xA11CE000u + fuzz_rng_below(rng, 0xFFFFu);
    ambre_journal_format(&sc->disk, &cfg);
    memcpy(ws->base, sc->image, (size_t)sc->size);
    sc->op_count = 0u;
    sc->log_ops  = true;
    ambre_journal_open(&sc->disk, &cfg, false, &j);
    build_class_b(&j, &ts, rng);
    build_events(&j, &ts, rng);
}

/* Replay the first `count` logged writes of the scratch journal onto md,
 * starting from the just-formatted base image. */
static void replay_prefix(fuzz_gen_ws_t *ws, ambre_memdisk_t *md,
                          uint32_t count)
{
    ambre_memdisk_apply_log(&ws->scratch, count, md, ws->base);
}

static const uint8_t *op_payload(const fuzz_gen_ws_t *ws, const ambre_op_t *op)
{
    return ws->scratch.payloads + (size_t)op->data_id * AMBRE_MEMDISK_MAX_WRITE;
}

/* ------------------------------------------------------------- strategies - */

static void gen_noise(ambre_memdisk_t *md, fuzz_rng *rng)
{
    fill_noise(md->image, 0u, md->size, rng);
}

static void gen_sparse(ambre_memdisk_t *md, fuzz_rng *rng)
{
    uint64_t blocks = md->size / AMBRE_BLOCK_SIZE;

    memset(md->image, 0, (size_t)md->size);
    uint64_t sectors = 64u + fuzz_rng_below(rng, 192u);
    for (uint64_t i = 0u; i < sectors; i++) {
        uint64_t off = fuzz_rng_below(rng, blocks) * AMBRE_BLOCK_SIZE;
        fill_noise(md->image, off, AMBRE_BLOCK_SIZE, rng);
    }
    if (fuzz_rng_below(rng, 2u) == 0u) {
        fill_noise(md->image, AMBRE_TABLE_A_OFFSET, AMBRE_BLOCK_SIZE, rng);
    } else {
        fill_noise(md->image, AMBRE_TABLE_B_OFFSET, AMBRE_BLOCK_SIZE, rng);
    }
}

/* Power cut after k logged writes; half of the time the last (multi-sector)
 * write only partially reached the media: a torn block at a sector boundary. */
static void gen_partial(fuzz_gen_ws_t *ws, ambre_memdisk_t *md, fuzz_rng *rng)
{
    uint32_t k = (uint32_t)fuzz_rng_below(rng, ws->scratch.op_count + 1u);

    replay_prefix(ws, md, k);
    if (k == 0u) {
        return;
    }
    const ambre_op_t *op = &ws->scratch.ops[k - 1u];
    if (op->len <= AMBRE_BLOCK_SIZE || fuzz_rng_below(rng, 2u) == 0u) {
        return;
    }
    uint32_t sectors = op->len / AMBRE_BLOCK_SIZE;
    uint32_t kept = (uint32_t)(1u + fuzz_rng_below(rng, sectors - 1u))
                    * AMBRE_BLOCK_SIZE;
    memcpy(md->image + op->off, op_payload(ws, op), kept);
    corrupt_at(md->image, op->off + kept, op->len - kept, rng);
}

/* A multi-block record whose continuation blocks are destroyed: recovery must
 * drop it as torn (REP-05), never reassemble garbage. */
static void gen_torn_record(fuzz_gen_ws_t *ws, ambre_memdisk_t *md,
                            fuzz_rng *rng)
{
    uint32_t candidates[16];
    uint32_t n = 0u;

    replay_prefix(ws, md, ws->scratch.op_count);
    for (uint32_t i = 0u; i < ws->scratch.op_count && n < 16u; i++) {
        if (ws->scratch.ops[i].len > AMBRE_BLOCK_SIZE) {
            candidates[n++] = i;
        }
    }
    if (n == 0u) {
        return;
    }
    const ambre_op_t *op = &ws->scratch.ops[candidates[fuzz_rng_below(rng, n)]];
    uint32_t sectors = op->len / AMBRE_BLOCK_SIZE;
    uint32_t at = (uint32_t)(1u + fuzz_rng_below(rng, sectors)) * AMBRE_BLOCK_SIZE;
    if (at >= op->len) {
        at = (sectors - 1u) * AMBRE_BLOCK_SIZE;
    }
    corrupt_at(md->image, op->off + at, op->len - at, rng);
}

/* Allocation table copies destroyed: A, B, both, or a torn half-sector. */
static void gen_bad_table(fuzz_gen_ws_t *ws, ambre_memdisk_t *md, fuzz_rng *rng)
{
    uint64_t pattern = fuzz_rng_below(rng, 4u);

    replay_prefix(ws, md, ws->scratch.op_count);
    if (pattern == 0u || pattern == 2u) {
        corrupt_at(md->image, AMBRE_TABLE_A_OFFSET, AMBRE_BLOCK_SIZE, rng);
    }
    if (pattern == 1u || pattern == 2u) {
        corrupt_at(md->image, AMBRE_TABLE_B_OFFSET, AMBRE_BLOCK_SIZE, rng);
    }
    if (pattern == 3u) {
        corrupt_at(md->image, AMBRE_TABLE_A_OFFSET, AMBRE_BLOCK_SIZE / 2u, rng);
    }
}

static void gen_bitflip(fuzz_gen_ws_t *ws, ambre_memdisk_t *md, fuzz_rng *rng)
{
    uint64_t flips = 1u + fuzz_rng_below(rng, 8u);

    replay_prefix(ws, md, ws->scratch.op_count);
    for (uint64_t i = 0u; i < flips; i++) {
        uint64_t off = fuzz_rng_below(rng, md->size);
        uint32_t bit = (uint32_t)fuzz_rng_below(rng, 8u);
        md->image[off] ^= (uint8_t)(1u << bit);
    }
}

static void gen_mix(fuzz_gen_ws_t *ws, ambre_memdisk_t *md, fuzz_rng *rng)
{
    uint64_t blocks = md->size / AMBRE_BLOCK_SIZE;
    uint64_t start = 2u + fuzz_rng_below(rng, blocks - 66u);
    uint64_t run = 1u + fuzz_rng_below(rng, 64u);

    replay_prefix(ws, md, ws->scratch.op_count);
    if (start + run > blocks) {
        run = blocks - start;
    }
    fill_noise(md->image, start * AMBRE_BLOCK_SIZE, run * AMBRE_BLOCK_SIZE, rng);
}

/* ------------------------------------------------------------------- API -- */

int fuzz_gen_image(fuzz_gen_ws_t *ws, ambre_memdisk_t *md,
                   uint64_t seed, uint64_t index)
{
    fuzz_rng rng;
    int strategy;

    fuzz_rng_seed(&rng, fuzz_mix64(seed, index));
    strategy = (int)fuzz_rng_below(&rng, FUZZ_S_COUNT);
    ambre_memdisk_reset(md);
    switch (strategy) {
    case FUZZ_S_NOISE:  gen_noise(md, &rng);                        break;
    case FUZZ_S_SPARSE: gen_sparse(md, &rng);                       break;
    default:
        build_valid_journal(ws, &rng);
        switch (strategy) {
        case FUZZ_S_PARTIAL:   gen_partial(ws, md, &rng);        break;
        case FUZZ_S_TORN_REC:  gen_torn_record(ws, md, &rng);    break;
        case FUZZ_S_BAD_TABLE: gen_bad_table(ws, md, &rng);      break;
        case FUZZ_S_BITFLIP:   gen_bitflip(ws, md, &rng);        break;
        default:               gen_mix(ws, md, &rng);            break;
        }
        break;
    }
    return strategy;
}
