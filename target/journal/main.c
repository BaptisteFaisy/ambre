/*
 * main.c — edr-journal process entry point.
 *
 * Command-line interface (docs/W2-PLAN.md, frozen):
 *   edr-journal --disk <file|/dev/hd0tNNN> --listen <sock>
 *               [--mode continuous|event] [--segment-limit N]
 *               [--session-id N] [--flush-us N] [--event-flush-us N]
 *
 * Exit codes: 0 ok, 1 functional failure, 2 usage error.
 * Priority 62 and memory locked at start-up (QNX-04, spec 4.4).
 * Start-up self-test (OBS-04), logs through ambre_port_log (OBS-01), text
 * state on SIGUSR1 (OBS-02). The recorder has no "stop" order (SEC-02): only
 * a signal from the supervisor ends it, and the supervisor restarts it.
 */

#include "edr_journal_svc.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SVC_TAG          "edr-journal"
#define PRIORITY_JOURNAL 62
#define POLL_TIMEOUT_US  10000u   /* 10 ms: the during-event cadence (GEL-04) */

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_dump_status = 0;

static void on_term(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_usr1(int sig)
{
    (void)sig;
    g_dump_status = 1;
}

/* Peers that announced themselves as the archive: they are the only ones to
 * receive EVENT_READY, which is a pulse on QNX and a datagram here. */
typedef struct {
    ambre_port_chan_t *chan;
    uint32_t peer[AMBRE_EDR_SVC_PEER_SLOTS];
    uint32_t count;
} peer_table_t;

typedef struct {
    const char *disk;
    const char *listen_sock;
    ambre_journal_cfg_t jcfg;
} cli_args_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --disk <file|/dev/hd0tNNN> --listen <sock> "
            "[--mode continuous|event] [--segment-limit N] [--session-id N] "
            "[--flush-us N] [--event-flush-us N]\n", prog);
}

static int parse_opt_value(int *i, int argc, char **argv, const char **out)
{
    if (*i + 1 >= argc) {
        return 2;
    }
    (*i)++;
    *out = argv[*i];
    return 0;
}

static int parse_args(int argc, char **argv, cli_args_t *args)
{
    memset(args, 0, sizeof(*args));
    ambre_journal_cfg_default(&args->jcfg);

    for (int i = 1; i < argc; i++) {
        const char *val = NULL;
        const char *opt = argv[i];
        int rc;

        if (strcmp(opt, "--disk") == 0) {
            rc = parse_opt_value(&i, argc, argv, &args->disk);
        } else if (strcmp(opt, "--listen") == 0) {
            rc = parse_opt_value(&i, argc, argv, &args->listen_sock);
        } else if (strcmp(opt, "--mode") == 0) {
            rc = parse_opt_value(&i, argc, argv, &val);
            if (rc == 0 && strcmp(val, "continuous") == 0) {
                args->jcfg.continuous_mode = true;
            } else if (rc == 0 && strcmp(val, "event") == 0) {
                args->jcfg.continuous_mode = false;
            } else {
                rc = 2;
            }
        } else if (strcmp(opt, "--segment-limit") == 0) {
            rc = parse_opt_value(&i, argc, argv, &val);
            if (rc == 0) {
                args->jcfg.segment_limit = (uint32_t)strtoul(val, NULL, 10);
            }
        } else if (strcmp(opt, "--session-id") == 0) {
            rc = parse_opt_value(&i, argc, argv, &val);
            if (rc == 0) {
                args->jcfg.session_id = (uint64_t)strtoull(val, NULL, 10);
            }
        } else if (strcmp(opt, "--flush-us") == 0) {
            rc = parse_opt_value(&i, argc, argv, &val);
            if (rc == 0) {
                args->jcfg.flush_period_us = (uint32_t)strtoul(val, NULL, 10);
            }
        } else if (strcmp(opt, "--event-flush-us") == 0) {
            rc = parse_opt_value(&i, argc, argv, &val);
            if (rc == 0) {
                args->jcfg.event_flush_period_us = (uint32_t)strtoul(val, NULL, 10);
            }
        } else {
            rc = 2;
        }
        if (rc != 0) {
            return rc;
        }
    }
    return (args->disk != NULL && args->listen_sock != NULL) ? 0 : 2;
}

/* PC harness: the POSIX transport hands out anonymous peer ids, so a sender
 * announces its type in the header. When it does not, the type is inferred
 * from the request class. On QNX the kernel-provided type wins (image/ambre.sec)
 * and no inference ever happens. */
static uint32_t resolve_peer_type(const uint8_t *msg, size_t len)
{
    uint32_t announced = ambre_edr_announced_peer(msg, len);
    ambre_ipc_hdr_t hdr;

    if (announced != (uint32_t)AMBRE_PEER_UNKNOWN) {
        return announced;
    }
    if (len < (size_t)AMBRE_IPC_HDR_SIZE) {
        return (uint32_t)AMBRE_PEER_UNKNOWN;
    }
    memcpy(&hdr, msg, sizeof(hdr));
    switch ((ambre_ipc_msg_t)hdr.msg_type) {
    case AMBRE_MSG_TRIGGER:
    case AMBRE_MSG_CONTINUE:
    case AMBRE_MSG_SEAL:
    case AMBRE_MSG_GAP:
        return (uint32_t)AMBRE_PEER_CAPTURE;
    case AMBRE_MSG_STATUS:
    case AMBRE_MSG_LIST:
    case AMBRE_MSG_EXPORT:
    case AMBRE_MSG_VERIFY:
        return (uint32_t)AMBRE_PEER_SERVICE;
    default:
        return (uint32_t)AMBRE_PEER_UNKNOWN;
    }
}

static void remember_archive(peer_table_t *pt, uint32_t peer, uint32_t type)
{
    if (type != (uint32_t)AMBRE_PEER_ARCHIVE || peer == 0u) {
        return;
    }
    for (uint32_t i = 0u; i < pt->count; i++) {
        if (pt->peer[i] == peer) {
            return;
        }
    }
    if (pt->count < AMBRE_EDR_SVC_PEER_SLOTS) {
        pt->peer[pt->count++] = peer;
    }
}

static int notify_archive(void *ctx, const void *msg, size_t len)
{
    peer_table_t *pt = (peer_table_t *)ctx;
    int sent = 0;

    for (uint32_t i = 0u; i < pt->count; i++) {
        if (ambre_port_chan_reply(pt->chan, pt->peer[i], msg, len) == AMBRE_PORT_OK) {
            sent++;
        }
    }
    if (sent == 0) {
        ambre_port_log(AMBRE_LOG_DEBUG, SVC_TAG, "EVENT_READY: no archive connected");
        return AMBRE_PORT_ERR_NOENT;
    }
    return AMBRE_PORT_OK;
}

static void serve_one(ambre_edr_service_t *svc, peer_table_t *pt,
                      uint8_t *msg, size_t len, uint32_t peer)
{
    uint8_t reply[AMBRE_EDR_SVC_REPLY_MAX];
    uint32_t type = resolve_peer_type(msg, len);
    size_t reply_len = 0u;

    remember_archive(pt, peer, type);
    (void)ambre_edr_service_handle(svc, type, msg, len, reply, sizeof(reply),
                                   &reply_len);
    if (reply_len > 0u) {
        (void)ambre_port_chan_reply(pt->chan, peer, reply, reply_len);
    }
}

static void dump_status(const ambre_edr_service_t *svc)
{
    char text[AMBRE_EDR_SVC_TEXT_MAX];

    if (ambre_edr_service_status_text(svc, text, sizeof(text)) == AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_INFO, SVC_TAG, "status: %s", text);
    }
}

static int run_loop(ambre_edr_service_t *svc, peer_table_t *pt)
{
    uint8_t msg[AMBRE_IPC_FRAME_MAX];

    while (g_running) {
        uint32_t peer = 0u;
        int got = ambre_port_chan_recv(pt->chan, msg, sizeof(msg),
                                       POLL_TIMEOUT_US, &peer);

        if (got > 0) {
            serve_one(svc, pt, msg, (size_t)got, peer);
        } else if (got != AMBRE_PORT_ERR_TIMEOUT) {
            ambre_port_log(AMBRE_LOG_WARN, SVC_TAG, "channel error %d", got);
        }
        if (g_dump_status) {
            g_dump_status = 0;
            dump_status(svc);
        }
        (void)ambre_edr_service_tick(svc, ambre_port_now_us());
    }
    return 0;
}

int main(int argc, char **argv)
{
    cli_args_t args;
    ambre_edr_service_cfg_t scfg;
    ambre_edr_service_t svc;
    peer_table_t peers;
    struct sigaction sa;
    int rc;

    rc = parse_args(argc, argv, &args);
    if (rc != 0) {
        usage(argv[0]);
        return 2;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_usr1;
    (void)sigaction(SIGUSR1, &sa, NULL);

    /* QNX-04: memory locked and priority raised before anything is served. */
    if (ambre_port_lock_memory() != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, SVC_TAG, "memory not locked (reported)");
    }
    if (ambre_port_set_priority(PRIORITY_JOURNAL) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, SVC_TAG, "priority %d not granted",
                       PRIORITY_JOURNAL);
    }

    memset(&scfg, 0, sizeof(scfg));
    scfg.disk_spec = args.disk;
    scfg.cfg = args.jcfg;

    rc = ambre_edr_service_open(&svc, &scfg);
    if (rc != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG, "cannot open %s (code %d)",
                       args.disk, rc);
        return 1;
    }
    if (ambre_edr_service_selftest(&svc) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG, "start-up self-test failed");
        ambre_edr_service_close(&svc);
        return 1;
    }

    memset(&peers, 0, sizeof(peers));
    rc = ambre_port_chan_open(args.listen_sock, &peers.chan);
    if (rc != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG, "cannot listen on %s (code %d)",
                       args.listen_sock, rc);
        ambre_edr_service_close(&svc);
        return 1;
    }
    ambre_edr_service_set_notify(&svc, notify_archive, &peers);

    ambre_port_log(AMBRE_LOG_INFO, SVC_TAG, "serving %s (backend %s)",
                   args.listen_sock, ambre_port_backend_name());
    dump_status(&svc);

    rc = run_loop(&svc, &peers);

    dump_status(&svc);
    ambre_port_chan_close(peers.chan);
    ambre_edr_service_close(&svc);
    return rc;
}
