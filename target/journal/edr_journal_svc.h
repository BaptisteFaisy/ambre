/*
 * edr_journal_svc.h — Service layer of edr-journal, on top of fmt/ambre_journal.h.
 *
 * The journal engine (fmt/) knows how to write, seal and read back events.
 * This layer knows how to SERVE them: it opens the partition through the
 * portability layer, runs the recovery before any new capture (REP-01),
 * applies the access control (SEC-01), answers the frozen protocol of
 * target/edr_ipc.h, and exposes the state in text (OBS-02).
 *
 * Nothing allocates: the service state is a fixed-size struct, the caller
 * supplies the reply buffer, and the disk is reached through the
 * ambre_disk_t vtable of the portability layer (GEL-03, STO-01).
 *
 * The logic is exercised on the PC without any socket: ambre_edr_service_handle()
 * takes the raw bytes of one message and fills the reply buffer.
 *
 * PEER IDENTITY (SEC-01)
 * ----------------------
 * ambre_edr_service_handle() takes the peer type as an argument: on QNX it
 * comes from the security policy (image/ambre.sec) through the transport,
 * on the PC the sender announces it in the "reserved" byte of the header
 * (see AMBRE_EDR_PEER_BYTE). The service NEVER guesses a type by itself.
 */

#ifndef EDR_JOURNAL_SVC_H
#define EDR_JOURNAL_SVC_H

#include "../port/ambre_port.h"
#include "../edr_ipc.h"
#include "../../fmt/ambre_journal.h"

/* Biggest answer the transport can carry (ACQ-01 ceiling). */
#define AMBRE_EDR_SVC_REPLY_MAX   AMBRE_IPC_FRAME_MAX
/* Size of the text status buffer (OBS-02). */
#define AMBRE_EDR_SVC_TEXT_MAX    512u
/* Offsets of the two header bytes the service cares about are NOT used: the
 * service only ever reads the named fields of ambre_ipc_hdr_t. The message
 * type is hdr.msg_type, and the sender announces its peer type in the spare
 * hdr.reserved byte (PC harness; on QNX the kernel-provided type wins). */
/* Number of peer slots the process keeps for the archive notifications. */
#define AMBRE_EDR_SVC_PEER_SLOTS  8u

/* Reply message types: the request type with the reply bit (0x80) set. */
#define AMBRE_EDR_REPLY_EXPORT    ((uint8_t)(AMBRE_MSG_EXPORT | 0x80u))
#define AMBRE_EDR_REPLY_VERIFY    ((uint8_t)(AMBRE_MSG_VERIFY | 0x80u))

#pragma pack(push, 1)

/* EXPORT request: which event, and where to resume the pagination. */
typedef struct {
    uint32_t event_id;
    uint32_t first_block;       /* index of the first raw block to return   */
} ambre_edr_export_req_t;

/* EXPORT reply body, followed by block_count raw 512-byte blocks. */
typedef struct {
    uint32_t event_id;
    uint32_t first_block;
    uint32_t block_count;
    uint32_t total_blocks;
} ambre_edr_export_body_t;

/* VERIFY request body. */
typedef struct {
    uint32_t event_id;
} ambre_edr_verify_req_t;

/* VERIFY reply body: the fingerprint of the event (SEC-04, EXP-02). */
typedef struct {
    uint32_t event_id;
    uint32_t block_count;
    uint32_t fingerprint;       /* CRC-32C over the raw blocks of the event */
    uint8_t  file_complete;
    uint8_t  locked;
    uint8_t  reserved[2];
} ambre_edr_verify_body_t;

/* LIST reply body: the events known to the journal (EXP-01). */
typedef struct {
    uint32_t count;
    ambre_ipc_event_line_t lines[AMBRE_MAX_EVENTS];
} ambre_edr_list_body_t;

/* ACK body: the journal's answer to an order it accepted. */
typedef struct {
    uint32_t event_id;
    uint32_t status;            /* 0 = ok                                     */
    uint64_t freeze_us;         /* trigger -> acquired, measured by the
                                 * journal (PERF-02, PERF-03)                 */
} ambre_edr_ack_body_t;

/* NAK body: why the order was refused (SEC-01, SEC-05). */
typedef struct {
    int32_t  code;              /* negative ambre_port_err_t / ambre_err_t   */
    uint32_t event_id;
} ambre_edr_nak_body_t;

/* EVENT_READY body (journal -> archive): a sealed event can be archived. */
typedef struct {
    uint32_t event_id;
    uint8_t  locked;
    uint8_t  file_complete;
    uint8_t  reserved[2];
} ambre_edr_event_ready_body_t;

#pragma pack(pop)

/* ------------------------------------------------------------- service --- */

/* Notification sink: the process turns it into a pulse or a datagram. */
typedef int (*ambre_edr_notify_fn)(void *ctx, const void *msg, size_t len);

typedef struct {
    ambre_port_disk_t  *disk;               /* owned by the service          */
    ambre_disk_t        vt;                 /* vtable copy: the engine needs  */
                                            /* a writable ambre_disk_t        */
    ambre_journal_t     journal;            /* the engine state (fixed size)  */
    ambre_journal_cfg_t cfg;                /* session, mode, cadences        */
    uint64_t            start_us;           /* uptime reference               */
    uint64_t            last_freeze_us;     /* trigger -> acquired            */
    uint64_t            worst_freeze_us;    /* worst case seen                */
    uint32_t            last_event_id;      /* last event served              */
    uint32_t            refused_count;      /* SEC-01 refusals                */
    uint32_t            malformed_count;    /* SEC-05 malformed frames        */
    uint8_t             health;             /* 0 ok, 1 storage, 2 memory full */
    ambre_edr_notify_fn notify;             /* archive notification           */
    void               *notify_ctx;
} ambre_edr_service_t;

typedef struct {
    const char          *disk_spec;         /* file or /dev/hd0tNNN           */
    uint64_t             disk_size;         /* 0 = AMBRE_JOURNAL_BYTES        */
    ambre_journal_cfg_t  cfg;               /* journal configuration          */
} ambre_edr_service_cfg_t;

/* Open the partition and the journal. A virgin partition is formatted, an
 * existing one goes through the recovery first (REP-01). */
int  ambre_edr_service_open(ambre_edr_service_t *svc,
                            const ambre_edr_service_cfg_t *cfg);
void ambre_edr_service_close(ambre_edr_service_t *svc);

/* Serve one message. Returns 0 when the reply buffer holds the answer, a
 * negative code when the order was refused (the reply then holds a NAK). */
int  ambre_edr_service_handle(ambre_edr_service_t *svc, uint32_t peer_type,
                              const void *msg, size_t len,
                              void *reply, size_t reply_cap, size_t *reply_len);

/* Periodic maintenance: flush cadence and segment rotation (GEL-07, STO-03). */
int  ambre_edr_service_tick(ambre_edr_service_t *svc, uint64_t now_us);

/* State in text: mode, segment, events, counters, health (OBS-02). */
int  ambre_edr_service_status_text(const ambre_edr_service_t *svc,
                                    char *buf, size_t cap);

/* Start-up self-test: write a test block, read it back, check the
 * configuration (OBS-04). */
int  ambre_edr_service_selftest(ambre_edr_service_t *svc);

/* GEL-06: no archive write while an event is open. */
bool ambre_edr_service_archive_allowed(const ambre_edr_service_t *svc);

/* Install the archive notification sink. */
void ambre_edr_service_set_notify(ambre_edr_service_t *svc,
                                  ambre_edr_notify_fn fn, void *ctx);

/* Peer type announced in the header, or AMBRE_PEER_UNKNOWN when absent. */
uint32_t ambre_edr_announced_peer(const void *msg, size_t len);

/* ------------------------------------------- internals (two modules) ---- */

/* Build a reply message: header + body. Returns the size, 0 when it does
 * not fit in the buffer. */
int  ambre_edr_build_reply(void *buf, size_t cap, uint8_t msg_type,
                           const void *body, size_t body_len);

/* SEC-01 decisions: the input path belongs to capture and to the producers,
 * the read path to the service tools and to the archive. */
int  ambre_edr_input_allowed(uint32_t peer_type);
int  ambre_edr_read_allowed(uint32_t peer_type, ambre_access_op_t op);

/* Build a NAK, count the incident and log it (SEC-01, SEC-05). */
int  ambre_edr_refuse(ambre_edr_service_t *svc, uint32_t peer_type,
                      uint8_t msg_type, int code,
                      void *reply, size_t reply_cap, size_t *reply_len);

/* Message handlers (edr_journal_svc_handlers.c), reached through the dispatch
 * of ambre_edr_service_handle() once the frame itself is known to be sound. */
int  ambre_edr_handle_input(ambre_edr_service_t *svc, uint32_t peer_type,
                            uint8_t msg_type, const uint8_t *body,
                            size_t body_len, void *reply, size_t reply_cap,
                            size_t *reply_len);
int  ambre_edr_handle_service(ambre_edr_service_t *svc, uint32_t peer_type,
                              uint8_t msg_type, const uint8_t *body,
                              size_t body_len, void *reply, size_t reply_cap,
                              size_t *reply_len);

#endif /* EDR_JOURNAL_SVC_H */
