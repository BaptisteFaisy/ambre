/*
 * fuzz_run.h — Shared types of the journal fuzzer (spec 12.10).
 *
 * Split out of fuzz_journal.c so that argument handling / regression saving
 * (fuzz_cli.c) and the fork-and-analyse campaign loop (fuzz_journal.c) stay
 * under the 500-line house limit each.
 */

#ifndef FUZZ_RUN_H
#define FUZZ_RUN_H

#include "fuzz_gen.h"
#include "fuzz_child.h"
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>

#define FUZZ_MAX_JOBS      16
#define FUZZ_DEFAULT_JOBS  2
#define FUZZ_TIMEOUT_MS    5000
#define FUZZ_QUICK_SECONDS 5.0
#define FUZZ_DEFAULT_SEED  20261111ull
#define FUZZ_REGRESS_DIR   "host/fuzz/regressions"

typedef struct {
    double      budget_s;        /* 0 = unbounded (use images)          */
    uint64_t    images;          /* 0 = unbounded (use budget_s)        */
    uint64_t    seed;
    int         jobs;
    int         timeout_ms;
    const char *regress_dir;
    const char *replay_file;
    const char *report_json;
} fuzz_opts;

typedef struct {
    ambre_memdisk_t md;
    pid_t           pid;
    uint64_t        index;
    int             strategy;
    bool            busy;
    bool            timed_out;
    struct timespec deadline;
} fuzz_slot;

typedef struct {
    uint64_t spawned;
    uint64_t violations;
    uint64_t per_strategy[FUZZ_S_COUNT];
    double   elapsed_s;
} fuzz_stats;

/* fuzz_cli.c */
void usage(const char *argv0);
bool opts_parse(fuzz_opts *o, int argc, char **argv);
void save_regression(const fuzz_slot *slot, const fuzz_opts *opts,
                     int exit_code, int signo, bool timed_out);

#endif /* FUZZ_RUN_H */
