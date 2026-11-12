/*
 * edr_journal_qnx_internal.h — shared shape of the edr-journal resource
 * manager on QNX 8.0 (QNX-01).
 *
 * Two translation units implement one resource manager:
 *   edr_journal_qnx.c      lifecycle: attach, thread pool, open/close_ocb
 *   edr_journal_io_qnx.c   the service handlers: read, write, devctl
 * They share the OCB type, the global state and the access-control gate, which
 * is what this header declares. IOFUNC_OCB_T / RESMGR_OCB_T must be defined
 * before <sys/iofunc.h> is included, so both files include this header first.
 */

#ifndef EDR_JOURNAL_QNX_INTERNAL_H
#define EDR_JOURNAL_QNX_INTERNAL_H

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
#include <sys/neutrino.h>
/* resmgr.h BEFORE dispatch.h: it is what sets THREAD_POOL_PARAM_T to
 * resmgr_context_t, so thread_pool_attr_t takes the resmgr pool helpers. */
#include <sys/resmgr.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
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

extern edr_journal_qnx_t g_edr_jrn;
extern struct edr_qnx_ocb g_edr_ocb_pool[EDR_MAX_EVENTS];   /* GEL-03: fixed pool */


/* Handlers installed by io_tables_init() (edr_journal_qnx.c) and implemented
 * in edr_journal_io_qnx.c. */
int edr_io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb);
int edr_io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb);
int edr_io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb);

/* SEC-01: whether the caller may perform `op` (the peer type it derives from
 * the kernel credentials stays private to edr_journal_io_qnx.c). */
int edr_gate(resmgr_context_t *ctp, ambre_access_op_t op, uint8_t msg_type);

#endif /* EDR_JOURNAL_QNX_INTERNAL_H */
