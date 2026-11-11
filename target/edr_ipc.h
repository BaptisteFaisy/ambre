/*
 * edr_ipc.h — Wire protocol between the five Ambre processes.
 *
 * These are the messages that travel over the portability transport
 * (target/port/ambre_port.h): QNX messages and pulses on target, AF_UNIX
 * SOCK_SEQPACKET on the PC. One message per datagram, fixed-size headers,
 * little-endian, no pointers, no absolute time (VIE-02).
 *
 * Size budget (ACQ-01): a producer frame covers at most 10 ms and carries at
 * most 4 KiB, so AMBRE_IPC_FRAME_MAX below is the hard ceiling on the wire.
 */

#ifndef EDR_IPC_H
#define EDR_IPC_H

#include "../fmt/ambre_types.h"

#define AMBRE_IPC_MAGIC      0x45445231u   /* "EDR1" */
#define AMBRE_IPC_VERSION    1u
#define AMBRE_IPC_HDR_SIZE   16u
#define AMBRE_IPC_FRAME_MAX  4096u         /* ACQ-01 ceiling                 */
#define AMBRE_IPC_MAX_SAMPLES ((AMBRE_IPC_FRAME_MAX - AMBRE_IPC_HDR_SIZE \
                                - sizeof(ambre_ipc_frame_body_t)) \
                               / sizeof(ambre_sample_t))

typedef enum {
    /* producer -> edr-capture */
    AMBRE_MSG_FRAME        = 0x01,   /* a batch of class B samples           */
    AMBRE_MSG_EXTERNAL     = 0x02,   /* restraint / VRU / service signal     */
    /* edr-capture -> edr-journal */
    AMBRE_MSG_TRIGGER      = 0x10,   /* class A snapshot: freeze now (GEL-01)*/
    AMBRE_MSG_CONTINUE     = 0x11,   /* class A during the crash (GEL-04)    */
    AMBRE_MSG_SEAL         = 0x12,   /* end of event (GEL-05)                */
    AMBRE_MSG_GAP          = 0x13,   /* capture interruption (ACQ-06)        */
    /* edr-journal -> edr-archive (pulse on QNX, datagram on PC) */
    AMBRE_MSG_EVENT_READY  = 0x20,   /* a sealed event can be archived       */
    /* edrctl -> edr-journal */
    AMBRE_MSG_STATUS       = 0x30,
    AMBRE_MSG_LIST         = 0x31,
    AMBRE_MSG_EXPORT       = 0x32,
    AMBRE_MSG_VERIFY       = 0x33,
    /* replies: request type | 0x80 */
    AMBRE_MSG_ACK          = 0x80,
    AMBRE_MSG_NAK          = 0x81,
    AMBRE_MSG_STATUS_REPLY = 0xB0,
    AMBRE_MSG_LIST_REPLY   = 0xB1
} ambre_ipc_msg_t;

#pragma pack(push, 1)

/* Every message starts with this header. The trailing reserved2 field pads the
 * header to 16 bytes so the body starts on a 4-byte boundary; it must stay
 * zero. */
typedef struct {
    uint32_t magic;             /* AMBRE_IPC_MAGIC                           */
    uint8_t  version;           /* AMBRE_IPC_VERSION                         */
    uint8_t  msg_type;          /* ambre_ipc_msg_t                           */
    uint8_t  flags;
    uint8_t  reserved;
    uint32_t body_len;          /* bytes after the header                    */
    uint32_t reserved2;         /* padding to 16 bytes, zero                 */
} ambre_ipc_hdr_t;

/* AMBRE_MSG_FRAME body: a batch of class B samples covering <= 10 ms. */
typedef struct {
    uint64_t first_ts_us;       /* monotonic, set by the producer             */
    uint32_t sample_count;
    uint32_t producer_id;
} ambre_ipc_frame_body_t;

/* AMBRE_MSG_EXTERNAL body: a signal that triggers or locks an event. */
typedef struct {
    uint64_t ts_us;
    uint32_t signal_mask;       /* bit0 restraint, bit1 VRU, bit2 emergency
                                 * brake, bit3 AEB, bit4 service command      */
    uint32_t producer_id;
} ambre_ipc_external_body_t;

/* AMBRE_MSG_STATUS_REPLY body (EXP-01, OBS-02). */
typedef struct {
    uint8_t  mode;              /* 0 = event-only, 1 = continuous (GEL-08)    */
    uint8_t  health;            /* 0 ok, 1 storage fault, 2 memory full       */
    uint8_t  backend;           /* ambre_port_backend_t                       */
    uint8_t  supervised;        /* QNX-06                                     */
    uint32_t current_seg;
    uint32_t current_block;
    uint32_t open_events;
    uint32_t acquired_events;
    uint32_t sealed_events;
    uint32_t refused_no_space;
    uint64_t bytes_written;
    uint64_t flush_count;
    uint64_t uptime_ms;
    uint64_t last_freeze_us;    /* trigger -> acquired, software+media        */
    uint64_t worst_freeze_us;
} ambre_ipc_status_body_t;

/* One line of AMBRE_MSG_LIST_REPLY. */
typedef struct {
    uint32_t event_id;
    uint8_t  state;             /* ambre_ev_state_t                           */
    uint8_t  locked;            /* SEC-03: a locked event cannot be erased    */
    uint8_t  file_complete;     /* UN R160 5.4 "complete file"                */
    uint8_t  trig_reason;
    uint64_t trig_ts_us;
    uint64_t seq_emerg;
    uint32_t start_seg;
    uint32_t start_block;
} ambre_ipc_event_line_t;

#pragma pack(pop)

_Static_assert(sizeof(ambre_ipc_hdr_t) == AMBRE_IPC_HDR_SIZE, "hdr must be 16");

/* ------------------------------------------------------------- helpers --- */

/* Fill a header. Returns the total message size, or 0 when body_len is too
 * large for the transport ceiling. */
size_t ambre_ipc_header_init(ambre_ipc_hdr_t *h, ambre_ipc_msg_t type,
                             uint32_t body_len);

/* Validate a received header. Returns AMBRE_OK or a negative ambre_err_t. */
int ambre_ipc_header_validate(const ambre_ipc_hdr_t *h);

#endif /* EDR_IPC_H */
