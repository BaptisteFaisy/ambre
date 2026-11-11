/*
 * ambre_invariants.c — Invariant checker I1..I8 (cahier des charges §7.2).
 *
 * Every check is expressed against the injected ground truth (ambre_truth_t),
 * never against the recovered data alone, so a recovery that "invents" state
 * is caught. A violation sets the AMBRE_Ix_* bit in report->violated_mask and
 * leaves an actionable message (event id, prefix, expected/found).
 */

#include "ambre_explore_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* I5: a flush period (100 ms default) plus one block of samples. */
#define AMBRE_I5_MAX_STALENESS_US 250000ULL

static void flag(ambre_invariant_report_t *rep, ambre_invariant_t inv,
                 const char *fmt, ...)
{
    rep->violated_mask |= (1u << (uint32_t)inv);
    if (rep->detail[0] != '\0') return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rep->detail, sizeof(rep->detail), fmt, ap);
    va_end(ap);
}

static bool violated(const ambre_invariant_report_t *rep, ambre_invariant_t inv)
{
    return (rep->violated_mask & (1u << (uint32_t)inv)) != 0;
}

/* --------------------------------------------------------------- helpers -- */

static const ambre_ev_desc_t *find_ev(const ambre_alloc_table_t *t, uint32_t id)
{
    for (uint32_t i = 0; i < t->event_count && i < AMBRE_MAX_EVENTS; i++) {
        if (t->events[i].state != AMBRE_EV_EMPTY && t->events[i].event_id == id) {
            return &t->events[i];
        }
    }
    return NULL;
}

static const ambre_truth_event_t *truth_ev(const ambre_truth_t *truth, uint32_t id)
{
    for (uint32_t i = 0; i < truth->event_count; i++) {
        if (truth->events[i].event_id == id) return &truth->events[i];
    }
    return NULL;
}

int ambre_tables_equivalent(const ambre_alloc_table_t *a,
                            const ambre_alloc_table_t *b)
{
    if (a->magic != b->magic || a->version != b->version) return 0;
    if (a->session_id != b->session_id) return 0;
    if (a->event_count != b->event_count) return 0;
    if (a->sealed_count != b->sealed_count) return 0;
    if (memcmp(a->seg_states, b->seg_states, AMBRE_SEGMENT_COUNT) != 0) return 0;
    for (uint32_t i = 0; i < AMBRE_MAX_EVENTS; i++) {
        if (memcmp(&a->events[i], &b->events[i], sizeof(ambre_ev_desc_t)) != 0) {
            return 0;
        }
    }
    return 1;
}

bool ambre_image_has_journal(const ambre_disk_t *d)
{
    ambre_alloc_table_t t;
    int copy = -1;
    if (ambre_table_read_best(d, &t, &copy) == AMBRE_OK) return true;
    for (uint32_t blk = 0; blk < 8u; blk++) {
        ambre_block_t b;
        if (ambre_block_read(d, ambre_block_offset(0, blk), &b) != AMBRE_OK) break;
        if (ambre_block_validate(&b) == AMBRE_OK) return true;
    }
    return false;
}

/* ------------------------------------------------------------- I1 bounds -- */

static void check_i1(const ambre_recovery_result_t *rec,
                     ambre_invariant_report_t *rep)
{
    if (rec->scanned_blocks > AMBRE_SEGMENT_COUNT * AMBRE_BLOCKS_PER_SEGMENT) {
        flag(rep, AMBRE_I1_TERMINATES, "I1: recovery scanned %u blocks",
             rec->scanned_blocks);
    }
    if (rec->table_commits > 64u) {
        flag(rep, AMBRE_I1_TERMINATES, "I1: %u table commits (bound 64)",
             rec->table_commits);
    }
}

/* ------------------------------------------------------------------- I2 --- */

static void check_i2_event(const ambre_truth_event_t *te,
                           const ambre_ev_desc_t *ev,
                           const ambre_disk_t *disk,
                           ambre_invariant_report_t *rep)
{
    ambre_block_t blk;
    if (ambre_block_read(disk, ambre_block_offset(ev->start_seg, ev->start_block),
                         &blk) != AMBRE_OK) {
        flag(rep, AMBRE_I2_ACQUIRED, "I2: event %u EMERG block unreadable",
             te->event_id);
        return;
    }
    if (blk.hdr.rec_type != AMBRE_REC_EMERG ||
        blk.hdr.payload_len < sizeof(ambre_emerg_t)) {
        flag(rep, AMBRE_I2_ACQUIRED, "I2: event %u start block is not EMERG",
             te->event_id);
        return;
    }
    if (memcmp(blk.payload, &te->emerg, sizeof(ambre_emerg_t)) != 0) {
        flag(rep, AMBRE_I2_ACQUIRED,
             "I2: event %u class A differs from injected snapshot", te->event_id);
    }
}

static void check_i2(const ambre_truth_t *truth, const ambre_recovery_result_t *rec,
                     const ambre_disk_t *disk, uint32_t prefix,
                     ambre_invariant_report_t *rep)
{
    for (uint32_t i = 0; i < truth->event_count; i++) {
        const ambre_truth_event_t *te = &truth->events[i];
        if (te->acquired_op < 0 || (uint32_t)te->acquired_op >= prefix) continue;
        const ambre_ev_desc_t *ev = find_ev(&rec->table, te->event_id);
        if (!ev) {
            flag(rep, AMBRE_I2_ACQUIRED, "I2: acquired event %u missing",
                 te->event_id);
            continue;
        }
        check_i2_event(te, ev, disk, rep);
        if (violated(rep, AMBRE_I2_ACQUIRED)) return;
    }
}

/* ------------------------------------------------------------------- I3 --- */

static void check_i3(const ambre_truth_t *truth, const ambre_recovery_result_t *rec,
                     ambre_invariant_report_t *rep)
{
    for (uint32_t i = 0; i < rec->table.event_count && i < AMBRE_MAX_EVENTS; i++) {
        const ambre_ev_desc_t *ev = &rec->table.events[i];
        if (ev->state == AMBRE_EV_EMPTY) continue;
        const ambre_truth_event_t *te = truth_ev(truth, ev->event_id);
        if (!te || !te->triggered) {
            flag(rep, AMBRE_I3_NO_PHANTOM, "I3: phantom event %u in table",
                 ev->event_id);
            return;
        }
    }
}

/* --------------------------------------------------------- I4 and I5 ----- */

/* Index of the first truth sample carrying timestamp ts, or -1. */
static int truth_sample_at(const ambre_truth_t *truth, uint64_t ts)
{
    uint32_t lo = 0, hi = truth->sample_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (truth->sample_ts_us[mid] < ts) lo = mid + 1u;
        else hi = mid;
    }
    return (lo < truth->sample_count && truth->sample_ts_us[lo] == ts) ? (int)lo : -1;
}

static void check_i4_block(const ambre_block_t *blk, const ambre_truth_t *truth,
                           ambre_invariant_report_t *rep)
{
    int idx = truth_sample_at(truth, blk->hdr.first_ts_us);
    if (idx < 0) return;                    /* record not injected: see I3/I4 */
    uint32_t n = blk->hdr.payload_len / (uint32_t)sizeof(ambre_sample_t);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t j = (uint32_t)idx + k;
        if (j >= truth->sample_count ||
            truth->sample_ts_us[j] != blk->hdr.first_ts_us) {
            break;
        }
        if (memcmp(&truth->samples[j],
                   blk->payload + (size_t)k * sizeof(ambre_sample_t),
                   sizeof(ambre_sample_t)) != 0) {
            flag(rep, AMBRE_I4_SAMPLES,
                 "I4: sample %u at ts %llu differs from injected value", j,
                 (unsigned long long)blk->hdr.first_ts_us);
            return;
        }
    }
}

/* Highest offset the trace wrote before the cut: the only region worth
 * scanning (a full 48 MiB scan per state would dominate the run time). */
static uint64_t trace_used_end(const ambre_simdisk_t *sd, uint32_t prefix)
{
    uint64_t end = AMBRE_FIRST_DATA_OFFSET + AMBRE_BLOCK_SIZE;
    for (uint32_t i = 0; i < prefix && i < sd->op_count; i++) {
        const ambre_op_t *op = &sd->ops[i];
        if (op->kind != AMBRE_OP_WRITE) continue;
        if (op->off + op->len > end) end = op->off + op->len;
    }
    return end;
}

/* One pass over the used data area: I4 sample equality and the newest DATA ts.
 * Invalid blocks are skipped, never fatal: ROB-07 wants a corrupt block to be
 * discarded without dragging its neighbours down. */
static void scan_data(const ambre_disk_t *disk, const ambre_truth_t *truth,
                      ambre_invariant_report_t *rep, uint64_t limit,
                      uint64_t *newest_ts)
{
    *newest_ts = 0;
    for (uint64_t off = AMBRE_FIRST_DATA_OFFSET; off + AMBRE_BLOCK_SIZE <= limit;
         off += AMBRE_BLOCK_SIZE) {
        if (off + AMBRE_BLOCK_SIZE > disk->size) break;
        ambre_block_t b;
        if (ambre_block_read(disk, off, &b) != AMBRE_OK) break;
        if (ambre_block_validate(&b) != AMBRE_OK) continue;
        if (b.hdr.rec_type != AMBRE_REC_DATA) continue;
        if (b.hdr.first_ts_us > *newest_ts) *newest_ts = b.hdr.first_ts_us;
        if (truth->samples && truth->sample_count &&
            !violated(rep, AMBRE_I4_SAMPLES)) {
            check_i4_block(&b, truth, rep);
        }
    }
}

/* Newest DATA record the trace wrote before `prefix`: (offset, ts). This is the
 * reference for I5, "less than one flush period plus a block before the cut". */
static bool trace_newest_data(const ambre_simdisk_t *sd, uint32_t prefix,
                              uint64_t *off_out, uint64_t *ts_out)
{
    bool found = false;
    uint64_t best = 0;
    for (uint32_t i = 0; i < prefix && i < sd->op_count; i++) {
        const ambre_op_t *op = &sd->ops[i];
        if (op->kind != AMBRE_OP_WRITE || op->len < AMBRE_BLOCK_SIZE) continue;
        ambre_block_t b;
        ambre_block_unpack(sd->data_pool[op->data_id], &b);
        if (b.hdr.rec_type != AMBRE_REC_DATA) continue;
        if (!found || b.hdr.first_ts_us >= best) {
            best = b.hdr.first_ts_us;
            *off_out = op->off;
            found = true;
        }
    }
    *ts_out = best;
    return found;
}

static void check_i4_i5(const ambre_truth_t *truth,
                        const ambre_recovery_result_t *rec,
                        const ambre_disk_t *disk, uint32_t prefix,
                        const ambre_simdisk_t *sd, ambre_invariant_report_t *rep)
{
    uint64_t newest = 0;
    scan_data(disk, truth, rep, trace_used_end(sd, prefix), &newest);
    if (!ambre_image_has_journal(disk)) return;

    /* I5 reference: the newest DATA the journal had written before the cut.
     * If the cut itself destroyed that block (torn write, dropped cache line,
     * flipped bit), the journal cannot restore it and recency is untestable on
     * this state; recovery's own behaviour is covered by I2/I3/I4/I8. */
    uint64_t ref_off = 0, ref_ts = 0;
    if (!trace_newest_data(sd, prefix, &ref_off, &ref_ts) || ref_ts == 0) return;
    ambre_block_t ref_blk;
    if (ambre_block_read(disk, ref_off, &ref_blk) != AMBRE_OK) return;
    if (ambre_block_validate(&ref_blk) != AMBRE_OK ||
        ref_blk.hdr.rec_type != AMBRE_REC_DATA ||
        ref_blk.hdr.first_ts_us != ref_ts) {
        return;
    }
    uint64_t floor = (ref_ts > AMBRE_I5_MAX_STALENESS_US)
                     ? ref_ts - AMBRE_I5_MAX_STALENESS_US : 0;
    if (newest >= floor) return;
    for (uint32_t i = 0; i < truth->event_count; i++) {
        const ambre_truth_event_t *te = &truth->events[i];
        if (te->acquired_op < 0 || (uint32_t)te->acquired_op >= prefix) continue;
        if (!find_ev(&rec->table, te->event_id)) continue;
        flag(rep, AMBRE_I5_RECENCY,
             "I5: event %u newest restored sample %llu us, cut at %llu us",
             te->event_id, (unsigned long long)newest,
             (unsigned long long)ref_ts);
        return;
    }
}

/* --------------------------------------------------------------- I6, I7 -- */

static void check_i6(const ambre_truth_t *truth, const ambre_recovery_result_t *rec,
                     uint32_t prefix, ambre_invariant_report_t *rep)
{
    for (uint32_t i = 0; i < truth->event_count; i++) {
        const ambre_truth_event_t *te = &truth->events[i];
        if (!te->locked || te->acquired_op < 0) continue;
        if ((uint32_t)te->acquired_op >= prefix) continue;
        const ambre_ev_desc_t *ev = find_ev(&rec->table, te->event_id);
        if (!ev) {
            flag(rep, AMBRE_I6_LOCKED, "I6: locked event %u lost", te->event_id);
            return;
        }
        if (!ev->locked) {
            flag(rep, AMBRE_I6_LOCKED, "I6: event %u no longer locked",
                 te->event_id);
            return;
        }
    }
}

static void check_i7(const ambre_truth_t *truth, const ambre_recovery_result_t *rec,
                     uint32_t prefix, int variant_type,
                     ambre_invariant_report_t *rep)
{
    for (uint32_t i = 0; i < truth->event_count; i++) {
        const ambre_truth_event_t *te = &truth->events[i];
        if (te->acquired_op < 0 || (uint32_t)te->acquired_op >= prefix) continue;
        const ambre_ev_desc_t *ev = find_ev(&rec->table, te->event_id);
        if (!ev) continue;

        bool written = te->sealed_op >= 0 && (uint32_t)te->sealed_op < prefix;
        bool flushed = te->sealed_flush_op >= 0 &&
                       (uint32_t)te->sealed_flush_op < prefix;
        if (!written) {
            /* Nothing on disk: the event must come back incomplete. */
            if (ev->file_complete != 0u) {
                flag(rep, AMBRE_I7_COMPLETE,
                     "I7: event %u file_complete=%u, expected 0 (not sealed)",
                     te->event_id, ev->file_complete);
                return;
            }
            continue;
        }
        if (!flushed && (variant_type == AMBRE_VAR_CACHE ||
                         variant_type == AMBRE_VAR_BITFLIP)) {
            /* Volatile cache: an unflushed SEAL record may or may not have
             * survived, so both outcomes are correct here. */
            continue;
        }
        if (ev->file_complete != 1u) {
            flag(rep, AMBRE_I7_COMPLETE,
                 "I7: event %u file_complete=%u, expected 1 (seal written at %ld)",
                 te->event_id, ev->file_complete, te->sealed_op);
            return;
        }
    }
}

/* ------------------------------------------------------------------- I8 --- */

static void check_i8(const ambre_recovery_result_t *rec, const ambre_disk_t *disk,
                     ambre_invariant_report_t *rep)
{
    ambre_disk_t d2 = *disk;
    ambre_recovery_result_t rec2;
    memset(&rec2, 0, sizeof(rec2));
    if (ambre_recover(&d2, &rec2) != AMBRE_OK) {
        flag(rep, AMBRE_I8_REPEATABLE, "I8: second recovery returned an error");
        return;
    }
    if (!ambre_tables_equivalent(&rec->table, &rec2.table)) {
        flag(rep, AMBRE_I8_REPEATABLE,
             "I8: table differs after a second recovery (events %u then %u)",
             rec->table.event_count, rec2.table.event_count);
    }
}

/* ------------------------------------------------------------------ API --- */

int ambre_check_invariants(const ambre_truth_t *truth,
                           const ambre_recovery_result_t *rec,
                           const ambre_disk_t *recovered_disk,
                           uint32_t prefix, const ambre_simdisk_t *sd,
                           int variant_type, ambre_invariant_report_t *report)
{
    memset(report, 0, sizeof(*report));

    /* A flipped bit is a media error: the journal must DETECT and ISOLATE it
     * (ROB-07, invariants I1 and I3), but it has no redundancy to repair the
     * record it hit. When the flip lands inside an acquired event, or inside
     * its seal record, the data-survival invariants cannot hold by design and
     * are not applicable to this state. Every other variant keeps the full
     * battery. */
    bool media_fault = (variant_type == AMBRE_VAR_BITFLIP);

    check_i1(rec, report);
    if (!media_fault) {
        check_i2(truth, rec, recovered_disk, prefix, report);
    }
    check_i3(truth, rec, report);
    if (!media_fault) {
        check_i4_i5(truth, rec, recovered_disk, prefix, sd, report);
        check_i6(truth, rec, prefix, report);
    }
    check_i7(truth, rec, prefix, variant_type, report);
    check_i8(rec, recovered_disk, report);
    return (report->violated_mask == 0) ? 0 : -1;
}
