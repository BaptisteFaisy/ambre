/*
 * ambre_explore_worker.c — Run one crash state: materialise the image, run
 * recovery (optionally cutting inside recovery), check I1..I8, and keep the
 * image when a violation is found (spec 7.7: violations become regression
 * tests). Images are rolled back block by block, never copied wholesale.
 */

#include "ambre_explore_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ambre_explore_ws_init(ambre_explore_ws_t *ws, uint64_t size)
{
    memset(ws, 0, sizeof(*ws));
    ws->size = size;
    ws->plain = (uint8_t *)calloc(1, (size_t)size);
    ws->empty = (uint8_t *)calloc(1, (size_t)size);
    if (!ws->plain || !ws->empty) return AMBRE_ERR_IO;
    if (ambre_memdisk_init(&ws->work, size) != AMBRE_OK) return AMBRE_ERR_IO;
    if (ambre_memdisk_init(&ws->nested, size) != AMBRE_OK) return AMBRE_ERR_IO;
    return AMBRE_OK;
}

void ambre_explore_ws_free(ambre_explore_ws_t *ws)
{
    if (!ws) return;
    free(ws->plain);
    free(ws->empty);
    ws->plain = NULL;
    ws->empty = NULL;
    ambre_memdisk_destroy(&ws->work);
    ambre_memdisk_destroy(&ws->nested);
}

/* Bring ws->plain up to `prefix`, one write at a time (states come in order). */
static void advance_plain(ambre_explore_ws_t *ws, const ambre_simdisk_t *sd,
                          uint32_t prefix)
{
    if (prefix < ws->plain_prefix) {
        memset(ws->plain, 0, (size_t)ws->size);
        ws->plain_prefix = 0;
    }
    for (uint32_t i = ws->plain_prefix; i < prefix; i++) {
        const ambre_op_t *op = ambre_simdisk_op(sd, i);
        if (!op || op->kind != AMBRE_OP_WRITE) continue;
        if (op->off + op->len > ws->size) continue;
        memcpy(ws->plain + op->off, sd->data_pool[op->data_id], op->len);
    }
    ws->plain_prefix = prefix;
}

/* Volatile disk cache: replay the prefix from scratch, keeping only the
 * unflushed writes selected by keep_mask. */
static int build_cache_image(ambre_explore_ws_t *ws, const ambre_simdisk_t *sd,
                             const ambre_cut_variant_t *v)
{
    if (ambre_memdisk_sync_from(&ws->work, ws->empty) != AMBRE_OK) return -1;
    long last_flush = ambre_simdisk_last_flush_before(sd, v->prefix);
    uint32_t unflushed = 0;
    for (uint32_t i = 0; i < v->prefix; i++) {
        const ambre_op_t *op = ambre_simdisk_op(sd, i);
        if (!op || op->kind != AMBRE_OP_WRITE) continue;
        if ((long)i > last_flush) {
            bool keep = (unflushed < 64u) &&
                        (((v->keep_mask >> unflushed) & 1u) != 0);
            unflushed++;
            if (!keep) continue;
        }
        if (ws->work.disk.write(ws->work.disk.ctx, op->off,
                                sd->data_pool[op->data_id], op->len) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Materialise the state's disk image into ws->work. */
static int build_image(ambre_explore_ws_t *ws, const ambre_simdisk_t *sd,
                       const ambre_explore_state_t *st)
{
    const ambre_cut_variant_t *v = &st->variant;
    advance_plain(ws, sd, st->prefix);
    if (st->variant_type == 2) return build_cache_image(ws, sd, v);
    if (ambre_memdisk_sync_from(&ws->work, ws->plain) != AMBRE_OK) return -1;
    ambre_disk_t *d = &ws->work.disk;
    if (st->variant_type == 1 && st->prefix > 0) {
        const ambre_op_t *op = ambre_simdisk_op(sd, st->prefix - 1u);
        if (op && op->kind == AMBRE_OP_WRITE && v->torn_bytes < op->len) {
            if (d->write(d->ctx, op->off, sd->data_pool[op->data_id],
                         v->torn_bytes) != 0) return -1;
        }
    } else if (st->variant_type == 3 && v->corrupt_off < ws->size) {
        uint8_t byte = ws->work.image[v->corrupt_off];
        byte ^= (uint8_t)(1u << (v->corrupt_bit_index & 7u));
        if (d->write(d->ctx, v->corrupt_off, &byte, 1u) != 0) return -1;
    } else if (st->variant_type == 4) {
        static const uint8_t zeros[AMBRE_BLOCK_SIZE] = {0};
        uint32_t len = v->wipe_both_tables ? 2u * AMBRE_BLOCK_SIZE
                                           : AMBRE_BLOCK_SIZE;
        if (d->write(d->ctx, 0, zeros, len) != 0) return -1;
    }
    return 0;
}

/* Plain cut: one recovery, then the whole invariant battery (I8 re-runs it). */
static int run_plain(ambre_explore_ws_t *ws, const ambre_truth_t *truth,
                     const ambre_explore_state_t *st, const ambre_simdisk_t *sd,
                     ambre_invariant_report_t *rep)
{
    ambre_recovery_result_t rec;
    memset(&rec, 0, sizeof(rec));
    int rc = ambre_recover(&ws->work.disk, &rec);
    if (rc != AMBRE_OK) {
        if (!ambre_image_has_journal(&ws->work.disk)) return 0;  /* blank */
        rep->violated_mask |= (1u << AMBRE_I1_TERMINATES);
        snprintf(rep->detail, sizeof(rep->detail),
                 "I1: recovery returned %d (prefix %u, variant %d)",
                 rc, st->prefix, st->variant_type);
        return 1;
    }
    return ambre_check_invariants(truth, &rec, &ws->work.disk, st->prefix, sd, rep);
}

/* Nested cut (I8): interrupt recovery after half of its own writes, recover
 * again, and check the invariants on that state. */
static int run_nested(ambre_explore_ws_t *ws, const ambre_truth_t *truth,
                      const ambre_explore_state_t *st, const ambre_simdisk_t *sd,
                      ambre_invariant_report_t *rep)
{
    ambre_recovery_result_t rec1;
    memset(&rec1, 0, sizeof(rec1));
    ws->work.log_ops = true;
    ws->work.op_count = 0;
    int rc = ambre_recover(&ws->work.disk, &rec1);
    uint32_t half = ws->work.op_count / 2u;
    ws->work.log_ops = false;
    if (rc != AMBRE_OK) return run_plain(ws, truth, st, sd, rep);
    if (half == 0) {
        return ambre_check_invariants(truth, &rec1, &ws->work.disk,
                                      st->prefix, sd, rep);
    }
    if (ambre_memdisk_apply_log(&ws->work, half, &ws->nested,
                                ws->plain) != AMBRE_OK) {
        return -1;
    }
    ambre_recovery_result_t rec2;
    memset(&rec2, 0, sizeof(rec2));
    if (ambre_recover(&ws->nested.disk, &rec2) != AMBRE_OK) {
        if (!ambre_image_has_journal(&ws->nested.disk)) return 0;
        rep->violated_mask |= (1u << AMBRE_I1_TERMINATES);
        snprintf(rep->detail, sizeof(rep->detail),
                 "I1: nested recovery failed (prefix %u)", st->prefix);
        return 1;
    }
    return ambre_check_invariants(truth, &rec2, &ws->nested.disk,
                                  st->prefix, sd, rep);
}

int ambre_explore_ws_test(ambre_explore_ws_t *ws,
                          const ambre_explore_cfg_t *cfg,
                          const ambre_simdisk_t *sd,
                          const ambre_truth_t *truth,
                          const ambre_explore_state_t *st,
                          ambre_invariant_report_t *rep)
{
    memset(rep, 0, sizeof(*rep));
    if (build_image(ws, sd, st) != 0) return -1;
    int rc;
    if (st->variant_type == 5) {
        rc = run_nested(ws, truth, st, sd, rep);
    } else {
        rc = run_plain(ws, truth, st, sd, rep);
    }
    if (rep->violated_mask != 0 && cfg->violation_dir) {
        /* Keep the crash image itself, not the recovered one. */
        if (build_image(ws, sd, st) == 0) {
            ambre_save_violation(cfg, st, ws->work.image, ws->size, rep);
        }
    }
    return rc;
}

void ambre_save_violation(const ambre_explore_cfg_t *cfg,
                          const ambre_explore_state_t *st,
                          const uint8_t *img, uint64_t size,
                          const ambre_invariant_report_t *rep)
{
    if (!cfg->violation_dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/violation_p%u_var%d.img",
             cfg->violation_dir, st->prefix, st->variant_type);
    FILE *f = fopen(path, "wb");
    if (f) {
        if (size) (void)fwrite(img, 1, (size_t)size, f);
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/violation_p%u_var%d.txt",
             cfg->violation_dir, st->prefix, st->variant_type);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "prefix=%u\nvariant=%d\ntorn_bytes=%u\nkeep_mask=%llu\n"
                   "corrupt_off=%llu\ncorrupt_bit=%u\nwipe_table_a=%d\n"
                   "wipe_both=%d\nviolated_mask=0x%08x\ndetail=%s\n",
                st->prefix, st->variant_type, st->variant.torn_bytes,
                (unsigned long long)st->variant.keep_mask,
                (unsigned long long)st->variant.corrupt_off,
                (unsigned)st->variant.corrupt_bit_index,
                st->variant.wipe_table_a ? 1 : 0,
                st->variant.wipe_both_tables ? 1 : 0,
                rep->violated_mask, rep->detail);
        fclose(f);
    }
}
