/*
 * fuzz_journal.c — Deterministic seeded fuzzer for the Ambre journal
 * recovery path (spec 12.10, "random-data testing", extreme level).
 *
 * Method: generate a random disk image (pure noise, partially written
 * journal, truncated block, corrupted table, torn record — see fuzz_gen.c),
 * fork a child that runs ambre_recover() then ambre_journal_read_event()
 * (fuzz_child.c), and enforce from the parent:
 *   - a per-image wall-clock bound  -> an infinite loop is a violation;
 *   - crash detection via the child exit status (signal = crash);
 *   - a nonzero child exit code = a failed post-condition.
 * Every violating image is saved with its seed into the regression directory
 * (spec 7.7) and can be replayed with --replay.
 *
 * Everything is reproducible: image #i of a run only depends on (--seed, i).
 *
 * Usage (all modes are time- or count-bounded):
 *   ambre-fuzz --quick                  # seconds, for the CI
 *   ambre-fuzz --minutes 10             # bounded campaign
 *   ambre-fuzz --hours 24 --jobs 4      # the 24 h soak of spec 12.10
 *   ambre-fuzz --seed S --images N      # exact reproducible run
 *   ambre-fuzz --replay path/to.img     # re-run one saved image
 * Exit code: 0 = no violation, 1 = at least one violation, 2 = usage error.
 */

#define _POSIX_C_SOURCE 200809L
#include "fuzz_run.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ time - */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void deadline_after(struct timespec *out, int ms)
{
    clock_gettime(CLOCK_MONOTONIC, out);
    out->tv_sec  += ms / 1000;
    out->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (out->tv_nsec >= 1000000000L) {
        out->tv_sec  += 1;
        out->tv_nsec -= 1000000000L;
    }
}

static bool deadline_passed(const struct timespec *d)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > d->tv_sec ||
           (now.tv_sec == d->tv_sec && now.tv_nsec >= d->tv_nsec);
}

static void nap_1ms(void)
{
    struct timespec ts = { 0, 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------- slots and pool -- */

/* Generate image #index into the slot, then fork the analysis child. */
static void spawn_slot(fuzz_gen_ws_t *ws, fuzz_slot *slot,
                       const fuzz_opts *opts, uint64_t index, fuzz_stats *st)
{
    slot->strategy = fuzz_gen_image(ws, &slot->md, opts->seed, index);
    if (slot->strategy < 0) {
        fprintf(stderr, "fuzz: image generation failed at index %llu\n",
                (unsigned long long)index);
        exit(2);
    }
    slot->index = index;
    slot->timed_out = false;
    st->spawned++;
    st->per_strategy[slot->strategy]++;
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fuzz: fork failed: %s\n", strerror(errno));
        exit(2);
    }
    if (pid == 0) {
        _exit(fuzz_child_run(&slot->md));   /* child: analyse, never flush */
    }
    slot->pid = pid;
    slot->busy = true;
    deadline_after(&slot->deadline, opts->timeout_ms);
}

/* Non-blocking reap of one slot. Returns true when the slot is free again. */
static bool reap_slot(fuzz_slot *slot, const fuzz_opts *opts, fuzz_stats *st)
{
    int status = 0;
    pid_t r = waitpid(slot->pid, &status, WNOHANG);

    if (r == 0) {
        if (!slot->timed_out && deadline_passed(&slot->deadline)) {
            slot->timed_out = true;         /* infinite loop is a violation */
            kill(slot->pid, SIGKILL);
        }
        return false;
    }
    if (r < 0) {
        slot->busy = false;
        return true;
    }
    slot->busy = false;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !slot->timed_out) {
        return true;                        /* clean image */
    }
    st->violations++;
    save_regression(slot, opts,
                    WIFEXITED(status) ? WEXITSTATUS(status) : 0,
                    WIFSIGNALED(status) ? WTERMSIG(status) : 0,
                    slot->timed_out);
    return true;
}

/* ---------------------------------------------------------------- report - */

static void write_report_json(const fuzz_opts *opts, const fuzz_stats *st)
{
    FILE *f = fopen(opts->report_json, "w");
    if (f == NULL) {
        fprintf(stderr, "fuzz: cannot write %s: %s\n",
                opts->report_json, strerror(errno));
        return;
    }
    fprintf(f, "{\n  \"tool\": \"ambre-fuzz\",\n  \"seed\": %llu,\n"
               "  \"jobs\": %d,\n  \"timeout_ms\": %d,\n  \"budget_s\": %.3f,\n"
               "  \"images\": %llu,\n  \"violations\": %llu,\n"
               "  \"elapsed_s\": %.3f,\n  \"img_per_s\": %.2f,\n"
               "  \"regress_dir\": \"%s\",\n  \"per_strategy\": {",
            (unsigned long long)opts->seed, opts->jobs, opts->timeout_ms,
            opts->budget_s, (unsigned long long)st->spawned,
            (unsigned long long)st->violations, st->elapsed_s,
            st->elapsed_s > 0.0 ? (double)st->spawned / st->elapsed_s : 0.0,
            opts->regress_dir);
    for (int i = 0; i < FUZZ_S_COUNT; i++) {
        fprintf(f, "%s\n    \"%s\": %llu", i == 0 ? "" : ",",
                fuzz_strategy_name(i), (unsigned long long)st->per_strategy[i]);
    }
    fprintf(f, "\n  }\n}\n");
    fclose(f);
}

static void print_summary(const fuzz_opts *opts, const fuzz_stats *st)
{
    printf("ambre-fuzz: seed=%llu images=%llu violations=%llu elapsed=%.1fs"
           " (%.1f img/s)\n",
           (unsigned long long)opts->seed, (unsigned long long)st->spawned,
           (unsigned long long)st->violations, st->elapsed_s,
           st->elapsed_s > 0.0 ? (double)st->spawned / st->elapsed_s : 0.0);
    for (int i = 0; i < FUZZ_S_COUNT; i++) {
        printf("  %-12s %llu image(s)\n", fuzz_strategy_name(i),
               (unsigned long long)st->per_strategy[i]);
    }
    if (st->violations > 0u) {
        printf("  regressions saved under %s (replay with --replay FILE)\n",
               opts->regress_dir);
    }
}

/* --------------------------------------------------------------- campaign - */

static int slots_init(fuzz_slot *slots, int jobs)
{
    for (int i = 0; i < jobs; i++) {
        if (ambre_memdisk_init(&slots[i].md, AMBRE_JOURNAL_BYTES) != AMBRE_OK) {
            fprintf(stderr, "fuzz: slot %d init failed\n", i);
            return -1;
        }
    }
    return 0;
}

/* One scheduling pass: fill a free slot, reap the finished ones. */
static bool campaign_step(fuzz_gen_ws_t *ws, fuzz_slot *slots,
                          const fuzz_opts *opts, uint64_t *next_index,
                          fuzz_stats *st, bool idle)
{
    bool any_busy = false;

    if (!idle) {
        for (int i = 0; i < opts->jobs; i++) {
            if (!slots[i].busy) {
                spawn_slot(ws, &slots[i], opts, (*next_index)++, st);
                break;
            }
        }
    }
    for (int i = 0; i < opts->jobs; i++) {
        if (slots[i].busy) {
            reap_slot(&slots[i], opts, st);
            any_busy = any_busy || slots[i].busy;
        }
    }
    return any_busy;
}

static void campaign_progress(const fuzz_stats *st, double t0,
                              double *next_progress)
{
    if (now_s() < *next_progress) {
        return;
    }
    *next_progress = now_s() + 2.0;
    printf("  ... %llu images, %llu violation(s), %.1f img/s\n",
           (unsigned long long)st->spawned, (unsigned long long)st->violations,
           (double)st->spawned / (now_s() - t0));
}

static int run_campaign(const fuzz_opts *opts)
{
    fuzz_gen_ws_t ws;
    fuzz_stats st;
    double t0 = now_s();
    double next_progress = t0 + 2.0;
    uint64_t next_index = 0u;
    int rc;

    memset(&st, 0, sizeof(st));
    fuzz_slot *slots = (fuzz_slot *)calloc((size_t)opts->jobs, sizeof(*slots));
    if (slots == NULL || fuzz_gen_ws_init(&ws) != 0 ||
        slots_init(slots, opts->jobs) != 0) {
        fprintf(stderr, "fuzz: out of memory\n");
        free(slots);
        return 2;
    }
    printf("ambre-fuzz: seed=%llu jobs=%d timeout=%dms budget=%.0fs\n",
           (unsigned long long)opts->seed, opts->jobs, opts->timeout_ms,
           opts->budget_s);
    for (;;) {
        bool idle = (opts->budget_s > 0.0 && now_s() - t0 >= opts->budget_s) ||
                    (opts->images > 0u && st.spawned >= opts->images);
        bool any_busy = campaign_step(&ws, slots, opts, &next_index, &st, idle);

        if (idle && !any_busy) {
            break;
        }
        campaign_progress(&st, t0, &next_progress);
        nap_1ms();
    }
    st.elapsed_s = now_s() - t0;
    print_summary(opts, &st);
    if (opts->report_json != NULL) {
        write_report_json(opts, &st);
    }
    rc = st.violations > 0u ? 1 : 0;
    for (int i = 0; i < opts->jobs; i++) {
        ambre_memdisk_destroy(&slots[i].md);
    }
    fuzz_gen_ws_free(&ws);
    free(slots);
    return rc;
}

/* ---------------------------------------------------------------- replay - */

static int replay_child(ambre_memdisk_t *md, int timeout_ms)
{
    struct timespec deadline;
    int status = 0;
    bool killed = false;
    pid_t pid = fork();

    if (pid < 0) {
        return 2;
    }
    if (pid == 0) {
        _exit(fuzz_child_run(md));
    }
    deadline_after(&deadline, timeout_ms);
    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (!killed && deadline_passed(&deadline)) {
            killed = true;
            kill(pid, SIGKILL);
        }
        nap_1ms();
    }
    if (killed) {
        printf("replay: VIOLATION timeout (> %d ms)\n", timeout_ms);
        return 1;
    }
    if (WIFSIGNALED(status)) {
        printf("replay: VIOLATION crash signal %d (%s)\n",
               WTERMSIG(status), strsignal(WTERMSIG(status)));
        return 1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        printf("replay: VIOLATION check %s\n",
               fuzz_fail_name(WEXITSTATUS(status)));
        return 1;
    }
    printf("replay: OK (recover + read_event clean)\n");
    return 0;
}

static int run_replay(const fuzz_opts *opts)
{
    ambre_memdisk_t md;
    int rc;
    FILE *f = fopen(opts->replay_file, "rb");

    if (f == NULL) {
        fprintf(stderr, "fuzz: cannot open %s: %s\n",
                opts->replay_file, strerror(errno));
        return 2;
    }
    if (ambre_memdisk_init(&md, AMBRE_JOURNAL_BYTES) != AMBRE_OK) {
        fclose(f);
        return 2;
    }
    size_t got = fread(md.image, 1, (size_t)AMBRE_JOURNAL_BYTES, f);
    fclose(f);
    if (got != (size_t)AMBRE_JOURNAL_BYTES) {
        fprintf(stderr, "fuzz: %s holds %zu bytes, expected %llu\n",
                opts->replay_file, got, (unsigned long long)AMBRE_JOURNAL_BYTES);
        ambre_memdisk_destroy(&md);
        return 2;
    }
    rc = replay_child(&md, opts->timeout_ms);
    ambre_memdisk_destroy(&md);
    return rc;
}

int main(int argc, char **argv)
{
    fuzz_opts opts;

    if (!opts_parse(&opts, argc, argv)) {
        return 2;
    }
    if (opts.replay_file != NULL) {
        return run_replay(&opts);
    }
    return run_campaign(&opts);
}
