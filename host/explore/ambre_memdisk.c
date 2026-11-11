/*
 * ambre_memdisk.c — In-memory disk with an optional op log and dirty tracking.
 *
 * The explorer materialises thousands of disk images. Copying a whole journal
 * per state would dominate the run time, so each memdisk remembers which 512-byte
 * blocks it modified since the last sync and can be rolled back to a base image
 * by restoring those blocks only.
 */

#include "ambre_explore_internal.h"
#include <stdlib.h>
#include <string.h>

static void mark_dirty(ambre_memdisk_t *md, uint64_t off, uint32_t len)
{
    uint64_t first = off / AMBRE_BLOCK_SIZE;
    uint64_t last = (off + len - 1u) / AMBRE_BLOCK_SIZE;
    for (uint64_t b = first; b <= last; b++) {
        bool seen = false;
        for (uint32_t i = 0; i < md->dirty_n; i++) {
            if (md->dirty[i] == b) { seen = true; break; }
        }
        if (seen) continue;
        if (md->dirty_n >= AMBRE_MEMDISK_MAX_DIRTY) {
            md->dirty_overflow = true;
            return;
        }
        md->dirty[md->dirty_n++] = b;
    }
}

static int md_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    ambre_memdisk_t *md = (ambre_memdisk_t *)ctx;
    if (!md || !buf || off + len > md->size || off + len < off) return -1;
    memcpy(buf, md->image + off, len);
    return 0;
}

static void md_log(ambre_memdisk_t *md, uint64_t off, const void *buf, uint32_t len)
{
    if (!md->log_ops || md->op_count >= AMBRE_MEMDISK_MAX_OPS) return;
    uint32_t n = (len > AMBRE_MEMDISK_MAX_WRITE) ? AMBRE_MEMDISK_MAX_WRITE : len;
    ambre_op_t *op = &md->ops[md->op_count];
    memset(op, 0, sizeof(*op));
    op->index = md->op_count;
    op->kind = AMBRE_OP_WRITE;
    op->off = off;
    op->len = n;
    op->data_id = md->op_count;
    memcpy(md->payloads + (size_t)md->op_count * AMBRE_MEMDISK_MAX_WRITE, buf, n);
    md->op_count++;
}

static int md_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    ambre_memdisk_t *md = (ambre_memdisk_t *)ctx;
    if (!md || !buf || off + len > md->size || off + len < off) return -1;
    md_log(md, off, buf, (uint32_t)len);
    mark_dirty(md, off, (uint32_t)len);
    memcpy(md->image + off, buf, len);
    return 0;
}

static int md_flush(void *ctx)
{
    (void)ctx;
    return 0;
}

int ambre_memdisk_init(ambre_memdisk_t *md, uint64_t size)
{
    if (!md || size < AMBRE_BLOCK_SIZE) return AMBRE_ERR_PARAM;
    memset(md, 0, sizeof(*md));
    md->image = (uint8_t *)calloc(1, (size_t)size);
    md->payloads = (uint8_t *)calloc(AMBRE_MEMDISK_MAX_OPS, AMBRE_MEMDISK_MAX_WRITE);
    if (!md->image || !md->payloads) {
        ambre_memdisk_destroy(md);
        return AMBRE_ERR_IO;
    }
    md->size = size;
    md->disk.ctx = md;
    md->disk.size = size;
    md->disk.sector_size = AMBRE_BLOCK_SIZE;
    md->disk.read = md_read;
    md->disk.write = md_write;
    md->disk.flush = md_flush;
    md->disk.mark = NULL;
    return AMBRE_OK;
}

void ambre_memdisk_destroy(ambre_memdisk_t *md)
{
    if (!md) return;
    free(md->image);
    free(md->payloads);
    md->image = NULL;
    md->payloads = NULL;
}

void ambre_memdisk_reset(ambre_memdisk_t *md)
{
    if (!md) return;
    md->op_count = 0;
    md->dirty_n = 0;
    md->dirty_overflow = false;
    memset(md->image, 0, (size_t)md->size);
}

int ambre_memdisk_copy_from(ambre_memdisk_t *md, const uint8_t *src)
{
    if (!md || !src) return AMBRE_ERR_PARAM;
    memcpy(md->image, src, (size_t)md->size);
    md->dirty_n = 0;
    md->dirty_overflow = false;
    md->synced = true;
    return AMBRE_OK;
}

int ambre_memdisk_sync_from(ambre_memdisk_t *md, const uint8_t *base)
{
    if (!md || !base) return AMBRE_ERR_PARAM;
    if (!md->synced || md->dirty_overflow) {
        memcpy(md->image, base, (size_t)md->size);
    } else {
        for (uint32_t i = 0; i < md->dirty_n; i++) {
            uint64_t off = md->dirty[i] * AMBRE_BLOCK_SIZE;
            if (off + AMBRE_BLOCK_SIZE <= md->size) {
                memcpy(md->image + off, base + off, AMBRE_BLOCK_SIZE);
            }
        }
    }
    md->dirty_n = 0;
    md->dirty_overflow = false;
    md->synced = true;
    return AMBRE_OK;
}

int ambre_memdisk_apply_log(const ambre_memdisk_t *src, uint32_t count,
                            ambre_memdisk_t *dst, const uint8_t *base)
{
    if (!src || !dst || !base) return AMBRE_ERR_PARAM;
    /* Force a full copy from base: dst may still hold blocks written by a
     * previous state that are not in its dirty list any more, and `base` has
     * moved on since (new prefix writes). A partial rollback would mix two
     * different cut instants. */
    dst->synced = false;
    if (ambre_memdisk_sync_from(dst, base) != AMBRE_OK) return AMBRE_ERR_IO;
    for (uint32_t i = 0; i < count && i < src->op_count; i++) {
        const ambre_op_t *op = &src->ops[i];
        if (op->off + op->len > dst->size) return AMBRE_ERR_IO;
        mark_dirty(dst, op->off, op->len);
        memcpy(dst->image + op->off,
               src->payloads + (size_t)op->data_id * AMBRE_MEMDISK_MAX_WRITE,
               op->len);
    }
    return AMBRE_OK;
}
