/*
 * edr_journal_qnx_internal.h — shared shape of the edr-journal resource
 * manager on QNX 8.0 (QNX-01).
 *
 * Two translation units implement one resource manager:
 *   edr_journal_qnx.c      lifecycle: attach, thread pool, open/close_ocb
 *   edr_journal_io_qnx.c   the service handlers: read, write, devctl
 * They share the global state and the access-control gate, which is what this
 * header declares.
 */

#ifndef EDR_JOURNAL_QNX_INTERNAL_H
#define EDR_JOURNAL_QNX_INTERNAL_H

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

/* L'OCB est celui de la bibliothèque QNX (struct _iofunc_ocb), alloué et libéré
 * par la couche iofunc : iofunc_ocb_calloc() en connaît la taille, pas un type
 * du projet. Les macros sont posées AVANT l'inclusion de <sys/resmgr.h> : sans
 * elles resmgr.h tombe sur son défaut `void *`, et les handlers ne peuvent plus
 * lire `ocb->offset`. Le nom du type est incomplet à ce point (il ne sera
 * défini qu'avec <sys/iofunc.h> plus bas) — c'est exactement ce que fait la
 * doublure (target/qnx-stub/include/sys/iofunc.h) et ce que la SDP autorise
 * via son garde-fou `#ifndef IOFUNC_OCB_T`. */
struct _iofunc_ocb;
#define IOFUNC_OCB_T  struct _iofunc_ocb
#define RESMGR_OCB_T  struct _iofunc_ocb
#include "ambre_port.h"
#include "edr_ipc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/dcmd_blk.h>
#include <sys/neutrino.h>
/* resmgr.h BEFORE dispatch.h: it is what sets THREAD_POOL_PARAM_T to
 * resmgr_context_t, so thread_pool_attr_t takes the resmgr pool helpers. */
#include <sys/resmgr.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/stat.h>

/* Les devctl d'Ambre viennent de target/edr_dcmd.h, encodés selon l'ABI QNX
 * (classe _DCMD_MISC, direction et TAILLE dans le code) : les numéros bruts du
 * projet (0x45000001..) encodaient une taille de 0, et le noyau ne renvoyait
 * rien au client — mesuré sur la cible le 2026-11-14. docs/QNX-ABI.md §3 garde
 * la table des deux représentations. */
#include "../edr_dcmd.h"

#define EDR_PATH        "/dev/edr"
#define EDR_EVENTS_DIR  "events/"
#define EDR_POOL_MIN    2
#define EDR_POOL_MAX    8
#define EDR_MAX_EVENTS  32u
#define EDR_TEXT_MAX    512u

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

extern edr_journal_qnx_t g_edr_jrn;


/* Handlers installed by io_tables_init() (edr_journal_qnx.c) and implemented
 * in edr_journal_io_qnx.c. */
int edr_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb);
int edr_io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb);
int edr_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb);

/* SEC-01: whether the caller may perform `op` (the peer type it derives from
 * the kernel credentials stays private to edr_journal_io_qnx.c). */
int edr_gate(resmgr_context_t *ctp, ambre_access_op_t op, uint8_t msg_type);

/* Lifecycle, called by edr_journal_serve_qnx.c (the adapter that binds the
 * portable service to these handlers) — nothing else used to call them, which
 * is why the devctl interface never existed on target. `set_source` MUST be
 * called before `start`: the handlers read the callbacks at request time and a
 * NULL source answers ENOSYS. */
void edr_journal_qnx_set_source(const edr_journal_qnx_src_t *src);
int  edr_journal_qnx_start(void);
void edr_journal_qnx_stop(void);

#endif /* EDR_JOURNAL_QNX_INTERNAL_H */
