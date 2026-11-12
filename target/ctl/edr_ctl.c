/*
 * edr_ctl.c — Transport seam and output formatting for edrctl.
 *
 * THE ONE MISSING PIECE (reported, not worked around)
 * ---------------------------------------------------
 * target/port/ambre_port.h is frozen and offers no client-side receive:
 * ambre_port_conn_send() waits for an acknowledgement, reads it into a private
 * 32-byte buffer and discards it, so a STATUS_REPLY (84 bytes) is truncated and
 * lost. The service tools therefore need one more function in the portability
 * layer, which maps exactly onto QNX MsgSend/MsgReceive where the reply is part
 * of the same transaction:
 *
 *   int ambre_port_conn_request(ambre_port_conn_t *c, const void *req,
 *                               size_t req_len, void *reply, size_t reply_cap,
 *                               uint64_t timeout_us, uint64_t *latency_ns);
 *
 * Until it lands, this file compiles without it and every exchange reports
 * AMBRE_PORT_ERR_UNSUPPORTED: edrctl then exits 1 with an explicit diagnostic
 * instead of inventing a state. Building with -DAMBRE_PORT_HAS_CONN_REQUEST
 * activates the real binding below — nothing else changes. Everything around
 * the seam (parsing, formatting, container I/O, integrity checks) is exercised
 * by tests/test_edrctl.c through an injected ctl_link_t.
 */

#include "edr_ctl.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- transport -- */

#ifdef AMBRE_PORT_HAS_CONN_REQUEST
/* Prototype of the portability-layer entry point this binding needs. It is
 * declared here because target/port/ambre_port.h is frozen and does not
 * declare it yet; the declaration is identical to the proposed patch, so it
 * stays valid (a compatible redeclaration) once the header gains it. */
int ambre_port_conn_request(ambre_port_conn_t *c, const void *req, size_t req_len,
                            void *reply, size_t reply_cap, uint64_t timeout_us,
                            uint64_t *latency_ns);

static int port_xfer(void *ctx, const void *req, size_t req_len, void *reply,
                     size_t reply_cap, uint32_t timeout_us, size_t *reply_len)
{
    ambre_port_conn_t *conn = (ambre_port_conn_t *)ctx;
    int got = ambre_port_conn_request(conn, req, req_len, reply, reply_cap,
                                      (uint64_t)timeout_us, NULL);

    if (got < 0) {
        return got;
    }
    /* The transport returns a status (0 = EOK), never a byte count: the reply
     * is self-describing, so read its length from the header. */
    *reply_len = ambre_ipc_frame_len(reply);
    if (*reply_len == 0u) {
        return AMBRE_ERR_TORN;
    }
    return AMBRE_PORT_OK;
}
#else
static int port_xfer(void *ctx, const void *req, size_t req_len, void *reply,
                     size_t reply_cap, uint32_t timeout_us, size_t *reply_len)
{
    (void)ctx; (void)req; (void)req_len; (void)reply; (void)reply_cap;
    (void)timeout_us; (void)reply_len;
    return AMBRE_PORT_ERR_UNSUPPORTED;
}
#endif

int ctl_link_open(const char *journal_sock, ctl_link_t *out)
{
    ambre_port_conn_t *conn = NULL;
    int rc;

    if (journal_sock == NULL || out == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    out->ctx = NULL;
    out->xfer = NULL;
    rc = ambre_port_conn_open(journal_sock, &conn);
    if (rc != AMBRE_PORT_OK) {
        return rc;
    }
    out->ctx = conn;
    out->xfer = port_xfer;
    return AMBRE_PORT_OK;
}

void ctl_link_close(ctl_link_t *link)
{
    if (link == NULL || link->ctx == NULL) {
        return;
    }
    ambre_port_conn_close((ambre_port_conn_t *)link->ctx);
    link->ctx = NULL;
    link->xfer = NULL;
}

/* Decode a NAK body when the datagram carries one (SEC-01: the refusal reason
 * is always shown). Returns true when a NAK was decoded. */
static bool decode_nak(const void *reply, size_t received, int32_t *nak_code)
{
    ctl_ipc_nak_body_t nak;

    if (received < AMBRE_IPC_HDR_SIZE + sizeof(nak)) {
        return false;
    }
    memcpy(&nak, ctl_ipc_body(reply), sizeof(nak));
    if (nak_code != NULL) {
        *nak_code = nak.code;
    }
    return true;
}

int ctl_transact(const ctl_link_t *link, uint8_t req_type, const void *body,
                 uint32_t body_len, uint32_t timeout_us,
                 void *reply, size_t reply_cap, size_t *reply_len,
                 int32_t *nak_code)
{
    uint8_t req[AMBRE_IPC_FRAME_MAX];
    size_t received = 0u;
    /* EXP-06: only a read of an event is logged; status and list are not. */
    uint8_t flags = (req_type == (uint8_t)AMBRE_MSG_EXPORT ||
                     req_type == (uint8_t)AMBRE_MSG_VERIFY)
                        ? CTL_REQ_FLAG_LOG_READ : 0u;
    int rc;

    if (nak_code != NULL) {
        *nak_code = 0;
    }
    if (link == NULL || link->xfer == NULL || reply == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    rc = ctl_ipc_build(req, sizeof(req), req_type, AMBRE_PEER_SERVICE,
                       flags, body, body_len);
    if (rc <= 0) {
        return AMBRE_ERR_PARAM;
    }
    rc = link->xfer(link->ctx, req, (size_t)rc, reply, reply_cap,
                    timeout_us, &received);
    if (rc != AMBRE_PORT_OK) {
        return rc;
    }
    rc = ambre_ipc_frame_check(reply, received);
    if (rc != AMBRE_OK) {
        return rc;                       /* SEC-05: malformed, never acted on */
    }
    if (ctl_ipc_type(reply, received) == (uint8_t)AMBRE_MSG_NAK) {
        (void)decode_nak(reply, received, nak_code);
        return AMBRE_PORT_ERR_PERM;
    }
    if (reply_len != NULL) {
        *reply_len = received;
    }
    return AMBRE_PORT_OK;
}

/* ------------------------------------------------------------- vocabularies - */

const char *ctl_trig_reason_str(uint8_t reason)
{
    switch (reason) {
    case AMBRE_TRIG_NONE:        return "none";
    case AMBRE_TRIG_DV_LONG:     return "dv_long";
    case AMBRE_TRIG_DV_LAT:      return "dv_lat";
    case AMBRE_TRIG_RESTRAINT:   return "restraint";
    case AMBRE_TRIG_DV25_LOCK:   return "dv25_lock";
    case AMBRE_TRIG_VRU:         return "vru";
    case AMBRE_TRIG_EMERG_BRAKE: return "emergency_brake";
    case AMBRE_TRIG_AEB:         return "aeb";
    case AMBRE_TRIG_SERVICE:     return "service";
    default:                     return "unknown";
    }
}

const char *ctl_state_str(uint8_t state)
{
    switch (state) {
    case AMBRE_EV_EMPTY:  return "empty";
    case AMBRE_EV_OPEN:   return "open";
    case AMBRE_EV_SEALED: return "sealed";
    default:              return "unknown";
    }
}

const char *ctl_backend_str(uint8_t backend)
{
    switch (backend) {
    case (uint8_t)AMBRE_PORT_BACKEND_POSIX: return "posix";
    case (uint8_t)AMBRE_PORT_BACKEND_QNX:   return "qnx";
    default:                                return "unknown";
    }
}

const char *ctl_health_str(uint8_t health)
{
    switch (health) {
    case 0u:  return "ok";
    case 1u:  return "storage_fault";
    case 2u:  return "memory_full";
    default:  return "unknown";
    }
}

/* ------------------------------------------------------------------ JSON --- */

size_t ctl_json_escape(const char *in, char *out, size_t cap)
{
    size_t n = 0u;

    if (out == NULL || cap == 0u) {
        return 0u;
    }
    for (const char *p = (in != NULL) ? in : ""; *p != '\0' && n + 7u < cap; p++) {
        unsigned char c = (unsigned char)*p;

        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            out[n++] = '\\';
            out[n++] = (c == '\n') ? 'n' : ((c == '\r') ? 'r' : 't');
        } else if (c < 0x20u) {
            n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = '\0';
    return n;
}

/* -------------------------------------------------------------- formatters - */

void ctl_format_status_human(const ambre_ipc_status_body_t *sb, FILE *out)
{
    if (sb == NULL || out == NULL) {
        return;
    }
    fprintf(out, "mode            %s\n", sb->mode ? "continuous" : "event-only");
    fprintf(out, "backend         %s\n", ctl_backend_str(sb->backend));
    fprintf(out, "health          %s\n", ctl_health_str(sb->health));
    fprintf(out, "supervised      %s\n", sb->supervised ? "yes" : "no");
    fprintf(out, "segment         %u (next block %u)\n",
            sb->current_seg, sb->current_block);
    fprintf(out, "events          %u open, %u acquired, %u sealed\n",
            sb->open_events, sb->acquired_events, sb->sealed_events);
    fprintf(out, "refused         %u (no space)\n", sb->refused_no_space);
    fprintf(out, "bytes written   %" PRIu64 "\n", sb->bytes_written);
    fprintf(out, "flushes         %" PRIu64 "\n", sb->flush_count);
    fprintf(out, "uptime          %" PRIu64 " ms\n", sb->uptime_ms);
    fprintf(out, "last freeze     %" PRIu64 " us\n", sb->last_freeze_us);
    fprintf(out, "worst freeze    %" PRIu64 " us\n", sb->worst_freeze_us);
}

void ctl_format_status_json(const ambre_ipc_status_body_t *sb, FILE *out)
{
    if (sb == NULL || out == NULL) {
        return;
    }
    fprintf(out, "{\"mode\":\"%s\",\"backend\":\"%s\",\"health\":\"%s\","
                 "\"supervised\":%s,\"current_seg\":%u,\"current_block\":%u,"
                 "\"open_events\":%u,\"acquired_events\":%u,"
                 "\"sealed_events\":%u,\"refused_no_space\":%u,"
                 "\"bytes_written\":%" PRIu64 ",\"flush_count\":%" PRIu64 ","
                 "\"uptime_ms\":%" PRIu64 ",\"last_freeze_us\":%" PRIu64 ","
                 "\"worst_freeze_us\":%" PRIu64 "}\n",
            sb->mode ? "continuous" : "event-only",
            ctl_backend_str(sb->backend), ctl_health_str(sb->health),
            sb->supervised ? "true" : "false",
            sb->current_seg, sb->current_block, sb->open_events,
            sb->acquired_events, sb->sealed_events, sb->refused_no_space,
            sb->bytes_written, sb->flush_count, sb->uptime_ms,
            sb->last_freeze_us, sb->worst_freeze_us);
}

void ctl_format_event_human(const ambre_ipc_event_line_t *line, uint64_t now_us,
                            FILE *out)
{
    uint64_t age_ms;

    if (line == NULL || out == NULL) {
        return;
    }
    /* VIE-02: the instant is shown relative to now, never as an absolute time. */
    age_ms = (now_us >= line->trig_ts_us) ? (now_us - line->trig_ts_us) / 1000u : 0u;
    fprintf(out, "event %-4u %-6s locked=%-3s complete_file=%-3s "
                 "reason=%-15s age=%" PRIu64 " ms (t+%" PRIu64 " us) "
                 "seg=%u blk=%u seq=%" PRIu64 "\n",
            line->event_id, ctl_state_str(line->state),
            line->locked ? "yes" : "no",
            line->file_complete ? "yes" : "no",
            ctl_trig_reason_str(line->trig_reason),
            age_ms, line->trig_ts_us, line->start_seg, line->start_block,
            line->seq_emerg);
}

void ctl_format_event_json(const ambre_ipc_event_line_t *line, uint64_t now_us,
                           FILE *out)
{
    uint64_t age_ms;

    if (line == NULL || out == NULL) {
        return;
    }
    age_ms = (now_us >= line->trig_ts_us) ? (now_us - line->trig_ts_us) / 1000u : 0u;
    fprintf(out, "{\"event_id\":%u,\"state\":\"%s\",\"locked\":%s,"
                 "\"file_complete\":%s,\"trig_reason\":\"%s\","
                 "\"age_ms\":%" PRIu64 ",\"trig_ts_us\":%" PRIu64 ","
                 "\"start_seg\":%u,\"start_block\":%u,\"seq_emerg\":%" PRIu64
                 "}\n",
            line->event_id, ctl_state_str(line->state),
            line->locked ? "true" : "false",
            line->file_complete ? "true" : "false",
            ctl_trig_reason_str(line->trig_reason),
            age_ms, line->trig_ts_us, line->start_seg, line->start_block,
            line->seq_emerg);
}

void ctl_format_verify_json(const ctl_ipc_verify_body_t *vb, FILE *out)
{
    if (vb == NULL || out == NULL) {
        return;
    }
    fprintf(out, "{\"event_id\":%u,\"block_count\":%u,\"fingerprint\":\"%08x\","
                 "\"file_complete\":%s,\"locked\":%s}\n",
            vb->event_id, vb->block_count, vb->fingerprint,
            vb->file_complete ? "true" : "false", vb->locked ? "true" : "false");
}

void ctl_format_container_json(const ctl_container_report_t *rep, FILE *out)
{
    if (rep == NULL || out == NULL) {
        return;
    }
    fprintf(out, "{\"container\":true,\"event_id\":%u,\"block_count\":%u,"
                 "\"fingerprint\":\"%08x\",\"fingerprint_recomputed\":\"%08x\","
                 "\"fingerprint_match\":%s,\"blocks_checked\":%u,"
                 "\"blocks_bad\":%u,\"first_bad_index\":%" PRId64 ",\"header_ok\":%s}\n",
            rep->info.event_id, rep->info.block_count, rep->info.fingerprint,
            rep->fingerprint_recomputed,
            rep->fingerprint_match ? "true" : "false",
            rep->blocks_checked, rep->blocks_bad,
            /* -1 when no block failed: UINT32_MAX is an internal marker */
            (rep->blocks_bad > 0u) ? (int64_t)rep->first_bad_index : (int64_t)-1,
            rep->header_ok ? "true" : "false");
}

void ctl_args_default(ctl_args_t *args)
{
    if (args == NULL) {
        return;
    }
    memset(args, 0, sizeof(*args));
    args->cmd = CTL_CMD_NONE;
    args->timeout_us = 2000000u;         /* 2 s: a service call is bounded */
}

/* ------------------------------------------------------------------- CLI --- */

void ctl_print_usage(const char *prog, FILE *out)
{
    fprintf(out,
        "Usage: %s --journal <sock> {status|list|export <id> --out <f.mcap>|"
        "verify [id]} [--json]\n"
        "Commands:\n"
        "  status             state of the journal service (EXP-01, OBS-02)\n"
        "  list               one event per line: state, lock, complete file,\n"
        "                     trigger reason, relative instant\n"
        "  export <id>        raw records of one event into a container file\n"
        "                     (--out); MCAP itself is built by edr-archive\n"
        "  verify [id]        integrity of an event: fingerprints, record chain.\n"
        "                     With --out <container>, the exported file is\n"
        "                     checked locally and cross-checked with the journal\n"
        "Options:\n"
        "  --journal <sock>   service socket of edr-journal (required)\n"
        "  --out <file>       export container, or container to verify\n"
        "  --json             machine readable output, one object per line\n"
        "Exit codes: 0 ok, 1 functional failure, 2 usage error.\n",
        prog);
}

static int parse_command_word(const char *arg, ctl_args_t *args)
{
    if (strcmp(arg, "status") == 0) {
        args->cmd = CTL_CMD_STATUS;
    } else if (strcmp(arg, "list") == 0) {
        args->cmd = CTL_CMD_LIST;
    } else if (strcmp(arg, "export") == 0) {
        args->cmd = CTL_CMD_EXPORT;
    } else if (strcmp(arg, "verify") == 0) {
        args->cmd = CTL_CMD_VERIFY;
    } else {
        return -1;
    }
    return 0;
}

static int parse_event_id(const char *arg, ctl_args_t *args)
{
    char *end = NULL;
    unsigned long v = strtoul(arg, &end, 10);

    if (end == arg || *end != '\0') {
        return -1;
    }
    args->event_id = (uint32_t)v;
    args->have_event_id = true;
    return 0;
}

/* A positional word is either the command (first one) or an event id. */
static int parse_positional(const char *arg, ctl_args_t *args)
{
    if (args->cmd == CTL_CMD_NONE) {
        return parse_command_word(arg, args);
    }
    if (args->have_event_id) {
        return -1;                       /* only one event id is accepted */
    }
    return parse_event_id(arg, args);
}

int ctl_parse_args(int argc, char **argv, ctl_args_t *args)
{
    ctl_args_default(args);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            ctl_print_usage(argv[0], stdout);
            return 1;
        }
        if (strcmp(a, "--json") == 0) {
            args->json = true;
        } else if (strcmp(a, "--journal") == 0 && i + 1 < argc) {
            args->journal_sock = argv[++i];
        } else if (strcmp(a, "--out") == 0 && i + 1 < argc) {
            args->out_path = argv[++i];
        } else if (a[0] != '-' && parse_positional(a, args) == 0) {
            continue;
        } else {
            return -1;
        }
    }
    if (args->cmd == CTL_CMD_NONE || args->journal_sock == NULL) {
        return -1;
    }
    if (args->cmd == CTL_CMD_EXPORT &&
        (!args->have_event_id || args->out_path == NULL)) {
        return -1;
    }
    return 0;
}
