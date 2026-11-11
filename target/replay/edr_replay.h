/*
 * edr_replay.h — The demonstration producer (ACQ-05).
 *
 * edr-replay reads a scenario file (docs/FORMAT.md §11) and injects it into
 * edr-capture as AMBRE_MSG_FRAME datagrams, at real cadence (--rate 1.0) or as
 * fast as the transport allows (--fast). Every send measures its own round
 * trip, which is what PERF-05 and feasibility essay E4 report (< 50 us at the
 * 99th percentile).
 *
 * Budget (ACQ-01): one frame covers at most 10 ms and at most 4096 bytes, so
 * the run loop builds every frame in a fixed buffer and never allocates.
 *
 * Privacy (VIE-01, VIE-02): only whitelisted signal ids are emitted and every
 * timestamp is monotonic and relative to the start of the run.
 */

#ifndef EDR_REPLAY_H
#define EDR_REPLAY_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "scenario_file.h"
#include "../ctl/edr_ipc_wire.h"

/* Rows per frame. The scenario is 1 kHz, so 10 rows = 10 ms = the ACQ-01
 * budget. A smaller window is accepted (tests use it); a larger one is
 * refused by replay_build_frame(). */
#define REPLAY_WINDOW_ROWS      10u
#define REPLAY_WINDOW_US     10000u

/* Samples that fit in one frame: (4096 - 16 - 16) / 8 = 508. */
#define REPLAY_SAMPLES_MAX    ((AMBRE_IPC_FRAME_MAX - AMBRE_IPC_HDR_SIZE \
                                - sizeof(ambre_ipc_frame_body_t)) \
                               / sizeof(ambre_sample_t))

/* Latency histogram (PERF-05): fixed buckets, no allocation, no sample list. */
#define REPLAY_HIST_BUCKETS     12u

typedef struct {
    uint64_t count[REPLAY_HIST_BUCKETS];
    uint64_t total;
    uint64_t worst_ns;
    uint64_t sum_ns;
} replay_hist_t;

typedef struct {
    uint32_t producer_id;
    uint32_t window_rows;       /* rows per frame, 0 = REPLAY_WINDOW_ROWS    */
} replay_cfg_t;

typedef struct {
    double   rate;              /* 1.0 = real time, 0 = as fast as possible  */
    uint32_t kill_after_ms;     /* 0 = never; else SIGKILL on self (QNX-05)  */
    uint32_t send_timeout_us;
    replay_cfg_t cfg;
} replay_run_cfg_t;

typedef struct {
    uint64_t frames_sent;
    uint64_t externals_sent;
    uint64_t samples_sent;
    uint64_t rejected_frames;   /* frame refused by the budget or the peer   */
    uint64_t send_errors;
    uint64_t elapsed_us;
    bool     killed;            /* --kill-after-ms fired                     */
    replay_hist_t hist;
} replay_stats_t;

/* ------------------------------------------------------------ pure part --- */

void replay_cfg_default(replay_cfg_t *cfg);
void replay_run_cfg_default(replay_run_cfg_t *cfg);

/* Frames needed to cover `row_count` rows of `window_rows` each. */
uint32_t replay_frames_needed(uint32_t row_count, uint32_t window_rows);

/* Build the AMBRE_MSG_FRAME of window `frame_index`. `base_ts_us` is the
 * monotonic instant the run started; the frame is stamped base + row ts, so
 * no absolute time ever leaves the producer (VIE-02).
 * Returns the wire size (16..4096), or a negative ambre_err_t:
 *   AMBRE_ERR_PARAM   bad arguments, window beyond the scenario
 *   AMBRE_ERR_TOO_BIG the window covers more than 10 ms (ACQ-01)
 *   AMBRE_ERR_FULL    the samples would not fit in `cap`
 * `samples_out` receives the number of samples carried. */
int replay_build_frame(const scenario_file_t *sc, uint32_t frame_index,
                       uint64_t base_ts_us, const replay_cfg_t *cfg,
                       void *buf, size_t cap, uint32_t *samples_out);

/* True when the external-signal state of `row` differs from `prev` (prev may
 * be NULL for the first row): the producer sends one AMBRE_MSG_EXTERNAL per
 * change, never one per row. */
bool replay_external_changed(const scenario_row_t *prev,
                             const scenario_row_t *row);

/* Build an AMBRE_MSG_EXTERNAL. Returns the wire size or a negative error. */
int replay_build_external(uint64_t ts_us, uint32_t signal_mask,
                          uint32_t producer_id, void *buf, size_t cap);

/* Histogram: add one round-trip measurement, then read it back. `permille`
 * is the percentile in thousandths (990 = p99, the PERF-05 threshold). */
void     replay_hist_init(replay_hist_t *h);
void     replay_hist_add(replay_hist_t *h, uint64_t latency_ns);
uint64_t replay_hist_percentile_ns(const replay_hist_t *h, uint32_t permille);
void     replay_hist_print(const replay_hist_t *h, FILE *out);

/* -------------------------------------------------------------- the run --- */

/* Send the whole scenario over an open connection. Returns 0 when every frame
 * was acknowledged, a negative ambre_port_err_t otherwise. */
int replay_run(ambre_port_conn_t *conn, const scenario_file_t *sc,
               const replay_run_cfg_t *cfg, replay_stats_t *out);

/* Options parsed from the CLI. */
typedef struct {
    const char      *capture_sock;
    const char      *scenario_path;
    replay_run_cfg_t run;
} replay_options_t;

void replay_options_default(replay_options_t *opt);
int  replay_parse_args(int argc, char **argv, replay_options_t *opt);
void replay_print_usage(const char *prog, FILE *out);

/* Process entry point: 0 ok, 1 functional failure, 2 usage error. */
int edr_replay_main(int argc, char **argv);

#endif /* EDR_REPLAY_H */
