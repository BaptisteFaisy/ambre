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
struct sigevent {
    int sigev_notify;
    int sigev_signo;
    union sigval sigev_value;
    void (*sigev_notify_function)(union sigval);
    void *sigev_notify_attributes;
    int __sigev_unused[4];
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

/* ClockTime() returns nanoseconds for the given clock; CLOCK_MONOTONIC is the
 * project clock (pgw/time.h). ClockCycles() is the free-running counter. */
uint64_t ClockTime(clockid_t id, uint64_t *new_time);
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

#endif /* PGW_STUB_SYS_NEUTRINO_H */
