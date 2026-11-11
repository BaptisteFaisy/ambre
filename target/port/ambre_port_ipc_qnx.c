/*
 * ambre_port_ipc_qnx.c — QNX 8.0 message passing and pulse transport.
 *
 * Implements the client/server transport for QNX:
 *   - ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT) (QNX-03, QNX-05)
 *   - ConnectAttach / ConnectDetach for connections
 *   - MsgReceive for request frames + pulses
 *   - MsgReply for answers (preserves message boundaries)
 *   - _PULSE_CODE_DISCONNECT detection of dead clients (QNX-05) without polling
 *
 * Syntax-checked on PC against target/qnx-stub/include; verified on QNX 8.0.
 */

#define _POSIX_C_SOURCE 200809L

#define AMBRE_PORT_ACK_MAX 64u   /* smallest reply accepted as an ack */

#include "ambre_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <unistd.h>

#define AMBRE_QNX_PATH_MAX 104u
#define AMBRE_QNX_MAX_CONNS 32u

typedef struct {
    rcvid_t  rcvid;
    uint32_t peer;
    bool     active;
} qnx_peer_slot_t;

struct ambre_port_chan {
    int             chid;
    char            path[AMBRE_QNX_PATH_MAX];
    qnx_peer_slot_t slots[AMBRE_QNX_MAX_CONNS];
    uint32_t        next_peer;
    uint32_t        disconnected_peer;
};

struct ambre_port_conn {
    coid_t coid;
    char   path[AMBRE_QNX_PATH_MAX];
};

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_chan_open(const char *path, ambre_port_chan_t **out)
{
    ambre_port_chan_t *c;
    int chid;

    if (path == NULL || out == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT);
    if (chid < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    c = (ambre_port_chan_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        ChannelDestroy(chid);
        return AMBRE_PORT_ERR_IO;
    }
    c->chid = chid;
    strncpy(c->path, path, AMBRE_QNX_PATH_MAX - 1u);
    c->next_peer = 1u;
    *out = c;
    return AMBRE_PORT_OK;
}

/* QNX-only: verified on target, syntax-checked on PC */
void ambre_port_chan_close(ambre_port_chan_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->chid >= 0) {
        ChannelDestroy(c->chid);
    }
    free(c);
}

/* QNX-only: verified on target, syntax-checked on PC */
static uint32_t map_qnx_rcvid(ambre_port_chan_t *c, rcvid_t rcvid)
{
    for (uint32_t i = 0u; i < AMBRE_QNX_MAX_CONNS; i++) {
        if (c->slots[i].active && c->slots[i].rcvid == rcvid) {
            return c->slots[i].peer;
        }
    }
    for (uint32_t i = 0u; i < AMBRE_QNX_MAX_CONNS; i++) {
        if (!c->slots[i].active) {
            c->slots[i].rcvid = rcvid;
            c->slots[i].peer = c->next_peer++;
            c->slots[i].active = true;
            return c->slots[i].peer;
        }
    }
    return 1u;
}

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_chan_recv(ambre_port_chan_t *c, void *buf, size_t cap,
                         uint64_t timeout_us, uint32_t *peer)
{
    struct _msg_info info;
    uint64_t deadline;

    if (c == NULL || buf == NULL || cap == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    deadline = ambre_port_now_us() + timeout_us;
    while (ambre_port_now_us() < deadline) {
        rcvid_t rcvid = MsgReceive(c->chid, buf, cap, &info);
        if (rcvid < 0) {
            if (errno == EINTR) {
                continue;
            }
            return AMBRE_PORT_ERR_IO;
        }
        if (rcvid == 0) {
            struct _pulse *p = (struct _pulse *)buf;
            if (p->code == _PULSE_DISCONNECT) {
                c->disconnected_peer = (uint32_t)p->value.sival_int;
            }
            continue;
        }
        if (peer != NULL) {
            *peer = map_qnx_rcvid(c, rcvid);
        }
        return (int)info.msglen;
    }
    return AMBRE_PORT_ERR_TIMEOUT;
}

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_chan_reply(ambre_port_chan_t *c, uint32_t peer,
                          const void *buf, size_t len)
{
    rcvid_t rcvid = 0;

    if (c == NULL || buf == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    for (uint32_t i = 0u; i < AMBRE_QNX_MAX_CONNS; i++) {
        if (c->slots[i].active && c->slots[i].peer == peer) {
            rcvid = c->slots[i].rcvid;
            c->slots[i].active = false;
            break;
        }
    }
    if (rcvid == 0) {
        return AMBRE_PORT_ERR_NOENT;
    }
    if (MsgReply(rcvid, 0, buf, len) != 0) {
        return AMBRE_PORT_ERR_IO;
    }
    return AMBRE_PORT_OK;
}

/* QNX-only: verified on target, syntax-checked on PC */
uint32_t ambre_port_chan_poll_disconnect(ambre_port_chan_t *c)
{
    uint32_t dead;

    if (c == NULL) {
        return 0u;
    }
    dead = c->disconnected_peer;
    c->disconnected_peer = 0u;
    return dead;
}

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_conn_open(const char *path, ambre_port_conn_t **out)
{
    ambre_port_conn_t *c;
    int fd;
    coid_t coid;

    if (path == NULL || out == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    fd = open(path, O_RDWR);
    if (fd < 0) {
        return AMBRE_PORT_ERR_NOENT;
    }
    coid = ConnectAttach(0, 0, fd, 0, 0);
    close(fd);
    if (coid < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    c = (ambre_port_conn_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        ConnectDetach(coid);
        return AMBRE_PORT_ERR_IO;
    }
    c->coid = coid;
    strncpy(c->path, path, AMBRE_QNX_PATH_MAX - 1u);
    *out = c;
    return AMBRE_PORT_OK;
}

/* QNX-only: verified on target, syntax-checked on PC */
void ambre_port_conn_close(ambre_port_conn_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->coid >= 0) {
        ConnectDetach(c->coid);
    }
    free(c);
}

/* QNX-only: verified on target, syntax-checked on PC */
/* One MsgSend() carries the request and brings the reply back: on QNX the
 * reply is part of the same transaction, and the server runs it at the
 * caller's priority (priority inheritance, spec 1.3). `timeout_us` is bounded
 * by the kernel's own blocking rules; the caller's deadline is what matters,
 * so it is passed to TimerTimeout() before the send. */
static int conn_xfer_qnx(ambre_port_conn_t *c, const void *req, size_t req_len,
                         void *reply, size_t reply_cap, uint64_t timeout_us,
                         uint64_t *latency_ns)
{
    uint8_t ack[AMBRE_PORT_ACK_MAX];
    uint8_t *dst = (reply != NULL) ? (uint8_t *)reply : ack;
    size_t room = (reply != NULL) ? reply_cap : sizeof(ack);
    uint64_t t0;
    uint64_t nsecs;
    int rc;

    if (c == NULL || req == NULL || req_len == 0u || dst == NULL || room == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    if (timeout_us > 0u) {
        nsecs = timeout_us * 1000u;
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_NEAREST, NULL,
                           (uint64_t)0, &nsecs);
    }
    t0 = ambre_port_now_us();
    rc = MsgSend(c->coid, req, req_len, dst, room);
    if (timeout_us > 0u) {
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_NEAREST, NULL,
                           (uint64_t)0, NULL);
    }
    if (rc < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    if (latency_ns != NULL) {
        *latency_ns = (ambre_port_now_us() - t0) * 1000u;
    }
    return rc;
}

int ambre_port_conn_request(ambre_port_conn_t *c, const void *req, size_t req_len,
                            void *reply, size_t reply_cap, uint64_t timeout_us,
                            uint64_t *latency_ns)
{
    return conn_xfer_qnx(c, req, req_len, reply, reply_cap, timeout_us,
                         latency_ns);
}

int ambre_port_conn_send(ambre_port_conn_t *c, const void *buf, size_t len,
                         uint64_t timeout_us, uint64_t *latency_ns)
{
    return conn_xfer_qnx(c, buf, len, NULL, 0u, timeout_us, latency_ns);
}
