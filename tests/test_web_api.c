/*
 * test_web_api.c — The browser API (host/web/) drives the real explorer.
 *
 * What this proves:
 *   1. the JSON API works on a real recorded trace (setup, info, ops, state);
 *   2. a cut state answers with the acquired event readable back, class A
 *      points included, and the eight invariants not violated;
 *   3. an exploration run through this API, single threaded like a browser,
 *      reports the same shape of counters as `ambre-explore --report`;
 *   4. every scenario shipped with the website fits the browser build's
 *      op-log bound (host/web/ambre_web_limits.h), so the WebAssembly module
 *      cannot refuse a write the page asks it to make.
 */

#include "ambre_test.h"
#include "host/web/ambre_web.h"
#include "host/web/ambre_web_limits.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WEB_SCENARIO "site/assets/data/scenario-web.txt"
#define WEB_SEGMENTS 2u
#define WEB_DURATION 1500u
#define WEB_CRASH    500u

/* ------------------------------------------------------------- JSON reads -- */

static const char *find_key(const char *json, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    return strstr(json, needle);
}

static int64_t json_i64(const char *json, const char *key, int64_t fallback)
{
    const char *p = find_key(json, key);
    if (!p) return fallback;
    p += strlen(key) + 3;
    return (int64_t)strtoll(p, NULL, 10);
}

static uint64_t json_u64(const char *json, const char *key, uint64_t fallback)
{
    const char *p = find_key(json, key);
    if (!p) return fallback;
    p += strlen(key) + 3;
    return (uint64_t)strtoull(p, NULL, 10);
}

static bool json_has(const char *json, const char *needle)
{
    return strstr(json, needle) != NULL;
}

/* ------------------------------------------------------------------- tests -- */

static void test_setup_and_info(void)
{
    const char *info;

    AMBRE_CHECK_OK(ambre_web_setup(NULL, 1234u, WEB_DURATION, WEB_CRASH,
                                    WEB_SEGMENTS));
    info = ambre_web_info_json();
    AMBRE_CHECK(info != NULL);
    AMBRE_CHECK(json_has(info, "\"ready\":true"));
    AMBRE_CHECK(json_u64(info, "op_count", 0u) > 100u);
    AMBRE_CHECK(json_u64(info, "segment_limit", 0u) == WEB_SEGMENTS);
    AMBRE_CHECK(json_has(info, "\"events\":["));
    AMBRE_CHECK(json_u64(info, "sample_count", 0u) > 0u);
}

static void test_trace_slice_and_stats(void)
{
    const char *ops = ambre_web_ops_json(0u, 8u);

    /* The API publishes into one buffer: a document is read before the next
     * call, exactly as the browser does with UTF8ToString. */
    AMBRE_CHECK(json_has(ops, "\"ops\":["));
    AMBRE_CHECK(json_has(ops, "\"from\":0"));

    ops = ambre_web_ops_json(0u, 100000000u);
    AMBRE_CHECK(json_has(ops, "\"ops\":["));

    const char *trace = ambre_web_trace_json();
    AMBRE_CHECK(json_u64(trace, "op_count", 0u) > 100u);
    AMBRE_CHECK(json_u64(trace, "writes", 0u) > 0u);
    AMBRE_CHECK(json_u64(trace, "flushes", 0u) > 0u);
    AMBRE_CHECK(json_u64(trace, "longest_write", 0u) <= 8192u);
    AMBRE_CHECK(json_u64(trace, "op_count", 0u) <=
                json_u64(trace, "max_ops_capacity", 0u));
}

/* A cut right after the event became durable must give it back intact. */
static void test_state_after_acquired(void)
{
    const char *info = ambre_web_info_json();
    int64_t acquired = json_i64(info, "acquired_op", -1);
    const char *state;

    AMBRE_CHECK_MSG(acquired > 0, "the trace never acquired an event");
    state = ambre_web_state_json((uint32_t)acquired + 1u, AMBRE_WEB_VAR_PREFIX,
                                  0u, 0u, 0u, 0u, 0);
    AMBRE_CHECK(json_has(state, "\"ok\":true"));
    AMBRE_CHECK(json_has(state, "\"have_emerg\":true"));
    AMBRE_CHECK(json_u64(state, "acquired_before_cut", 0u) >= 1u);
    AMBRE_CHECK(json_has(state, "\"violations\":[]"));
    AMBRE_CHECK(json_has(state, "\"image\":"));
    AMBRE_CHECK(json_has(state, "\"after\":"));
}

/* The same cut with each variant applied: recovery must still terminate and
 * keep the invariants (these are exactly the states the campaign explores). */
static void test_state_variants_are_clean(void)
{
    const char *info = ambre_web_info_json();
    int64_t acquired = json_i64(info, "acquired_op", -1);
    uint32_t prefix = (uint32_t)acquired + 1u;
    const char *state;

    state = ambre_web_state_json(prefix, AMBRE_WEB_VAR_TABLE, 0u, 0u, 0u, 0u, 1);
    AMBRE_CHECK(json_has(state, "\"ok\":true"));
    AMBRE_CHECK(json_has(state, "\"table_rebuilt\":true"));

    state = ambre_web_state_json(prefix, AMBRE_WEB_VAR_TORN, 512u, 0u, 0u, 0u, 0);
    AMBRE_CHECK(json_has(state, "\"ok\":true"));

    state = ambre_web_state_json(prefix, AMBRE_WEB_VAR_NESTED, 0u, 0u, 0u, 0u, 0);
    AMBRE_CHECK(json_has(state, "\"ok\":true"));
}

static void test_state_rejects_bad_input(void)
{
    const char *state = ambre_web_state_json(100000000u, AMBRE_WEB_VAR_PREFIX,
                                              0u, 0u, 0u, 0u, 0);
    AMBRE_CHECK(json_has(state, "\"error\""));
    state = ambre_web_state_json(1u, 42, 0u, 0u, 0u, 0u, 0);
    AMBRE_CHECK(json_has(state, "\"error\""));
}

/* An exploration through the browser API, one thread, in batches: the shape of
 * the counters must match what `ambre-explore --report` writes. */
static void test_sweep_batches_report_counters(void)
{
    const char *start = ambre_web_sweep_start_json(120u, 0x3eu);
    uint64_t total = json_u64(start, "total", 0u);
    const char *step = NULL;
    uint64_t done = 0u;
    int guard = 0;

    AMBRE_CHECK(total > 0u);
    AMBRE_CHECK(json_has(start, "\"planned\":"));
    while (done < total && guard++ < 100) {
        step = ambre_web_sweep_step_json(17u);
        done = json_u64(step, "done", 0u);
    }
    AMBRE_CHECK(step != NULL);
    AMBRE_CHECK(json_has(step, "\"finished\":true"));
    AMBRE_CHECK_EQ_U64(json_u64(step, "states_explored", 0u), done);
    AMBRE_CHECK_EQ_U64(json_u64(step, "violations", 1u), 0u);
    AMBRE_CHECK(json_has(step, "\"states_by_variant\":"));
    AMBRE_CHECK(json_has(step, "\"violations_by_invariant\":"));
}

/* The scenario the website ships must fit the browser build's op log, and its
 * longest write must fit the payload slot the simdisk keeps per write. */
static void test_shipped_scenario_fits_the_browser_bounds(void)
{
    const char *trace;
    FILE *f = fopen(WEB_SCENARIO, "r");

    if (!f) {
        printf("  (skip: %s not present)\n", WEB_SCENARIO);
        return;
    }
    fclose(f);
    AMBRE_CHECK_OK(ambre_web_setup(WEB_SCENARIO, 1234u, 0u, 0u, WEB_SEGMENTS));
    trace = ambre_web_trace_json();
    AMBRE_CHECK_MSG(json_u64(trace, "op_count", 0u) <= AMBRE_WEB_SIMDISK_MAX_OPS,
                    "the shipped scenario does not fit the browser op log");
    AMBRE_CHECK(json_u64(trace, "longest_write", 0u) <=
                json_u64(trace, "max_write_capacity", 0u));
    AMBRE_CHECK(json_u64(trace, "op_count", 0u) <=
                json_u64(trace, "max_ops_capacity", 0u));
    AMBRE_CHECK(json_has(ambre_web_info_json(), "\"scenario\":\"" WEB_SCENARIO));
}

static const ambre_test_case_t cases[] = {
    { "setup and info", test_setup_and_info },
    { "trace slice and stats", test_trace_slice_and_stats },
    { "state after the event was acquired", test_state_after_acquired },
    { "state variants stay clean", test_state_variants_are_clean },
    { "state rejects bad input", test_state_rejects_bad_input },
    { "sweep batches report counters", test_sweep_batches_report_counters },
    { "shipped scenario fits the browser bounds",
      test_shipped_scenario_fits_the_browser_bounds },
};

AMBRE_TEST_MAIN(cases)
