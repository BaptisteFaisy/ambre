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

#include "edr_journal_qnx_internal.h"

/* The one instance of the resource manager, and its fixed OCB pool
 * (GEL-03: nothing is allocated on a request path). */
edr_journal_qnx_t g_edr_jrn;
struct edr_qnx_ocb g_edr_ocb_pool[EDR_MAX_EVENTS];

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
        if (!g_edr_ocb_pool[i].in_use) {
            memset(&g_edr_ocb_pool[i], 0, sizeof(g_edr_ocb_pool[i]));
            g_edr_ocb_pool[i].in_use = true;
            return &g_edr_ocb_pool[i];
        }
    }
    return NULL;
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_open(resmgr_context_t *ctp, io_open_t *msg,
                    RESMGR_HANDLE_T *handle, void *extra)
{
    struct edr_qnx_ocb *ocb;
    uint32_t id = event_id_from_path(msg->connect.path, msg->connect.path_len);
    iofunc_attr_t *attr = (id != 0u) ? &g_edr_jrn.ev_attr : &g_edr_jrn.root_attr;
    int status;

    (void)handle;
    (void)extra;
    if (id != 0u) {
        if (g_edr_jrn.src.event_block_count == NULL
            || g_edr_jrn.src.event_block_count(g_edr_jrn.src.ctx, id) == 0u) {
            return ENOENT;
        }
        if (edr_gate(ctp, AMBRE_OP_READ_EVENTS, AMBRE_MSG_EXPORT) != EOK) {
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
    /* The 5th argument is the io-functions table the OCB is bound to, not the
     * opaque `extra` of the open handler (real SDP signature). */
    status = iofunc_ocb_attach(ctp, msg, &ocb->ocb, attr, &g_edr_jrn.io_funcs);
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
    return iofunc_close_ocb_default(ctp, reserved, (iofunc_ocb_t *)ocb);
}

/* ---------------------------------------------------------- lifecycle ----- */

/* QNX-only: verified on target, syntax-checked on PC */
static void io_tables_init(void)
{
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &g_edr_jrn.connect_funcs,
                     _RESMGR_IO_NFUNCS, &g_edr_jrn.io_funcs);
    g_edr_jrn.connect_funcs.open = edr_open;
    g_edr_jrn.io_funcs.read = edr_io_read;
    g_edr_jrn.io_funcs.write = edr_io_write;
    g_edr_jrn.io_funcs.devctl = edr_io_devctl;
    g_edr_jrn.io_funcs.close_ocb = edr_close_ocb;
    iofunc_attr_init(&g_edr_jrn.root_attr, S_IFCHR | 0660, NULL, NULL);
    iofunc_attr_init(&g_edr_jrn.ev_attr, S_IFREG | 0444, NULL, NULL);
}

/* The portable service installs its callbacks here, before the start. */
void edr_journal_qnx_set_source(const edr_journal_qnx_src_t *src)
{
    if (src != NULL) {
        g_edr_jrn.src = *src;
    }
}

/* QNX-only: verified on target, syntax-checked on PC */
/* The resource-manager pool uses the resmgr_* context helpers (they return
 * resmgr_context_t, the type resmgr_attach's handlers expect), not the raw
 * dispatch_* ones. */
static resmgr_context_t *pool_alloc(dispatch_t *handle)
{
    return resmgr_context_alloc(handle);
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_journal_qnx_start(void)
{
    resmgr_attr_t attr;
    thread_pool_attr_t pool;

    g_edr_jrn.dpp = dispatch_create();
    if (g_edr_jrn.dpp == NULL) {
        return AMBRE_PORT_ERR_IO;
    }
    io_tables_init();
    memset(&attr, 0, sizeof(attr));
    attr.nparts_max = 4u;
    attr.msg_max_size = AMBRE_IPC_FRAME_MAX;
    g_edr_jrn.root_id = resmgr_attach(g_edr_jrn.dpp, &attr, EDR_PATH, _FTYPE_ANY,
                                  _RESMGR_FLAG_DIR, &g_edr_jrn.connect_funcs,
                                  &g_edr_jrn.io_funcs, &g_edr_jrn.root_attr);
    if (g_edr_jrn.root_id < 0) {
        (void)dispatch_destroy(g_edr_jrn.dpp);
        return AMBRE_PORT_ERR_IO;
    }
    memset(&pool, 0, sizeof(pool));
    pool.handle = g_edr_jrn.dpp;
    pool.context_alloc = pool_alloc;
    pool.context_free = resmgr_context_free;
    pool.block_func = resmgr_block;
    pool.handler_func = resmgr_handler;
    pool.lo_water = EDR_POOL_MIN;
    pool.increment = 1;
    pool.hi_water = EDR_POOL_MAX;
    pool.maximum = EDR_POOL_MAX;
    g_edr_jrn.pool = thread_pool_create(&pool, POOL_FLAG_EXIT_SELF);
    if (g_edr_jrn.pool == NULL) {
        (void)resmgr_detach(g_edr_jrn.dpp, g_edr_jrn.root_id, _RESMGR_DETACH_PATHNAME);
        (void)dispatch_destroy(g_edr_jrn.dpp);
        return AMBRE_PORT_ERR_IO;
    }
    g_edr_jrn.started = true;
    return (thread_pool_start(g_edr_jrn.pool) == 0) ? AMBRE_PORT_OK
                                                : AMBRE_PORT_ERR_IO;
}

/* QNX-only: verified on target, syntax-checked on PC */
void edr_journal_qnx_stop(void)
{
    if (!g_edr_jrn.started) {
        return;
    }
    g_edr_jrn.started = false;
    (void)thread_pool_destroy(g_edr_jrn.pool);
    (void)resmgr_detach(g_edr_jrn.dpp, g_edr_jrn.root_id, _RESMGR_DETACH_PATHNAME);
    (void)dispatch_destroy(g_edr_jrn.dpp);
    g_edr_jrn.pool = NULL;
    g_edr_jrn.dpp = NULL;
}
