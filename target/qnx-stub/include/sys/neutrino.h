/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Substitute for <sys/neutrino.h> - building on the PC only.
 *
 * substitute for building on the PC only; the real QNX header is used on
 * target
 *
 * The real QNX header from the SDP is used on target; this file exists so that
 * the QNX glue of the drivers (`*_qnx.c`) is compiled by the PC build and a
 * typo or a wrong prototype is caught without an SDP install (see
 * tests/qnx-stub/README.md: no QNX behaviour is simulated, only compilation).
 *
 * Covered requirements: QNX-01 (resource manager), QNX-03 (caller priority),
 * QNX-05 (no thread pinned by a blocked client), QNX-09 (kernel trace),
 * VIO-04 (interrupt service thread).
 *
 * Only the declarations the drivers actually use are present. Every constant
 * whose *numeric* value matters carries its provenance; values we never depend
 * on numerically are declared as symbolic placeholders and are flagged.
 *
 * Provenance of the prototypes: QNX SDP 8.0 C Library Reference
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/i/interruptattachthread.html
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/traceevent.html
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/m/msginfo.html
 */
#ifndef PGW_STUB_SYS_NEUTRINO_H
#define PGW_STUB_SYS_NEUTRINO_H

#include <sched.h>   /* struct sched_param, used by SchedSet() below */
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/iomsg.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>

/* QNX's <errno.h> defines EOK (success, 0); a POSIX host <errno.h> does not.
 * The drivers use it as QNX code does, so it is spelled out here rather than
 * replaced by a literal 0 everywhere. */
#ifndef EOK
#define EOK 0
#endif

/* --- kernel handle types (QNX: <sys/neutrino.h>) -------------------------- */

typedef int32_t rcvid_t; /* receive id returned by MsgReceive*() */
typedef int32_t coid_t;  /* connection id returned by ConnectAttach() */
typedef uint32_t sync_t; /* sync object id (mutex, condvar, semaphore) */

/* --- pulses (QNX: struct _pulse) ------------------------------------------ */

/* QNX takes `union sigval` and `struct sigevent` from <signal.h> (POSIX realtime
 * extensions). A strict C11 host build does not expose them, so they are spelled
 * out here; the guards keep them from clashing when the host does define them.
 * This file is only ever on the PC include path, never on target. */
#ifndef ____sigval_t_defined
#define ____sigval_t_defined 1
union sigval {
    int sival_int;
    void *sival_ptr;
};
#endif

#ifndef __sigevent_t_defined
#define __sigevent_t_defined 1
/* QNX spells its sigevent with a union of kernel notification forms
 * (signal, pulse, callback...); the POSIX shape below is a PC stand-in with
 * the SAME member names the glue uses. The QNX-specific members were added
 * for the Ambre capture glue (QNX-03: timers notify by pulse).
 * Provenance: QNX SDP 8.0 C Library Reference, struct sigevent /
 * "Pulse notification" (sigev_notify = SIGEV_PULSE, channel in sigev_coid,
 * pulse code in sigev_code, 32-bit payload in sigev_value.sival_int):
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigevent.html
 */
#define SIGEV_NONE  0
#define SIGEV_SIGNAL 1
#define SIGEV_THREAD 2
#define SIGEV_PULSE  4   /* QNX: deliver a pulse instead of a signal */
#define SIGEV_UNBLOCK 5  /* QNX: unblock a thread blocked in a kernel call */

struct sigevent {
    int sigev_notify;
    int sigev_signo;
    union sigval sigev_value;
    void (*sigev_notify_function)(union sigval);
    void *sigev_notify_attributes;
    int __sigev_unused[4];
    /* --- QNX-specific notification members, ADDED for the Ambre capture
     * glue (on target these live in the sigevent union); the padding above
     * is untouched. --- */
    int sigev_coid;      /* SIGEV_PULSE: connection to pulse (channel) */
    int sigev_priority;  /* SIGEV_PULSE: priority of the pulse         */
    int sigev_code;      /* SIGEV_PULSE: pulse code (_PULSE_CODE_*)    */
};
#endif

struct _pulse {
    uint16_t type;
    uint16_t subtype;
    int8_t code;
    uint8_t zero[3];
    union sigval value;
    int32_t scoid;
};

/* --- flags we pass to kernel calls ---------------------------------------- */

/* ChannelCreate() flags. Values are from <sys/neutrino.h> of the SDP; the
 * driver only ever passes them as a bit set, never compares them. */
#define _NTO_CHF_PRIVATE    0x00000001u
#define _NTO_CHF_UNBLOCK    0x00000010u
#define _NTO_CHF_DISCONNECT 0x00000020u

/* MsgInfo() result flags (struct _msg_info.flags). _NTO_MI_UNBLOCK_REQ is the
 * one that tells a server thread its client asked to be unblocked (QNX-05:
 * a signal or a timeout must not wait for the whole transfer). */
#define _NTO_MI_UNBLOCK_REQ 0x00000002u

/* InterruptWait() flags. */
#define _NTO_INTR_WAIT_FLAGS_UNMASK 0x00000001u
#define _NTO_INTR_WAIT_FLAGS_INTR   0x00000002u

/* InterruptAttachThread() flags (QNX 8.0; InterruptAttach() no longer exists). */
#define _NTO_INTR_FLAGS_END        0x00000004u
#define _NTO_INTR_FLAGS_NO_UNMASK  0x00000008u
#define _NTO_INTR_FLAGS_EXCLUSIVE  0x00000010u
#define _NTO_INTR_FLAGS_CPU_LOCAL  0x00000020u

/* Caller credentials as the kernel reports them. On target this is
 * <sys/cred.h> `struct _cred_info`; the fields used by the drivers are euid,
 * egid and the supplementary group list (FS-09 / SEC-01). */
#define _NTO_CRED_MAX_GROUPS 8

struct _cred_info {
    uint32_t info_size;
    uint32_t flags;
    uint32_t ruid;
    uint32_t euid;
    uint32_t suid;
    uint32_t rgid;
    uint32_t egid;
    uint32_t sgid;
    uint32_t ngroups;
    uint32_t grouplist[_NTO_CRED_MAX_GROUPS];
};

/* Message info filled by MsgInfo(). `priority` is the sender's priority, which
 * is what the driver runs a request at (QNX-03), and `flags` carries
 * _NTO_MI_UNBLOCK_REQ (QNX-05). Layout is a documented subset. */
struct _msg_info {
    uint32_t nd;
    int32_t srcnd;
    pid_t pid;
    int32_t tid;
    int32_t chid;
    int32_t scoid;
    int32_t coid;
    uint32_t msglen;
    uint32_t srcmsglen;
    uint32_t dstmsglen;
    uint16_t priority;
    uint32_t flags;
    struct _cred_info cred;
};

/* --- channel and message passing ----------------------------------------- */

int ChannelCreate(unsigned flags);
int ChannelDestroy(int chid);
coid_t ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(coid_t coid);

int MsgSend(coid_t coid, const void *smsg, size_t sbytes, void *rmsg, size_t rbytes);
int MsgSendPulse(coid_t coid, int priority, int code, int value);
int MsgSendv(coid_t coid, const struct iovec *sv, unsigned sn, struct iovec *rv,
             unsigned rn);
int MsgReceive(int chid, void *msg, size_t bytes, struct _msg_info *info);
int MsgReceivePulse(int chid, void *pulse, size_t bytes, struct _msg_info *info);
int MsgReply(rcvid_t rcvid, long status, const void *msg, size_t bytes);
int MsgReplyv(rcvid_t rcvid, long status, const struct iovec *iov, unsigned n);
int MsgError(rcvid_t rcvid, int err);
int MsgInfo(rcvid_t rcvid, struct _msg_info *info);
int MsgDeliverEvent(rcvid_t rcvid, const struct sigevent *event);

/* --- synchronisation (SyncMutex* is the QNX mutex; pthread_mutex works too
 *     but the kernel object is what a resource manager needs when the lock is
 *     taken from a thread that may be boosted by priority inheritance). ----- */

int SyncMutexInit(sync_t *sync);
int SyncMutexLock(sync_t *sync);
int SyncMutexUnlock(sync_t *sync);
int SyncMutexDestroy(sync_t *sync);
int SyncTypeCreate(unsigned type, sync_t *sync, const void *attr);
int SyncDestroy(sync_t *sync);

/* --- threads and scheduling ---------------------------------------------- */

int ThreadCreate(void *(*start_routine)(void *), const void *arg, size_t stacksize);
int ThreadCtl(int cmd, void *data);
int SchedSet(pid_t pid, int tid, int policy, const struct sched_param *param);

/* --- clocks -------------------------------------------------------------- */

/* QNX's own width typedefs (sys/platform.h on target); the stubs only need the
 * unsigned ones plus _Int32t, which a few kernel calls use. */
#ifndef _QNX_UINT_TYPES_DEFINED
#define _QNX_UINT_TYPES_DEFINED
typedef uint8_t  _Uint8t;
typedef uint16_t _Uint16t;
typedef uint32_t _Uint32t;
typedef uint64_t _Uint64t;
typedef int32_t  _Int32t;
typedef int64_t  _Int64t;
#endif

/* Corrected 2026-11-12 against the real SDP 8.0 header (line 936): ClockTime()
 * RETURNS A STATUS and hands the nanoseconds back through `old`. ClockCycles()
 * is the free-running counter. */
int ClockTime(clockid_t id, const _Uint64t *new_time, _Uint64t *old_time);
uint64_t ClockCycles(void);
uint64_t ClockPeriod(void);

/* --- interrupts (QNX 8.0 removed InterruptAttach()) ----------------------- */

/* InterruptAttachThread() turns the *calling thread* into an Interrupt Service
 * Thread; it returns an interrupt id, or -1. Requires the `interrupt` ability
 * (QNX-06 / SEC-01). Source: the C Library Reference page cited above. */
int InterruptAttachThread(int intr, unsigned flags);

/* InterruptAttachEvent() is the other QNX 8.0 way: the kernel delivers the
 * sigevent instead of running a handler in interrupt context. */
int InterruptAttachEvent(int intr, const struct sigevent *event, unsigned flags);

int InterruptWait(int flags, const uint64_t *timeout);
int InterruptUnmask(int intr, int id);
int InterruptMask(int intr, int id);
int InterruptDetach(int id);

/* --- kernel trace (QNX-09) ------------------------------------------------ */

/* TraceEvent() is the kernel-trace control call. User events are inserted with
 * _NTO_TRACE_INSERTSUSEREVENT / _NTO_TRACE_INSERTUSRSTREVENT; the event id must
 * be within _NTO_TRACE_USERFIRST.._NTO_TRACE_USERLAST (all defined in
 * <sys/trace.h>). Source: TraceEvent() in the QNX SDP 8.0 C Library Reference. */
int TraceEvent(int cmd, ...);

/* --- misc ---------------------------------------------------------------- */

unsigned delay(unsigned milliseconds);


/*
 * Provenance: QNX SDP 8.0 <sys/neutrino.h>, System Pulses
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.getting_started/topic/s1_procs_pulses.html
 */
#define _PULSE_CODE_MINAVAIL   0
#define _PULSE_CODE_MAXAVAIL   127
#define _PULSE_CODE_DISCONNECT (-1)
#define _PULSE_CODE_UNBLOCK    (-2)
#define _PULSE_DISCONNECT      _PULSE_CODE_DISCONNECT
#define _PULSE_UNBLOCK         _PULSE_CODE_UNBLOCK



/*
 * --- POSIX timers on QNX, used by the Ambre capture glue (QNX-03) ---------
 *
 * timer_create()/timer_settime() live in <time.h> on both QNX and POSIX, but
 * a strict C11 host build (-std=c11 without feature macros) hides them from
 * glibc's <time.h>, while the glue does not define _POSIX_C_SOURCE (it needs
 * the QNX-flavoured struct sigevent above: SIGEV_PULSE with sigev_coid /
 * sigev_code / sigev_priority, which the POSIX shape lacks).
 *
 * The declarations below carry the guard macros glibc itself uses
 * (__itimerspec_defined, __timer_t_defined), so this block is inert on a
 * feature-macro host build and fills the gap on a strict C11 one. On target
 * the real SDP <time.h> is used and this file is not on the include path.
 *
 * Provenance: QNX SDP 8.0 C Library Reference, timer_create() /
 * timer_settime(); struct itimerspec and the SIGEV_PULSE notification form
 * are documented on the sigevent page:
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timer_create.html
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/s/sigevent.html
 */
/* The whole POSIX-timer block below is needed only by a STRICT C11 host
 * build. When the glue defines a feature macro (_POSIX_C_SOURCE >= 199309L,
 * which glibc reports as __USE_POSIX199309), the host's own <time.h> already
 * declares these names and this block stays inert: two different shapes of
 * the same prototype would be a hard error otherwise. On target none of this
 * file is used: the SDP's <time.h> is. */
#if !defined(__USE_POSIX199309)

/* CLOCK_MONOTONIC is the Ambre project clock (ACQ-02, VIE-02). It comes from
 * <time.h> on QNX; a strict C11 host build hides it, so it is declared here
 * when the host has not. The numeric value is SYMBOLIC on the PC: the glue
 * only passes it to ClockTime()/timer_create(), never decodes it. The QNX
 * value is asserted on target by target/tests/qnx_abi_assert.c and recorded
 * in docs/QNX-ABI.md.
 * Provenance: QNX SDP 8.0 C Library Reference, ClockTime() / clockid_t. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 3
#endif

/* ConnectAttach() index argument: _NTO_SIDE_CHANNEL asks the kernel for a
 * private "side channel" connection, used when a process connects to its own
 * channel to receive its own pulses (QNX-03: timer pulses delivered to the
 * timer owner). Symbolic on the PC: only passed through.
 * Provenance: QNX SDP 8.0 C Library Reference, ConnectAttach():
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/c/connectattach.html */
#ifndef _NTO_SIDE_CHANNEL
#define _NTO_SIDE_CHANNEL 0x40000000
#endif

#ifndef __itimerspec_defined
#define __itimerspec_defined 1
struct itimerspec {
    struct timespec it_interval;   /* reload value (periodic pulse)  */
    struct timespec it_value;      /* initial expiry                 */
};
#endif

/* Pulse-timer convenience macro documented on the sigevent page: fill the
 * sigevent for a pulse to (coid, code, priority). It is spelled as a macro,
 * exactly like QNX's own SIGEV_PULSE_INIT, so it never depends on struct
 * layout beyond the members above. */
#ifndef SIGEV_PULSE
#define SIGEV_PULSE 4   /* QNX: deliver a pulse instead of a signal */
#endif

#ifndef __timer_t_defined
#define __timer_t_defined 1
typedef unsigned long timer_t;   /* kernel timer id, never inspected numerically */
#endif

int timer_create(clockid_t clock_id, const struct sigevent *evp,
                 timer_t *timerid);
int timer_settime(timer_t timerid, int flags, const struct itimerspec *value,
                  struct itimerspec *ovalue);
int timer_delete(timer_t timerid);

#endif /* !__USE_POSIX199309 */

/* Bounded blocking: TimerTimeout() arms a kernel timeout for the next
 * blocking call (MsgSend, MsgReceive, ...). Passing NULL as nsecs cancels it.
 *
 * Provenance: QNX SDP 8.0 C Library Reference, TimerTimeout(), and the
 * Neutrino "timeouts" documentation for the _NTO_TIMEOUT_* flags.
 *   https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.lib_ref/topic/t/timertimeout.html
 */
/* Corrected 2026-11-12 against the real SDP 8.0 header (lines 120-127, 900):
 * the flags select WHICH blocking state times out, and `ntime` is an ABSOLUTE
 * deadline on clock_id (NULL cancels the timeout). There is no
 * _NTO_TIMEOUT_NEAREST. */
#define _NTO_TIMEOUT_FLAG(__state)  ((_Uint32t)1 << (__state))
#define _NTO_TIMEOUT_RECEIVE        _NTO_TIMEOUT_FLAG(1)
#define _NTO_TIMEOUT_SEND           _NTO_TIMEOUT_FLAG(2)
#define _NTO_TIMEOUT_REPLY          _NTO_TIMEOUT_FLAG(3)
#define _NTO_TIMEOUT_MQ_SEND        _NTO_TIMEOUT_FLAG(4)
#define _NTO_TIMEOUT_MQ_RECEIVE     _NTO_TIMEOUT_FLAG(5)
#define _NTO_TIMEOUT_SIGSUSPEND     _NTO_TIMEOUT_FLAG(6)
#define _NTO_TIMEOUT_SIGWAITINFO    _NTO_TIMEOUT_FLAG(7)

int TimerTimeout(clockid_t clock_id, _Uint32t flags,
                 const struct sigevent *notify, const _Uint64t *ntime,
                 _Uint64t *remaining);

#endif /* PGW_STUB_SYS_NEUTRINO_H */

