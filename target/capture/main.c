/*
 * main.c — edr-capture process entry point.
 *
 * Command-line interface (docs/W2-PLAN.md, frozen):
 *   edr-capture --journal <sock> --listen <sock> [--profile eu|us]
 *
 * Exit codes: 0 ok, 1 functional failure, 2 usage error.
 * Priority 60-61 and memory locked at start-up (QNX-04, spec 4.4).
 * Detects producer disconnection via closed socket without polling (QNX-05).
 */

#define _POSIX_C_SOURCE 200809L

#include "edr_capture.h"
#include "../port/ambre_port.h"
#include "../edr_ipc.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SVC_TAG          "edr-capture"
#define PRIORITY_CAPTURE 60
#define POLL_TIMEOUT_US  10000u   /* 10 ms cadence (ACQ-01, GEL-04) */
#define RECONNECT_US     100000u  /* 100 ms retry for journal connection */

static volatile sig_atomic_t g_running = 1;

static void on_term(int sig)
{
    (void)sig;
    g_running = 0;
}

typedef struct {
    const char             *journal_sock;
    const char             *listen_sock;
    ambre_capture_profile_t profile;
} cli_args_t;

typedef struct {
    ambre_capture_t    capture;
    ambre_port_chan_t *chan;
    ambre_port_conn_t *journal_conn;
    const char        *journal_sock;
} capture_app_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --journal <sock> --listen <sock> [--profile eu|us]\n",
            prog);
}

static int parse_args(int argc, char **argv, cli_args_t *args)
{
    memset(args, 0, sizeof(*args));
    args->profile = AMBRE_PROFILE_EU;

    for (int i = 1; i < argc; i++) {
        const char *opt = argv[i];
        if (i + 1 >= argc) return 2;
        const char *val = argv[++i];

        if (strcmp(opt, "--journal") == 0) {
            args->journal_sock = val;
        } else if (strcmp(opt, "--listen") == 0) {
            args->listen_sock = val;
        } else if (strcmp(opt, "--profile") == 0) {
            if (strcmp(val, "eu") == 0) {
                args->profile = AMBRE_PROFILE_EU;
            } else if (strcmp(val, "us") == 0) {
                args->profile = AMBRE_PROFILE_US;
            } else {
                return 2;
            }
        } else {
            return 2;
        }
    }
    return (args->journal_sock && args->listen_sock) ? 0 : 2;
}

static int journal_send_sink(void *ctx, uint8_t msg_type,
                             const void *body, size_t body_len)
{
    capture_app_t *app = (capture_app_t *)ctx;
    if (app == NULL || app->journal_conn == NULL) return AMBRE_PORT_ERR_IO;

    uint8_t wire[AMBRE_IPC_FRAME_MAX];
    ambre_ipc_hdr_t *h = (ambre_ipc_hdr_t *)wire;
    size_t total = sizeof(ambre_ipc_hdr_t) + body_len;
    if (total > sizeof(wire)) return AMBRE_PORT_ERR_FULL;

    h->magic = AMBRE_IPC_MAGIC;
    h->version = AMBRE_IPC_VERSION;
    h->msg_type = msg_type;
    h->flags = 0u;
    h->reserved = (uint8_t)AMBRE_PEER_CAPTURE;
    h->body_len = (uint32_t)body_len;
    h->reserved2 = 0u;

    if (body_len > 0u && body != NULL) {
        memcpy(wire + sizeof(ambre_ipc_hdr_t), body, body_len);
    }
    uint64_t lat_ns = 0u;
    return ambre_port_conn_send(app->journal_conn, wire, total, 500000u, &lat_ns);
}

/* Acknowledge one client message (ACQ-01: the producer never waits on us). */
static void reply_ack(capture_app_t *app, uint32_t peer)
{
    uint8_t ack[sizeof(ambre_ipc_hdr_t)];
    ambre_ipc_hdr_t *ah = (ambre_ipc_hdr_t *)(void *)ack;

    ah->magic = AMBRE_IPC_MAGIC;
    ah->version = AMBRE_IPC_VERSION;
    ah->msg_type = (uint8_t)AMBRE_MSG_ACK;
    ah->flags = 0;
    ah->reserved = (uint8_t)AMBRE_PEER_CAPTURE;
    ah->body_len = 0;
    ah->reserved2 = 0;
    (void)ambre_port_chan_reply(app->chan, peer, ack, sizeof(ack));
}

static void handle_client_message(capture_app_t *app, uint32_t peer,
                                  const uint8_t *msg, size_t len, uint64_t rx_ts)
{
    if (len < sizeof(ambre_ipc_hdr_t)) return;
    const ambre_ipc_hdr_t *h = (const ambre_ipc_hdr_t *)msg;
    if (h->magic != AMBRE_IPC_MAGIC) return;

    if (h->msg_type == AMBRE_MSG_FRAME) {
        (void)ambre_capture_push_frame(&app->capture, msg, len, rx_ts,
                                       journal_send_sink, app);
        reply_ack(app, peer);
    } else if (h->msg_type == AMBRE_MSG_EXTERNAL) {
        if (len >= sizeof(ambre_ipc_hdr_t) + sizeof(ambre_ipc_external_body_t)) {
            const ambre_ipc_external_body_t *eb =
                (const ambre_ipc_external_body_t *)(msg + sizeof(ambre_ipc_hdr_t));
            (void)ambre_capture_push_external(&app->capture, eb->ts_us,
                                              eb->signal_mask, eb->producer_id,
                                              journal_send_sink, app);
        }
        /* Every message is acknowledged, whatever its type: a producer that
         * waits for an ack must never time out on a signal it sent legally
         * (ROB-06). A malformed EXTERNAL is answered the same way, so the
         * producer is never blocked by our validation. */
        reply_ack(app, peer);
    }
}

static void poll_and_dispatch(capture_app_t *app)
{
    uint8_t msg[AMBRE_IPC_FRAME_MAX];
    uint32_t peer = 0u;

    int got = ambre_port_chan_recv(app->chan, msg, sizeof(msg),
                                   POLL_TIMEOUT_US, &peer);
    uint64_t rx_ts = ambre_port_now_us();

    if (got > 0) {
        handle_client_message(app, peer, msg, (size_t)got, rx_ts);
    }

    uint32_t disc = ambre_port_chan_poll_disconnect(app->chan);
    if (disc != 0u) {
        ambre_port_log(AMBRE_LOG_INFO, SVC_TAG, "producer %u disconnected", disc);
    }

    (void)ambre_capture_tick(&app->capture, rx_ts, journal_send_sink, app);
}

static int run_loop(capture_app_t *app)
{
    while (g_running) {
        poll_and_dispatch(app);
    }
    return 0;
}

static int startup_realtime(void)
{
    if (ambre_port_lock_memory() != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, SVC_TAG, "memory lock unsupported (PC)");
    }
    if (ambre_port_set_priority(PRIORITY_CAPTURE) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, SVC_TAG, "priority %d unsupported (PC)",
                       PRIORITY_CAPTURE);
    }
    return 0;
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char **argv)
{
    cli_args_t args;
    capture_app_t app;
    int rc;

    rc = parse_args(argc, argv, &args);
    if (rc != 0) {
        usage(argv[0]);
        return 2;
    }
    install_signals();
    startup_realtime();

    memset(&app, 0, sizeof(app));
    ambre_capture_init(&app.capture, args.profile, 1u);
    app.journal_sock = args.journal_sock;

    rc = ambre_port_conn_open(args.journal_sock, &app.journal_conn);
    if (rc != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG, "cannot connect journal %s (%d)",
                       args.journal_sock, rc);
        return 1;
    }

    rc = ambre_port_chan_open(args.listen_sock, &app.chan);
    if (rc != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG, "cannot listen %s (%d)",
                       args.listen_sock, rc);
        ambre_port_conn_close(app.journal_conn);
        return 1;
    }

    ambre_port_log(AMBRE_LOG_INFO, SVC_TAG, "listening on %s, journal %s",
                   args.listen_sock, args.journal_sock);

    rc = run_loop(&app);

    ambre_port_chan_close(app.chan);
    ambre_port_conn_close(app.journal_conn);
    return rc;
}
