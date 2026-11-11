/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/trace.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so the
 * QNX glue of the drivers compiles on the PC. No QNX behaviour is simulated.
 *
 * Covered requirement: QNX-09 (a kernel trace event at request submission and
 * at reply reception).
 *
 * Provenance: QNX SDP 8.0 C Library Reference, TraceEvent():
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/traceevent.html
 * The page documents the commands (`_NTO_TRACE_INSERTSUSEREVENT`,
 * `_NTO_TRACE_INSERTUSRSTREVENT`, ...), the class `_NTO_TRACE_USER`, the event
 * id range `_NTO_TRACE_USERFIRST`..`_NTO_TRACE_USERLAST`, and the convenience
 * covers `trace_logi()`, `trace_logb()`, `trace_logf()`, `trace_nlogf()`.
 * The *numeric* values below are placeholders used only on the PC: the drivers
 * pass them through, and the QNX build uses the SDP's own numbers (asserted by
 * drivers/tests/qnx_abi_assert.c).
 */
#ifndef PGW_STUB_SYS_TRACE_H
#define PGW_STUB_SYS_TRACE_H

#include <stdint.h>

#define _NTO_TRACE_INSERTSUSEREVENT  21
#define _NTO_TRACE_INSERTCUSEREVENT  22
#define _NTO_TRACE_INSERTUSRSTREVENT 23
#define _NTO_TRACE_USERFIRST 0x2000
#define _NTO_TRACE_USERLAST  0x2fff

/* Covers documented on the TraceEvent() page; they insert into class
 * _NTO_TRACE_USER and do not need the PROCMGR_AID_TRACE ability. */
void trace_logi(int event, int data0, int data1);
void trace_logb(int event, const unsigned *buf, unsigned len);
void trace_logf(int event, const char *fmt, ...);
void trace_nlogf(int event, const char *fmt, ...);

#endif /* PGW_STUB_SYS_TRACE_H */
