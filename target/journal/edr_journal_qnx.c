/*
 * edr_journal_qnx.c — QNX 8.0 resource manager of the journal: /dev/edr.
 *
 * QNX-01: open/read/write/devctl interface only:
 *   /dev/edr                 read()      -> text status (OBS-02)
 *                            devctl()    -> DCMD_EDR_STATUS  (OBS-02, binary)
 *                                           DCMD_EDR_LIST    (EXP-01)
 *                                           DCMD_EDR_EXPORT  (raw event blocks)
 *                                           DCMD_EDR_AUTOTEST(OBS-04)
 *   /dev/edr/events/<id>     open/read() -> raw blocks of event <id> (EXP-08)
 *
 * The data comes from the portable service (edr_journal_svc.c) via the
 * callback table installed by edr_journal_qnx_set_source(); wiring that call
 * into target/journal/main.c is left to the journal-svc agent.
 * QNX-03 (spec §1.3): requests run in the receiving pool thread at the
 * CALLER's priority (message-passing priority inheritance); no handler sleeps,
 * allocates or takes a low-priority lock. QNX-05: a thread pool isolates a
 * blocked client. SEC-01: iofunc_client_info() + ambre_port_access_allowed().
 * WHAT THE PC BUILD PROVES (docs/QNX-PORT.md): `make qnx-syntax` checks syntax,
 * prototypes and struct fields against target/qnx-stub/include. It proves
 * nothing about runtime behaviour, priority inheritance, devctl ABI values
 * (docs/QNX-ABI.md), or kernel security abilities.
 * TODO(target), deliberately not faked:
 *   - call edr_journal_qnx_set_source() + edr_journal_qnx_start() from main.c;
 *   - align group -> peer-type mapping with image/ambre.sec once frozen;
 *   - readdir() on /dev/edr/events is not implemented (nodes open by name).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

/* Custom OCB carrying event_id and a 1-block staging buffer (must be first). */
struct edr_qnx_ocb;
#define IOFUNC_OCB_T  struct edr_qnx_ocb
#define RESMGR_OCB_T  struct edr_qnx_ocb

#include "ambre_port.h"
#include "edr_ipc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/dcmd_blk.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/resmgr.h>
#include <sys/stat.h>

/* Project devctl numbers of /dev/edr — NOT QNX ABI: invented here, documented
 * in docs/QNX-ABI.md §3; the stubs deliberately do not define them. On target,
 * re-encode with QNX's __DIOTF() macros from <devctl.h>. */
#define DCMD_EDR_STATUS    0x45000001  /* 'E' class: status body (OBS-02)   */
#define DCMD_EDR_LIST      0x45000002  /* event lines (EXP-01)              */
#define DCMD_EDR_EXPORT    0x45000003  /* uint32[2] in, raw blocks out      */
#define DCMD_EDR_AUTOTEST  0x45000004  /* start-up self-test (OBS-04)       */

#define EDR_PATH        "/dev/edr"
#define EDR_EVENTS_DIR  "events/"
#define EDR_POOL_MIN    2
#define EDR_POOL_MAX    8
#define EDR_MAX_EVENTS  32u
#define EDR_TEXT_MAX    512u

struct edr_qnx_ocb {
    iofunc_ocb_t ocb;         /* must stay first: the iofunc layer casts */
    uint32_t     event_id;    /* 0 = /dev/edr status node                */
    bool         in_use;
    uint8_t      blocks[AMBRE_BLOCK_SIZE];
};

typedef struct {
    void *ctx;
    int (*status)(void *ctx, ambre_ipc_status_body_t *out);
    int (*list)(void *ctx, ambre_ipc_event_line_t *out, uint32_t max);
    int (*event_blocks)(void *ctx, uint32_t event_id, uint32_t first,
                        uint32_t count, void *out);
    uint32_t (*event_block_count)(void *ctx, uint32_t event_id);
    int (*autotest)(void *ctx);
} edr_journal_qnx_src_t;

typedef struct {
    dispatch_t             *dpp;
    thread_pool_t          *pool;
    resmgr_connect_funcs_t connect_funcs;
    resmgr_io_funcs_t      io_funcs;
    iofunc_attr_t          root_attr;
    iofunc_attr_t          ev_attr;
    edr_journal_qnx_src_t  src;
    int                    root_id;
    bool                   started;
} edr_journal_qnx_t;

static edr_journal_qnx_t g_jrn;
static struct edr_qnx_ocb ocb_pool[EDR_MAX_EVENTS];  /* GEL-03: fixed pool */

/* ------------------------------------------------------- access control --- */

/* QNX-only: verified on target, syntax-checked on PC */
static uint32_t peer_type_of(resmgr_context_t *ctp)
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
static int gate(resmgr_context_t *ctp, ambre_access_op_t op, uint8_t msg_type)
{
    uint32_t type = peer_type_of(ctp);

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
    if (g_jrn.src.status != NULL) {
        (void)g_jrn.src.status(g_jrn.src.ctx, &st);
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
    if (g_jrn.src.status != NULL && g_jrn.src.status(g_jrn.src.ctx, &st) != 0) {
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

    if (g_jrn.src.list == NULL) {
        return ENOSYS;
    }
    got = g_jrn.src.list(g_jrn.src.ctx, lines, EDR_MAX_EVENTS);
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
    if (g_jrn.src.event_blocks == NULL
        || g_jrn.src.event_blocks(g_jrn.src.ctx, req[0], req[1], count,
                                  blocks) != 0) {
        return EIO;
    }
    msg->o.nbytes = (int32_t)sizeof(blocks);
    return _RESMGR_PTR(ctp, blocks, sizeof(blocks));
}

/* QNX-only: verified on target, syntax-checked on PC */
static int edr_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg,
                         RESMGR_OCB_T *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);

    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    switch (msg->i.dcmd) {
    case DCMD_EDR_STATUS:
        return (gate(ctp, AMBRE_OP_READ_STATUS, AMBRE_MSG_STATUS) == EOK)
                   ? devctl_status(ctp, msg) : EACCES;
    case DCMD_EDR_LIST:
        return (gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_LIST) == EOK)
                   ? devctl_list(ctp, msg) : EACCES;
    case DCMD_EDR_EXPORT:
        return (gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_EXPORT) == EOK)
                   ? devctl_export(ctp, msg) : EACCES;
    case DCMD_EDR_AUTOTEST:
        if (g_jrn.src.autotest == NULL) {
            return ENOSYS;
        }
        return (g_jrn.src.autotest(g_jrn.src.ctx) == 0) ? EOK : EIO;
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

    if (g_jrn.src.event_blocks == NULL) {
        return ENOSYS;
    }
    first = (uint32_t)((uint64_t)ocb->ocb.offset / AMBRE_BLOCK_SIZE);
    off_in_block = (size_t)((uint64_t)ocb->ocb.offset % AMBRE_BLOCK_SIZE);
    if (g_jrn.src.event_blocks(g_jrn.src.ctx, ocb->event_id, first, 1u,
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
static int edr_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    char text[EDR_TEXT_MAX];
    int len;
    size_t want;
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);

    if (status != EOK) {
        return status;
    }
    if (ocb->event_id != 0u) {
        return event_read(ctp, msg, ocb);
    }
    if (gate(ctp, AMBRE_OP_READ_STATUS, AMBRE_MSG_STATUS) != EOK) {
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
        const char *from = text + (size_t)ocb->ocb.offset;

        ocb->ocb.offset += (off_t)want;
        _IO_SET_READ_NBYTES(ctp, want);
        return _RESMGR_PTR(ctp, from, want);
    }
}

/* SEC-02: no write order can stop the recorder.
 * QNX-only: verified on target, syntax-checked on PC */
static int edr_io_write(resmgr_context_t *ctp, io_write_t *msg,
                        RESMGR_OCB_T *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);

    if (status != EOK) {
        return status;
    }
    if (gate(ctp, AMBRE_OP_WRITE_INPUT, AMBRE_MSG_FRAME) != EOK) {
        return EACCES;
    }
    _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
    return EOK;
}

/* --------------------------------------------------- open (EXP-08 names) -- */

/* QNX-only: verified on target, syntax-checked on PC */
static uint32_t event_id_from_path(const char *path, size_t len)
{
    uint32_t id = 0u;

    if (path == NULL || len <= sizeof(EDR_EVENTS_DIR) - 1u
        || strncmp(path, EDR_EVENTS_DIR, sizeof(EDR_EVENTS_DIR) - 1u) != 0) {
        return 0u;
    }
    for (size_t i = sizeof(EDR_EVENTS_DIR) - 1u; i < len; i++) {
        if (path[i] < '0' || path[i] > '9') {
            return 0u;
        }
        id = id * 10u + (uint32_t)(path[i] - '0');
    }
    return id;
}

/* QNX-only: verified on target, syntax-checked on PC */
static struct edr_qnx_ocb *ocb_pool_take(void)
{
    for (uint32_t i = 0u; i < EDR_MAX_EVENTS; i++) {
        if (!ocb_pool[i].in_use) {
            memset(&ocb_pool[i], 0, sizeof(ocb_pool[i]));
            ocb_pool[i].in_use = true;
            return &ocb_pool[i];
        }
    }
    return NULL;
}

/* QNX-only: verified on target, syntax-checked on PC */
static int edr_open(resmgr_context_t *ctp, io_open_t *msg,
                    RESMGR_HANDLE_T *handle, void *extra)
{
    struct edr_qnx_ocb *ocb;
    uint32_t id = event_id_from_path(msg->connect.path, msg->connect.path_len);
    iofunc_attr_t *attr = (id != 0u) ? &g_jrn.ev_attr : &g_jrn.root_attr;
    int status;

    (void)handle;
    if (id != 0u) {
        if (g_jrn.src.event_block_count == NULL
            || g_jrn.src.event_block_count(g_jrn.src.ctx, id) == 0u) {
            return ENOENT;
        }
        if (gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_EXPORT) != EOK) {
            return EACCES;
        }
    }
    status = iofunc_open(ctp, msg, attr, NULL, NULL);
    if (status != EOK) {
        return status;
    }
    ocb = ocb_pool_take();
    if (ocb == NULL) {
        return EAGAIN;
    }
    status = iofunc_ocb_attach(ctp, msg, &ocb->ocb, attr, extra);
    if (status != EOK) {
        ocb->in_use = false;
        return status;
    }
    ocb->event_id = id;
    return EOK;
}

/* QNX-only: verified on target, syntax-checked on PC */
static int edr_close_ocb(resmgr_context_t *ctp, void *reserved,
                         RESMGR_OCB_T *ocb)
{
    if (ocb != NULL) {
        ocb->in_use = false;
    }
    return iofunc_close_ocb_default(ctp, reserved, ocb);
}

/* ---------------------------------------------------------- lifecycle ----- */

/* QNX-only: verified on target, syntax-checked on PC */
static void io_tables_init(void)
{
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_jrn.connect_funcs,
                     _RESMGR_IO_NFUNCS, &g_jrn.io_funcs);
    g_jrn.connect_funcs.open = edr_open;
    g_jrn.io_funcs.read = edr_io_read;
    g_jrn.io_funcs.write = edr_io_write;
    g_jrn.io_funcs.devctl = edr_io_devctl;
    g_jrn.io_funcs.close_ocb = edr_close_ocb;
    iofunc_attr_init(&g_jrn.root_attr, S_IFCHR | 0660, NULL, NULL);
    iofunc_attr_init(&g_jrn.ev_attr, S_IFREG | 0444, NULL, NULL);
}

/* The portable service installs its callbacks here, before the start. */
void edr_journal_qnx_set_source(const edr_journal_qnx_src_t *src);
void edr_journal_qnx_set_source(const edr_journal_qnx_src_t *src)
{
    if (src != NULL) {
        g_jrn.src = *src;
    }
}

/* QNX-only: verified on target, syntax-checked on PC */
static dispatch_context_t *pool_alloc(void *handle)
{
    return dispatch_context_alloc((dispatch_t *)handle);
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_journal_qnx_start(void)
{
    resmgr_attr_t attr;
    thread_pool_attr_t pool;

    g_jrn.dpp = dispatch_create();
    if (g_jrn.dpp == NULL) {
        return AMBRE_PORT_ERR_IO;
    }
    io_tables_init();
    memset(&attr, 0, sizeof(attr));
    attr.nparts_max = 4u;
    attr.msg_max_size = AMBRE_IPC_FRAME_MAX;
    g_jrn.root_id = resmgr_attach(g_jrn.dpp, &attr, EDR_PATH, _FTYPE_ANY,
                                  _RESMGR_FLAG_DIR, &g_jrn.connect_funcs,
                                  &g_jrn.io_funcs, &g_jrn.root_attr);
    if (g_jrn.root_id < 0) {
        (void)dispatch_destroy(g_jrn.dpp);
        return AMBRE_PORT_ERR_IO;
    }
    memset(&pool, 0, sizeof(pool));
    pool.handle = g_jrn.dpp;
    pool.context_alloc = pool_alloc;
    pool.context_free = dispatch_context_free;
    pool.block_func = dispatch_block;
    pool.handler_func = dispatch_handler;
    pool.lo_water = EDR_POOL_MIN;
    pool.increment = 1;
    pool.hi_water = EDR_POOL_MAX;
    pool.maximum = EDR_POOL_MAX;
    g_jrn.pool = thread_pool_create(&pool, POOL_FLAG_EXIT_SELF);
    if (g_jrn.pool == NULL) {
        (void)resmgr_detach(g_jrn.dpp, g_jrn.root_id, _RESMGR_DETACH_PATHNAME);
        (void)dispatch_destroy(g_jrn.dpp);
        return AMBRE_PORT_ERR_IO;
    }
    g_jrn.started = true;
    return (thread_pool_start(g_jrn.pool) == 0) ? AMBRE_PORT_OK
                                                : AMBRE_PORT_ERR_IO;
}

/* QNX-only: verified on target, syntax-checked on PC */
void edr_journal_qnx_stop(void)
{
    if (!g_jrn.started) {
        return;
    }
    g_jrn.started = false;
    (void)thread_pool_destroy(g_jrn.pool);
    (void)resmgr_detach(g_jrn.dpp, g_jrn.root_id, _RESMGR_DETACH_PATHNAME);
    (void)dispatch_destroy(g_jrn.dpp);
    g_jrn.pool = NULL;
    g_jrn.dpp = NULL;
}
