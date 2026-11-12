/*
 * ambre_port_qnx.c — QNX 8.0 backend of the portability layer: identity,
 * clock, slog2, kernel trace, real-time priority and memory lock, access
 * control, supervision. Disk lives in ambre_port_disk_qnx.c and the message
 * transport in ambre_port_ipc_qnx.c, mirroring the POSIX backend split
 * (ambre_port_posix{,_disk,_ipc}.c) and keeping every file under 500 lines.
 *
 * QNX APIs used here: ClockTime(CLOCK_MONOTONIC), ClockCycles(), slog2 with a
 * per-process buffer set (OBS-01), trace_logi() user kernel-trace events
 * (OBS-03), SchedSet(SCHED_FIFO) (QNX-04), mlockall() (QNX-04), HAM/ppid
 * supervision detection (QNX-06).
 *
 * WHAT THE PC BUILD PROVES: this file is compiled on the PC against the stub
 * headers in target/qnx-stub/include with -fsyntax-only (make qnx-syntax):
 * typos, wrong prototypes and wrong struct fields are caught. It proves
 * NOTHING about runtime behaviour, numeric ABI values or kernel abilities —
 * see docs/QNX-PORT.md and docs/QNX-ABI.md.
 */

#define _POSIX_C_SOURCE 200809L   /* nanosleep() on the PC host */

#include "ambre_port.h"

#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/slog2.h>
#include <sys/trace.h>
#include <time.h>
#include <unistd.h>
#include <unistd.h>

/* ---------------------------------------------------------------- identity - */

/* QNX-only: verified on target, syntax-checked on PC */
const char *ambre_port_backend_name(void)
{
    return "qnx";
}

/* QNX-only: verified on target, syntax-checked on PC */
ambre_port_backend_t ambre_port_backend(void)
{
    return AMBRE_PORT_BACKEND_QNX;
}

/* -------------------------------------------------------------------- time - */

/* QNX-only: verified on target with the real SDP (qcc 12.2.0, nto x86_64) */
uint64_t ambre_port_now_us(void)
{
    /* ClockTime(CLOCK_MONOTONIC, NULL, &now) fills `now` with the monotonic
     * clock in nanoseconds and returns EOK (QNX C Library Reference). No
     * absolute epoch is ever exposed (ACQ-02, VIE-02). */
    _Uint64t now = 0u;

    if (ClockTime(CLOCK_MONOTONIC, NULL, &now) != EOK) {
        return 0u;
    }
    return (uint64_t)now / 1000u;
}

/* QNX-only: verified on target, syntax-checked on PC */
uint64_t ambre_port_sleep_us(uint64_t us)
{
    struct timespec req;
    struct timespec rem;
    uint64_t start = ambre_port_now_us();

    req.tv_sec = (time_t)(us / 1000000u);
    req.tv_nsec = (long)((us % 1000000u) * 1000u);
    while (nanosleep(&req, &rem) != 0 && errno == EINTR) {
        req = rem;                       /* interrupted: keep sleeping */
    }
    return ambre_port_now_us() - start;
}

/* --------------------------------------------------------------------- log - */

/* OBS-01: one slog2 buffer set per process, registered once. The buffer set
 * is process-wide so `slog2info -b ambre` shows every Ambre process. */
static slog2_buffer_t slog_buffers[1];
static bool slog_ready;

/* QNX-only: verified with the real SDP (slog2_buffer_set_config_t carries a
 * buffer_config[] array sized in 4K pages, not num_lines/buffer_size). */
static void slog_init_once(const char *tag)
{
    slog2_buffer_set_config_t cfg;
    /* The buffer SET name must be unique per process: two processes that
     * register the same set name are refused by slogger2, and the second
     * one's diagnostics vanish (we lost the client's log that way). */
    static char set_name[32];

    if (set_name[0] == '\0') {
        snprintf(set_name, sizeof(set_name), "ambre.%ld", (long)getpid());
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_buffers = 1;
    cfg.verbosity_level = SLOG2_INFO;
    cfg.buffer_set_name = set_name;
    cfg.buffer_config[0].buffer_name = (tag != NULL) ? tag : "edr";
    cfg.buffer_config[0].num_pages = 1;      /* one 4K page of log */
    slog_ready = (slog2_register(&cfg, slog_buffers, 0) == 0);
}

/* QNX-only: verified on target, syntax-checked on PC */
static uint8_t slog_severity(ambre_log_level_t lvl)
{
    switch (lvl) {
    case AMBRE_LOG_ERROR: return SLOG2_ERROR;
    case AMBRE_LOG_WARN:  return SLOG2_WARNING;
    case AMBRE_LOG_INFO:  return SLOG2_INFO;
    default:              return SLOG2_DEBUG1;
    }
}

/* QNX-only: verified on target, syntax-checked on PC */
void ambre_port_log(ambre_log_level_t lvl, const char *tag, const char *fmt, ...)
{
    va_list ap;
    char text[256];

    if (!slog_ready) {
        slog_init_once(tag);
    }
    if (!slog_ready) {
        return;                          /* never blocks the critical path */
    }
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    /* slog2f(buffer, code, severity, fmt, ...): one buffer set per process
     * (OBS-01), the tag goes into the text so slogger2 shows who spoke. */
    slog2f(slog_buffers[0], 0u, slog_severity(lvl), "%s: %s",
           (tag != NULL) ? tag : "edr", text);
}

/* ------------------------------------------------------------------- trace - */

/* QNX-only: verified on target, syntax-checked on PC */
void ambre_port_trace(uint32_t event_id, uint64_t value)
{
    /* OBS-03: the five AMBRE_TRACE_* ids map into the user event range
     * _NTO_TRACE_USERFIRST.._NTO_TRACE_USERLAST. trace_logi() inserts a
     * two-integer user event into class _NTO_TRACE_USER (QNX C Library
     * Reference, TraceEvent()/trace_logi); it shows up in a kernel trace
     * next to the disk driver's activity. Value is split in two 32-bit
     * halves because trace_logi carries exactly two integers. */
    int evt = _NTO_TRACE_USERFIRST + (int)(event_id & 0x0fffu);

    trace_logi(evt, (int)(value & 0xffffffffu), (int)(value >> 32));
}

/* -------------------------------------------------------------- real time - */

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_lock_memory(void)
{
    /* QNX-04: lock the process image so the freeze path cannot page fault
     * (GEL-03). Requires PROCMGR_AID_MEM_PHYS on a secured system; the
     * refusal is reported, never hidden. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        return AMBRE_PORT_OK;
    }
    return AMBRE_PORT_ERR_UNSUPPORTED;
}

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_set_priority(int priority)
{
    struct sched_param sp;

    if (priority < 1 || priority > 255) {
        return AMBRE_PORT_ERR_PARAM;     /* QNX FIFO runs 1..255 */
    }
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = priority;
    /* SchedSet(0, 0, ...) sets the calling thread of the calling process
     * (QNX C Library Reference, SchedSet()). A refusal (unprivileged,
     * secured policy) is reported honestly. */
    if (SchedSet(0, 0, SCHED_FIFO, &sp) != EOK) {
        return AMBRE_PORT_ERR_UNSUPPORTED;
    }
    return AMBRE_PORT_OK;
}

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_get_priority(void)
{
    struct sched_param sp;

    memset(&sp, 0, sizeof(sp));
    if (sched_getparam(0, &sp) != 0) {
        return AMBRE_PORT_ERR_UNSUPPORTED;
    }
    return sp.sched_priority;
}

/* ---------------------------------------------------------- access control - */

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_access_allowed(uint32_t peer_type, ambre_access_op_t op)
{
    /* SEC-01: same decision table as the POSIX backend (the two backends are
     * never linked together, so each carries the table; the rule itself is
     * exercised on the PC by tests/test_ipc.c). On target the kernel ALSO
     * enforces the typed policy of image/ambre.sec; this is the second,
     * in-process gate. */
    switch (op) {
    case AMBRE_OP_WRITE_INPUT:
        return (peer_type == AMBRE_PEER_PRODUCER) ? AMBRE_PORT_OK
                                                  : AMBRE_PORT_ERR_PERM;
    case AMBRE_OP_READ_STATUS:
    case AMBRE_OP_READ_EVENTS:
        return (peer_type == AMBRE_PEER_SERVICE ||
                peer_type == AMBRE_PEER_ARCHIVE) ? AMBRE_PORT_OK
                                                 : AMBRE_PORT_ERR_PERM;
    case AMBRE_OP_RAW_DISK:
        return (peer_type == AMBRE_PEER_JOURNAL) ? AMBRE_PORT_OK
                                                 : AMBRE_PORT_ERR_PERM;
    default:
        return AMBRE_PORT_ERR_PARAM;
    }
}

/* ----------------------------------------------------------- supervision -- */

/* QNX-only: verified on target, syntax-checked on PC */
bool ambre_port_supervised(void)
{
    /* QNX-06: on target the processes are launched by the HAM-based
     * supervision of the boot image (image/ambre.build). A full HAM
     * heartbeat registration needs <sys/ham.h>, which is not stubbed yet;
     * until then, report the two observable facts honestly:
     *   - the harness marker (same convention as the POSIX backend), or
     *   - reparenting to procnto (pid 1), which is what a boot-image
     *     process sees when its launcher exits.
     * TODO(target): replace with ham_register()/heartbeat once <sys/ham.h>
     * is imported in the stubs, and report the supervisor entity. */
    return getenv("AMBRE_SUPERVISED") != NULL || getppid() == 1;
}
