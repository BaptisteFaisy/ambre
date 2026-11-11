/*
 * ambre_port.h — Portability layer between the EDR processes and the OS.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The five Ambre processes (edr-capture, edr-journal, edr-archive, edrctl,
 * edr-replay) are written once. Everything that differs between QNX 8.0 and a
 * POSIX host lives behind the functions below:
 *
 *   target/port/ambre_port_posix.c  PC backend: a regular file as the raw
 *                                   partition, CLOCK_MONOTONIC, AF_UNIX
 *                                   SOCK_SEQPACKET for message passing, stderr
 *                                   for logs, no-op kernel trace.
 *   target/port/ambre_port_qnx.c    QNX backend: /dev/hd0tXX through io-blk
 *                                   with DCMD_FLUSH_CACHE, MsgSend/MsgReceive
 *                                   and pulses, slog2, TraceUser events,
 *                                   SCHED_FIFO priorities, mlock().
 *
 * The QNX backend is compiled on the PC against the stub headers in
 * target/qnx-stub/include (syntax and prototype check only, nothing is
 * executed); it runs for real only on a QNX 8.0 target. See docs/QNX-PORT.md
 * for exactly what is and is not proven by each build.
 *
 * RULES
 * -----
 * - No dynamic allocation on the critical path (GEL-03): every buffer the
 *   caller needs is supplied by the caller or pre-allocated at open time.
 * - Every function returns 0 on success and a negative ambre_port_err_t.
 * - No absolute time is ever produced (VIE-02): the clock is monotonic and
 *   relative to an unspecified epoch.
 */

#ifndef AMBRE_PORT_H
#define AMBRE_PORT_H

#include "../../fmt/ambre_fmt.h"

typedef enum {
    AMBRE_PORT_OK          =   0,
    AMBRE_PORT_ERR_IO      =  -1,
    AMBRE_PORT_ERR_PARAM   =  -2,
    AMBRE_PORT_ERR_NOENT   =  -3,
    AMBRE_PORT_ERR_TIMEOUT =  -4,
    AMBRE_PORT_ERR_PERM    =  -5,   /* refused by the access control (SEC-01) */
    AMBRE_PORT_ERR_FULL    =  -6,
    AMBRE_PORT_ERR_UNSUPPORTED = -7 /* backend cannot do it (reported, not faked) */
} ambre_port_err_t;

/* Which backend is linked in. Reported by `edrctl status` and stamped into
 * every campaign report, so a PC number is never mistaken for a QNX one. */
typedef enum {
    AMBRE_PORT_BACKEND_POSIX = 1,   /* PC: file-backed, real processes       */
    AMBRE_PORT_BACKEND_QNX   = 2    /* QNX 8.0 target                        */
} ambre_port_backend_t;

const char *ambre_port_backend_name(void);
ambre_port_backend_t ambre_port_backend(void);

/* ------------------------------------------------------------------ time -- */

/* Monotonic microseconds since an unspecified epoch (ACQ-02, VIE-02). */
uint64_t ambre_port_now_us(void);

/* Bounded sleep; returns the microseconds actually slept. */
uint64_t ambre_port_sleep_us(uint64_t us);

/* ------------------------------------------------------------------ disk -- */

typedef struct ambre_port_disk ambre_port_disk_t;

/* Open the raw journal partition. On QNX `spec` is a device path such as
 * /dev/hd0t177; on the PC it is a regular file, created if missing. */
int ambre_port_disk_open(const char *spec, uint64_t size, ambre_port_disk_t **out);
void ambre_port_disk_close(ambre_port_disk_t *d);

/* ambre_disk_t vtable bound to this backend, for the fmt/ library. The mark
 * callback is NULL on both backends: marks exist only for the PC explorer. */
const ambre_disk_t *ambre_port_disk_vtable(ambre_port_disk_t *d);

/* Feasibility essay E1: write one block then order a cache flush, and report
 * whether the flush order reached the device and what it cost.
 * `flush_seen` is only meaningful on the QNX backend (or against the faulty
 * block server, which logs the FLUSH commands it receives); the POSIX backend
 * reports AMBRE_PORT_ERR_UNSUPPORTED rather than pretending. */
int ambre_port_disk_flush_probe(ambre_port_disk_t *d, bool *flush_seen,
                                uint64_t *flush_ns);

/* ------------------------------------------------------------------- log -- */

typedef enum {
    AMBRE_LOG_ERROR = 0,
    AMBRE_LOG_WARN  = 1,
    AMBRE_LOG_INFO  = 2,
    AMBRE_LOG_DEBUG = 3
} ambre_log_level_t;

/* OBS-01: slog2 on QNX, stderr on the PC. Never blocks the critical path. */
void ambre_port_log(ambre_log_level_t lvl, const char *tag, const char *fmt, ...);

/* ------------------------------------------------------ kernel trace ------ */

/* OBS-03: TraceUser events on QNX (visible in a kernel trace next to the
 * disk driver's activity), timestamped log lines on the PC. */
#define AMBRE_TRACE_TRIGGER      1u  /* threshold crossed, pulse sent        */
#define AMBRE_TRACE_WRITE_SUBMIT 2u  /* emergency write handed to the driver */
#define AMBRE_TRACE_FLUSH_ACK    3u  /* flush acknowledged: event acquired   */
#define AMBRE_TRACE_SEAL         4u  /* event sealed                         */
#define AMBRE_TRACE_RECOVER      5u  /* recovery started/finished            */

void ambre_port_trace(uint32_t event_id, uint64_t value);

/* ------------------------------------------------------- real-time -------- */

/* QNX-04: lock the critical processes' memory at startup so the freeze path
 * cannot take a page fault (GEL-03). Returns UNSUPPORTED where meaningless. */
int ambre_port_lock_memory(void);

/* Set this process/thread priority. QNX: SCHED_FIFO 1..255 (lower = more
 * urgent). POSIX: best effort through sched_setscheduler, UNSUPPORTED if the
 * kernel refuses (reported honestly, never faked). */
int ambre_port_set_priority(int priority);
int ambre_port_get_priority(void);

/* ---------------------------------------------------------- transport ----- */

/* Message passing between processes (ACQ-01, QNX-03).
 * QNX: a channel owned by edr-journal, clients MsgSend/MsgReceive, pulses for
 * notifications. POSIX: AF_UNIX SOCK_SEQPACKET, which preserves message
 * boundaries exactly like a QNX message. */
typedef struct ambre_port_chan ambre_port_chan_t;   /* server side */
typedef struct ambre_port_conn ambre_port_conn_t;   /* client side */

int ambre_port_chan_open(const char *path, ambre_port_chan_t **out);
void ambre_port_chan_close(ambre_port_chan_t *c);
/* Receive one message, bounded wait. Returns the byte count, or a negative
 * error. `peer` receives the sender identity used for access control. */
int ambre_port_chan_recv(ambre_port_chan_t *c, void *buf, size_t cap,
                         uint64_t timeout_us, uint32_t *peer);
int ambre_port_chan_reply(ambre_port_chan_t *c, uint32_t peer,
                          const void *buf, size_t len);

int ambre_port_conn_open(const char *path, ambre_port_conn_t **out);
void ambre_port_conn_close(ambre_port_conn_t *c);
/* Send one frame and wait for the ack. `latency_ns` (optional) is the
 * round-trip cost measured at the producer: that is PERF-05 (< 50 us at p99)
 * and feasibility essay E4. */
int ambre_port_conn_send(ambre_port_conn_t *c, const void *buf, size_t len,
                         uint64_t timeout_us, uint64_t *latency_ns);

/* Request/reply: send one message and read the answer into `reply`.
 * Returns the number of bytes received, or a negative ambre_port_err_t.
 * On QNX this is a single MsgSend() (the reply is part of the transaction);
 * on POSIX it is a send followed by a bounded receive on the same
 * SOCK_SEQPACKET connection, which preserves the message boundary. */
#define AMBRE_PORT_HAS_CONN_REQUEST 1
int ambre_port_conn_request(ambre_port_conn_t *c, const void *req, size_t req_len,
                            void *reply, size_t reply_cap, uint64_t timeout_us,
                            uint64_t *latency_ns);

/* Request/reply: send one message and read the server's answer into `reply`.
 * Returns the number of bytes received, or a negative ambre_port_err_t. This
 * maps onto QNX MsgSend(), where the reply is part of the same transaction;
 * on POSIX it is a send followed by a bounded receive on the same
 * connection. */
int ambre_port_conn_request(ambre_port_conn_t *c, const void *req,
                            size_t req_len, void *reply, size_t reply_cap,
                            uint64_t timeout_us, uint64_t *latency_ns);

/* QNX-05: a producer's death is detected by its connection closing, with no
 * polling. Returns the peer id that disconnected, or 0 if none. */
uint32_t ambre_port_chan_poll_disconnect(ambre_port_chan_t *c);

/* ------------------------------------------------------- access control --- */

/* SEC-01: decide whether `peer` may perform `op`. On QNX this mirrors the
 * security policy types in image/ambre.sec; the decision function itself is
 * portable so it can be tested on the PC. */
/* Process types, mirroring the types declared in image/ambre.sec. */
typedef enum {
    AMBRE_PEER_JOURNAL  = 1,    /* edr-journal: the only raw-disk writer      */
    AMBRE_PEER_CAPTURE  = 2,    /* edr-capture: may write the input channel   */
    AMBRE_PEER_PRODUCER = 3,    /* edr-replay, CAN or zenoh producers         */
    AMBRE_PEER_SERVICE  = 4,    /* edrctl and the read path                   */
    AMBRE_PEER_ARCHIVE  = 5,    /* edr-archive: reads frozen segments         */
    AMBRE_PEER_UNKNOWN  = 99
} ambre_peer_type_t;

typedef enum {
    AMBRE_OP_WRITE_INPUT = 1,   /* producers only                            */
    AMBRE_OP_READ_STATUS = 2,   /* service tools only                        */
    AMBRE_OP_READ_EVENTS = 3,   /* service tools only                        */
    AMBRE_OP_RAW_DISK    = 4    /* edr-journal only                          */
} ambre_access_op_t;

int ambre_port_access_allowed(uint32_t peer_type, ambre_access_op_t op);

/* --------------------------------------------------------- supervision ---- */

/* QNX-06 / SEC-02: the recorder cannot be disabled. This reports whether the
 * current process is supervised (respawned on death) and by what. */
bool ambre_port_supervised(void);

#endif /* AMBRE_PORT_H */
