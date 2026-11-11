/*
 * edr_replay.c — The demonstration producer (ACQ-05).
 *
 * One scenario row is one millisecond of driving; a frame carries at most
 * REPLAY_WINDOW_ROWS of them, so it covers at most 10 ms and stays well under
 * the 4096-byte ceiling of ACQ-01. Every frame is built in a caller-supplied
 * buffer: the send loop never allocates (GEL-03).
 *
 * Rules: files < 500 lines, functions < 50 lines, C11, no warning.
 */

#include "edr_replay.h"

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Round-trip bucket ceilings, in nanoseconds. The 50 us bucket is the PERF-05
 * threshold: everything at or below it is inside the requirement. */
static const uint64_t HIST_CEIL_NS[REPLAY_HIST_BUCKETS] = {
    1000u, 2000u, 5000u, 10000u, 25000u, 50000u,
    100000u, 250000u, 500000u, 1000000u, 5000000u, UINT64_MAX
};

/* ------------------------------------------------------------- pure part -- */

void replay_cfg_default(replay_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->producer_id = 1u;
    cfg->window_rows = REPLAY_WINDOW_ROWS;
}

void replay_run_cfg_default(replay_run_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    cfg->rate = 1.0;
    cfg->kill_after_ms = 0u;
    cfg->send_timeout_us = 500000u;      /* 500 ms: a producer never blocks */
    replay_cfg_default(&cfg->cfg);
}

uint32_t replay_frames_needed(uint32_t row_count, uint32_t window_rows)
{
    uint32_t w = (window_rows == 0u) ? REPLAY_WINDOW_ROWS : window_rows;

    return (row_count + w - 1u) / w;
}

void replay_hist_init(replay_hist_t *h)
{
    if (h != NULL) {
        memset(h, 0, sizeof(*h));
    }
}

void replay_hist_add(replay_hist_t *h, uint64_t latency_ns)
{
    if (h == NULL) {
        return;
    }
    h->total++;
    h->sum_ns += latency_ns;
    if (latency_ns > h->worst_ns) {
        h->worst_ns = latency_ns;
    }
    for (uint32_t i = 0u; i < REPLAY_HIST_BUCKETS; i++) {
        if (latency_ns <= HIST_CEIL_NS[i]) {
            h->count[i]++;
            return;
        }
    }
}

/* Upper bound of the bucket holding the requested permille (990 = p99). */
uint64_t replay_hist_percentile_ns(const replay_hist_t *h, uint32_t permille)
{
    uint64_t rank;
    uint64_t seen = 0u;

    if (h == NULL || h->total == 0u) {
        return 0u;
    }
    if (permille >= 1000u) {
        return h->worst_ns;
    }
    rank = (h->total * (uint64_t)permille + 999u) / 1000u;
    for (uint32_t i = 0u; i < REPLAY_HIST_BUCKETS; i++) {
        seen += h->count[i];
        if (seen >= rank) {
            return HIST_CEIL_NS[i];
        }
    }
    return h->worst_ns;
}

static void hist_print_bucket(const replay_hist_t *h, uint32_t i, FILE *out)
{
    double share = 100.0 * (double)h->count[i] / (double)h->total;

    if (i + 1u == REPLAY_HIST_BUCKETS) {
        fprintf(out, "  > %8" PRIu64 " ns : %8" PRIu64 "  (%5.2f%%)\n",
                HIST_CEIL_NS[i - 1u], h->count[i], share);
        return;
    }
    fprintf(out, "  <= %7" PRIu64 " ns : %8" PRIu64 "  (%5.2f%%)%s\n",
            HIST_CEIL_NS[i], h->count[i], share,
            (HIST_CEIL_NS[i] == 50000u) ? "   <- PERF-05 threshold" : "");
}

void replay_hist_print(const replay_hist_t *h, FILE *out)
{
    if (h == NULL || out == NULL || h->total == 0u) {
        return;
    }
    fprintf(out, "send round trip (PERF-05 / essay E4): %" PRIu64 " messages, "
                 "mean %" PRIu64 " ns, worst %" PRIu64 " ns\n",
            h->total, h->sum_ns / h->total, h->worst_ns);
    fprintf(out, "  p50 <= %" PRIu64 " ns, p99 <= %" PRIu64 " ns, "
                 "p99.9 <= %" PRIu64 " ns\n",
            replay_hist_percentile_ns(h, 500u),
            replay_hist_percentile_ns(h, 990u),
            replay_hist_percentile_ns(h, 999u));
    for (uint32_t i = 0u; i < REPLAY_HIST_BUCKETS; i++) {
        if (h->count[i] != 0u) {
            hist_print_bucket(h, i, out);
        }
    }
    fprintf(out, "  verdict: p99 %s the 50 us requirement\n",
            replay_hist_percentile_ns(h, 990u) <= 50000u ? "meets" : "EXCEEDS");
}

bool replay_external_changed(const scenario_row_t *prev,
                             const scenario_row_t *row)
{
    if (row == NULL) {
        return false;
    }
    return scenario_external_mask(row) != scenario_external_mask(prev);
}

int replay_build_external(uint64_t ts_us, uint32_t signal_mask,
                          uint32_t producer_id, void *buf, size_t cap)
{
    ambre_ipc_external_body_t body;

    body.ts_us = ts_us;
    body.signal_mask = signal_mask;      /* bits 0..4 only, see scenario_file */
    body.producer_id = producer_id;
    return ctl_ipc_build(buf, cap, (uint8_t)AMBRE_MSG_EXTERNAL,
                         AMBRE_PEER_PRODUCER, 0u, &body, (uint32_t)sizeof(body));
}

/* Convert the rows of one window into class B samples, refusing any id the
 * whitelist does not carry (ACQ-04, VIE-01). */
static uint32_t pack_window(const scenario_file_t *sc, uint32_t start,
                            uint32_t rows, ambre_sample_t *out, uint32_t max,
                            uint32_t *rejected)
{
    uint32_t n = 0u;

    for (uint32_t r = 0u; r < rows && n < max; r++) {
        ambre_sample_t tmp[8];
        uint32_t got = scenario_row_to_samples(&sc->rows[start + r], start + r,
                                               tmp, 8u);
        for (uint32_t k = 0u; k < got && n < max; k++) {
            if (!scenario_signal_whitelisted(tmp[k].signal_id)) {
                (*rejected)++;
                continue;
            }
            out[n++] = tmp[k];
        }
    }
    return n;
}

int replay_build_frame(const scenario_file_t *sc, uint32_t frame_index,
                       uint64_t base_ts_us, const replay_cfg_t *cfg,
                       void *buf, size_t cap, uint32_t *samples_out)
{
    ambre_sample_t samples[REPLAY_SAMPLES_MAX];
    ambre_ipc_frame_body_t body;
    uint32_t w = (cfg != NULL && cfg->window_rows != 0u) ? cfg->window_rows
                                                         : REPLAY_WINDOW_ROWS;
    uint32_t start = frame_index * w;
    uint32_t rows;
    uint32_t rejected = 0u;
    uint32_t span_us;
    size_t payload;

    if (sc == NULL || buf == NULL || sc->rows == NULL || start >= sc->count) {
        return AMBRE_ERR_PARAM;
    }
    rows = (sc->count - start < w) ? sc->count - start : w;
    span_us = (uint32_t)(sc->rows[start + rows - 1u].ts_us - sc->rows[start].ts_us);
    if (span_us >= REPLAY_WINDOW_US) {
        return AMBRE_ERR_TOO_BIG;        /* ACQ-01: a frame covers <= 10 ms */
    }
    body.sample_count = pack_window(sc, start, rows, samples,
                                    (uint32_t)REPLAY_SAMPLES_MAX, &rejected);
    body.first_ts_us = base_ts_us + sc->rows[start].ts_us;
    body.producer_id = (cfg != NULL) ? cfg->producer_id : 1u;
    payload = sizeof(body) + (size_t)body.sample_count * sizeof(ambre_sample_t);
    if (AMBRE_IPC_HDR_SIZE + payload > cap) {
        return AMBRE_ERR_FULL;
    }
    if (ctl_ipc_header(buf, cap, (uint8_t)AMBRE_MSG_FRAME, AMBRE_PEER_PRODUCER,
                       0u, (uint32_t)payload) <= 0) {
        return AMBRE_ERR_TOO_BIG;
    }
    memcpy((uint8_t *)buf + AMBRE_IPC_HDR_SIZE, &body, sizeof(body));
    memcpy((uint8_t *)buf + AMBRE_IPC_HDR_SIZE + sizeof(body), samples,
           (size_t)body.sample_count * sizeof(ambre_sample_t));
    if (samples_out != NULL) {
        *samples_out = body.sample_count;
    }
    return (int)(AMBRE_IPC_HDR_SIZE + payload);
}

/* ---------------------------------------------------------------- the run -- */

/* QNX-05 / robustness: --kill-after-ms makes the producer die brutally, so
 * the recorder has to notice through the closing of its connection, without
 * polling. SIGKILL leaves no handler a chance to run, which is the point. */
static void kill_self_if_due(uint64_t elapsed_us, uint32_t kill_after_ms)
{
    if (kill_after_ms == 0u || elapsed_us < (uint64_t)kill_after_ms * 1000u) {
        return;
    }
    ambre_port_log(AMBRE_LOG_WARN, "replay",
                   "kill-after-ms %u reached: SIGKILL on self (QNX-05)",
                   kill_after_ms);
    fflush(NULL);                        /* do not lose the report */
    (void)raise(SIGKILL);
    _exit(1);                            /* unreachable unless SIGKILL is blocked */
}

/* Real-time pacing: window i is due at base + i * window_us / rate. */
static void pace_window(uint64_t base_us, uint32_t frame_index,
                        uint32_t window_us, double rate)
{
    uint64_t due_us;
    uint64_t now_us;

    if (rate <= 0.0) {
        return;                          /* --fast: no pacing at all */
    }
    due_us = base_us + (uint64_t)((double)((uint64_t)frame_index *
                                           (uint64_t)window_us) / rate);
    now_us = ambre_port_now_us();
    if (due_us > now_us) {
        ambre_port_sleep_us(due_us - now_us);
    }
}

/* One send, with its round trip measured (PERF-05, essay E4). */
static int send_measured(ambre_port_conn_t *conn, const void *buf, size_t len,
                         const replay_run_cfg_t *cfg, replay_stats_t *out)
{
    uint64_t latency_ns = 0u;
    int rc = ambre_port_conn_send(conn, buf, len, cfg->send_timeout_us,
                                  &latency_ns);

    if (rc != AMBRE_PORT_OK) {
        out->send_errors++;
        return rc;
    }
    replay_hist_add(&out->hist, latency_ns);
    return AMBRE_PORT_OK;
}

/* External signals of one window, then the frame that covers them: the
 * capture never sees a frame before the signal that belongs to it. */
static int send_window_externals(ambre_port_conn_t *conn,
                                 const scenario_file_t *sc, uint32_t start,
                                 uint32_t rows, uint64_t base_us,
                                 const replay_run_cfg_t *cfg,
                                 replay_stats_t *out,
                                 const scenario_row_t **last_ext)
{
    uint8_t wire[AMBRE_IPC_HDR_SIZE + sizeof(ambre_ipc_external_body_t)];
    int rc;

    for (uint32_t r = 0u; r < rows; r++) {
        const scenario_row_t *row = &sc->rows[start + r];
        int len;

        if (!replay_external_changed(*last_ext, row)) {
            *last_ext = row;
            continue;
        }
        len = replay_build_external(base_us + row->ts_us,
                                    scenario_external_mask(row),
                                    cfg->cfg.producer_id, wire, sizeof(wire));
        *last_ext = row;
        if (len <= 0) {
            continue;
        }
        rc = send_measured(conn, wire, (size_t)len, cfg, out);
        if (rc != AMBRE_PORT_OK) {
            /* Propagate the transport's own error: AMBRE_PORT_ERR_TIMEOUT here
             * means the peer did not acknowledge an EXTERNAL message, which the
             * ack-based transport of ambre_port.h requires (reported, not
             * masked as a plain I/O error). */
            return rc;
        }
        out->externals_sent++;
    }
    return AMBRE_PORT_OK;
}

static int send_window(ambre_port_conn_t *conn, const scenario_file_t *sc,
                       uint32_t index, uint32_t window, uint64_t base_us,
                       const replay_run_cfg_t *cfg, replay_stats_t *out,
                       const scenario_row_t **last_ext)
{
    uint8_t frame[AMBRE_IPC_FRAME_MAX];
    uint32_t start = index * window;
    uint32_t rows = (sc->count - start < window) ? sc->count - start : window;
    uint32_t samples = 0u;
    int len;
    int rc = send_window_externals(conn, sc, start, rows, base_us, cfg, out,
                                   last_ext);

    if (rc != AMBRE_PORT_OK) {
        return rc;
    }
    len = replay_build_frame(sc, index, base_us, &cfg->cfg, frame, sizeof(frame),
                             &samples);
    if (len <= 0) {
        out->rejected_frames++;
        return len;
    }
    rc = send_measured(conn, frame, (size_t)len, cfg, out);
    if (rc != AMBRE_PORT_OK) {
        return rc;
    }
    out->frames_sent++;
    out->samples_sent += samples;
    return AMBRE_PORT_OK;
}

int replay_run(ambre_port_conn_t *conn, const scenario_file_t *sc,
               const replay_run_cfg_t *cfg, replay_stats_t *out)
{
    uint64_t base_us = ambre_port_now_us();
    uint32_t window = (cfg->cfg.window_rows != 0u) ? cfg->cfg.window_rows
                                                   : REPLAY_WINDOW_ROWS;
    uint32_t window_us = window * scenario_row_period_us(sc);
    uint32_t frames = replay_frames_needed(sc->count, window);
    const scenario_row_t *last_ext = NULL;
    int rc = AMBRE_PORT_OK;

    memset(out, 0, sizeof(*out));
    replay_hist_init(&out->hist);
    for (uint32_t i = 0u; i < frames; i++) {
        kill_self_if_due(ambre_port_now_us() - base_us, cfg->kill_after_ms);
        pace_window(base_us, i, window_us, cfg->rate);
        rc = send_window(conn, sc, i, window, base_us, cfg, out, &last_ext);
        if (rc != AMBRE_PORT_OK) {
            break;
        }
    }
    out->elapsed_us = ambre_port_now_us() - base_us;
    return rc;
}

/* ------------------------------------------------------------------- CLI --- */

void replay_print_usage(const char *prog, FILE *out)
{
    fprintf(out,
        "Usage: %s --capture <sock> --scenario <file.txt> "
        "[--rate 1.0 | --fast] [--kill-after-ms N]\n"
        "Options:\n"
        "  --capture <sock>     input socket of edr-capture\n"
        "  --scenario <file>    scenario file (docs/FORMAT.md section 11)\n"
        "  --rate <x>           replay speed, 1.0 = real time (default)\n"
        "  --fast               no pacing: as fast as the transport allows\n"
        "  --kill-after-ms N    SIGKILL on self N ms after the start, to test\n"
        "                       the detection of a producer's death (QNX-05)\n"
        "Exit codes: 0 ok, 1 functional failure, 2 usage error.\n",
        prog);
}

void replay_options_default(replay_options_t *opt)
{
    if (opt == NULL) {
        return;
    }
    memset(opt, 0, sizeof(*opt));
    replay_run_cfg_default(&opt->run);
}

static int parse_replay_option(int argc, char **argv, int *i,
                               replay_options_t *opt)
{
    const char *a = argv[*i];

    if (strcmp(a, "--capture") == 0 && *i + 1 < argc) {
        opt->capture_sock = argv[++(*i)];
    } else if (strcmp(a, "--scenario") == 0 && *i + 1 < argc) {
        opt->scenario_path = argv[++(*i)];
    } else if (strcmp(a, "--rate") == 0 && *i + 1 < argc) {
        opt->run.rate = strtod(argv[++(*i)], NULL);
        if (opt->run.rate <= 0.0) {
            return -1;                   /* a non-positive rate paces forever */
        }
    } else if (strcmp(a, "--fast") == 0) {
        opt->run.rate = 0.0;
    } else if (strcmp(a, "--kill-after-ms") == 0 && *i + 1 < argc) {
        opt->run.kill_after_ms = (uint32_t)strtoul(argv[++(*i)], NULL, 10);
    } else {
        return -1;
    }
    return 0;
}

int replay_parse_args(int argc, char **argv, replay_options_t *opt)
{
    replay_options_default(opt);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            replay_print_usage(argv[0], stdout);
            return 1;
        }
        if (parse_replay_option(argc, argv, &i, opt) != 0) {
            return -1;
        }
    }
    if (opt->capture_sock == NULL || opt->scenario_path == NULL) {
        return -1;
    }
    return 0;
}

int edr_replay_main(int argc, char **argv)
{
    replay_options_t opt;
    scenario_file_t sc;
    ambre_port_conn_t *conn = NULL;
    replay_stats_t stats;
    int rc = replay_parse_args(argc, argv, &opt);

    if (rc > 0) {
        return 0;
    }
    if (rc < 0) {
        replay_print_usage(argv[0], stderr);
        return 2;
    }
    if (scenario_file_load(opt.scenario_path, &sc) != 0) {
        ambre_port_log(AMBRE_LOG_ERROR, "replay", "cannot load scenario %s",
                       opt.scenario_path);
        return 1;
    }
    if (ambre_port_conn_open(opt.capture_sock, &conn) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, "replay", "cannot reach %s",
                       opt.capture_sock);
        scenario_file_free(&sc);
        return 1;
    }
    ambre_port_log(AMBRE_LOG_INFO, "replay", "%u rows, rate %.2f, kill %u ms",
                   sc.count, opt.run.rate, opt.run.kill_after_ms);
    rc = replay_run(conn, &sc, &opt.run, &stats);
    ambre_port_conn_close(conn);
    scenario_file_free(&sc);
    if (rc != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, "replay", "run failed (%d) after %"
                       PRIu64 " frames", rc, stats.frames_sent);
        return 1;
    }
    printf("edr-replay: %" PRIu64 " frames, %" PRIu64 " samples, %" PRIu64
           " external signal(s) in %" PRIu64 " ms\n",
           stats.frames_sent, stats.samples_sent, stats.externals_sent,
           stats.elapsed_us / 1000u);
    replay_hist_print(&stats.hist, stdout);
    return 0;
}
