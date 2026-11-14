/*
 * edr_journal_svc_handlers.c — Message handlers: input path (GEL-01..GEL-05,
 * OBS-03 trace, PERF-02/03 freeze latency) and service path (EXP-01 status
 * and list, EXP-02 export and fingerprint, GEL-06 archive gate, SEC-01).
 */

#include "edr_journal_svc.h"
#include "edr_journal_bodies.h"
#include <string.h>

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

/* Queue the archive notification (§4.4): ambre_edr_service_handle() drains it
 * through the notify sink once the SEAL reply is encoded, so a client never
 * sees a notification before its own answer. */
static void queue_ready(ambre_edr_service_t *svc, const ambre_seal_t *seal)
{
    svc->ready.event_id      = seal->event_id;
    svc->ready.locked        = seal->locked;
    svc->ready.file_complete = seal->file_complete;
    svc->ready_pending       = true;
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
    queue_ready(svc, &seal);
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
    ambre_ipc_status_body_t stat;
    int n;

    /* Le corps est rempli par edr_journal_bodies.c : le devctl du resource
     * manager rend exactement les mêmes octets que cette réponse IPC. */
    ambre_edr_fill_status(svc, &stat);

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
    ambre_edr_list_body_t list;
    uint32_t count;
    int n;

    memset(&list, 0, sizeof(list));
    count = ambre_edr_list_events(svc, list.lines, AMBRE_MAX_EVENTS);
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
    const ambre_ev_desc_t *e;

    /* GEL-06: no export / archive while an event is still open. */
    if (!ambre_edr_service_archive_allowed(svc)) {
        (void)ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_PERM,
                               reply, reply_cap, reply_len);
        return NULL;
    }
    e = ambre_edr_find_event(svc, event_id);
    if (e == NULL) {
        (void)ambre_edr_refuse(svc, peer_type, msg_type, AMBRE_PORT_ERR_NOENT,
                               reply, reply_cap, reply_len);
        return NULL;
    }
    return e;
}

static int read_export_blocks(ambre_edr_service_t *svc, const ambre_ev_desc_t *e,
                              uint32_t first_blk, uint32_t take,
                              uint8_t *dst, size_t hdr_off)
{
    uint8_t *payload = dst + hdr_off + sizeof(ambre_edr_export_body_t);

    for (uint32_t k = 0u; k < take; k++) {
        if (ambre_edr_read_event_block(svc, e, first_blk + k,
                             payload + (size_t)k * AMBRE_BLOCK_SIZE)
                != AMBRE_PORT_OK) {
            return AMBRE_PORT_ERR_IO;
        }
    }
    return AMBRE_PORT_OK;
}

/* How many raw blocks of an event fit in one reply, given the reply ceiling
 * and the pagination offset. */
static uint32_t export_take(const ambre_ev_desc_t *e, uint32_t first_blk,
                            size_t reply_cap)
{
    uint32_t total = ambre_edr_event_blocks(e);
    size_t max_body = (reply_cap < (size_t)AMBRE_IPC_FRAME_MAX) ?
                      reply_cap : (size_t)AMBRE_IPC_FRAME_MAX;
    size_t room;
    uint32_t take;

    if (max_body < (size_t)AMBRE_IPC_HDR_SIZE + sizeof(ambre_edr_export_body_t)) {
        return 0u;
    }
    room = max_body - (size_t)AMBRE_IPC_HDR_SIZE - sizeof(ambre_edr_export_body_t);
    take = (uint32_t)(room / AMBRE_BLOCK_SIZE);
    return (take > total - first_blk) ? (total - first_blk) : take;
}

static int handle_export(ambre_edr_service_t *svc, uint32_t peer_type,
                         const uint8_t *body, size_t len,
                         void *reply, size_t reply_cap, size_t *reply_len)
{
    const ambre_ev_desc_t *e;
    ambre_edr_export_req_t req;
    ambre_edr_export_body_t out;
    uint32_t total, blk_idx, take;
    size_t hdr_off;

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
    total = ambre_edr_event_blocks(e);
    blk_idx = (req.first_block < total) ? req.first_block : total;
    take = export_take(e, blk_idx, reply_cap);
    memset(&out, 0, sizeof(out));
    out.event_id = req.event_id;
    out.first_block = blk_idx;
    out.block_count = take;
    out.total_blocks = total;
    hdr_off = ambre_edr_reply_reserve(reply, reply_cap, AMBRE_EDR_REPLY_EXPORT,
        (uint32_t)(sizeof(out) + (size_t)take * AMBRE_BLOCK_SIZE));
    if (hdr_off == 0u) {
        return AMBRE_PORT_ERR_FULL;
    }
    memcpy((uint8_t *)reply + hdr_off, &out, sizeof(out));
    if (read_export_blocks(svc, e, blk_idx, take, (uint8_t *)reply, hdr_off)
        != AMBRE_PORT_OK) {
        return ambre_edr_refuse(svc, peer_type, (uint8_t)AMBRE_MSG_EXPORT,
                                AMBRE_PORT_ERR_IO, reply, reply_cap, reply_len);
    }
    *reply_len = hdr_off + sizeof(out) + (size_t)take * AMBRE_BLOCK_SIZE;
    return AMBRE_PORT_OK;
}

static int handle_verify(ambre_edr_service_t *svc, uint32_t peer_type,
                         const uint8_t *body, size_t len,
                         void *reply, size_t reply_cap, size_t *reply_len)
{
    const ambre_ev_desc_t *e;
    ambre_edr_verify_req_t req;
    ambre_edr_verify_body_t out;
    uint8_t blk[AMBRE_BLOCK_SIZE];
    uint32_t total;
    uint32_t fp = 0u;
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
    total = ambre_edr_event_blocks(e);
    for (uint32_t k = 0u; k < total; k++) {
        if (ambre_edr_read_event_block(svc, e, k, blk) != AMBRE_PORT_OK) {
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

/* SEC-01: the read operation a service message performs, -1 when unknown. */
static int service_op(uint8_t msg_type)
{
    switch ((ambre_ipc_msg_t)msg_type) {
    case AMBRE_MSG_STATUS: return (int)AMBRE_OP_READ_STATUS;
    case AMBRE_MSG_LIST:
    case AMBRE_MSG_EXPORT:
    case AMBRE_MSG_VERIFY: return (int)AMBRE_OP_READ_EVENTS;
    default:               return -1;
    }
}

int ambre_edr_handle_service(ambre_edr_service_t *svc, uint32_t peer_type,
                             uint8_t msg_type, const uint8_t *body,
                             size_t body_len, void *reply, size_t reply_cap,
                             size_t *reply_len)
{
    int op = service_op(msg_type);

    if (op < 0 ||
        ambre_edr_read_allowed(peer_type, (ambre_access_op_t)op) != AMBRE_PORT_OK) {
        return ambre_edr_refuse(svc, peer_type, msg_type,
                                (op < 0) ? AMBRE_PORT_ERR_PARAM
                                         : AMBRE_PORT_ERR_PERM,
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
    default:
        return handle_verify(svc, peer_type, body, body_len, reply, reply_cap,
                             reply_len);
    }
}
