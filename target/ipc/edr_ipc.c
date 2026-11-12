/*
 * edr_ipc.c — Wire protocol helpers (contract: target/edr_ipc.h, frozen).
 *
 * One message = one datagram: a 16-byte header (magic, version, type, flags,
 * body_len) followed by the body. The ceiling is ACQ-01: a whole message
 * never exceeds AMBRE_IPC_FRAME_MAX (4096 bytes), so the largest body is
 * AMBRE_IPC_MAX_BODY (4080 bytes).
 *
 * Endianness: the contract is little-endian and every field has a fixed
 * width, so the header travels by copy (memcpy), exactly like the on-disk
 * structures in fmt/. Little-endian hosts (x86, ARM) are the targets.
 *
 * Error convention: 0 = AMBRE_OK, negative = ambre_err_t (fmt/ambre_fmt.h).
 */

#include "edr_ipc_ext.h"

#include <string.h>

size_t ambre_ipc_frame_total(const ambre_ipc_hdr_t *h)
{
    if (h == NULL) {
        return 0u;
    }
    return (size_t)AMBRE_IPC_HDR_SIZE + (size_t)h->body_len;
}

size_t ambre_ipc_header_init(ambre_ipc_hdr_t *h, ambre_ipc_msg_t type,
                             uint32_t body_len)
{
    if (h == NULL) {
        return 0u;
    }
    memset(h, 0, sizeof(*h));
    h->magic = AMBRE_IPC_MAGIC;
    h->version = AMBRE_IPC_VERSION;
    h->msg_type = (uint8_t)type;
    h->body_len = body_len;
    /* The body is announced even when it is refused: a caller that ignores
     * the return value still hands the receiver a header it will reject. */
    if (body_len > AMBRE_IPC_MAX_BODY) {
        return 0u;
    }
    return (size_t)AMBRE_IPC_HDR_SIZE + (size_t)body_len;
}

int ambre_ipc_header_validate(const ambre_ipc_hdr_t *h)
{
    if (h == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (h->magic != AMBRE_IPC_MAGIC) {
        return AMBRE_ERR_BAD_MAGIC;
    }
    if (h->version != AMBRE_IPC_VERSION) {
        return AMBRE_ERR_BAD_VER;
    }
    if (h->body_len > AMBRE_IPC_MAX_BODY) {
        return AMBRE_ERR_TOO_BIG;
    }
    return AMBRE_OK;
}

int ambre_ipc_build(void *buf, size_t cap, ambre_ipc_msg_t type,
                    const void *body, uint32_t body_len)
{
    ambre_ipc_hdr_t hdr;
    size_t total;

    if (buf == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (body_len > 0u && body == NULL) {
        return AMBRE_ERR_PARAM;
    }
    total = ambre_ipc_header_init(&hdr, type, body_len);
    if (total == 0u) {
        return AMBRE_ERR_TOO_BIG;        /* body above the frame ceiling */
    }
    if (cap < total) {
        return AMBRE_ERR_PARAM;          /* caller buffer too small */
    }
    memcpy(buf, &hdr, sizeof(hdr));
    if (body_len > 0u) {
        memcpy((uint8_t *)buf + AMBRE_IPC_HDR_SIZE, body, body_len);
    }
    return (int)total;
}

size_t ambre_ipc_frame_len(const void *buf)
{
    ambre_ipc_hdr_t hdr;

    if (buf == NULL) {
        return 0u;
    }
    memcpy(&hdr, buf, sizeof(hdr));
    if (ambre_ipc_header_validate(&hdr) != AMBRE_OK) {
        return 0u;
    }
    return ambre_ipc_frame_total(&hdr);
}

int ambre_ipc_frame_check(const void *buf, size_t received)
{
    ambre_ipc_hdr_t hdr;
    size_t total;
    int rc;

    if (buf == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (received < (size_t)AMBRE_IPC_HDR_SIZE) {
        return AMBRE_ERR_TORN;           /* not even a header arrived */
    }
    memcpy(&hdr, buf, sizeof(hdr));
    rc = ambre_ipc_header_validate(&hdr);
    if (rc != AMBRE_OK) {
        return rc;
    }
    total = ambre_ipc_frame_total(&hdr);
    if (received < total) {
        return AMBRE_ERR_TORN;           /* truncated datagram */
    }
    if (received > total) {
        return AMBRE_ERR_TOO_BIG;        /* more bytes than announced */
    }
    return AMBRE_OK;
}
