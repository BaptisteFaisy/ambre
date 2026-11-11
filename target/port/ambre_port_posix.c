/*
 * ambre_port_posix.c — POSIX backend of the portability layer.
 *
 * Runs on the development machine and on the CI runners: a regular file stands
 * in for the raw partition, CLOCK_MONOTONIC for the QNX clock, AF_UNIX
 * SOCK_SEQPACKET for QNX message passing (see ambre_port_posix_ipc.c), stderr
 * for slog2. Everything the backend cannot honestly provide returns
 * AMBRE_PORT_ERR_UNSUPPORTED instead of faking a result.
 */

#define _POSIX_C_SOURCE 200809L

#include "ambre_port.h"

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- identity - */

const char *ambre_port_backend_name(void)
{
    return "posix";
}

ambre_port_backend_t ambre_port_backend(void)
{
    return AMBRE_PORT_BACKEND_POSIX;
}

/* -------------------------------------------------------------------- time - */

uint64_t ambre_port_now_us(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

uint64_t ambre_port_sleep_us(uint64_t us)
{
    struct timespec req;
    struct timespec rem;
    uint64_t start = ambre_port_now_us();

    req.tv_sec = (time_t)(us / 1000000u);
    req.tv_nsec = (long)((us % 1000000u) * 1000u);
    while (nanosleep(&req, &rem) != 0) {
        req = rem;                       /* interrupted: keep sleeping */
    }
    return ambre_port_now_us() - start;
}

/* --------------------------------------------------------------------- log - */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(ambre_log_level_t lvl)
{
    switch (lvl) {
    case AMBRE_LOG_ERROR: return "ERROR";
    case AMBRE_LOG_WARN:  return "WARN";
    case AMBRE_LOG_INFO:  return "INFO";
    default:              return "DEBUG";
    }
}

void ambre_port_log(ambre_log_level_t lvl, const char *tag, const char *fmt, ...)
{
    va_list ap;
    const char *min = getenv("AMBRE_LOG");

    if (min != NULL && lvl > (ambre_log_level_t)atoi(min)) {
        return;
    }
    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "[%s] %-8s ", level_name(lvl), tag ? tag : "-");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    pthread_mutex_unlock(&log_lock);
}

/* ------------------------------------------------------------------- trace - */

/* OBS-03 on the PC: a timestamped line on the trace stream, which the latency
 * harness parses. On QNX the same call becomes TraceUser() and shows up in a
 * kernel trace next to the disk driver's own activity. */
void ambre_port_trace(uint32_t event_id, uint64_t value)
{
    static FILE *stream;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    const char *path = getenv("AMBRE_TRACE");

    if (path == NULL) {
        return;
    }
    pthread_mutex_lock(&lock);
    if (stream == NULL) {
        stream = fopen(path, "a");
    }
    if (stream != NULL) {
        fprintf(stream, "%" PRIu64 " %u %" PRIu64 "\n", ambre_port_now_us(),
                event_id, value);
        fflush(stream);
    }
    pthread_mutex_unlock(&lock);
}

/* -------------------------------------------------------------- real time - */

int ambre_port_lock_memory(void)
{
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        return AMBRE_PORT_OK;
    }
    /* Not privileged on a workstation: report it instead of pretending. */
    return AMBRE_PORT_ERR_UNSUPPORTED;
}

int ambre_port_set_priority(int priority)
{
    struct sched_param sp;

    if (priority <= 0) {
        return AMBRE_PORT_ERR_PARAM;
    }
    memset(&sp, 0, sizeof(sp));
    /* QNX priorities run 1 (most urgent) .. 255; SCHED_FIFO wants the reverse
     * order, so map the QNX number onto the Linux range. */
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO) -
                        (priority % sched_get_priority_max(SCHED_FIFO));
    if (sp.sched_priority < sched_get_priority_min(SCHED_FIFO)) {
        sp.sched_priority = sched_get_priority_min(SCHED_FIFO);
    }
    if (sched_setscheduler(0, SCHED_FIFO, &sp) != 0) {
        return AMBRE_PORT_ERR_UNSUPPORTED;   /* unprivileged: reported, not faked */
    }
    return AMBRE_PORT_OK;
}

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

/* SEC-01, portable half: the same decision table the QNX security policy in
 * image/ambre.sec expresses as process types. Keeping it here means the rule
 * is testable on the PC, while the kernel enforces it on target. */
int ambre_port_access_allowed(uint32_t peer_type, ambre_access_op_t op)
{
    switch (op) {
    case AMBRE_OP_WRITE_INPUT:
        return (peer_type == AMBRE_PEER_PRODUCER) ? AMBRE_PORT_OK
                                                  : AMBRE_PORT_ERR_PERM;
    case AMBRE_OP_READ_STATUS:
    case AMBRE_OP_READ_EVENTS:
        return (peer_type == AMBRE_PEER_SERVICE || peer_type == AMBRE_PEER_ARCHIVE)
                   ? AMBRE_PORT_OK : AMBRE_PORT_ERR_PERM;
    case AMBRE_OP_RAW_DISK:
        return (peer_type == AMBRE_PEER_JOURNAL) ? AMBRE_PORT_OK
                                                 : AMBRE_PORT_ERR_PERM;
    default:
        return AMBRE_PORT_ERR_PARAM;
    }
}

/* ----------------------------------------------------------- supervision -- */

bool ambre_port_supervised(void)
{
    /* On the PC a process is supervised when the harness set the marker. */
    return getenv("AMBRE_SUPERVISED") != NULL;
}
