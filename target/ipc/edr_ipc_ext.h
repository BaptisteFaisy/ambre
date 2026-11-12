/*
 * edr_ipc_ext.h — Extra wire helpers that could not go into the frozen
 * target/edr_ipc.h (that header is a contract and is not modified).
 *
 * The three helpers below are the ones the five processes need on top of the
 * frozen ones, so that no process re-implements them:
 *
 *   ambre_ipc_build()      write header + body in one call
 *   ambre_ipc_frame_total()  wire size announced by a header
 *   ambre_ipc_frame_check()  validate a received datagram (header + length)
 *
 * Frame ceiling (ACQ-01): a whole message never exceeds AMBRE_IPC_FRAME_MAX
 * (4096 bytes), so the largest body is AMBRE_IPC_MAX_BODY (4080 bytes).
 */

#ifndef EDR_IPC_EXT_H
#define EDR_IPC_EXT_H

#include "../edr_ipc.h"
#include "../../fmt/ambre_fmt.h"   /* AMBRE_OK, AMBRE_ERR_* */

/* Largest body that still fits the 4096-byte frame ceiling. */
#define AMBRE_IPC_MAX_BODY ((uint32_t)(AMBRE_IPC_FRAME_MAX - AMBRE_IPC_HDR_SIZE))

/* Write a complete message (16-byte header then body) into `buf`.
 * Returns the total number of bytes written, or a negative ambre_err_t:
 *   AMBRE_ERR_PARAM    buf is NULL, body is NULL with a non-zero body_len,
 *                      or `cap` is smaller than the message
 *   AMBRE_ERR_TOO_BIG  body_len does not fit the frame ceiling
 */
int ambre_ipc_build(void *buf, size_t cap, ambre_ipc_msg_t type,
                    const void *body, uint32_t body_len);

/* Total wire size announced by a header: header + body (0 if h is NULL). */
size_t ambre_ipc_frame_total(const ambre_ipc_hdr_t *h);
/* The wire size a reply buffer announces: AMBRE_IPC_HDR_SIZE + body_len.
 * Returns 0 when the header is malformed. Used by the channel clients instead
 * of the transport's return value, which is a status on QNX (MsgSend). */
size_t ambre_ipc_frame_len(const void *buf);

/* Check a received datagram: the header must be valid AND the number of
 * bytes received must match the announced total.
 * Returns AMBRE_OK, or the error of the header (BAD_MAGIC, BAD_VER,
 * TOO_BIG), AMBRE_ERR_TORN when the datagram is shorter than announced and
 * AMBRE_ERR_TOO_BIG when it carries extra bytes.
 */
int ambre_ipc_frame_check(const void *buf, size_t received);

#endif /* EDR_IPC_EXT_H */
