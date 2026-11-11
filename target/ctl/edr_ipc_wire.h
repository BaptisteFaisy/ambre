/*
 * edr_ipc_wire.h — The service half of the wire protocol, as edrctl and
 * edr-replay use it.
 *
 * Everything the frozen contract target/edr_ipc.h already declares is taken
 * from there: the 16-byte header, the FRAME and EXTERNAL bodies, the
 * STATUS_REPLY body, the event lines, and the framing helpers of
 * target/ipc/edr_ipc_ext.h. Nothing is duplicated here.
 *
 * This file only adds what the frozen header cannot declare, because it is
 * frozen:
 *
 *   - the request bodies of AMBRE_MSG_EXPORT and AMBRE_MSG_VERIFY, and the
 *     type of their replies (0xB2 and 0xB3, following the "request | 0x80"
 *     rule the frozen header documents for its own replies);
 *   - the bodies of the generic ACK and NAK;
 *   - the maximum number of raw blocks one EXPORT reply may carry.
 *
 * Those layouts mirror the service contract published by the journal-svc
 * agent, so that the two processes meet on the wire and not on each other's
 * internal headers (docs/W2-PLAN.md). Every size is asserted below: a change
 * on either side fails to compile instead of silently misreading a datagram.
 *
 * Peer identity (SEC-01): the sender announces its ambre_peer_type_t in the
 * header's `reserved` byte. edrctl announces AMBRE_PEER_SERVICE and edr-replay
 * announces AMBRE_PEER_PRODUCER, so the journal can refuse an intruder — and a
 * service tool never writes to a producer input.
 */

#ifndef EDR_IPC_WIRE_H
#define EDR_IPC_WIRE_H

#include <string.h>

#include "../edr_ipc.h"                  /* frozen contract                  */
#include "../port/ambre_port.h"          /* ambre_peer_type_t, error codes   */
#include "../ipc/edr_ipc_ext.h"          /* ambre_ipc_build(), frame_check() */

/* Reply types the frozen enumeration does not name (request | 0x80). */
#define CTL_MSG_EXPORT_REPLY  ((uint8_t)0xB2u)
#define CTL_MSG_VERIFY_REPLY  ((uint8_t)0xB3u)

/* An EXPORT reply carries at most this many raw 512-byte blocks, so that one
 * datagram stays under the ACQ-01 ceiling: 16 + 16 + 7 * 512 = 3616 bytes. */
#define CTL_EXPORT_BLOCKS_MAX 7u

/* Header flags bit: ask the service to log this read and to increment the
 * "ignition cycle to download" counter (EXP-06). The frozen header declares a
 * `flags` byte without assigning any meaning to it, and the frozen request
 * bodies have no room for the request, so the bit travels there. */
#define CTL_REQ_FLAG_LOG_READ 0x01u

#pragma pack(push, 1)

/* AMBRE_MSG_EXPORT request: read the raw blocks of one event, paginated.
 * Send it again with first_block += block_count until first_block reaches
 * total_blocks. */
typedef struct {
    uint32_t event_id;
    uint32_t first_block;     /* pagination cursor, in blocks of the event   */
} ctl_ipc_export_req_t;

/* AMBRE_MSG_VERIFY request. */
typedef struct {
    uint32_t event_id;
} ctl_ipc_verify_req_t;

/* AMBRE_MSG_EXPORT_REPLY (0xB2): this header, then `block_count` raw journal
 * blocks, exactly as they sit on the partition (block CRCs included). */
typedef struct {
    uint32_t event_id;
    uint32_t first_block;
    uint32_t block_count;
    uint32_t total_blocks;
} ctl_ipc_export_hdr_t;

/* AMBRE_MSG_VERIFY_REPLY (0xB3): `fingerprint` is the CRC-32C chained over the
 * event's raw blocks in order (SEC-04 fingerprint chain). */
typedef struct {
    uint32_t event_id;
    uint32_t block_count;
    uint32_t fingerprint;
    uint8_t  file_complete;
    uint8_t  locked;
} ctl_ipc_verify_body_t;

/* AMBRE_MSG_ACK (0x80) body. */
typedef struct {
    uint32_t event_id;
    uint32_t status;          /* 0 = ok, negative = ambre_err_t              */
    uint64_t freeze_us;       /* trigger -> acquired (PERF-02, PERF-03)      */
} ctl_ipc_ack_body_t;

/* AMBRE_MSG_NAK (0x81) body: the service refused the request. */
typedef struct {
    int32_t  code;            /* negative ambre_err_t / ambre_port_err_t     */
    uint32_t event_id;
} ctl_ipc_nak_body_t;

#pragma pack(pop)

_Static_assert(sizeof(ctl_ipc_export_req_t) == 8, "export request");
_Static_assert(sizeof(ctl_ipc_verify_req_t) == 4, "verify request");
_Static_assert(sizeof(ctl_ipc_export_hdr_t) == 16, "export header");
_Static_assert(sizeof(ctl_ipc_verify_body_t) == 14, "verify body");
_Static_assert(sizeof(ctl_ipc_ack_body_t) == 16, "ack body");
_Static_assert(sizeof(ctl_ipc_nak_body_t) == 8, "nak body");
_Static_assert(AMBRE_IPC_HDR_SIZE + sizeof(ctl_ipc_export_hdr_t)
               + CTL_EXPORT_BLOCKS_MAX * AMBRE_BLOCK_SIZE
               <= AMBRE_IPC_FRAME_MAX,
               "an export chunk must fit in one datagram");

/* Write a 16-byte header announcing `peer` (SEC-01) for a body of `body_len`
 * bytes that the caller copies itself. Returns the total wire size, or a
 * negative error when the body would break the ACQ-01 ceiling. */
static inline int ctl_ipc_header(void *buf, size_t cap, uint8_t type,
                                 ambre_peer_type_t peer, uint8_t flags,
                                 uint32_t body_len)
{
    ambre_ipc_hdr_t *h = (ambre_ipc_hdr_t *)buf;
    size_t total = ambre_ipc_header_init(h, (ambre_ipc_msg_t)type, body_len);

    if (total == 0u || total > cap) {
        return AMBRE_ERR_TOO_BIG;
    }
    h->reserved = (uint8_t)peer;
    h->flags = flags;
    return (int)total;
}

/* Build one complete message (header + body). Thin wrapper around
 * ctl_ipc_header(), so the framing exists in exactly one place. */
static inline int ctl_ipc_build(void *buf, size_t cap, uint8_t type,
                                ambre_peer_type_t peer, uint8_t flags,
                                const void *body, uint32_t body_len)
{
    int rc = ctl_ipc_header(buf, cap, type, peer, flags, body_len);

    if (rc > 0 && body_len > 0u && body != NULL) {
        memcpy((uint8_t *)buf + AMBRE_IPC_HDR_SIZE, body, body_len);
    }
    return rc;
}

/* Pointer to the body of a received message (never a cast: the contract is
 * packed and travels by copy). */
static inline const void *ctl_ipc_body(const void *msg)
{
    return (const uint8_t *)msg + AMBRE_IPC_HDR_SIZE;
}

/* Message type of a received datagram, 0 when it is too short to hold one. */
static inline uint8_t ctl_ipc_type(const void *msg, size_t received)
{
    ambre_ipc_hdr_t h;

    if (msg == NULL || received < AMBRE_IPC_HDR_SIZE) {
        return 0u;
    }
    memcpy(&h, msg, sizeof(h));
    return h.msg_type;
}

#endif /* EDR_IPC_WIRE_H */
