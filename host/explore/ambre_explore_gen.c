/*
 * ambre_explore_gen.c — Build the power-cut state space (spec 7.3).
 *
 * Per prefix of the write trace we add: the plain prefix, a torn last write at
 * every sector boundary, subsets of the unflushed writes (volatile disk cache),
 * one flipped bit, and the two table-destruction variants (REP-02). Nested
 * states (cut inside recovery) are added by the worker, see ambre_explore_run.
 */

#include "ambre_explore_internal.h"
#include <stdlib.h>
#include <string.h>

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int queue_push(ambre_state_queue_t *q, uint32_t prefix, int type,
                      const ambre_cut_variant_t *var)
{
    if (q->count == q->cap) {
        size_t ncap = q->cap ? q->cap * 2u : 2048u;
        ambre_explore_state_t *p = (ambre_explore_state_t *)
            realloc(q->items, ncap * sizeof(*p));
        if (!p) return AMBRE_ERR_IO;
        q->items = p;
        q->cap = ncap;
    }
    ambre_explore_state_t *st = &q->items[q->count++];
    memset(st, 0, sizeof(*st));
    st->prefix = prefix;
    st->variant_type = type;
    st->variant.prefix = prefix;
    if (var) st->variant = *var;
    return AMBRE_OK;
}

/* Torn last write: one state per sector boundary actually reached. */
static void add_torn(const ambre_simdisk_t *sd, uint32_t prefix,
                     ambre_state_queue_t *q)
{
    if (prefix == 0) return;
    const ambre_op_t *op = ambre_simdisk_op(sd, prefix - 1u);
    if (!op || op->kind != AMBRE_OP_WRITE || op->len <= AMBRE_BLOCK_SIZE) return;
    for (uint32_t cut = AMBRE_BLOCK_SIZE; cut < op->len; cut += AMBRE_BLOCK_SIZE) {
        ambre_cut_variant_t v;
        memset(&v, 0, sizeof(v));
        v.prefix = prefix;
        v.torn_last_write = true;
        v.torn_bytes = cut;
        if (queue_push(q, prefix, AMBRE_VAR_TORN, &v) != AMBRE_OK) return;
    }
}

/* Volatile disk cache: drop some of the writes not yet flushed at the cut. */
static void add_cache(const ambre_simdisk_t *sd, uint32_t prefix,
                      ambre_state_queue_t *q)
{
    long last_flush = ambre_simdisk_last_flush_before(sd, prefix);
    uint32_t first = (last_flush >= 0) ? (uint32_t)last_flush + 1u : 0u;
    uint32_t n = 0;
    for (uint32_t i = first; i < prefix; i++) {
        const ambre_op_t *op = ambre_simdisk_op(sd, i);
        if (op && op->kind == AMBRE_OP_WRITE) n++;
    }
    if (n == 0) return;
    /* Exhaustive below 4 unflushed writes, otherwise 4 deterministic samples. */
    uint32_t patterns = (n <= 3u) ? (1u << n) : 4u;
    for (uint32_t k = 0; k < patterns; k++) {
        uint64_t mask = (n <= 3u) ? (uint64_t)k
                                  : (uint64_t)mix32(prefix * 2654435761u + k);
        ambre_cut_variant_t v;
        memset(&v, 0, sizeof(v));
        v.prefix = prefix;
        v.drop_unflushed = true;
        v.keep_mask = mask;
        if (queue_push(q, prefix, AMBRE_VAR_CACHE, &v) != AMBRE_OK) return;
    }
}

/* One flipped bit in the data area (ROB-07). */
static void add_bitflip(const ambre_simdisk_t *sd, uint32_t prefix,
                        ambre_state_queue_t *q)
{
    if (prefix == 0) return;
    uint32_t span = (uint32_t)(sd->size - AMBRE_FIRST_DATA_OFFSET);
    if (span < AMBRE_BLOCK_SIZE) return;
    uint32_t r = mix32(prefix);
    ambre_cut_variant_t v;
    memset(&v, 0, sizeof(v));
    v.prefix = prefix;
    v.corrupt_bit = true;
    v.corrupt_off = AMBRE_FIRST_DATA_OFFSET + (uint64_t)(r % (span / AMBRE_BLOCK_SIZE))
                    * AMBRE_BLOCK_SIZE + (r >> 8) % AMBRE_BLOCK_SIZE;
    v.corrupt_bit_index = (uint8_t)((r >> 20) & 7u);
    if (queue_push(q, prefix, AMBRE_VAR_BITFLIP, &v) != AMBRE_OK) return;
}

/* Table copy A destroyed, then both copies destroyed (REP-02). */
static void add_table_wipe(uint32_t prefix, ambre_state_queue_t *q)
{
    ambre_cut_variant_t v;
    memset(&v, 0, sizeof(v));
    v.prefix = prefix;
    v.wipe_table_a = true;
    if (queue_push(q, prefix, AMBRE_VAR_TABLE, &v) != AMBRE_OK) return;
    memset(&v, 0, sizeof(v));
    v.prefix = prefix;
    v.wipe_both_tables = true;
    if (queue_push(q, prefix, AMBRE_VAR_TABLE, &v) != AMBRE_OK) return;
}

/* Nested cut (I8): a state whose recovery is itself interrupted by a cut. */
static void add_nested(uint32_t prefix, ambre_state_queue_t *q)
{
    ambre_cut_variant_t v;
    memset(&v, 0, sizeof(v));
    v.prefix = prefix;
    if (queue_push(q, prefix, AMBRE_VAR_NESTED, &v) != AMBRE_OK) return;
}

int ambre_explore_gen_states(const ambre_simdisk_t *sd,
                             const ambre_explore_cfg_t *cfg,
                             ambre_state_queue_t *q)
{
    uint32_t total = ambre_simdisk_op_count(sd);

    /* Pass 1: every plain prefix, so the whole set of reachable cut points is
     * covered before any variant spends the budget. The variants of the
     * early prefixes would otherwise eat the whole budget and the crash
     * window would never be explored. */
    for (uint32_t p = 1; p <= total; p++) {
        if (queue_push(q, p, AMBRE_VAR_PREFIX, NULL) != AMBRE_OK) return AMBRE_ERR_IO;
        if (cfg->max_states && q->count >= cfg->max_states) {
            q->count = cfg->max_states;
            return AMBRE_OK;
        }
    }

    /* Pass 2: the variants, prefix by prefix. */
    for (uint32_t p = 1; p <= total; p++) {
        if (cfg->variant_torn) add_torn(sd, p, q);
        if (cfg->variant_volatile_cache) add_cache(sd, p, q);
        if (cfg->variant_bitflip) add_bitflip(sd, p, q);
        if (cfg->variant_table_wipe) add_table_wipe(p, q);
        if (cfg->nested_recovery) add_nested(p, q);
        if (cfg->max_states && q->count >= cfg->max_states) {
            q->count = cfg->max_states;
            break;
        }
    }
    return AMBRE_OK;
}

void ambre_state_queue_free(ambre_state_queue_t *q)
{
    if (!q) return;
    free(q->items);
    q->items = NULL;
    q->count = 0;
    q->cap = 0;
}
