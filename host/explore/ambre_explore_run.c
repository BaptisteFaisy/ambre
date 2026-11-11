/*
 * ambre_explore_run.c — Exploration driver: split the state space across
 * workers (fork on PC), run every state, merge the counters, write the report.
 *
 * Each worker owns its scratch images (ambre_explore_ws_t) and inherits the
 * simdisk copy-on-write, so no multi-megabyte image is duplicated per state.
 */

#include "ambre_explore_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static uint32_t resolve_jobs(const ambre_explore_cfg_t *cfg)
{
    if (cfg->jobs > 0) return cfg->jobs;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (uint32_t)n : 1u;
}

static void merge_stats(ambre_explore_stats_t *dst,
                        const ambre_explore_stats_t *src)
{
    dst->states_explored += src->states_explored;
    dst->violations += src->violations;
    dst->events_acquired += src->events_acquired;
    dst->events_phantom += src->events_phantom;
    dst->recoveries_run += src->recoveries_run;
    for (uint32_t i = 0; i < 6u; i++) {
        dst->states_by_variant[i] += src->states_by_variant[i];
    }
    for (uint32_t i = 0; i < AMBRE_INVARIANT_COUNT; i++) {
        dst->per_invariant[i] += src->per_invariant[i];
    }
    if (dst->first_violation[0] == '\0' && src->first_violation[0] != '\0') {
        snprintf(dst->first_violation, sizeof(dst->first_violation), "%s",
                 src->first_violation);
    }
}

static void run_range(const ambre_explore_cfg_t *cfg, const ambre_simdisk_t *sd,
                      const ambre_truth_t *truth, const ambre_state_queue_t *q,
                      size_t start, size_t end, ambre_explore_stats_t *out)
{
    ambre_explore_ws_t ws;
    if (ambre_explore_ws_init(&ws, sd->size) != AMBRE_OK) return;
    for (size_t i = start; i < end; i++) {
        const ambre_explore_state_t *st = &q->items[i];
        ambre_invariant_report_t rep;
        int rc = ambre_explore_ws_test(&ws, cfg, sd, truth, st, &rep);
        if (rc < 0) continue;
        out->states_explored++;
        out->recoveries_run++;
        if (st->variant_type >= 0 && st->variant_type < 6) {
            out->states_by_variant[st->variant_type]++;
        }
        if (rc > 0) {
            out->violations++;
            for (uint32_t k = 0; k < AMBRE_INVARIANT_COUNT; k++) {
                if (rep.violated_mask & (1u << k)) out->per_invariant[k]++;
            }
            if (out->first_violation[0] == '\0') {
                snprintf(out->first_violation, sizeof(out->first_violation),
                         "prefix=%u variant=%d: %s", st->prefix,
                         st->variant_type, rep.detail);
            }
        }
    }
    ambre_explore_ws_free(&ws);
}

/* Child side: run a slice and hand the counters back on the shared pipe. */
static void child_slice(const ambre_explore_cfg_t *cfg, const ambre_simdisk_t *sd,
                        const ambre_truth_t *truth, const ambre_state_queue_t *q,
                        size_t start, size_t end, int fd)
{
    ambre_explore_stats_t local;
    memset(&local, 0, sizeof(local));
    run_range(cfg, sd, truth, q, start, end, &local);
    ssize_t w = write(fd, &local, sizeof(local));
    (void)w;
    close(fd);
    _exit(0);
}

static int run_parallel(const ambre_explore_cfg_t *cfg, const ambre_simdisk_t *sd,
                        const ambre_truth_t *truth, const ambre_state_queue_t *q,
                        ambre_explore_stats_t *out)
{
    uint32_t jobs = resolve_jobs(cfg);
    if (q->count == 0) return 0;
    if (jobs > q->count) jobs = (uint32_t)q->count;
    if (jobs <= 1) {
        run_range(cfg, sd, truth, q, 0, q->count, out);
        return 0;
    }
    int fds[2];
    if (pipe(fds) != 0) return -1;
    size_t chunk = (q->count + jobs - 1u) / jobs;
    uint32_t launched = 0;
    for (uint32_t k = 0; k < jobs; k++) {
        size_t s = (size_t)k * chunk;
        if (s >= q->count) break;
        size_t e = s + chunk;
        if (e > q->count) e = q->count;
        pid_t pid = fork();
        if (pid == 0) {
            close(fds[0]);
            child_slice(cfg, sd, truth, q, s, e, fds[1]);
        }
        if (pid > 0) launched++;
    }
    close(fds[1]);
    for (uint32_t k = 0; k < launched; k++) {
        ambre_explore_stats_t part;
        ssize_t n = read(fds[0], &part, sizeof(part));
        if (n == (ssize_t)sizeof(part)) merge_stats(out, &part);
        (void)wait(NULL);
    }
    close(fds[0]);
    return 0;
}

static void write_report(const ambre_explore_cfg_t *cfg,
                         const ambre_explore_stats_t *st)
{
    if (!cfg->report_json) return;
    FILE *f = fopen(cfg->report_json, "w");
    if (!f) return;
    double rate = (st->seconds > 0.0)
                  ? (double)st->states_explored / st->seconds : 0.0;
    fprintf(f, "{\n");
    fprintf(f, "  \"states_explored\": %llu,\n",
            (unsigned long long)st->states_explored);
    fprintf(f, "  \"violations\": %llu,\n", (unsigned long long)st->violations);
    fprintf(f, "  \"seconds\": %.3f,\n", st->seconds);
    fprintf(f, "  \"states_per_second\": %.1f,\n", rate);
    fprintf(f, "  \"recoveries_run\": %llu,\n",
            (unsigned long long)st->recoveries_run);
    fprintf(f, "  \"events_acquired\": %llu,\n",
            (unsigned long long)st->events_acquired);
    fprintf(f, "  \"events_phantom\": %llu,\n",
            (unsigned long long)st->events_phantom);
    static const char *names[6] = { "prefix", "torn", "cache", "bitflip",
                                    "table", "nested" };
    fprintf(f, "  \"states_by_variant\": {");
    for (uint32_t i = 0; i < 6u; i++) {
        fprintf(f, "%s\"%s\": %llu", i ? ", " : "", names[i],
                (unsigned long long)st->states_by_variant[i]);
    }
    fprintf(f, "},\n  \"violations_by_invariant\": {");
    static const char *inv[AMBRE_INVARIANT_COUNT] = {
        "I1", "I2", "I3", "I4", "I5", "I6", "I7", "I8"
    };
    for (uint32_t i = 0; i < AMBRE_INVARIANT_COUNT; i++) {
        fprintf(f, "%s\"%s\": %llu", i ? ", " : "", inv[i],
                (unsigned long long)st->per_invariant[i]);
    }
    fprintf(f, "},\n  \"first_violation\": \"%s\"\n}\n", st->first_violation);
    fclose(f);
}

int ambre_explore_run(const ambre_explore_cfg_t *cfg, ambre_explore_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    clock_t t0 = clock();
    uint64_t size = AMBRE_FIRST_DATA_OFFSET +
                    (uint64_t)cfg->segment_limit * AMBRE_SEGMENT_SIZE;
    ambre_simdisk_t sd;
    if (ambre_simdisk_create(&sd, size) != AMBRE_OK) return -1;
    ambre_truth_t truth;
    if (ambre_explore_record_truth(cfg, &sd, &truth) != AMBRE_OK) {
        ambre_simdisk_destroy(&sd);
        return -1;
    }
    ambre_state_queue_t q;
    memset(&q, 0, sizeof(q));
    if (ambre_explore_gen_states(&sd, cfg, &q) != AMBRE_OK) {
        ambre_state_queue_free(&q);
        ambre_truth_free(&truth);
        ambre_simdisk_destroy(&sd);
        return -1;
    }
    (void)run_parallel(cfg, &sd, &truth, &q, out);
    out->seconds = (double)(clock() - t0) / (double)CLOCKS_PER_SEC;
    if (out->seconds <= 0.0) out->seconds = 0.001;
    write_report(cfg, out);
    ambre_state_queue_free(&q);
    ambre_truth_free(&truth);
    ambre_simdisk_destroy(&sd);
    return (out->violations == 0) ? 0 : 1;
}
