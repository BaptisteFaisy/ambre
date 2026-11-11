/*
 * test_explore.c — Explorer tests: nominal exploration, invariant targeting on
 * both sides of acquisition and sealing, double recovery (I8), bit flip
 * (ROB-07) and destroyed tables (REP-02).
 *
 * Small segment_limit keeps the whole binary well under 30 s.
 */

#include "ambre_test.h"
#include "../host/explore/ambre_explore_internal.h"
#include <stdlib.h>
#include <string.h>

#define TEST_SEGMENTS 2u
#define TEST_DURATION_MS 600u
#define TEST_CRASH_MS 300u

typedef struct {
    ambre_simdisk_t sd;
    ambre_truth_t   truth;
    ambre_explore_cfg_t cfg;
} fixture_t;

static int fixture_init(fixture_t *fx, uint32_t duration_ms, uint32_t crash_ms)
{
    memset(fx, 0, sizeof(*fx));
    ambre_explore_cfg_default(&fx->cfg);
    fx->cfg.segment_limit = TEST_SEGMENTS;
    fx->cfg.duration_ms = duration_ms;
    fx->cfg.crash_at_ms = crash_ms;
    uint64_t size = AMBRE_FIRST_DATA_OFFSET +
                    (uint64_t)TEST_SEGMENTS * AMBRE_SEGMENT_SIZE;
    if (ambre_simdisk_create(&fx->sd, size) != AMBRE_OK) return -1;
    return ambre_explore_record_truth(&fx->cfg, &fx->sd, &fx->truth);
}

static void fixture_free(fixture_t *fx)
{
    ambre_truth_free(&fx->truth);
    ambre_simdisk_destroy(&fx->sd);
}

/* Materialise `prefix` (optionally with a cut variant) and run recovery once. */
static int recover_state(fixture_t *fx, uint32_t prefix,
                         const ambre_cut_variant_t *var,
                         ambre_memdisk_t *md, ambre_recovery_result_t *rec)
{
    if (ambre_memdisk_init(md, fx->sd.size) != AMBRE_OK) return -1;
    if (ambre_simdisk_snapshot(&fx->sd, prefix, var, md->image, md->size)
        != AMBRE_OK) {
        ambre_memdisk_destroy(md);
        return -1;
    }
    memset(rec, 0, sizeof(*rec));
    int rc = ambre_recover(&md->disk, rec);
    return (rc == AMBRE_OK) ? 0 : -1;
}

static const ambre_ev_desc_t *table_event(const ambre_alloc_table_t *t,
                                          uint32_t id)
{
    for (uint32_t i = 0; i < t->event_count && i < AMBRE_MAX_EVENTS; i++) {
        if (t->events[i].state != AMBRE_EV_EMPTY &&
            t->events[i].event_id == id) {
            return &t->events[i];
        }
    }
    return NULL;
}

/* 1. Nominal scenario: every prefix, no cut variant, zero violation. */
static void test_nominal_prefixes(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, TEST_DURATION_MS, TEST_CRASH_MS));
    ambre_explore_cfg_t cfg = fx.cfg;
    cfg.variant_torn = false;
    cfg.variant_volatile_cache = false;
    cfg.variant_bitflip = false;
    cfg.variant_table_wipe = false;
    ambre_explore_stats_t st;
    AMBRE_CHECK_OK(ambre_explore_run(&cfg, &st));
    AMBRE_CHECK_EQ_U64(st.violations, 0);
    AMBRE_CHECK_EQ_U64(st.states_by_variant[0], st.states_explored);
    AMBRE_CHECK(st.states_explored > 100u);
    AMBRE_CHECK(st.recoveries_run == st.states_explored);
    fixture_free(&fx);
}

/* 2. No crash in the scenario: no event may ever appear (I3). */
static void test_no_crash_no_event(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, 300u, 0u));
    AMBRE_CHECK_EQ_U64(fx.truth.event_count, 0);
    ambre_explore_stats_t st;
    AMBRE_CHECK_OK(ambre_explore_run(&fx.cfg, &st));
    AMBRE_CHECK_EQ_U64(st.violations, 0);

    ambre_memdisk_t md;
    ambre_recovery_result_t rec;
    uint32_t prefixes[] = { 5u, 40u, 120u, 200u };
    for (uint32_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (recover_state(&fx, prefixes[i], NULL, &md, &rec) != 0) continue;
        AMBRE_CHECK_EQ_U64(rec.table.event_count, 0);
        ambre_memdisk_destroy(&md);
    }
    fixture_free(&fx);
}

/* 3. Counters are consistent: every state is counted in exactly one bucket. */
static void test_counters_consistent(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, TEST_DURATION_MS, TEST_CRASH_MS));
    ambre_explore_cfg_t cfg = fx.cfg;
    cfg.max_states = 400u;
    cfg.nested_recovery = true;
    ambre_explore_stats_t st;
    AMBRE_CHECK_OK(ambre_explore_run(&cfg, &st));
    AMBRE_CHECK(st.states_explored > 0);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < 6u; i++) sum += st.states_by_variant[i];
    AMBRE_CHECK_EQ_U64(sum, st.states_explored);
    AMBRE_CHECK_EQ_U64(st.violations, 0);
    fixture_free(&fx);
}

/* 4. Acquisition boundary: before the EMERG write nothing exists, right after
 *    the acquisition the event is present with its class A bytes intact. */
static void test_acquisition_boundary(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, TEST_DURATION_MS, TEST_CRASH_MS));
    AMBRE_CHECK(fx.truth.event_count >= 1u);
    const ambre_truth_event_t *te = &fx.truth.events[0];
    AMBRE_CHECK(te->acquired_op >= 0);
    AMBRE_CHECK(te->emerg_op >= 0);

    ambre_memdisk_t md;
    ambre_recovery_result_t rec;
    /* Just before the emergency write: the event must not exist yet. */
    AMBRE_CHECK_OK(recover_state(&fx, (uint32_t)te->emerg_op, NULL, &md, &rec));
    AMBRE_CHECK(table_event(&rec.table, te->event_id) == NULL);
    ambre_memdisk_destroy(&md);

    /* Just after the flush that made it acquired: present, class A identical. */
    uint32_t after = (uint32_t)te->acquired_op + 1u;
    AMBRE_CHECK_OK(recover_state(&fx, after, NULL, &md, &rec));
    const ambre_ev_desc_t *ev = table_event(&rec.table, te->event_id);
    AMBRE_CHECK(ev != NULL);
    if (ev) {
        ambre_block_t blk;
        AMBRE_CHECK_OK(ambre_block_read(&md.disk,
                       ambre_block_offset(ev->start_seg, ev->start_block), &blk));
        AMBRE_CHECK_EQ_U64(blk.hdr.rec_type, AMBRE_REC_EMERG);
        AMBRE_CHECK_MEM_EQ(blk.payload, &te->emerg, sizeof(ambre_emerg_t));
    }
    ambre_invariant_report_t rep;
    AMBRE_CHECK_EQ_I64(ambre_check_invariants(&fx.truth, &rec, &md.disk, after,
                                              &fx.sd, 0, &rep), 0);
    ambre_memdisk_destroy(&md);
    fixture_free(&fx);
}

/* 5. I7 on both sides of the seal: file_complete is 0 before, 1 after. */
static void test_complete_flag_both_sides(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, TEST_DURATION_MS, TEST_CRASH_MS));
    const ambre_truth_event_t *te = &fx.truth.events[0];
    AMBRE_CHECK(te->sealed_op >= 0);

    ambre_memdisk_t md;
    ambre_recovery_result_t rec;
    /* The SEAL mark is emitted after the table commit, so "before the seal" is
     * the state where the event is acquired but still open. */
    uint32_t before = (uint32_t)te->acquired_op + 1u;
    AMBRE_CHECK_OK(recover_state(&fx, before, NULL, &md, &rec));
    const ambre_ev_desc_t *ev = table_event(&rec.table, te->event_id);
    AMBRE_CHECK(ev != NULL);
    if (ev) AMBRE_CHECK_EQ_U64(ev->file_complete, 0);
    ambre_invariant_report_t rep;
    AMBRE_CHECK_EQ_I64(ambre_check_invariants(&fx.truth, &rec, &md.disk, before,
                                              &fx.sd, 0, &rep), 0);
    ambre_memdisk_destroy(&md);

    uint32_t after = (uint32_t)te->sealed_op + 1u;
    AMBRE_CHECK_OK(recover_state(&fx, after, NULL, &md, &rec));
    ev = table_event(&rec.table, te->event_id);
    AMBRE_CHECK(ev != NULL);
    if (ev) AMBRE_CHECK_EQ_U64(ev->file_complete, 1);
    AMBRE_CHECK_EQ_I64(ambre_check_invariants(&fx.truth, &rec, &md.disk, after,
                                              &fx.sd, 0, &rep), 0);
    ambre_memdisk_destroy(&md);
    fixture_free(&fx);
}

/* 6. I8: recovery is idempotent, and cutting inside recovery keeps the state. */
static void test_double_recovery(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, TEST_DURATION_MS, TEST_CRASH_MS));
    uint32_t prefix = (uint32_t)fx.truth.events[0].acquired_op + 1u;
    ambre_memdisk_t md;
    ambre_recovery_result_t rec;
    AMBRE_CHECK_OK(recover_state(&fx, prefix, NULL, &md, &rec));
    ambre_recovery_result_t rec2;
    memset(&rec2, 0, sizeof(rec2));
    AMBRE_CHECK_OK(ambre_recover(&md.disk, &rec2));
    AMBRE_CHECK_EQ_U64(ambre_tables_equivalent(&rec.table, &rec2.table), 1);
    ambre_memdisk_destroy(&md);

    ambre_explore_cfg_t cfg = fx.cfg;
    cfg.nested_recovery = true;
    /* No state budget: the generator spends it on the plain prefixes first
     * (every reachable cut instant), then on the variants. A small budget
     * would never reach the nested states this test is about. */
    cfg.max_states = 0u;
    cfg.variant_bitflip = false;
    cfg.variant_table_wipe = false;
    ambre_explore_stats_t st;
    AMBRE_CHECK_OK(ambre_explore_run(&cfg, &st));
    AMBRE_CHECK(st.states_by_variant[5] > 0u);
    AMBRE_CHECK_EQ_U64(st.violations, 0);
    fixture_free(&fx);
}

/* 7. ROB-07: one flipped bit is detected and discarded, nothing else suffers. */
static void test_bitflip_detected(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, TEST_DURATION_MS, TEST_CRASH_MS));
    uint32_t prefix = (uint32_t)fx.truth.events[0].acquired_op + 1u;
    ambre_cut_variant_t var;
    memset(&var, 0, sizeof(var));
    var.prefix = prefix;
    var.corrupt_bit = true;
    var.corrupt_off = AMBRE_FIRST_DATA_OFFSET + 3u * AMBRE_BLOCK_SIZE + 64u;
    var.corrupt_bit_index = 3u;
    ambre_memdisk_t md;
    ambre_recovery_result_t rec;
    AMBRE_CHECK_OK(recover_state(&fx, prefix, &var, &md, &rec));
    ambre_invariant_report_t rep;
    AMBRE_CHECK_EQ_I64(ambre_check_invariants(&fx.truth, &rec, &md.disk, prefix,
                                              &fx.sd, 0, &rep), 0);
    ambre_memdisk_destroy(&md);
    fixture_free(&fx);
}

/* 8. REP-02: table copy A destroyed, then both copies destroyed. */
static void test_table_destruction(void)
{
    fixture_t fx;
    AMBRE_CHECK_OK(fixture_init(&fx, TEST_DURATION_MS, TEST_CRASH_MS));
    const ambre_truth_event_t *te = &fx.truth.events[0];
    uint32_t prefix = (uint32_t)te->acquired_op + 1u;
    for (int both = 0; both < 2; both++) {
        ambre_cut_variant_t var;
        memset(&var, 0, sizeof(var));
        var.prefix = prefix;
        var.wipe_table_a = (both == 0);
        var.wipe_both_tables = (both == 1);
        ambre_memdisk_t md;
        ambre_recovery_result_t rec;
        AMBRE_CHECK_OK(recover_state(&fx, prefix, &var, &md, &rec));
        const ambre_ev_desc_t *ev = table_event(&rec.table, te->event_id);
        AMBRE_CHECK(ev != NULL);
        ambre_invariant_report_t rep;
        AMBRE_CHECK_EQ_I64(ambre_check_invariants(&fx.truth, &rec, &md.disk,
                                                  prefix, &fx.sd, 0, &rep), 0);
        ambre_memdisk_destroy(&md);
    }
    fixture_free(&fx);
}

static const ambre_test_case_t cases[] = {
    { "explore_nominal_prefixes", test_nominal_prefixes },
    { "explore_no_crash_no_event", test_no_crash_no_event },
    { "explore_counters_consistent", test_counters_consistent },
    { "explore_acquisition_boundary", test_acquisition_boundary },
    { "explore_complete_flag_both_sides", test_complete_flag_both_sides },
    { "explore_double_recovery", test_double_recovery },
    { "explore_bitflip_detected", test_bitflip_detected },
    { "explore_table_destruction", test_table_destruction }
};

AMBRE_TEST_MAIN(cases)
