/*
 * edr_capture_qnx.c — QNX 8.0 glue of the capture process (edr-capture).
 *
 * QNX duties of the capture box (spec §4.4, priorities 60/61):
 *   - SCHED_FIFO priority 60 (intake) and 61 (detection), QNX-04;
 *   - mlockall() so the freeze path cannot page fault (GEL-03, QNX-04);
 *   - one channel for producers, MsgReceive for their frames (ACQ-01);
 *   - a periodic PULSE timer for the batch tick and the heartbeat: the
 *     kernel delivers an impulse, no thread ever sleeps on a poll (QNX-03);
 *   - MsgSend to edr-journal for the emergency snapshot (GEL-01) and the
 *     continuation frames (GEL-04), with the round trip measured at the
 *     producer side (PERF-05, essay E4);
 *   - a producer's death is reported by the kernel as a _PULSE_CODE_DISCONNECT
 *     impulse on the channel, never by polling (QNX-05).
 *
 * WHAT THE PC BUILD PROVES: this file is compiled with -fsyntax-only against
 * target/qnx-stub/include (make qnx-syntax): prototypes, types and struct
 * fields. It proves nothing about behaviour, numeric ABI values, priorities
 * granted by the kernel, or whether a pulse is really delivered on time.
 * See docs/QNX-PORT.md.
 */

#include "ambre_port.h"
#include "edr_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/trace.h>
#include <unistd.h>

#define EDR_CAP_PRIO_INTAKE 60
#define EDR_CAP_PRIO_DETECT 61
#define EDR_CAP_TICK_US     10000u          /* 10 ms batch tick (ACQ-01)    */
#define EDR_CAP_TICK_CODE   (_PULSE_CODE_MINAVAIL + 1)

typedef struct {
    int      chid;               /* channel producers send their frames to  */
    int      journal_coid;       /* connection to edr-journal (scoid)        */
    timer_t  tick_timer;         /* periodic pulse timer (QNX-03)           */
    bool     mem_locked;
    bool     timer_armed;
    uint32_t dead_producer;      /* last producer reported dead (QNX-05)    */
    uint32_t ticks;              /* pulses received: heartbeat counter      */
} edr_capture_qnx_t;

static edr_capture_qnx_t g_cap = { .chid = -1, .journal_coid = -1 };

/* QNX-only: verified on target, syntax-checked on PC */
static int capture_arm_tick_timer(int chid)
{
    struct sigevent sev;
    struct itimerspec its;

    /* QNX-03: the tick is a kernel timer delivering a PULSE, not a sleeping
     * thread. SIGEV_PULSE_INIT spells the same thing on target; the field
     * assignments below are the portable spelling used by the glue. */
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_PULSE;
    sev.sigev_coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    sev.sigev_priority = EDR_CAP_PRIO_DETECT;
    sev.sigev_code = EDR_CAP_TICK_CODE;
    sev.sigev_value.sival_int = 0;

    if (timer_create(CLOCK_MONOTONIC, &sev, &g_cap.tick_timer) != 0) {
        return AMBRE_PORT_ERR_IO;
    }
    its.it_interval.tv_sec = 0;
    its.it_interval.tv_nsec = (long)EDR_CAP_TICK_US * 1000L;
    its.it_value = its.it_interval;
    if (timer_settime(g_cap.tick_timer, 0, &its, NULL) != 0) {
        (void)timer_delete(g_cap.tick_timer);
        return AMBRE_PORT_ERR_IO;
    }
    g_cap.timer_armed = true;
    return AMBRE_PORT_OK;
}

/* QNX-only: verified on target, syntax-checked on PC */
int edr_capture_qnx_init(const char *journal_path)
{
    int fd;

    if (journal_path == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    /* QNX-04 / GEL-03: lock the image before the first frame arrives. */
    g_cap.mem_locked = (ambre_port_lock_memory() == AMBRE_PORT_OK);
    if (ambre_port_set_priority(EDR_CAP_PRIO_INTAKE) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, "capture",
                       "priority %d refused by the kernel (QNX-04)",
                       EDR_CAP_PRIO_INTAKE);
    }
    g_cap.chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT);
    if (g_cap.chid < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    fd = open(journal_path, O_RDWR);
    if (fd < 0) {
        ChannelDestroy(g_cap.chid);
        g_cap.chid = -1;
        return AMBRE_PORT_ERR_NOENT;
    }
    g_cap.journal_coid = ConnectAttach(0, 0, fd, 0, 0);
    close(fd);
    if (g_cap.journal_coid < 0) {
        ChannelDestroy(g_cap.chid);
        g_cap.chid = -1;
        return AMBRE_PORT_ERR_IO;
    }
    (void)capture_arm_tick_timer(g_cap.chid);
    return AMBRE_PORT_OK;
}

/* QNX-only: verified on target, syntax-checked on PC */
void edr_capture_qnx_cleanup(void)
{
    if (g_cap.timer_armed) {
        (void)timer_delete(g_cap.tick_timer);
        g_cap.timer_armed = false;
    }
    if (g_cap.journal_coid >= 0) {
        ConnectDetach(g_cap.journal_coid);
        g_cap.journal_coid = -1;
    }
    if (g_cap.chid >= 0) {
        ChannelDestroy(g_cap.chid);
        g_cap.chid = -1;
    }
}

/* One receive on the capture channel. Returns the byte count of a frame, 0
 * when the impulse was the tick (nothing to process), or a negative error.
 * QNX-only: verified on target, syntax-checked on PC */
int edr_capture_qnx_recv(void *buf, size_t cap, uint32_t *producer)
{
    struct _msg_info info;
    rcvid_t rcvid;

    if (g_cap.chid < 0 || buf == NULL || cap == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    rcvid = MsgReceive(g_cap.chid, buf, cap, &info);
    if (rcvid < 0) {
        return (errno == EINTR) ? 0 : AMBRE_PORT_ERR_IO;
    }
    if (rcvid == 0) {
        /* An impulse, not a message: the tick or a producer's death. */
        struct _pulse *p = (struct _pulse *)buf;

        if (p->code == EDR_CAP_TICK_CODE) {
            g_cap.ticks++;
        } else if (p->code == _PULSE_CODE_DISCONNECT) {
            /* QNX-05: the kernel tells us the producer is gone. */
            g_cap.dead_producer = (uint32_t)p->value.sival_int;
            ambre_port_log(AMBRE_LOG_WARN, "capture",
                           "producer %u disconnected (QNX-05)",
                           (unsigned)g_cap.dead_producer);
        }
        return 0;
    }
    if (producer != NULL) {
        *producer = (uint32_t)info.scoid;
    }
    return (int)info.msglen;
}

/* Hand a frame to edr-journal and wait for its ack: that round trip is what
 * PERF-05 and essay E4 measure at the producer. QNX-only: verified on target,
 * syntax-checked on PC */
int edr_capture_qnx_forward(const void *msg, size_t len, uint64_t *latency_ns)
{
    uint8_t reply[64];
    uint64_t t0;
    int rc;

    if (g_cap.journal_coid < 0 || msg == NULL || len == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    t0 = ambre_port_now_us();
    ambre_port_trace(AMBRE_TRACE_WRITE_SUBMIT, (uint64_t)len);
    rc = MsgSend(g_cap.journal_coid, msg, len, reply, sizeof(reply));
    if (rc < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    if (latency_ns != NULL) {
        *latency_ns = (ambre_port_now_us() - t0) * 1000u;
    }
    return AMBRE_PORT_OK;
}

/* The emergency snapshot path: raise the detection priority for the duration
 * of the freeze, hand the class A snapshot to the journal, drop back.
 * QNX-only: verified on target, syntax-checked on PC */
int edr_capture_qnx_trigger(const void *snapshot, size_t len)
{
    int rc;

    if (ambre_port_set_priority(EDR_CAP_PRIO_DETECT) != AMBRE_PORT_OK) {
        ambre_port_log(AMBRE_LOG_WARN, "capture", "detection priority refused");
    }
    ambre_port_trace(AMBRE_TRACE_TRIGGER, (uint64_t)len);
    rc = edr_capture_qnx_forward(snapshot, len, NULL);
    (void)ambre_port_set_priority(EDR_CAP_PRIO_INTAKE);
    return rc;
}

/* QNX-05, read side: the peer id of the last producer the kernel reported
 * dead, consumed once. QNX-only: verified on target, syntax-checked on PC */
uint32_t edr_capture_qnx_poll_dead(void)
{
    uint32_t dead = g_cap.dead_producer;

    g_cap.dead_producer = 0u;
    return dead;
}
