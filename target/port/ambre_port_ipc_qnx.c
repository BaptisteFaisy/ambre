/*
 * ambre_port_ipc_qnx.c — QNX 8.0 message passing and pulse transport.
 *
 * Implements the client/server transport for QNX:
 *   - ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT) (QNX-03, QNX-05)
 *   - name_attach / name_open so a client finds the server by its service path
 *   - name_close / name_detach to release the connection and the name
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>     /* name_attach / name_open */
#include <sys/neutrino.h>
#include <sys/procmgr.h>      /* PROCMGR_AID_PUBLIC_CHANNEL */
#include <unistd.h>

#define AMBRE_QNX_PATH_MAX 104u
#define AMBRE_QNX_MAX_CONNS 32u
#define AMBRE_QNX_NAME_ROOT "/dev/name/local/"

/* name_attach() publishes a server under /dev/name/local/ and the registered
 * name cannot hold a leading slash, so a service path is flattened:
 * "/dev/edr/journal" -> "dev-edr-journal". BOTH sides of the transport use this
 * mapping, which is why the command lines (--listen /dev/edr/journal,
 * --journal /dev/edr/journal) are unchanged between the PC and the target.
 *
 * Why a name and not a resource manager: the frozen frame protocol is one raw
 * message per MsgSend with its reply in the same transaction (ACQ-01, QNX-03).
 * name_attach()/name_open() is exactly QNX's "pathname-to-server-connection
 * mapping, without having to become a full resource manager" (Library
 * Reference). The channel is created here with _NTO_CHF_DISCONNECT so a dead
 * producer is reported by the kernel (QNX-05) instead of being polled. */

static void qnx_service_name(const char *path, char *out, size_t cap)
{
    const char *p = path;
    size_t n = 0u;

    while (*p == '/') {
        p++;
    }
    for (; *p != '\0' && n + 1u < cap; p++) {
        out[n++] = (*p == '/') ? '-' : *p;
    }
    out[n] = '\0';
}

typedef struct {
    rcvid_t  rcvid;   /* the message being answered                          */
    uint32_t scoid;   /* the connection: stable for the whole connection     */
    uint32_t peer;    /* the peer id announced by the server for that frame  */
    bool     active;
} qnx_peer_slot_t;

struct ambre_port_chan {
    int              chid;
    name_attach_t   *attach;
    char             path[AMBRE_QNX_PATH_MAX];
    qnx_peer_slot_t slots[AMBRE_QNX_MAX_CONNS];
    uint32_t        next_peer;
    uint32_t        disconnected_peer;
};

struct ambre_port_conn {
    int  coid;                  /* ConnectAttach() returns an int (scoid) */
    char path[AMBRE_QNX_PATH_MAX];
};

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_chan_open(const char *path, ambre_port_chan_t **out)
{
    ambre_port_chan_t *c;
    char name[AMBRE_QNX_PATH_MAX];
    name_attach_t *attach;
    dispatch_t *dpp;
    int chid;

    if (path == NULL || out == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    qnx_service_name(path, name, sizeof(name));
    chid = ChannelCreate(_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT);
    if (chid < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    /* Publishing a name needs the public-channel ability on a secured system
     * (image/ambre.sec grants it); root already holds it. */
    /* The ability word packs the identifier, the operation AND the domains it
     * applies to (root and non-root); the list then ends with PROCMGR_AID_EOL.
     * Without the domain flags the request applies to nobody and the channel
     * stays private: a client then finds no server at all. */
    (void)procmgr_ability(0,
                          PROCMGR_AID_PUBLIC_CHANNEL | PROCMGR_AOP_ALLOW |
                          PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT,
                          PROCMGR_AID_EOL);
    dpp = dispatch_create_channel(chid, 0u);
    attach = (dpp != NULL) ? name_attach(dpp, name, 0u) : NULL;
    if (attach == NULL) {
        ChannelDestroy(chid);
        return AMBRE_PORT_ERR_IO;
    }
    c = (ambre_port_chan_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        (void)name_detach(attach, 0u);
        ChannelDestroy(chid);
        return AMBRE_PORT_ERR_IO;
    }
    c->chid = chid;
    c->attach = attach;
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
    if (c->attach != NULL) {
        /* name_detach() removes the name and destroys the dispatch, which owns
         * the channel created above. */
        (void)name_detach(c->attach, 0u);
    } else if (c->chid >= 0) {
        ChannelDestroy(c->chid);
    }
    free(c);
}

/* QNX-only: verified on target, syntax-checked on PC */
/* Map a received message to its peer id.
 *
 * The slot is keyed by the CONNECTION (scoid), not by the message (rcvid):
 * the rcvid changes with every message, and a connection that dies takes its
 * slot with it. Keying by rcvid and never freeing the slot on disconnect
 * filled the table with stale entries - after AMBRE_QNX_MAX_CONNS connections
 * the table was full and every reply went to the wrong transaction, so the
 * client got an empty answer. */
static uint32_t map_qnx_rcvid(ambre_port_chan_t *c, rcvid_t rcvid, uint32_t scoid)
{
    for (uint32_t i = 0u; i < AMBRE_QNX_MAX_CONNS; i++) {
        if (c->slots[i].active && c->slots[i].scoid == scoid) {
            c->slots[i].rcvid = rcvid;
            return c->slots[i].peer;
        }
    }
    for (uint32_t i = 0u; i < AMBRE_QNX_MAX_CONNS; i++) {
        if (!c->slots[i].active) {
            c->slots[i].rcvid = rcvid;
            c->slots[i].scoid = scoid;
            c->slots[i].peer = c->next_peer++;
            c->slots[i].active = true;
            return c->slots[i].peer;
        }
    }
    return 1u;
}

/* A connection died: its slot goes back to the pool (QNX-05). */
static void free_qnx_scoid(ambre_port_chan_t *c, uint32_t scoid)
{
    for (uint32_t i = 0u; i < AMBRE_QNX_MAX_CONNS; i++) {
        if (c->slots[i].active && c->slots[i].scoid == scoid) {
            c->slots[i].active = false;
            return;
        }
    }
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
            if (p->code == _PULSE_CODE_DISCONNECT) {
                free_qnx_scoid(c, (uint32_t)p->scoid);
                c->disconnected_peer = (uint32_t)p->value.sival_int;
            }
            continue;
        }
        if (peer != NULL) {
            *peer = map_qnx_rcvid(c, rcvid, (uint32_t)info.scoid);
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
    char name[AMBRE_QNX_PATH_MAX];
    char full[AMBRE_QNX_PATH_MAX + 32u];
    int coid;

    if (path == NULL || out == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    qnx_service_name(path, name, sizeof(name));
    /* name_open() takes the name as the server registered it. The server
     * registered it under the LOCAL namespace (/dev/name/local/<name>), so the
     * client asks for the same full path: the short form resolves to the
     * system manager instead of our channel. */
    snprintf(full, sizeof(full), "%s%s", AMBRE_QNX_NAME_ROOT, name);
    coid = name_open(full, 0);
    if (coid < 0) {
        coid = name_open(name, 0);
    }
    if (coid < 0) {
        return AMBRE_PORT_ERR_NOENT;
    }
    c = (ambre_port_conn_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        (void)name_close(coid);
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
        (void)name_close(c->coid);
    }
    free(c);
}

/* Arm a bounded request/reply: TimerTimeout takes an ABSOLUTE monotonic
 * deadline and applies to the states MsgSend blocks in (SEND then REPLY).
 * A NULL deadline cancels the timeout. */
static void arm_request_timeout(uint64_t timeout_us)
{
    _Uint64t now = 0u;
    _Uint64t deadline;

    if (timeout_us == 0u) {
        return;
    }
    (void)ClockTime(CLOCK_MONOTONIC, NULL, &now);
    deadline = now + (_Uint64t)timeout_us * 1000u;
    (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                       NULL, &deadline, NULL);
}

static void disarm_request_timeout(uint64_t timeout_us)
{
    if (timeout_us == 0u) {
        return;
    }
    (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY,
                       NULL, NULL, NULL);
}

/* QNX-only: verified with the real SDP.
 * One MsgSend() carries the request and brings the reply back: on QNX the
 * reply is part of the same transaction, and the server runs it at the
 * caller's priority (priority inheritance, spec 1.3). The caller's deadline
 * is armed with TimerTimeout() just before the send and cancelled after. */
static int conn_xfer_qnx(ambre_port_conn_t *c, const void *req, size_t req_len,
                         void *reply, size_t reply_cap, uint64_t timeout_us,
                         uint64_t *latency_ns)
{
    uint8_t ack[AMBRE_PORT_ACK_MAX];
    uint8_t *dst = (reply != NULL) ? (uint8_t *)reply : ack;
    size_t room = (reply != NULL) ? reply_cap : sizeof(ack);
    uint64_t t0;
    int rc;

    if (c == NULL || req == NULL || req_len == 0u || dst == NULL || room == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    arm_request_timeout(timeout_us);
    t0 = ambre_port_now_us();
    rc = MsgSend(c->coid, req, req_len, dst, room);
    disarm_request_timeout(timeout_us);
    if (rc < 0) {
        return (errno == ETIMEDOUT) ? AMBRE_PORT_ERR_TIMEOUT : AMBRE_PORT_ERR_IO;
    }
    if (latency_ns != NULL) {
        *latency_ns = (ambre_port_now_us() - t0) * 1000u;
    }
    /* MsgSend does NOT return the reply byte count: "any other return value is
     * the status from MsgReply()" (QNX Library Reference, MsgSend). A reply is
     * fully in the buffer iff the call returned EOK, so success is 0 here; the
     * caller reads the reply's length from its own header. */
    return AMBRE_PORT_OK;
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
