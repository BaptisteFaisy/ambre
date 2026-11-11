/*
 * edr_journal_svc_handlers.c — Handlers for input and service messages.
 *
 * Requirements served: GEL-01..GEL-05 (critical path, continuation, seal),
 * GEL-06 (no archive while an event is open), OBS-03 (kernel trace at the 4
 * key milestones: trigger, submit, ack, seal), EXP-01 (status and list),
 * EXP-02 (export and fingerprint), SEC-01 (access control per message),
 * PERF-02/PERF-03 (freeze latency measurement: last_freeze_us/worst_freeze_us).
 */

#include "edr_journal_svc.h"
#include <string.h>

#define SVC_TAG "edr-journal"

static int ack_reply(uint32_t event_id, uint64_t freeze_us,
                     void *reply, size_t reply_cap, size_t *reply_len)
{
    ambre_edr_ack_body_t ack;
    int n;

    memset(&ack, 0, sizeof(ack));
    ack.event_id  = event_id;
    ack.status    = 0u;
    ack.freeze_us = freeze_us;
    n = ambre_edr_build_reply(reply, reply_cap, (uint8_t)AMBRE_MSG_ACK,
                              &ack, sizeof(ack));
    if (n <= 0) {
        return AMBRE_PORT_ERR_FULL;
    }
    *reply_len = (size_t)n;
    return AMBRE_PORT_OK;
}

/* ------------------------------------------------------------- trigger --- */

static int handle_trigger(ambre_edr_service_t *svc, const uint8_t *body,
                          size_t len, void *reply, size_t reply_cap,
                          size_t *reply_len)
{
    ambre_emerg_t emerg;
    uint64_t t0;
    uint64_t freeze_us;
    int rc;

    if (len < sizeof(emerg)) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_TRIGGER, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }
    memcpy(&emerg, body, sizeof(emerg));
    svc->last_event_id = emerg.event_id;

    /* OBS-03 point 1: threshold crossed, trigger arrives. */
    ambre_port_trace(emerg.event_id, AMBRE_TRACE_TRIGGER);
    t0 = ambre_port_now_us();

    /* OBS-03 point 2: emergency write handed to the driver. */
    ambre_port_trace(emerg.event_id, AMBRE_TRACE_WRITE_SUBMIT);

    rc = ambre_journal_trigger(&svc->journal, &emerg);
    if (rc != AMBRE_OK) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_TRIGGER,
                                (rc == AMBRE_ERR_FULL) ? AMBRE_PORT_ERR_FULL
                                                       : AMBRE_PORT_ERR_IO,
                                reply, reply_cap, reply_len);
    }

    /* OBS-03 point 3: flush acknowledged: event is ACQUIRED (GEL-02). */
    ambre_port_trace(emerg.event_id, AMBRE_TRACE_FLUSH_ACK);
    freeze_us = ambre_port_now_us() - t0;
    svc->last_freeze_us = freeze_us;
    if (freeze_us > svc->worst_freeze_us) {
        svc->worst_freeze_us = freeze_us;
    }
    return ack_reply(emerg.event_id, freeze_us, reply, reply_cap, reply_len);
}

/* -------------------------------------------------------- continuation --- */

static int handle_continue(ambre_edr_service_t *svc, const uint8_t *body,
                           size_t len, void *reply, size_t reply_cap,
                           size_t *reply_len)
{
    ambre_cont_t cont;
    int rc;

    if (len < sizeof(cont)) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_CONTINUE, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }
    memcpy(&cont, body, sizeof(cont));
    rc = ambre_journal_continue(&svc->journal, &cont);
    if (rc != AMBRE_OK) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_CONTINUE, AMBRE_PORT_ERR_IO,
                                reply, reply_cap, reply_len);
    }
    return ack_reply(cont.event_id, 0u, reply, reply_cap, reply_len);
}

/* --------------------------------------------------------------- sealing -- */

static void notify_archive(ambre_edr_service_t *svc, const ambre_seal_t *seal)
{
    ambre_edr_event_ready_body_t ready;
    uint8_t msg[AMBRE_IPC_HDR_SIZE + sizeof(ready)];
    int n;

    if (svc->notify == NULL) {
        return;
    }
    memset(&ready, 0, sizeof(ready));
    ready.event_id      = seal->event_id;
    ready.locked        = seal->locked;
    ready.file_complete = seal->file_complete;
    n = ambre_edr_build_reply(msg, sizeof(msg), (uint8_t)AMBRE_MSG_EVENT_READY,
                              &ready, sizeof(ready));
    if (n > 0) {
        (void)svc->notify(svc->notify_ctx, msg, (size_t)n);
    }
}

static int handle_seal(ambre_edr_service_t *svc, const uint8_t *body,
                       size_t len, void *reply, size_t reply_cap,
                       size_t *reply_len)
{
    ambre_seal_t seal;
    int rc;

    if (len < sizeof(seal)) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_SEAL, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }
    memcpy(&seal, body, sizeof(seal));
    rc = ambre_journal_seal(&svc->journal, &seal);
    if (rc != AMBRE_OK) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_SEAL, AMBRE_PORT_ERR_IO,
                                reply, reply_cap, reply_len);
    }

    /* OBS-03 point 4: event sealed. */
    ambre_port_trace(seal.event_id, AMBRE_TRACE_SEAL);
    notify_archive(svc, &seal);
    return ack_reply(seal.event_id, 0u, reply, reply_cap, reply_len);
}

/* ------------------------------------------------------------------ gap --- */

static int handle_gap(ambre_edr_service_t *svc, const uint8_t *body,
                      size_t len, void *reply, size_t reply_cap,
                      size_t *reply_len)
{
    ambre_gap_t gap;
    int rc;

    if (len < sizeof(gap)) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_GAP, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }
    memcpy(&gap, body, sizeof(gap));
    rc = ambre_journal_append_gap(&svc->journal, &gap);
    if (rc != AMBRE_OK) {
        return ambre_edr_refuse(svc, (uint32_t)AMBRE_PEER_CAPTURE,
                                (uint8_t)AMBRE_MSG_GAP, AMBRE_PORT_ERR_IO,
                                reply, reply_cap, reply_len);
    }
    return ack_reply(0u, 0u, reply, reply_cap, reply_len);
}

int ambre_edr_handle_input(ambre_edr_service_t *svc, uint32_t peer_type,
                           uint8_t msg_type, const uint8_t *body,
                           size_t body_len, void *reply, size_t reply_cap,
                           size_t *reply_len)
{
    if (ambre_edr_input_allowed(peer_type) != AMBRE_PORT_OK) {
        return ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_PERM,
                                reply, reply_cap, reply_len);
    }
    switch ((ambre_ipc_msg_t)msg_type) {
    case AMBRE_MSG_TRIGGER:
        return handle_trigger(svc, body, body_len, reply, reply_cap, reply_len);
    case AMBRE_MSG_CONTINUE:
        return handle_continue(svc, body, body_len, reply, reply_cap, reply_len);
    case AMBRE_MSG_SEAL:
        return handle_seal(svc, body, body_len, reply, reply_cap, reply_len);
    case AMBRE_MSG_GAP:
        return handle_gap(svc, body, body_len, reply, reply_cap, reply_len);
    default:
        return ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }
}

/* ------------------------------------------------------------- service --- */

static int handle_status(ambre_edr_service_t *svc, void *reply,
                         size_t reply_cap, size_t *reply_len)
{
    const ambre_journal_stats_t *s = ambre_journal_stats(&svc->journal);
    const ambre_alloc_table_t *t = ambre_journal_table(&svc->journal);
    ambre_ipc_status_body_t stat;
    int n;

    memset(&stat, 0, sizeof(stat));
    stat.mode              = svc->cfg.continuous_mode ? 1u : 0u;
    stat.health            = svc->health;
    stat.backend           = (uint8_t)ambre_port_backend();
    stat.supervised        = ambre_port_supervised() ? 1u : 0u;
    stat.current_seg       = t->current_seg;
    stat.current_block     = t->current_block;
    stat.open_events       = s->open_events;
    stat.acquired_events   = s->acquired_events;
    stat.sealed_events     = s->sealed_events;
    stat.refused_no_space  = s->refused_no_space;
    stat.bytes_written     = s->bytes_written;
    stat.flush_count       = s->flush_count;
    stat.uptime_ms         = (ambre_port_now_us() - svc->start_us) / 1000u;
    stat.last_freeze_us    = svc->last_freeze_us;
    stat.worst_freeze_us   = svc->worst_freeze_us;

    n = ambre_edr_build_reply(reply, reply_cap, (uint8_t)AMBRE_MSG_STATUS_REPLY,
                              &stat, sizeof(stat));
    if (n <= 0) {
        return AMBRE_PORT_ERR_FULL;
    }
    *reply_len = (size_t)n;
    return AMBRE_PORT_OK;
}

static int handle_list(ambre_edr_service_t *svc, void *reply,
                       size_t reply_cap, size_t *reply_len)
{
    const ambre_alloc_table_t *t = ambre_journal_table(&svc->journal);
    ambre_edr_list_body_t list;
    uint32_t count = 0u;
    int n;

    memset(&list, 0, sizeof(list));
    for (uint32_t i = 0u; i < t->event_count && count < AMBRE_MAX_EVENTS; i++) {
        const ambre_ev_desc_t *e = &t->events[i];
        ambre_ipc_event_line_t *line;

        if (e->state == (uint8_t)AMBRE_EV_EMPTY) {
            continue;
        }
        line = &list.lines[count++];
        line->event_id      = e->event_id;
        line->state         = e->state;
        line->locked        = e->locked;
        line->file_complete = e->file_complete;
        line->trig_reason   = e->trig_reason;
        line->trig_ts_us    = e->trig_ts_us;
        line->seq_emerg     = e->seq_emerg;
        line->start_seg     = e->start_seg;
        line->start_block   = e->start_block;
    }
    list.count = count;
    n = ambre_edr_build_reply(reply, reply_cap, (uint8_t)AMBRE_MSG_LIST_REPLY,
                              &list, sizeof(list.count) +
                                     (size_t)count * sizeof(ambre_ipc_event_line_t));
    if (n <= 0) {
        return AMBRE_PORT_ERR_FULL;
    }
    *reply_len = (size_t)n;
    return AMBRE_PORT_OK;
}

static const ambre_ev_desc_t *find_event_or_refuse(ambre_edr_service_t *svc,
                                                   uint32_t event_id,
                                                   uint32_t peer_type,
                                                   uint8_t msg_type,
                                                   void *reply, size_t reply_cap,
                                                   size_t *reply_len)
{
    const ambre_alloc_table_t *t = ambre_journal_table(&svc->journal);
    const ambre_ev_desc_t *e;

    /* GEL-06: no export / archive while an event is still open. */
    if (!ambre_edr_service_archive_allowed(svc)) {
        (void)ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_PERM,
                               reply, reply_cap, reply_len);
        return NULL;
    }
    e = ambre_table_find_event_const(t, event_id);
    if (e == NULL) {
        (void)ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_NOENT,
                               reply, reply_cap, reply_len);
        return NULL;
    }
    return e;
}

static int handle_export(ambre_edr_service_t *svc, uint32_t peer_type,
                         const uint8_t *body, size_t len,
                         void *reply, size_t reply_cap, size_t *reply_len)
{
    const ambre_ev_desc_t *e;
    ambre_edr_export_req_t req;
    ambre_edr_export_body_t out;
    uint32_t total;
    uint32_t blk_idx;
    uint32_t take;
    uint8_t *payload;
    size_t space;
    int n;

    if (len < sizeof(req)) {
        return ambre_edr_refuse(svc, peer_type, (uint8_t)AMBRE_MSG_EXPORT,
                                AMBRE_PORT_ERR_PARAM, reply, reply_cap, reply_len);
    }
    memcpy(&req, body, sizeof(req));
    e = find_event_or_refuse(svc, req.event_id, peer_type,
                             (uint8_t)AMBRE_MSG_EXPORT, reply, reply_cap, reply_len);
    if (e == NULL) {
        return AMBRE_PORT_ERR_NOENT;
    }
    total = (e->end_block >= e->start_block) ?
            (e->end_block - e->start_block + 1u) : 0u;
    blk_idx = (req.first_block < total) ? req.first_block : total;
    if (reply_cap < AMBRE_IPC_HDR_SIZE + sizeof(out)) {
        return AMBRE_PORT_ERR_FULL;
    }
    space = reply_cap - AMBRE_IPC_HDR_SIZE - sizeof(out);
    take = (uint32_t)(space / AMBRE_BLOCK_SIZE);
    if (take > total - blk_idx) {
        take = total - blk_idx;
    }
    memset(&out, 0, sizeof(out));
    out.event_id     = req.event_id;
    out.first_block  = blk_idx;
    out.block_count  = take;
    out.total_blocks = total;

    n = ambre_edr_build_reply(reply, reply_cap, AMBRE_EDR_REPLY_EXPORT,
                              &out, sizeof(out) + (size_t)take * AMBRE_BLOCK_SIZE);
    if (n <= 0) {
        return AMBRE_PORT_ERR_FULL;
    }
    payload = (uint8_t *)reply + AMBRE_IPC_HDR_SIZE + sizeof(out);
    for (uint32_t k = 0u; k < take; k++) {
        uint64_t off = ambre_block_offset(e->start_seg, e->start_block + blk_idx + k);
        if (svc->vt->read(svc->vt->ctx, off, payload + (size_t)k * AMBRE_BLOCK_SIZE,
                          AMBRE_BLOCK_SIZE) != 0) {
            return ambre_edr_refuse(svc, peer_type, (uint8_t)AMBRE_MSG_EXPORT,
                                    AMBRE_PORT_ERR_IO, reply, reply_cap, reply_len);
        }
    }
    *reply_len = (size_t)n;
    return AMBRE_PORT_OK;
}

static int handle_verify(ambre_edr_service_t *svc, uint32_t peer_type,
                         const uint8_t *body, size_t len,
                         void *reply, size_t reply_cap, size_t *reply_len)
{
    const ambre_ev_desc_t *e;
    ambre_edr_verify_req_t req;
    ambre_edr_verify_body_t out;
    uint32_t total;
    uint32_t fp = 0xFFFFFFFFu;
    uint8_t blk[AMBRE_BLOCK_SIZE];
    int n;

    if (len < sizeof(req)) {
        return ambre_edr_refuse(svc, peer_type, (uint8_t)AMBRE_MSG_VERIFY,
                                AMBRE_PORT_ERR_PARAM, reply, reply_cap, reply_len);
    }
    memcpy(&req, body, sizeof(req));
    e = find_event_or_refuse(svc, req.event_id, peer_type,
                             (uint8_t)AMBRE_MSG_VERIFY, reply, reply_cap, reply_len);
    if (e == NULL) {
        return AMBRE_PORT_ERR_NOENT;
    }
    total = (e->end_block >= e->start_block) ?
            (e->end_block - e->start_block + 1u) : 0u;
    for (uint32_t k = 0u; k < total; k++) {
        uint64_t off = ambre_block_offset(e->start_seg, e->start_block + k);
        if (svc->vt->read(svc->vt->ctx, off, blk, sizeof(blk)) != 0) {
            return ambre_edr_refuse(svc, peer_type, (uint8_t)AMBRE_MSG_VERIFY,
                                    AMBRE_PORT_ERR_IO, reply, reply_cap, reply_len);
        }
        fp = ambre_crc32c(fp, blk, sizeof(blk));
    }
    memset(&out, 0, sizeof(out));
    out.event_id      = req.event_id;
    out.block_count   = total;
    out.fingerprint   = fp;
    out.file_complete = e->file_complete;
    out.locked        = e->locked;

    n = ambre_edr_build_reply(reply, reply_cap, AMBRE_EDR_REPLY_VERIFY,
                              &out, sizeof(out));
    if (n <= 0) {
        return AMBRE_PORT_ERR_FULL;
    }
    *reply_len = (size_t)n;
    return AMBRE_PORT_OK;
}

int ambre_edr_handle_service(ambre_edr_service_t *svc, uint32_t peer_type,
                             uint8_t msg_type, const uint8_t *body,
                             size_t body_len, void *reply, size_t reply_cap,
                             size_t *reply_len)
{
    ambre_access_op_t op;

    switch ((ambre_ipc_msg_t)msg_type) {
    case AMBRE_MSG_STATUS:
        op = AMBRE_OP_READ_STATUS;
        break;
    case AMBRE_MSG_LIST:
    case AMBRE_MSG_EXPORT:
    case AMBRE_MSG_VERIFY:
        op = AMBRE_OP_READ_EVENTS;
        break;
    default:
        return ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }

    if (ambre_edr_read_allowed(peer_type, op) != AMBRE_PORT_OK) {
        return ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_PERM,
                                reply, reply_cap, reply_len);
    }

    switch ((ambre_ipc_msg_t)msg_type) {
    case AMBRE_MSG_STATUS:
        return handle_status(svc, reply, reply_cap, reply_len);
    case AMBRE_MSG_LIST:
        return handle_list(svc, reply, reply_cap, reply_len);
    case AMBRE_MSG_EXPORT:
        return handle_export(svc, peer_type, body, body_len, reply, reply_cap,
                             reply_len);
    case AMBRE_MSG_VERIFY:
        return handle_verify(svc, peer_type, body, body_len, reply, reply_cap,
                             reply_len);
    default:
        return ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }
}
