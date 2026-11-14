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

/* L'instance unique du resource manager. L'OCB est alloué et libéré par la
 * couche iofunc de QNX (iofunc_ocb_calloc / close_ocb_default) : un pool fixe
 * côté projet serait alloué avec la mauvaise taille — mesuré le 2026-11-14, le
 * service mourait (SIGABRT) à la première ouverture par un client. */
edr_journal_qnx_t g_edr_jrn;

/* QNX-only: verified on target, syntax-checked on PC */
int edr_open(resmgr_context_t *ctp, io_open_t *msg,
                    RESMGR_HANDLE_T *handle, void *extra)
{
    (void)ctp;
    (void)msg;
    (void)handle;
    (void)extra;
    /* OCB /dev/edr standard de la bibliothèque QNX : iofunc_open_default_cinfo
     * l'alloue avec la bonne taille et l'attache en une seule fois. Les anciens
     * chemins /dev/edr/events/<id> ont besoin de stocker l'id dans l'OCB : pas
     * encore supportés dans cette version; l'export des blocs bruts passe par
     * DCMD_EDR_EXPORT (un vrai devctl, testé dans host/qnx_probe). */
    return iofunc_open_default(ctp, msg, &g_edr_jrn.root_attr, NULL);
}

/* QNX-only: verified on target, syntax-checked on PC */
static int edr_close_ocb(resmgr_context_t *ctp, void *reserved,
                         RESMGR_OCB_T *ocb)
{
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

static int resmgr_attach_root(void)
{
    resmgr_attr_t attr;

    memset(&attr, 0, sizeof(attr));
    attr.nparts_max = 4u;
    attr.msg_max_size = AMBRE_IPC_FRAME_MAX;
    /* L'ORDRE DES ARGUMENTS EST flags PUIS ftype (signature de la SDP 8.0) : les
     * passer dans l'autre ordre donnait au noyau un `flags` = _FTYPE_ANY et un
     * `ftype` = _RESMGR_FLAG_DIR — un abort (SIGABRT) au démarrage du service,
     * sur la cible seulement : les en-têtes de la doublure (target/qnx-stub)
     * ont la même signature que la SDP, donc la vérification syntaxique ne
     * pouvait pas le voir.
     *
     * /dev/edr est un NOEUD, pas un répertoire : le client ouvre exactement ce
     * chemin et le devctl y répond (status, list, export, autotest). La forme
     * « répertoire » (_RESMGR_FLAG_DIR, qui délègue au gestionnaire la
     * résolution des chemins plus longs, /dev/edr/events/<id>) faisait
     * .abortir le service à l'ouverture du premier client — mesuré sur la cible
     * le 2026-11-14 : le nœud /dev/edr existait, et l'open de /dev/edr/journal
     * tuait le processus. La lecture des blocs bruts d'un événement par chemin
     * (/dev/edr/events/<id>, EXP-08) reste donc à faire ; elle passe aujourd'hui
     * par devctl DCMD_EDR_EXPORT, qui est mesuré. */
    g_edr_jrn.root_id = resmgr_attach(g_edr_jrn.dpp, &attr, EDR_PATH,
                                      0u, _FTYPE_ANY,
                                      &g_edr_jrn.connect_funcs,
                                      &g_edr_jrn.io_funcs, &g_edr_jrn.root_attr);
    return (g_edr_jrn.root_id < 0) ? AMBRE_PORT_ERR_IO : AMBRE_PORT_OK;
}

static int resmgr_create_pool(void)
{
    thread_pool_attr_t pool;

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
    g_edr_jrn.pool = thread_pool_create(&pool, 0u);
    return (g_edr_jrn.pool == NULL) ? AMBRE_PORT_ERR_IO : AMBRE_PORT_OK;
}

static void resmgr_cleanup(void)
{
    (void)resmgr_detach(g_edr_jrn.dpp, g_edr_jrn.root_id,
                        _RESMGR_DETACH_PATHNAME);
    (void)dispatch_destroy(g_edr_jrn.dpp);
    g_edr_jrn.pool = NULL;
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_journal_qnx_start(void)
{
    if (g_edr_jrn.dpp != NULL) {
        return AMBRE_PORT_ERR_IO;
    }
    g_edr_jrn.dpp = dispatch_create();
    if (g_edr_jrn.dpp == NULL) {
        return AMBRE_PORT_ERR_IO;
    }
    io_tables_init();
    if (resmgr_attach_root() != AMBRE_PORT_OK
        || resmgr_create_pool() != AMBRE_PORT_OK) {
        resmgr_cleanup();
        return AMBRE_PORT_ERR_IO;
    }
    g_edr_jrn.started = true;
    /* Sans POOL_FLAG_EXIT_SELF, thread_pool_start() lance les fils du pool et
     * rend la main : l'IPC (name_attach) et le resource manager répondent en
     * parallèle. Avec EXIT_SELF, la fonction ne revient pas et la boucle IPC
     * du journal ne tourne jamais — mesuré sur la cible le 2026-11-14. */
    if (thread_pool_start(g_edr_jrn.pool) != 0) {
        g_edr_jrn.started = false;
        resmgr_cleanup();
        return AMBRE_PORT_ERR_IO;
    }
    return AMBRE_PORT_OK;
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
