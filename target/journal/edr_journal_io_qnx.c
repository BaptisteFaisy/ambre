/*
 * edr_journal_io_qnx.c — the service handlers of /dev/edr on QNX 8.0.
 *
 * read() answers the text status of the journal (OBS-02) and the raw blocks of
 * one event (EXP-01, EXP-08); devctl() answers DCMD_EDR_STATUS / LIST / EXPORT
 * / AUTOTEST. Every request runs in the receiving pool thread at the CALLER'S
 * priority (QNX-03), and every one of them goes through edr_gate() first
 * (SEC-01). The lifecycle half — attach, thread pool, open — lives in
 * edr_journal_qnx.c; the shared shape is edr_journal_qnx_internal.h.
 *
 * Compiled with the real QNX SDP 8.0 (qcc 12.2.0, gcc_ntox86_64).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "edr_journal_qnx_internal.h"

/* ------------------------------------------------------- access control --- */

/* QNX-only: verified on target, syntax-checked on PC */
static uint32_t edr_peer_type_of(resmgr_context_t *ctp)
{
    struct _client_info info;

    memset(&info, 0, sizeof(info));
    if (iofunc_client_info(ctp, 0, &info) != EOK) {
        return (uint32_t)AMBRE_PEER_UNKNOWN;
    }
    switch (info.cred.egid) {
    case 1u: return (uint32_t)AMBRE_PEER_JOURNAL;
    case 2u: return (uint32_t)AMBRE_PEER_CAPTURE;
    case 3u: return (uint32_t)AMBRE_PEER_PRODUCER;
    case 4u: return (uint32_t)AMBRE_PEER_SERVICE;
    case 5u: return (uint32_t)AMBRE_PEER_ARCHIVE;
    default: return (uint32_t)AMBRE_PEER_UNKNOWN;
    }
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_gate(resmgr_context_t *ctp, ambre_access_op_t op, uint8_t msg_type)
{
    uint32_t type = edr_peer_type_of(ctp);

    if (ambre_port_access_allowed(type, op) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, "edr-journal",
                       "SEC-01: refused msg 0x%02x from peer type %u",
                       (unsigned)msg_type, (unsigned)type);
        return EACCES;
    }
    return EOK;
}

/* ------------------------------------------------------------ status text - */

/* QNX-only: verified on target, syntax-checked on PC */
static int build_status_text(char *out, size_t cap)
{
    ambre_ipc_status_body_t st;

    memset(&st, 0, sizeof(st));
    if (g_edr_jrn.src.status != NULL) {
        (void)g_edr_jrn.src.status(g_edr_jrn.src.ctx, &st);
    }
    st.backend = (uint8_t)ambre_port_backend();
    st.supervised = ambre_port_supervised() ? 1u : 0u;
    return snprintf(out, cap,
                    "mode=%s health=%u backend=%s supervised=%u\n"
                    "segment=%u block=%u open=%u acquired=%u sealed=%u\n"
                    "refused_no_space=%u bytes_written=%llu flush_count=%llu\n"
                    "uptime_ms=%llu last_freeze_us=%llu worst_freeze_us=%llu\n",
                    st.mode ? "continuous" : "event-only", (unsigned)st.health,
                    ambre_port_backend_name(), (unsigned)st.supervised,
                    (unsigned)st.current_seg, (unsigned)st.current_block,
                    (unsigned)st.open_events, (unsigned)st.acquired_events,
                    (unsigned)st.sealed_events, (unsigned)st.refused_no_space,
                    (unsigned long long)st.bytes_written,
                    (unsigned long long)st.flush_count,
                    (unsigned long long)st.uptime_ms,
                    (unsigned long long)st.last_freeze_us,
                    (unsigned long long)st.worst_freeze_us);
}

/* ------------------------------------------------------------- devctls ---- */

/* QNX-only: verified on target, syntax-checked on PC */
static int devctl_status(resmgr_context_t *ctp, io_devctl_t *msg)
{
    ambre_ipc_status_body_t st;

    memset(&st, 0, sizeof(st));
    if (g_edr_jrn.src.status != NULL && g_edr_jrn.src.status(g_edr_jrn.src.ctx, &st) != 0) {
        return EIO;
    }
    st.backend = (uint8_t)ambre_port_backend();
    st.supervised = ambre_port_supervised() ? 1u : 0u;
    msg->o.nbytes = (int32_t)sizeof(st);
    return _RESMGR_PTR(ctp, &st, sizeof(st));
}

/* QNX-only: verified on target, syntax-checked on PC */
static int devctl_list(resmgr_context_t *ctp, io_devctl_t *msg)
{
    static ambre_ipc_event_line_t lines[EDR_MAX_EVENTS];
    int got;
    size_t bytes;

    if (g_edr_jrn.src.list == NULL) {
        return ENOSYS;
    }
    got = g_edr_jrn.src.list(g_edr_jrn.src.ctx, lines, EDR_MAX_EVENTS);
    if (got < 0) {
        return EIO;
    }
    bytes = (size_t)got * sizeof(lines[0]);
    msg->o.nbytes = (int32_t)bytes;
    return _RESMGR_PTR(ctp, lines, bytes);
}

/* QNX-only: verified on target, syntax-checked on PC */
static int devctl_export(resmgr_context_t *ctp, io_devctl_t *msg)
{
    static uint8_t blocks[7u * AMBRE_BLOCK_SIZE];
    uint32_t req[2];
    uint32_t count = (uint32_t)(sizeof(blocks) / AMBRE_BLOCK_SIZE);

    if (msg->i.nbytes < sizeof(req)) {
        return EINVAL;
    }
    if (resmgr_msgread(ctp, req, sizeof(req), sizeof(io_devctl_t))
        != (ssize_t)sizeof(req)) {
        return EFAULT;
    }
    if (g_edr_jrn.src.event_blocks == NULL
        || g_edr_jrn.src.event_blocks(g_edr_jrn.src.ctx, req[0], req[1], count,
                                  blocks) != 0) {
        return EIO;
    }
    msg->o.nbytes = (int32_t)sizeof(blocks);
    return _RESMGR_PTR(ctp, blocks, sizeof(blocks));
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                         RESMGR_OCB_T *ocb)
{
    /* struct edr_qnx_ocb keeps iofunc_ocb_t as its first member, so the cast
     * is the one the iofunc layer itself performs. */
    int status = iofunc_devctl_default(ctp, msg, (iofunc_ocb_t *)ocb);

    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    switch (msg->i.dcmd) {
    case DCMD_EDR_STATUS:
        return (edr_gate(ctp, AMBRE_OP_READ_STATUS, AMBRE_MSG_STATUS) == EOK)
                   ? devctl_status(ctp, msg) : EACCES;
    case DCMD_EDR_LIST:
        return (edr_gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_LIST) == EOK)
                   ? devctl_list(ctp, msg) : EACCES;
    case DCMD_EDR_EXPORT:
        return (edr_gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_EXPORT) == EOK)
                   ? devctl_export(ctp, msg) : EACCES;
    case DCMD_EDR_AUTOTEST:
        if (g_edr_jrn.src.autotest == NULL) {
            return ENOSYS;
        }
        return (g_edr_jrn.src.autotest(g_edr_jrn.src.ctx) == 0) ? EOK : EIO;
    default:
        return ENOTTY;
    }
}

/* ------------------------------------------------------- read / write ----- */

/* EXP-08: raw blocks of one event, resumable by byte offset.
 * QNX-only: verified on target, syntax-checked on PC */
static int event_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    uint32_t first;
    size_t off_in_block;
    size_t want = msg->i.nbytes;

    if (g_edr_jrn.src.event_blocks == NULL) {
        return ENOSYS;
    }
    first = (uint32_t)((uint64_t)ocb->ocb.offset / AMBRE_BLOCK_SIZE);
    off_in_block = (size_t)((uint64_t)ocb->ocb.offset % AMBRE_BLOCK_SIZE);
    if (g_edr_jrn.src.event_blocks(g_edr_jrn.src.ctx, ocb->event_id, first, 1u,
                               ocb->blocks) != 0) {
        return EIO;
    }
    if (want > sizeof(ocb->blocks) - off_in_block) {
        want = sizeof(ocb->blocks) - off_in_block;
    }
    ocb->ocb.offset += (off_t)want;
    _IO_SET_READ_NBYTES(ctp, want);
    return _RESMGR_PTR(ctp, ocb->blocks + off_in_block, want);
}

/* OBS-02: read() on /dev/edr returns the text status.
 * QNX-only: verified on target, syntax-checked on PC */
int edr_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    char text[EDR_TEXT_MAX];
    int len;
    size_t want;
    int status = iofunc_read_verify(ctp, msg, (iofunc_ocb_t *)ocb, NULL);

    if (status != EOK) {
        return status;
    }
    if (ocb->event_id != 0u) {
        return event_read(ctp, msg, ocb);
    }
    if (edr_gate(ctp, AMBRE_OP_READ_STATUS, AMBRE_MSG_STATUS) != EOK) {
        return EACCES;
    }
    len = build_status_text(text, sizeof(text));
    if (len <= 0) {
        return EIO;
    }
    if ((uint64_t)ocb->ocb.offset >= (uint64_t)len) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_PTR(ctp, text, 0);
    }
    want = (size_t)len - (size_t)ocb->ocb.offset;
    if (want > msg->i.nbytes) {
        want = msg->i.nbytes;
    }
    {
        char *from = text + (size_t)ocb->ocb.offset;

        ocb->ocb.offset += (off_t)want;
        _IO_SET_READ_NBYTES(ctp, want);
        return _RESMGR_PTR(ctp, from, want);
    }
}

/* SEC-02: no write order can stop the recorder.
 * QNX-only: verified on target, syntax-checked on PC */
int edr_io_write(resmgr_context_t *ctp, io_write_t *msg,
                        RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, (iofunc_ocb_t *)ocb, NULL);

    if (status != EOK) {
        return status;
    }
    if (edr_gate(ctp, AMBRE_OP_WRITE_INPUT, AMBRE_MSG_FRAME) != EOK) {
        return EACCES;
    }
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

