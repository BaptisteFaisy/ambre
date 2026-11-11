/*
 * ambre_simdisk.c — PC-side simulated raw partition with a full op log.
 *
 * Implements the ambre_disk_t vtable in memory, records all write/flush/mark
 * operations, and reconstructs arbitrary power-cut states and crash variants.
 *
 * Variant application order documented in ambre_simdisk_snapshot():
 *   1. Prefix replay: replay writes [0, prefix) with cache filtering if active.
 *   2. Torn write: truncate the last write of the prefix to torn_bytes.
 *   3. Volatile cache: drop unflushed writes according to drop_unflushed/keep_mask.
 *   4. Bit flip: invert corrupt_bit_index at corrupt_off.
 *   5. Allocation table wipe: clear table copy A, or both copies A and B.
 */

#include "ambre_simdisk.h"
#include <stdlib.h>
#include <string.h>

#define AMBRE_SIMDISK_SECTOR 512u

/* Forward declarations of vtable callbacks */
static int simdisk_read(void *ctx, uint64_t off, void *buf, size_t len);
static int simdisk_write(void *ctx, uint64_t off, const void *buf, size_t len);
static int simdisk_flush(void *ctx);
static void simdisk_mark(void *ctx, uint32_t kind, uint64_t val, uint64_t aux);

/* Allocate all memory in a single contiguous block (the only malloc). */
int ambre_simdisk_create(ambre_simdisk_t *sd, uint64_t size)
{
    if (!sd || size < AMBRE_SIMDISK_SECTOR) {
        return AMBRE_ERR_PARAM;
    }
    memset(sd, 0, sizeof(*sd));

    size_t sz_img = (size_t)size;
    size_t sz_ops = (size_t)AMBRE_SIMDISK_MAX_OPS * sizeof(ambre_op_t);
    size_t sz_dptr = (size_t)AMBRE_SIMDISK_MAX_OPS * sizeof(uint8_t *);
    size_t sz_dlen = (size_t)AMBRE_SIMDISK_MAX_OPS * sizeof(uint32_t);
    size_t sz_dpool = (size_t)AMBRE_SIMDISK_MAX_OPS * AMBRE_SIMDISK_MAX_DATA;
    size_t total = sz_img + sz_ops + sz_dptr + sz_dlen + sz_dpool;

    uint8_t *blob = (uint8_t *)calloc(1, total);
    if (!blob) {
        return AMBRE_ERR_IO;
    }

    sd->image = blob;
    sd->size = size;
    sd->ops = (ambre_op_t *)(blob + sz_img);
    sd->op_cap = AMBRE_SIMDISK_MAX_OPS;
    sd->data_pool = (uint8_t **)(blob + sz_img + sz_ops);
    sd->data_len = (uint32_t *)(blob + sz_img + sz_ops + sz_dptr);
    sd->data_pool_cap = AMBRE_SIMDISK_MAX_OPS;

    uint8_t *payloads = blob + sz_img + sz_ops + sz_dptr + sz_dlen;
    for (uint32_t i = 0; i < AMBRE_SIMDISK_MAX_OPS; i++) {
        sd->data_pool[i] = payloads + ((size_t)i * AMBRE_SIMDISK_MAX_DATA);
    }

    sd->disk.ctx = sd;
    sd->disk.size = size;
    sd->disk.sector_size = AMBRE_SIMDISK_SECTOR;
    sd->disk.read = simdisk_read;
    sd->disk.write = simdisk_write;
    sd->disk.flush = simdisk_flush;
    sd->disk.mark = simdisk_mark;
    return AMBRE_OK;
}

void ambre_simdisk_destroy(ambre_simdisk_t *sd)
{
    if (sd && sd->image) {
        free(sd->image);
        memset(sd, 0, sizeof(*sd));
    }
}

const ambre_disk_t *ambre_simdisk_disk(ambre_simdisk_t *sd)
{
    return sd ? &sd->disk : NULL;
}

uint32_t ambre_simdisk_op_count(const ambre_simdisk_t *sd)
{
    return sd ? sd->op_count : 0;
}

const ambre_op_t *ambre_simdisk_op(const ambre_simdisk_t *sd, uint32_t i)
{
    return (sd && i < sd->op_count) ? &sd->ops[i] : NULL;
}

long ambre_simdisk_find_mark(const ambre_simdisk_t *sd, uint32_t mark_kind,
                             uint64_t value)
{
    if (!sd) {
        return -1;
    }
    for (long i = (long)sd->op_count - 1; i >= 0; i--) {
        const ambre_op_t *op = &sd->ops[i];
        if (op->kind == AMBRE_OP_MARK && op->mark_kind == mark_kind &&
            op->mark_value == value) {
            return i;
        }
    }
    return -1;
}

long ambre_simdisk_last_flush_before(const ambre_simdisk_t *sd, uint32_t prefix)
{
    if (!sd) {
        return -1;
    }
    uint32_t limit = prefix < sd->op_count ? prefix : sd->op_count;
    for (long i = (long)limit - 1; i >= 0; i--) {
        if (sd->ops[i].kind == AMBRE_OP_FLUSH) {
            return i;
        }
    }
    return -1;
}

static int simdisk_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    ambre_simdisk_t *sd = (ambre_simdisk_t *)ctx;
    if (!sd || !buf || off + len > sd->size || off + len < off) {
        return AMBRE_ERR_IO;
    }
    memcpy(buf, sd->image + off, len);
    if (sd->log_reads && sd->op_count < sd->op_cap) {
        ambre_op_t *op = &sd->ops[sd->op_count];
        memset(op, 0, sizeof(*op));
        op->index = sd->op_count++;
        op->kind = AMBRE_OP_READ;
        op->off = off;
        op->len = (uint32_t)len;
    }
    return AMBRE_OK;
}

static int simdisk_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    ambre_simdisk_t *sd = (ambre_simdisk_t *)ctx;
    if (!sd || !buf || off + len > sd->size || off + len < off) {
        return AMBRE_ERR_IO;
    }
    if (sd->op_count >= sd->op_cap || sd->data_id_next >= sd->data_pool_cap) {
        return AMBRE_ERR_IO;
    }
    if (len > AMBRE_SIMDISK_MAX_DATA) {
        return AMBRE_ERR_TOO_BIG;
    }

    uint64_t did = sd->data_id_next++;
    memcpy(sd->data_pool[did], buf, len);
    sd->data_len[did] = (uint32_t)len;

    ambre_op_t *op = &sd->ops[sd->op_count];
    memset(op, 0, sizeof(*op));
    op->index = sd->op_count++;
    op->kind = AMBRE_OP_WRITE;
    op->off = off;
    op->len = (uint32_t)len;
    op->data_id = did;

    memcpy(sd->image + off, buf, len);
    return AMBRE_OK;
}

static int simdisk_flush(void *ctx)
{
    ambre_simdisk_t *sd = (ambre_simdisk_t *)ctx;
    if (!sd) {
        return AMBRE_ERR_IO;
    }
    if (sd->op_count >= sd->op_cap) {
        return AMBRE_ERR_IO;
    }
    ambre_op_t *op = &sd->ops[sd->op_count];
    memset(op, 0, sizeof(*op));
    op->index = sd->op_count++;
    op->kind = AMBRE_OP_FLUSH;
    return AMBRE_OK;
}

static void simdisk_mark(void *ctx, uint32_t kind, uint64_t val, uint64_t aux)
{
    ambre_simdisk_t *sd = (ambre_simdisk_t *)ctx;
    if (!sd || sd->op_count >= sd->op_cap) {
        return;
    }
    ambre_op_t *op = &sd->ops[sd->op_count];
    memset(op, 0, sizeof(*op));
    op->index = sd->op_count++;
    op->kind = AMBRE_OP_MARK;
    op->mark_kind = kind;
    op->mark_value = val;
    op->mark_aux = aux;
}

/* Find index of the last write op in [0, limit), or -1 if none. */
static long find_last_write(const ambre_simdisk_t *sd, uint32_t limit)
{
    for (long i = (long)limit - 1; i >= 0; i--) {
        if (sd->ops[i].kind == AMBRE_OP_WRITE) {
            return i;
        }
    }
    return -1;
}

/* Check if an unflushed write should be kept according to keep_mask. */
static bool should_keep_write(long last_flush, uint32_t op_idx,
                              const ambre_cut_variant_t *var,
                              uint32_t *unflushed_cnt)
{
    if (!var || !var->drop_unflushed || (long)op_idx <= last_flush) {
        return true;
    }
    uint32_t k = (*unflushed_cnt)++;
    if (k < 64u) {
        return (var->keep_mask & (1ULL << k)) != 0;
    }
    return false;
}

/* Replay writes [0, limit) into dst buffer with optional torn/cache filtering. */
static void replay_writes(const ambre_simdisk_t *sd, uint32_t limit,
                          const ambre_cut_variant_t *var, uint8_t *dst)
{
    long last_flush = ambre_simdisk_last_flush_before(sd, limit);
    long last_w_idx = find_last_write(sd, limit);
    uint32_t unflushed_cnt = 0;

    for (uint32_t i = 0; i < limit; i++) {
        const ambre_op_t *op = &sd->ops[i];
        if (op->kind != AMBRE_OP_WRITE) {
            continue;
        }
        if (!should_keep_write(last_flush, i, var, &unflushed_cnt)) {
            continue;
        }

        uint32_t write_len = op->len;
        if (var && var->torn_last_write && (long)i == last_w_idx) {
            if (var->torn_bytes < write_len) {
                write_len = var->torn_bytes;
            }
        }
        if (write_len > 0) {
            memcpy(dst + op->off, sd->data_pool[op->data_id], write_len);
        }
    }
}

/* Apply corruption and table destruction variants to dst buffer. */
static void apply_post_replay_variants(const ambre_simdisk_t *sd,
                                       const ambre_cut_variant_t *var,
                                       uint8_t *dst)
{
    if (!var) {
        return;
    }
    if (var->corrupt_bit && var->corrupt_off < sd->size) {
        uint8_t bit = (uint8_t)(1u << (var->corrupt_bit_index & 7u));
        dst[var->corrupt_off] ^= bit;
    }
    if (var->wipe_both_tables) {
        size_t wipe_len = (size_t)(2u * AMBRE_SIMDISK_SECTOR);
        if (wipe_len > (size_t)sd->size) {
            wipe_len = (size_t)sd->size;
        }
        memset(dst, 0, wipe_len);
    } else if (var->wipe_table_a) {
        size_t wipe_len = (size_t)AMBRE_SIMDISK_SECTOR;
        if (wipe_len > (size_t)sd->size) {
            wipe_len = (size_t)sd->size;
        }
        memset(dst, 0, wipe_len);
    }
}

int ambre_simdisk_snapshot(const ambre_simdisk_t *sd, uint32_t prefix,
                           const ambre_cut_variant_t *var,
                           uint8_t *dst, uint64_t dst_size)
{
    if (!sd || !dst || dst_size != sd->size) {
        return AMBRE_ERR_PARAM;
    }
    uint32_t limit = prefix;
    if (var && var->prefix != 0) {
        limit = var->prefix;
    }
    if (limit > sd->op_count) {
        limit = sd->op_count;
    }

    memset(dst, 0, (size_t)dst_size);
    replay_writes(sd, limit, var, dst);
    apply_post_replay_variants(sd, var, dst);
    return AMBRE_OK;
}

int ambre_simdisk_apply_prefix(ambre_simdisk_t *sd, uint32_t prefix)
{
    if (!sd) {
        return AMBRE_ERR_PARAM;
    }
    return ambre_simdisk_snapshot(sd, prefix, NULL, sd->image, sd->size);
}

void ambre_simdisk_restore(ambre_simdisk_t *sd, const uint8_t *src)
{
    if (sd && src && sd->image) {
        memcpy(sd->image, src, (size_t)sd->size);
    }
}
