/*
 * qnx_abi_assert.c — Target-only ABI compile-time assertions (docs/QNX-ABI.md).
 *
 * HONESTY NOTICE (docs/QNX-PORT.md):
 * Compiling this file on the PC against target/qnx-stub/include proves NOTHING
 * about the real QNX 8.0 numeric ABI — the #ifdef __QNX__ block is skipped on
 * the PC, because the stub headers only hold symbolic placeholders for most
 * constants. The assertions below ONLY fire when compiled with QNX's `qcc`
 * against the real QNX SDP 8.0 headers on target.
 *
 * The portable wire sizes at the bottom ARE checked on both PC and target.
 */

#include "edr_ipc.h"
#include "ambre_port.h"

/* Portable wire / format ABI: checked on both PC and QNX. */
_Static_assert(sizeof(ambre_ipc_hdr_t) == 16u, "wire header must be 16 bytes");
_Static_assert(sizeof(ambre_block_t) == 512u, "journal block must be 512 bytes");
_Static_assert(sizeof(ambre_alloc_table_t) == 512u, "table must be 512 bytes");
_Static_assert(sizeof(ambre_ipc_status_body_t) == 68u, "status body must be 68");
_Static_assert(sizeof(ambre_ipc_event_line_t) == 32u, "event line must be 32");

#ifdef __QNX__

#include <errno.h>
#include <sched.h>
#include <sys/dcmd_blk.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/mount.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <sys/resmgr.h>
#include <sys/slog2.h>
#include <sys/trace.h>

/* Success status assumed by all *_qnx.c glue files. */
_Static_assert(EOK == 0, "EOK must be 0");

/* Pulse codes: _PULSE_CODE_DISCONNECT is compared in MsgReceive loops (QNX-05)
 * and _PULSE_CODE_MINAVAIL + 1 is used for the capture tick timer (QNX-03). */
_Static_assert(_PULSE_CODE_DISCONNECT < 0, "disconnect pulse code must be < 0");
_Static_assert(_PULSE_CODE_UNBLOCK < 0, "unblock pulse code must be < 0");
_Static_assert(_PULSE_CODE_MINAVAIL >= 0, "MINAVAIL must be >= 0");
_Static_assert(_PULSE_CODE_MAXAVAIL >= _PULSE_CODE_MINAVAIL + 8,
               "need at least 8 user pulse codes");

/* Kernel trace user event range (OBS-03): AMBRE_TRACE_TRIGGER..RECOVER (1..5)
 * are added to _NTO_TRACE_USERFIRST and must stay within USERFIRST..USERLAST. */
_Static_assert((int)_NTO_TRACE_USERFIRST + (int)AMBRE_TRACE_RECOVER
               <= (int)_NTO_TRACE_USERLAST,
               "Ambre user trace ids overflow _NTO_TRACE_USERLAST");

/* Block cache-flush command used by essay E1 and qnx_disk_flush(). */
_Static_assert(DCMD_FLUSH_CACHE != 0, "DCMD_FLUSH_CACHE must be non-zero");

/* Channel flags & scheduling policy. */
_Static_assert(_NTO_CHF_DISCONNECT != 0u, "_NTO_CHF_DISCONNECT must be non-zero");
_Static_assert(_NTO_CHF_UNBLOCK != 0u, "_NTO_CHF_UNBLOCK must be non-zero");
_Static_assert(SCHED_FIFO > 0, "SCHED_FIFO must be positive");

/* Resource manager flags used by edr_journal_qnx.c (QNX-01). */
_Static_assert(_RESMGR_FLAG_DIR != 0u, "_RESMGR_FLAG_DIR must be non-zero");

#endif /* __QNX__ */
