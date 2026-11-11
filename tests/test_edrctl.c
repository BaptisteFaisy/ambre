/*
 * test_edrctl.c — Unit and integration tests for edrctl and edr-replay.
 *
 * Rules: files < 500 lines, functions < 50 lines, C11, no warnings.
 */

#define _POSIX_C_SOURCE 200809L

#include "ambre_test.h"
#include "ambre_test_port.h"

#include "../target/replay/scenario_file.h"
#include "../target/replay/edr_replay.h"
#include "../target/ctl/edr_ctl.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void write_test_scenario(const char *path, int rows)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# ambre test scenario\nname=test-frontal\nseed=42\n"
               "rate_hz=1000\nduration_ms=%d\ncrash_at_ms=10\n"
               "# t_us a_long a_lat speed throttle brake rpm steer ext\n", rows);
    for (int i = 0; i < rows; i++) {
        long al = (i >= 10 && i < 15) ? -80000 : 12;
        long alat = (i >= 10 && i < 15) ? 5000 : -3;
        long brk = (i >= 10) ? 1 : 0;
        long ext = (i == 10) ? EXT_FLAG_RESTRAINT : 0;
        fprintf(f, "%d %ld %ld 300 0 %ld 800 0 %ld\n",
                i * 1000, al, alat, brk, ext);
    }
    fclose(f);
}

/* ------------------------------------------------------------- mock xfer -- */

typedef struct {
    uint8_t reply[AMBRE_IPC_FRAME_MAX];
    size_t  reply_len;
    uint8_t last_req_type;
    uint8_t last_peer_type;
    uint8_t last_flags;
} mock_loop_t;

static int mock_loop_xfer(void *ctx, const void *req, size_t req_len,
                          void *reply, size_t reply_cap,
                          uint32_t timeout_us, size_t *reply_len)
{
    (void)timeout_us;
    mock_loop_t *m = (mock_loop_t *)ctx;
    ambre_ipc_hdr_t h;
    if (req_len < AMBRE_IPC_HDR_SIZE) return AMBRE_PORT_ERR_PARAM;
    memcpy(&h, req, sizeof(h));
    m->last_req_type = h.msg_type;
    m->last_peer_type = h.reserved;
    m->last_flags = h.flags;
    if (m->reply_len > reply_cap) return AMBRE_PORT_ERR_FULL;
    memcpy(reply, m->reply, m->reply_len);
    *reply_len = m->reply_len;
    return AMBRE_PORT_OK;
}

/* ------------------------------------------------------------- unit tests - */

static void test_scenario_load(void)
{
    char path[128];
    scenario_file_t sc;
    ambre_test_tmp_path(path, sizeof(path), "scen.txt");
    write_test_scenario(path, 20);
    AMBRE_CHECK_EQ_I64(scenario_file_load(path, &sc), 0);
    AMBRE_CHECK_EQ_U64(sc.count, 20);
    AMBRE_CHECK_EQ_U64(sc.rate_hz, 1000);
    AMBRE_CHECK_EQ_U64(sc.duration_ms, 20);
    AMBRE_CHECK_EQ_U64(sc.crash_at_ms, 10);
    AMBRE_CHECK_EQ_I64(strcmp(sc.name, "test-frontal"), 0);
    AMBRE_CHECK_EQ_I64(sc.rows[10].a_long_mm_s2, -80000);
    AMBRE_CHECK_EQ_U64(sc.rows[10].ext_flags, EXT_FLAG_RESTRAINT);
    scenario_file_free(&sc);
    (void)unlink(path);
}

static void test_whitelist(void)
{
    AMBRE_CHECK(scenario_signal_whitelisted(SIG_ACCEL_X));
    AMBRE_CHECK(scenario_signal_whitelisted(SIG_ACCEL_Y));
    AMBRE_CHECK(scenario_signal_whitelisted(SIG_WHEEL_FL));
    AMBRE_CHECK(scenario_signal_whitelisted(SIG_STEER_ANGLE));
    AMBRE_CHECK(scenario_signal_whitelisted(SIG_ADAS_STATES));
    AMBRE_CHECK(scenario_signal_whitelisted(SIG_GEAR_SELECTOR));
    for (uint16_t id = 90; id <= 99; id++) {
        AMBRE_CHECK(!scenario_signal_whitelisted(id));
    }
    AMBRE_CHECK(!scenario_signal_whitelisted(0));
    AMBRE_CHECK(!scenario_signal_whitelisted(4));
    AMBRE_CHECK(!scenario_signal_whitelisted(100));
}

static void test_row_to_samples(void)
{
    scenario_row_t row;
    memset(&row, 0, sizeof(row));
    row.a_long_mm_s2 = 1200;
    row.a_lat_mm_s2 = -300;
    row.steer_deg_x2 = 10;
    row.throttle_pct = 50;
    row.engine_rpm = 2500;
    row.ext_flags = EXT_FLAG_AEB;

    ambre_sample_t s[16];
    AMBRE_CHECK_EQ_U64(scenario_row_to_samples(&row, 0, s, 16), 7);
    AMBRE_CHECK_EQ_U64(s[0].signal_id, SIG_ACCEL_X);
    AMBRE_CHECK_EQ_I64(s[0].value, 1200);
    AMBRE_CHECK_EQ_U64(s[1].signal_id, SIG_ACCEL_Y);
    AMBRE_CHECK_EQ_U64(s[6].signal_id, SIG_ADAS_STATES);
    AMBRE_CHECK_EQ_U64(scenario_row_to_samples(&row, 1, s, 16), 2);
    AMBRE_CHECK_EQ_U64(scenario_row_to_samples(&row, 10, s, 16), 6);
}

static void test_frame_builder_budget(void)
{
    char path[128];
    scenario_file_t sc;
    ambre_test_tmp_path(path, sizeof(path), "budget.txt");
    write_test_scenario(path, 20);
    AMBRE_CHECK_OK(scenario_file_load(path, &sc));

    uint8_t buf[AMBRE_IPC_FRAME_MAX];
    uint32_t samples_out = 0;
    replay_cfg_t cfg;
    replay_cfg_default(&cfg);

    for (uint32_t i = 0; i < 2; i++) {
        int len = replay_build_frame(&sc, i, 1000000u, &cfg, buf, sizeof(buf), &samples_out);
        AMBRE_CHECK(len > 0 && len <= (int)AMBRE_IPC_FRAME_MAX);
        AMBRE_CHECK_OK(ambre_ipc_frame_check(buf, (size_t)len));
    }
    AMBRE_CHECK(replay_build_frame(&sc, 2, 1000000u, &cfg, buf, sizeof(buf), &samples_out) < 0);
    cfg.window_rows = 11;
    AMBRE_CHECK_EQ_I64(replay_build_frame(&sc, 0, 1000000u, &cfg, buf, sizeof(buf), &samples_out),
                       AMBRE_ERR_TOO_BIG);
    scenario_file_free(&sc);
    (void)unlink(path);
}

static void test_histogram_and_cli(void)
{
    replay_hist_t h;
    replay_hist_init(&h);
    for (int i = 0; i < 990; i++) replay_hist_add(&h, 20000u);
    for (int i = 0; i < 10; i++)  replay_hist_add(&h, 45000u);
    AMBRE_CHECK_EQ_U64(h.total, 1000);
    AMBRE_CHECK(replay_hist_percentile_ns(&h, 990) <= 50000u);

    replay_options_t ropt;
    char *r_help[] = {"edr-replay", "--help"};
    char *r_bad[] = {"edr-replay", "--bogus"};
    char *r_ok[] = {"edr-replay", "--capture", "/tmp/c.sock", "--scenario", "/tmp/s.txt", "--fast"};
    AMBRE_CHECK_EQ_I64(replay_parse_args(2, r_help, &ropt), 1);
    AMBRE_CHECK_EQ_I64(replay_parse_args(2, r_bad, &ropt), -1);
    AMBRE_CHECK_EQ_I64(replay_parse_args(6, r_ok, &ropt), 0);
    AMBRE_CHECK(ropt.run.rate == 0.0);

    ctl_args_t cargs;
    char *c_help[] = {"edrctl", "--help"};
    char *c_bad[] = {"edrctl", "--journal", "/tmp/j.sock"};
    char *c_st[] = {"edrctl", "--journal", "/tmp/j.sock", "status", "--json"};
    char *c_ex[] = {"edrctl", "--journal", "/tmp/j.sock", "export", "3", "--out", "/tmp/e.amex"};
    AMBRE_CHECK_EQ_I64(ctl_parse_args(2, c_help, &cargs), 1);
    AMBRE_CHECK_EQ_I64(ctl_parse_args(3, c_bad, &cargs), -1);
    AMBRE_CHECK_EQ_I64(ctl_parse_args(5, c_st, &cargs), 0);
    AMBRE_CHECK(cargs.cmd == CTL_CMD_STATUS && cargs.json);
    AMBRE_CHECK_EQ_I64(ctl_parse_args(7, c_ex, &cargs), 0);
    AMBRE_CHECK(cargs.cmd == CTL_CMD_EXPORT && cargs.event_id == 3);
}

static void test_container_roundtrip(void)
{
    char path[128];
    ambre_test_tmp_path(path, sizeof(path), "out.amex");
    FILE *f = NULL;
    AMBRE_CHECK_OK(ctl_container_open(path, &f));

    ambre_block_t blk;
    uint8_t raw[AMBRE_BLOCK_SIZE];
    uint32_t fp = 0u;
    for (uint32_t i = 0; i < 3; i++) {
        ambre_sample_t smp = { .signal_id = 1, .quality = 0, .pad = 0, .value = (int32_t)(100 + i) };
        AMBRE_CHECK_OK(ambre_block_build(&blk, AMBRE_REC_DATA, i + 1, 100, 1000u * i, &smp, sizeof(smp)));
        ambre_block_pack(&blk, raw);
        AMBRE_CHECK_OK(ctl_container_append(f, raw));
        fp = ambre_crc32c(fp, raw, sizeof(raw));
    }
    ctl_container_info_t info = {
        .event_id = 7, .block_count = 3, .fingerprint = fp, .flags = CTL_EXPORT_FLAG_COMPLETE
    };
    AMBRE_CHECK_OK(ctl_container_finish(f, &info));

    ctl_container_report_t rep;
    AMBRE_CHECK_OK(ctl_container_verify(path, &rep));
    AMBRE_CHECK(rep.header_ok && rep.fingerprint_match);
    AMBRE_CHECK_EQ_U64(rep.blocks_checked, 3);
    AMBRE_CHECK_EQ_U64(rep.blocks_bad, 0);
    (void)unlink(path);
}

/* ----------------------------------------------------- mock integration --- */

static void test_mock_status_list_nak(void)
{
    mock_loop_t mock;
    memset(&mock, 0, sizeof(mock));
    ctl_link_t link = { .ctx = &mock, .xfer = mock_loop_xfer };
    ctl_args_t args;
    FILE *devnull = fopen("/dev/null", "w");
    AMBRE_CHECK(devnull != NULL);

    ambre_ipc_status_body_t sb = { .mode = 1, .backend = (uint8_t)AMBRE_PORT_BACKEND_POSIX,
                                   .sealed_events = 2, .last_freeze_us = 12500 };
    int len = ctl_ipc_build(mock.reply, sizeof(mock.reply), (uint8_t)AMBRE_MSG_STATUS_REPLY,
                            AMBRE_PEER_JOURNAL, 0u, &sb, sizeof(sb));
    AMBRE_CHECK(len > 0);
    mock.reply_len = (size_t)len;
    ctl_args_default(&args);
    args.cmd = CTL_CMD_STATUS; args.json = true;
    AMBRE_CHECK_EQ_I64(ctl_run_status(&link, &args, devnull), 0);
    AMBRE_CHECK_EQ_U64(mock.last_peer_type, (uint8_t)AMBRE_PEER_SERVICE);

    uint8_t lbuf[sizeof(uint32_t) + sizeof(ambre_ipc_event_line_t)];
    uint32_t cnt = 1;
    ambre_ipc_event_line_t ev = { .event_id = 4, .state = AMBRE_EV_SEALED,
                                  .locked = 1, .file_complete = 1, .trig_reason = AMBRE_TRIG_RESTRAINT };
    memcpy(lbuf, &cnt, sizeof(cnt));
    memcpy(lbuf + sizeof(cnt), &ev, sizeof(ev));
    mock.reply_len = (size_t)ctl_ipc_build(mock.reply, sizeof(mock.reply),
                                           (uint8_t)AMBRE_MSG_LIST_REPLY, AMBRE_PEER_JOURNAL, 0u,
                                           lbuf, sizeof(lbuf));
    args.cmd = CTL_CMD_LIST;
    AMBRE_CHECK_EQ_I64(ctl_run_list(&link, &args, devnull), 0);

    ctl_ipc_nak_body_t nak = { .code = AMBRE_PORT_ERR_PERM, .event_id = 4 };
    mock.reply_len = (size_t)ctl_ipc_build(mock.reply, sizeof(mock.reply),
                                           (uint8_t)AMBRE_MSG_NAK, AMBRE_PEER_JOURNAL, 0u,
                                           &nak, sizeof(nak));
    AMBRE_CHECK_EQ_I64(ctl_run_status(&link, &args, devnull), 1);
    fclose(devnull);
}

static void test_mock_export_and_verify(void)
{
    mock_loop_t mock;
    memset(&mock, 0, sizeof(mock));
    ctl_link_t link = { .ctx = &mock, .xfer = mock_loop_xfer };
    char out_path[128];
    ambre_test_tmp_path(out_path, sizeof(out_path), "exp.amex");

    /* Build a canned 1-block EXPORT_REPLY */
    ambre_block_t blk;
    uint8_t raw[AMBRE_BLOCK_SIZE];
    ambre_sample_t smp = { .signal_id = 1, .value = 500 };
    AMBRE_CHECK_OK(ambre_block_build(&blk, AMBRE_REC_DATA, 1, 10, 1000, &smp, sizeof(smp)));
    ambre_block_pack(&blk, raw);
    uint32_t fp = ambre_crc32c(0u, raw, sizeof(raw));

    uint8_t epay[sizeof(ctl_ipc_export_hdr_t) + AMBRE_BLOCK_SIZE];
    ctl_ipc_export_hdr_t eh = { .event_id = 9, .first_block = 0, .block_count = 1, .total_blocks = 1 };
    memcpy(epay, &eh, sizeof(eh));
    memcpy(epay + sizeof(eh), raw, sizeof(raw));
    mock.reply_len = (size_t)ctl_ipc_build(mock.reply, sizeof(mock.reply), CTL_MSG_EXPORT_REPLY,
                                           AMBRE_PEER_JOURNAL, 0u, epay, sizeof(epay));

    ctl_args_t args;
    ctl_args_default(&args);
    args.cmd = CTL_CMD_EXPORT; args.event_id = 9; args.have_event_id = true; args.out_path = out_path;
    FILE *devnull = fopen("/dev/null", "w");
    AMBRE_CHECK_EQ_I64(ctl_run_export(&link, &args, devnull), 0);
    AMBRE_CHECK_EQ_U64(mock.last_flags & CTL_REQ_FLAG_LOG_READ, CTL_REQ_FLAG_LOG_READ);

    /* Cross-check verify against canned VERIFY_REPLY */
    ctl_ipc_verify_body_t vb = { .event_id = 9, .block_count = 1, .fingerprint = fp,
                                 .file_complete = 1, .locked = 1 };
    mock.reply_len = (size_t)ctl_ipc_build(mock.reply, sizeof(mock.reply), CTL_MSG_VERIFY_REPLY,
                                           AMBRE_PEER_JOURNAL, 0u, &vb, sizeof(vb));
    args.cmd = CTL_CMD_VERIFY;
    AMBRE_CHECK_EQ_I64(ctl_run_verify(&link, &args, devnull), 0);
    fclose(devnull);
    (void)unlink(out_path);
}

/* ------------------------------------------------- real socket integration - */

static void test_replay_fake_capture(void)
{
    char sock[128], scen[128];
    ambre_test_tmp_path(sock, sizeof(sock), "cap.sock");
    ambre_test_tmp_path(scen, sizeof(scen), "cap.txt");
    write_test_scenario(scen, 20);

    ambre_port_chan_t *chan = NULL;
    AMBRE_CHECK_OK(ambre_port_chan_open(sock, &chan));
    scenario_file_t sc;
    AMBRE_CHECK_OK(scenario_file_load(scen, &sc));

    pid_t pid = fork();
    if (pid == 0) {
        ambre_port_conn_t *conn = NULL;
        if (ambre_port_conn_open(sock, &conn) != AMBRE_PORT_OK) _exit(10);
        replay_run_cfg_t rcfg;
        replay_run_cfg_default(&rcfg);
        rcfg.rate = 0.0;
        replay_stats_t st;
        int rc = replay_run(conn, &sc, &rcfg, &st);
        ambre_port_conn_close(conn);
        _exit(rc == 0 ? 0 : 11);
    }

    uint8_t msg[AMBRE_IPC_FRAME_MAX], ack[32];
    ctl_ipc_ack_body_t ab = { .event_id = 0, .status = 0, .freeze_us = 100 };
    int alen = ctl_ipc_build(ack, sizeof(ack), (uint8_t)AMBRE_MSG_ACK, AMBRE_PEER_CAPTURE, 0u, &ab, sizeof(ab));
    uint32_t frames = 0, exts = 0;
    /* window 0: 1 frame; window 1: 2 external changes (0->1 at row 10,
     * 1->0 at row 11) then 1 frame = 4 messages in total. */
    for (int i = 0; i < 4; i++) {
        uint32_t peer = 0;
        int got = ambre_port_chan_recv(chan, msg, sizeof(msg), 2000000u, &peer);
        if (got <= 0) break;
        AMBRE_CHECK_OK(ambre_ipc_frame_check(msg, (size_t)got));
        uint8_t t = ctl_ipc_type(msg, (size_t)got);
        if (t == (uint8_t)AMBRE_MSG_FRAME) frames++;
        if (t == (uint8_t)AMBRE_MSG_EXTERNAL) exts++;
        AMBRE_CHECK_OK(ambre_port_chan_reply(chan, peer, ack, (size_t)alen));
    }
    AMBRE_CHECK_EQ_I64(ambre_test_reap(pid), 0);
    AMBRE_CHECK_EQ_U64(frames, 2);
    AMBRE_CHECK_EQ_U64(exts, 2);
    ambre_port_chan_close(chan);
    scenario_file_free(&sc);
    (void)unlink(sock);
    (void)unlink(scen);
}

static void test_replay_kill_after_ms(void)
{
    char sock[128], scen[128];
    ambre_test_tmp_path(sock, sizeof(sock), "kill.sock");
    ambre_test_tmp_path(scen, sizeof(scen), "kill.txt");
    write_test_scenario(scen, 50);

    ambre_port_chan_t *chan = NULL;
    AMBRE_CHECK_OK(ambre_port_chan_open(sock, &chan));
    scenario_file_t sc;
    AMBRE_CHECK_OK(scenario_file_load(scen, &sc));

    pid_t pid = fork();
    if (pid == 0) {
        ambre_port_conn_t *conn = NULL;
        if (ambre_port_conn_open(sock, &conn) != AMBRE_PORT_OK) _exit(20);
        replay_run_cfg_t rcfg;
        replay_run_cfg_default(&rcfg);
        rcfg.rate = 1.0;
        rcfg.kill_after_ms = 5;
        replay_stats_t st;
        (void)replay_run(conn, &sc, &rcfg, &st);
        ambre_port_conn_close(conn);
        _exit(0);
    }

    /* Ack every frame so the producer keeps running until its own deadline;
     * after the SIGKILL the channel notices the closed connection. */
    uint8_t msg[AMBRE_IPC_FRAME_MAX], ack[32];
    ctl_ipc_ack_body_t ab = { .event_id = 0, .status = 0, .freeze_us = 100 };
    int alen = ctl_ipc_build(ack, sizeof(ack), (uint8_t)AMBRE_MSG_ACK,
                             AMBRE_PEER_CAPTURE, 0u, &ab, sizeof(ab));
    for (int i = 0; i < 10; i++) {
        uint32_t peer = 0;
        int got = ambre_port_chan_recv(chan, msg, sizeof(msg), 300000u, &peer);
        if (got <= 0) break;
        (void)ambre_port_chan_reply(chan, peer, ack, (size_t)alen);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    AMBRE_CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    ambre_port_chan_close(chan);
    scenario_file_free(&sc);
    (void)unlink(sock);
    (void)unlink(scen);
}

/* SEC-01: a service tool may read, and may never write a producer input. */
static void test_access_control(void)
{
    AMBRE_CHECK_EQ_I64(ambre_port_access_allowed(AMBRE_PEER_SERVICE, AMBRE_OP_READ_STATUS),
                       AMBRE_PORT_OK);
    AMBRE_CHECK_EQ_I64(ambre_port_access_allowed(AMBRE_PEER_SERVICE, AMBRE_OP_READ_EVENTS),
                       AMBRE_PORT_OK);
    AMBRE_CHECK_EQ_I64(ambre_port_access_allowed(AMBRE_PEER_SERVICE, AMBRE_OP_WRITE_INPUT),
                       AMBRE_PORT_ERR_PERM);
    AMBRE_CHECK_EQ_I64(ambre_port_access_allowed(AMBRE_PEER_SERVICE, AMBRE_OP_RAW_DISK),
                       AMBRE_PORT_ERR_PERM);
    AMBRE_CHECK_EQ_I64(ambre_port_access_allowed(AMBRE_PEER_PRODUCER, AMBRE_OP_WRITE_INPUT),
                       AMBRE_PORT_OK);
    AMBRE_CHECK_EQ_I64(ambre_port_access_allowed(AMBRE_PEER_UNKNOWN, AMBRE_OP_READ_STATUS),
                       AMBRE_PORT_ERR_PERM);
}

static const ambre_test_case_t cases[] = {
    { "access_control_sec01",  test_access_control },
    { "scenario_load",         test_scenario_load },
    { "whitelist",             test_whitelist },
    { "row_to_samples",        test_row_to_samples },
    { "frame_builder_budget",  test_frame_builder_budget },
    { "histogram_and_cli",     test_histogram_and_cli },
    { "container_roundtrip",   test_container_roundtrip },
    { "mock_status_list_nak",  test_mock_status_list_nak },
    { "mock_export_verify",    test_mock_export_and_verify },
    { "replay_fake_capture",   test_replay_fake_capture },
    { "replay_kill_after_ms",  test_replay_kill_after_ms },
};

AMBRE_TEST_MAIN(cases)
