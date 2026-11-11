/*
 * edr_journal_svc.c — Service core of edr-journal: lifecycle, access control,
 * message dispatch, text status and the start-up self-test.
 *
 * The per-message handlers live in edr_journal_svc_handlers.c so that both
 * files stay small; they share the helpers declared at the bottom of
 * edr_journal_svc.h (one implementation per logic, no duplication).
 *
 * Requirements served here: REP-01 (recovery before any capture), SEC-01
 * (access control), SEC-02 (no order can disable the recorder), SEC-05
 * (malformed frames are counted and refused), OBS-02 (text status), OBS-04
 * (start-up self-test), GEL-03 (no allocation on the critical path).
 */

#include "edr_journal_svc.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define SVC_TAG "edr-journal"

/* ------------------------------------------------------------- replies --- */

int ambre_edr_build_reply(void *buf, size_t cap, uint8_t msg_type,
                           const void *body, size_t body_len)
{
    size_t off = ambre_edr_reply_reserve(buf, cap, msg_type, (uint32_t)body_len);

    if (off == 0u) {
        return 0;
    }
    if (body_len > 0u && body != NULL) {
        memcpy((uint8_t *)buf + off, body, body_len);
    }
    return (int)(off + body_len);
}

size_t ambre_edr_reply_reserve(void *buf, size_t cap, uint8_t msg_type,
                               uint32_t body_len)
{
    ambre_ipc_hdr_t h;
    size_t total = (size_t)AMBRE_IPC_HDR_SIZE + (size_t)body_len;

    if (buf == NULL || total > cap || total > (size_t)AMBRE_IPC_FRAME_MAX) {
        return 0u;
    }
    h.magic     = AMBRE_IPC_MAGIC;
    h.version   = (uint8_t)AMBRE_IPC_VERSION;
    h.msg_type  = msg_type;
    h.flags     = 0u;
    h.reserved  = 0u;
    h.body_len  = body_len;
    h.reserved2 = 0u;
    memcpy(buf, &h, sizeof(h));
    return (size_t)AMBRE_IPC_HDR_SIZE;
}

uint32_t ambre_edr_announced_peer(const void *msg, size_t len)
{
    ambre_ipc_hdr_t hdr;

    if (msg == NULL || len < (size_t)AMBRE_IPC_HDR_SIZE) {
        return (uint32_t)AMBRE_PEER_UNKNOWN;
    }
    memcpy(&hdr, msg, sizeof(hdr));
    /* The spare byte of the header carries the announced type (PC harness). */
    return (hdr.reserved == 0u) ? (uint32_t)AMBRE_PEER_UNKNOWN
                                : (uint32_t)hdr.reserved;
}

/* ------------------------------------------------------- access control --- */

int ambre_edr_input_allowed(uint32_t peer_type)
{
    /* The capture is the journal's upstream producer: it owns the input path
     * (TRIGGER / CONTINUE / SEAL / GAP). The portability table grants the
     * same right to the producer type. */
    if (peer_type == (uint32_t)AMBRE_PEER_CAPTURE) {
        return AMBRE_PORT_OK;
    }
    return ambre_port_access_allowed(peer_type, AMBRE_OP_WRITE_INPUT);
}

int ambre_edr_read_allowed(uint32_t peer_type, ambre_access_op_t op)
{
    return ambre_port_access_allowed(peer_type, op);
}

int ambre_edr_refuse(ambre_edr_service_t *svc, uint32_t peer_type,
                      uint8_t msg_type, int code,
                      void *reply, size_t reply_cap, size_t *reply_len)
{
    ambre_edr_nak_body_t nak;

    if (reply_len != NULL) {
        *reply_len = 0u;
    }
    if (svc == NULL) {
        return (code != 0) ? code : AMBRE_PORT_ERR_PARAM;
    }
    svc->refused_count++;
    /* SEC-05: a refused frame is counted and logged, it never stops the
     * service, and it never reaches the medium. */
    ambre_port_log(AMBRE_LOG_WARN, SVC_TAG,
                   "SEC-01/05: refused msg 0x%02x from peer type %u (code %d)",
                   (unsigned)msg_type, (unsigned)peer_type, code);

    memset(&nak, 0, sizeof(nak));
    nak.code = (int32_t)code;
    nak.event_id = svc->last_event_id;
    if (reply != NULL && reply_len != NULL) {
        int n = ambre_edr_build_reply(reply, reply_cap, (uint8_t)AMBRE_MSG_NAK,
                                      &nak, sizeof(nak));
        *reply_len = (n > 0) ? (size_t)n : 0u;
    }
    return (code != 0) ? code : AMBRE_PORT_ERR_PARAM;
}

/* ------------------------------------------------------------- opening --- */

/* A partition whose two table copies are unusable AND whose first data block
 * is all zeroes has never been formatted: it must be laid out, not recovered. */
static bool partition_is_virgin(const ambre_disk_t *d)
{
    ambre_alloc_table_t t;
    uint8_t first[AMBRE_BLOCK_SIZE];
    int copy_used = -1;

    if (ambre_table_read_best(d, &t, &copy_used) == AMBRE_OK) {
        return false;
    }
    if (d->read(d->ctx, AMBRE_FIRST_DATA_OFFSET, first, sizeof(first)) != 0) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(first); i++) {
        if (first[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* REP-01: the recovery runs before any new capture is accepted. */
static int open_journal(ambre_edr_service_t *svc)
{
    ambre_journal_t *j = &svc->journal;
    bool virgin = partition_is_virgin(&svc->vt);
    int rc;

    if (virgin) {
        ambre_port_log(AMBRE_LOG_INFO, SVC_TAG,
                       "virgin partition: formatting (session %" PRIu64 ")",
                       svc->cfg.session_id);
        rc = ambre_journal_format(&svc->vt, &svc->cfg);
        if (rc != AMBRE_OK) {
            return AMBRE_PORT_ERR_IO;
        }
        rc = ambre_journal_open(&svc->vt, &svc->cfg, false, j);
        return (rc == AMBRE_OK) ? AMBRE_PORT_OK : AMBRE_PORT_ERR_IO;
    }

    ambre_port_trace(0u, AMBRE_TRACE_RECOVER);
    rc = ambre_journal_open(&svc->vt, &svc->cfg, true, j);
    if (rc != AMBRE_OK) {
        return AMBRE_PORT_ERR_IO;
    }
    ambre_port_log(AMBRE_LOG_INFO, SVC_TAG,
                   "recovery done: %u event(s), %u sealed incomplete, %s",
                   (unsigned)j->last_recovery.events_after,
                   (unsigned)j->last_recovery.sealed_incomplete,
                   j->last_recovery.diag);
    /* The session id belongs to the journal that formatted the partition: the
     * recovery chain scan requires one session per chain, so it is kept. */
    svc->cfg.session_id = j->table.session_id;
    return AMBRE_PORT_OK;
}

int ambre_edr_service_open(ambre_edr_service_t *svc,
                            const ambre_edr_service_cfg_t *cfg)
{
    const ambre_disk_t *vt;
    uint64_t size;
    int rc;

    if (svc == NULL || cfg == NULL || cfg->disk_spec == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    memset(svc, 0, sizeof(*svc));
    svc->cfg = cfg->cfg;
    if (svc->cfg.segment_limit == 0u || svc->cfg.segment_limit > AMBRE_SEGMENT_COUNT) {
        svc->cfg.segment_limit = AMBRE_SEGMENT_COUNT;
    }
    size = (cfg->disk_size != 0u) ? cfg->disk_size : (uint64_t)AMBRE_JOURNAL_BYTES;

    rc = ambre_port_disk_open(cfg->disk_spec, size, &svc->disk);
    if (rc != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG, "cannot open %s (code %d)",
                       cfg->disk_spec, rc);
        return rc;
    }
    vt = ambre_port_disk_vtable(svc->disk);
    if (vt == NULL) {
        ambre_port_disk_close(svc->disk);
        svc->disk = NULL;
        return AMBRE_PORT_ERR_IO;
    }
    /* The engine needs a writable vtable; the port hands out a const one, so
     * the service keeps its own copy (the ctx and the callbacks are shared). */
    svc->vt = *vt;

    rc = open_journal(svc);
    if (rc != AMBRE_PORT_OK) {
        ambre_edr_service_close(svc);
        return rc;
    }

    svc->start_us = ambre_port_now_us();
    svc->health = 0u;
    svc->refused_count = 0u;
    svc->malformed_count = 0u;
    return AMBRE_PORT_OK;
}

void ambre_edr_service_close(ambre_edr_service_t *svc)
{
    if (svc == NULL) {
        return;
    }
    if (svc->disk != NULL) {
        (void)ambre_journal_flush(&svc->journal, ambre_port_now_us());
        ambre_port_disk_close(svc->disk);
        svc->disk = NULL;
    }
}

void ambre_edr_service_set_notify(ambre_edr_service_t *svc,
                                  ambre_edr_notify_fn fn, void *ctx)
{
    if (svc == NULL) {
        return;
    }
    svc->notify = fn;
    svc->notify_ctx = ctx;
}

/* -------------------------------------------------------- maintenance ---- */

int ambre_edr_service_tick(ambre_edr_service_t *svc, uint64_t now_us)
{
    int rc;

    if (svc == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    rc = ambre_journal_tick(&svc->journal, now_us);
    if (rc == AMBRE_ERR_FULL) {
        svc->health = 2u;                /* memory full (STO-08) */
        return AMBRE_PORT_ERR_FULL;
    }
    if (rc != AMBRE_OK) {
        svc->health = 1u;                /* storage fault */
        return AMBRE_PORT_ERR_IO;
    }
    return AMBRE_PORT_OK;
}

bool ambre_edr_service_archive_allowed(const ambre_edr_service_t *svc)
{
    const ambre_journal_stats_t *s;

    if (svc == NULL) {
        return false;
    }
    s = ambre_journal_stats(&svc->journal);
    /* GEL-06: an archive write never competes with an open event. */
    return (s != NULL) ? (s->open_events == 0u) : false;
}

/* ------------------------------------------------------------ text state - */

int ambre_edr_service_status_text(const ambre_edr_service_t *svc,
                                   char *buf, size_t cap)
{
    const ambre_journal_stats_t *s;
    const ambre_alloc_table_t *t;
    static const char *health_name[] = { "ok", "storage-fault", "memory-full" };
    int n;

    if (svc == NULL || buf == NULL || cap == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    s = ambre_journal_stats(&svc->journal);
    t = ambre_journal_table(&svc->journal);
    n = snprintf(buf, cap,
                 "mode=%s health=%s backend=%s supervised=%d "
                 "seg=%u blk=%u events=%u open=%u acquired=%u sealed=%u "
                 "refused_no_space=%u refused_access=%u malformed=%u "
                 "bytes_written=%" PRIu64 " flush_count=%" PRIu64
                 " uptime_ms=%" PRIu64 " last_freeze_us=%" PRIu64
                 " worst_freeze_us=%" PRIu64,
                 svc->cfg.continuous_mode ? "continuous" : "event",
                 health_name[(svc->health < 3u) ? svc->health : 0u],
                 ambre_port_backend_name(),
                 ambre_port_supervised() ? 1 : 0,
                 t->current_seg, t->current_block, t->event_count,
                 s->open_events, s->acquired_events, s->sealed_events,
                 s->refused_no_space, svc->refused_count, svc->malformed_count,
                 s->bytes_written, s->flush_count,
                 (ambre_port_now_us() - svc->start_us) / 1000u,
                 svc->last_freeze_us, svc->worst_freeze_us);
    return (n > 0 && (size_t)n < cap) ? AMBRE_PORT_OK : AMBRE_PORT_ERR_PARAM;
}

/* ------------------------------------------------------- start-up check -- */

/* OBS-04: the configuration must be coherent before the recorder accepts
 * anything. */
static int check_config(const ambre_edr_service_t *svc)
{
    const ambre_journal_cfg_t *c = &svc->cfg;

    if (c->segment_limit == 0u || c->segment_limit > AMBRE_SEGMENT_COUNT) {
        return AMBRE_PORT_ERR_PARAM;
    }
    if (c->session_id == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    if (c->flush_period_us == 0u || c->event_flush_period_us == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    if (c->event_flush_period_us > c->flush_period_us) {
        return AMBRE_PORT_ERR_PARAM;     /* the event cadence is the tighter */
    }
    return AMBRE_PORT_OK;
}

/* Write one test block through the engine, then read it back from the medium
 * and check both CRCs. The block stays in the chain: it is a valid DATA
 * record, which is exactly what the recorder writes anyway. */
static int roundtrip_test_block(ambre_edr_service_t *svc)
{
    ambre_sample_t sample;
    ambre_block_t back;
    uint8_t raw[AMBRE_BLOCK_SIZE];
    ambre_journal_t *j = &svc->journal;
    uint64_t off;
    bool continuous;
    int rc;

    if (j->table.current_block + 1u >= AMBRE_BLOCKS_PER_SEGMENT) {
        return AMBRE_PORT_OK;            /* no room: the medium is not tested */
    }
    memset(&sample, 0, sizeof(sample));
    sample.signal_id = 1u;               /* acceleration X (FORMAT.md §5)    */
    sample.quality = 0u;
    sample.value = 0x5A5A5A5A;

    off = ambre_block_offset(j->table.current_seg, j->table.current_block);
    continuous = j->cfg.continuous_mode;
    j->cfg.continuous_mode = true;       /* the test block must reach the disk */
    rc = ambre_journal_append_data(j, ambre_port_now_us(), &sample, 1u);
    j->cfg.continuous_mode = continuous;
    if (rc != AMBRE_OK) {
        return AMBRE_PORT_ERR_IO;
    }
    (void)ambre_journal_flush(j, ambre_port_now_us());

    if (j->disk->read(j->disk->ctx, off, raw, sizeof(raw)) != 0) {
        return AMBRE_PORT_ERR_IO;
    }
    ambre_block_unpack(raw, &back);
    if (ambre_block_validate(&back) != AMBRE_OK) {
        return AMBRE_PORT_ERR_IO;        /* the medium did not keep the block */
    }
    if (back.hdr.rec_type != (uint8_t)AMBRE_REC_DATA ||
        memcmp(back.payload, &sample, sizeof(sample)) != 0) {
        return AMBRE_PORT_ERR_IO;
    }
    return AMBRE_PORT_OK;
}

int ambre_edr_service_selftest(ambre_edr_service_t *svc)
{
    int rc;

    if (svc == NULL || svc->disk == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    rc = check_config(svc);
    if (rc != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG, "self-test: bad configuration");
        return rc;
    }
    rc = roundtrip_test_block(svc);
    if (rc != AMBRE_PORT_OK) {
        svc->health = 1u;
        ambre_port_log(AMBRE_LOG_ERROR, SVC_TAG,
                       "self-test: test block did not survive the medium");
        return rc;
    }
    ambre_port_log(AMBRE_LOG_INFO, SVC_TAG, "self-test: OK (OBS-04)");
    return AMBRE_PORT_OK;
}

/* ----------------------------------------------------------- dispatch --- */

/* SEC-05: the frame must carry the magic, the version and a body that fits
 * the datagram. Anything else is counted and refused without any effect. */
static int check_frame(const ambre_ipc_hdr_t *hdr, size_t len, size_t *body_len)
{
    if (hdr->magic != AMBRE_IPC_MAGIC || hdr->version != AMBRE_IPC_VERSION) {
        return AMBRE_PORT_ERR_PARAM;
    }
    if ((size_t)hdr->body_len > len - (size_t)AMBRE_IPC_HDR_SIZE) {
        return AMBRE_PORT_ERR_PARAM;
    }
    *body_len = hdr->body_len;
    return AMBRE_PORT_OK;
}

static int dispatch(ambre_edr_service_t *svc, uint32_t peer_type,
                    const ambre_ipc_hdr_t *hdr, const uint8_t *body,
                    size_t body_len, void *reply, size_t reply_cap,
                    size_t *reply_len)
{
    switch ((ambre_ipc_msg_t)hdr->msg_type) {
    case AMBRE_MSG_TRIGGER:
    case AMBRE_MSG_CONTINUE:
    case AMBRE_MSG_SEAL:
    case AMBRE_MSG_GAP:
        return ambre_edr_handle_input(svc, peer_type, hdr->msg_type, body,
                                      body_len, reply, reply_cap, reply_len);
    case AMBRE_MSG_STATUS:
    case AMBRE_MSG_LIST:
    case AMBRE_MSG_EXPORT:
    case AMBRE_MSG_VERIFY:
        return ambre_edr_handle_service(svc, peer_type, hdr->msg_type, body,
                                        body_len, reply, reply_cap, reply_len);
    default:
        /* SEC-02: there is no "stop", no "disable" and no "erase" order. Any
         * other message type is refused and logged; the recorder keeps
         * running and the supervisor restarts it if it is killed. */
        svc->malformed_count++;
        return ambre_edr_refuse(svc, peer_type, hdr->msg_type,
                                AMBRE_PORT_ERR_PERM, reply, reply_cap,
                                reply_len);
    }
}

/* §4.4: notify the archive after sealing; the reply is already encoded. */
static void drain_ready(ambre_edr_service_t *svc)
{
    uint8_t frame[AMBRE_IPC_HDR_SIZE + sizeof(ambre_edr_event_ready_body_t)];

    if (!svc->ready_pending || svc->notify == NULL) {
        return;
    }
    svc->ready_pending = false;
    if (ambre_edr_build_reply(frame, sizeof(frame),
                              (uint8_t)AMBRE_MSG_EVENT_READY,
                              &svc->ready, sizeof(svc->ready)) > 0) {
        (void)svc->notify(svc->notify_ctx, frame, sizeof(frame));
    }
}

int ambre_edr_service_handle(ambre_edr_service_t *svc, uint32_t peer_type,
                              const void *msg, size_t len,
                              void *reply, size_t reply_cap, size_t *reply_len)
{
    ambre_ipc_hdr_t hdr;
    const uint8_t *body;
    size_t body_len = 0u;
    int rc;

    if (reply_len != NULL) {
        *reply_len = 0u;
    }
    if (svc == NULL || msg == NULL || reply == NULL || reply_len == NULL ||
        reply_cap == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    if (len < (size_t)AMBRE_IPC_HDR_SIZE) {
        svc->malformed_count++;
        return ambre_edr_refuse(svc, peer_type, 0u, AMBRE_PORT_ERR_PARAM,
                                reply, reply_cap, reply_len);
    }
    memcpy(&hdr, msg, sizeof(hdr));
    body = (const uint8_t *)msg + AMBRE_IPC_HDR_SIZE;
    rc = check_frame(&hdr, len, &body_len);
    if (rc != AMBRE_PORT_OK) {
        svc->malformed_count++;
        return ambre_edr_refuse(svc, peer_type, hdr.msg_type, rc, reply,
                                reply_cap, reply_len);
    }
    rc = dispatch(svc, peer_type, &hdr, body, body_len, reply, reply_cap,
                  reply_len);
    drain_ready(svc);
    return rc;
}
