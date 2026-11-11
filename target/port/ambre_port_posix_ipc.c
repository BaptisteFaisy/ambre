/*
 * ambre_port_posix_ipc.c — POSIX backend of the message transport.
 *
 * AF_UNIX SOCK_SEQPACKET preserves message boundaries exactly like a QNX
 * message, so the framing in target/edr_ipc.h is exercised for real on the PC:
 * one send is one receive, a short buffer is an error, never a partial frame.
 * The QNX backend replaces this with a channel, connections and pulses.
 *
 * Peer identity: QNX gives the server the caller's scoid and pid; here each
 * accepted connection gets a monotonic peer id, which is enough to exercise
 * the access-control decision (SEC-01) and the "producer died" detection
 * (QNX-05) without polling.
 */

#define _POSIX_C_SOURCE 200809L

#include "ambre_port.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define AMBRE_PORT_MAX_CONNS 32u
#define AMBRE_PORT_PATH_MAX  104u
#define AMBRE_PORT_ACK_MAX   64u   /* smallest reply we accept as an ack */

typedef struct {
    int      fd;
    uint32_t peer;
    bool     used;
} port_conn_slot_t;

struct ambre_port_chan {
    int      listen_fd;
    char     path[AMBRE_PORT_PATH_MAX];
    port_conn_slot_t slots[AMBRE_PORT_MAX_CONNS];
    uint32_t next_peer;
    uint32_t disconnected_peer;
};

struct ambre_port_conn {
    int  fd;
    char path[AMBRE_PORT_PATH_MAX];
};

static int fill_addr(struct sockaddr_un *sa, const char *path)
{
    if (path == NULL || strlen(path) >= AMBRE_PORT_PATH_MAX) {
        return AMBRE_PORT_ERR_PARAM;
    }
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    strncpy(sa->sun_path, path, sizeof(sa->sun_path) - 1u);
    return AMBRE_PORT_OK;
}

/* ------------------------------------------------------------- server ------ */

int ambre_port_chan_open(const char *path, ambre_port_chan_t **out)
{
    struct sockaddr_un sa;
    ambre_port_chan_t *c;
    int fd;
    int rc = fill_addr(&sa, path);

    if (rc != AMBRE_PORT_OK || out == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    (void)unlink(sa.sun_path);          /* a stale socket from a killed run */
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 16) != 0) {
        close(fd);
        return AMBRE_PORT_ERR_IO;
    }
    c = (ambre_port_chan_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        close(fd);
        return AMBRE_PORT_ERR_IO;
    }
    c->listen_fd = fd;
    strncpy(c->path, path, AMBRE_PORT_PATH_MAX - 1u);
    c->next_peer = 1u;
    *out = c;
    return AMBRE_PORT_OK;
}

void ambre_port_chan_close(ambre_port_chan_t *c)
{
    if (c == NULL) {
        return;
    }
    for (uint32_t i = 0u; i < AMBRE_PORT_MAX_CONNS; i++) {
        if (c->slots[i].used) {
            close(c->slots[i].fd);
        }
    }
    if (c->listen_fd >= 0) {
        close(c->listen_fd);
        (void)unlink(c->path);
    }
    free(c);
}

static int accept_new(ambre_port_chan_t *c)
{
    int fd = accept(c->listen_fd, NULL, NULL);

    if (fd < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    for (uint32_t i = 0u; i < AMBRE_PORT_MAX_CONNS; i++) {
        if (!c->slots[i].used) {
            c->slots[i].fd = fd;
            c->slots[i].peer = c->next_peer++;
            c->slots[i].used = true;
            return AMBRE_PORT_OK;
        }
    }
    close(fd);                           /* no room: refuse, never drop data */
    return AMBRE_PORT_ERR_FULL;
}

static void drop_conn(ambre_port_chan_t *c, uint32_t idx)
{
    /* QNX-05: the death of a producer shows up as its connection closing. */
    c->disconnected_peer = c->slots[idx].peer;
    close(c->slots[idx].fd);
    c->slots[idx].used = false;
    c->slots[idx].fd = -1;
}

/* Result of drain_ready(): nothing readable yet, keep waiting. */
#define AMBRE_PORT_RECV_AGAIN (-1000)

/* Serve the sockets that poll() reported readable. Returns the byte count of
 * the first datagram received, AMBRE_PORT_RECV_AGAIN when only a connection or
 * a disconnection happened, or a negative port error. */
static int drain_ready(ambre_port_chan_t *c, const struct pollfd *fds,
                       const uint32_t *map, nfds_t n, void *buf, size_t cap,
                       uint32_t *peer)
{
    for (nfds_t k = 0u; k < n; k++) {
        int got;

        if ((fds[k].revents & POLLIN) == 0) {
            continue;
        }
        if (map[k] == UINT32_MAX) {
            (void)accept_new(c);
            continue;
        }
        got = (int)recv(c->slots[map[k]].fd, buf, cap, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            drop_conn(c, map[k]);          /* peer closed or failed: QNX-05 */
            continue;
        }
        if (peer != NULL) {
            *peer = c->slots[map[k]].peer;
        }
        return got;
    }
    return AMBRE_PORT_RECV_AGAIN;
}

/* Build the poll set: the listening socket plus every live connection.
 * map[k] is the slot index, or UINT32_MAX for the listener. */
static nfds_t build_pollset(const ambre_port_chan_t *c, struct pollfd *fds,
                            uint32_t *map)
{
    nfds_t n = 0;

    fds[n].fd = c->listen_fd;
    fds[n].events = POLLIN;
    map[n++] = UINT32_MAX;
    for (uint32_t i = 0u; i < AMBRE_PORT_MAX_CONNS; i++) {
        if (!c->slots[i].used) {
            continue;
        }
        fds[n].fd = c->slots[i].fd;
        fds[n].events = POLLIN;
        map[n++] = i;
    }
    return n;
}

int ambre_port_chan_recv(ambre_port_chan_t *c, void *buf, size_t cap,
                         uint64_t timeout_us, uint32_t *peer)
{
    uint64_t deadline = ambre_port_now_us() + timeout_us;

    if (c == NULL || buf == NULL || cap == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    for (;;) {
        struct pollfd fds[1u + AMBRE_PORT_MAX_CONNS];
        uint32_t map[1u + AMBRE_PORT_MAX_CONNS];
        uint64_t now = ambre_port_now_us();
        nfds_t n;
        int remain_ms;
        int ready;

        if (now >= deadline) {
            return AMBRE_PORT_ERR_TIMEOUT;
        }
        remain_ms = (int)((deadline - now + 999u) / 1000u);
        n = build_pollset(c, fds, map);

        ready = poll(fds, n, remain_ms);
        if (ready < 0) {
            return (errno == EINTR) ? AMBRE_PORT_ERR_TIMEOUT : AMBRE_PORT_ERR_IO;
        }
        if (ready == 0) {
            return AMBRE_PORT_ERR_TIMEOUT;
        }
        int got = drain_ready(c, fds, map, n, buf, cap, peer);

        if (got != AMBRE_PORT_RECV_AGAIN) {
            return got;
        }
    }
}

int ambre_port_chan_reply(ambre_port_chan_t *c, uint32_t peer,
                          const void *buf, size_t len)
{
    if (c == NULL || buf == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    for (uint32_t i = 0u; i < AMBRE_PORT_MAX_CONNS; i++) {
        if (c->slots[i].used && c->slots[i].peer == peer) {
            ssize_t put = send(c->slots[i].fd, buf, len, MSG_NOSIGNAL);

            return (put == (ssize_t)len) ? AMBRE_PORT_OK : AMBRE_PORT_ERR_IO;
        }
    }
    return AMBRE_PORT_ERR_NOENT;
}

uint32_t ambre_port_chan_poll_disconnect(ambre_port_chan_t *c)
{
    uint32_t peer;

    if (c == NULL) {
        return 0u;
    }
    peer = c->disconnected_peer;
    c->disconnected_peer = 0u;
    return peer;
}

/* ------------------------------------------------------------- client ------ */

int ambre_port_conn_open(const char *path, ambre_port_conn_t **out)
{
    struct sockaddr_un sa;
    ambre_port_conn_t *c;
    int fd;
    int rc = fill_addr(&sa, path);

    if (rc != AMBRE_PORT_OK || out == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return AMBRE_PORT_ERR_NOENT;
    }
    c = (ambre_port_conn_t *)calloc(1, sizeof(*c));
    if (c == NULL) {
        close(fd);
        return AMBRE_PORT_ERR_IO;
    }
    c->fd = fd;
    strncpy(c->path, path, AMBRE_PORT_PATH_MAX - 1u);
    *out = c;
    return AMBRE_PORT_OK;
}

void ambre_port_conn_close(ambre_port_conn_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->fd >= 0) {
        close(c->fd);
    }
    free(c);
}

/* Send one datagram and wait for the peer's answer. `reply` may be NULL when
 * only the acknowledgement is wanted. This round trip is what PERF-05 and
 * feasibility essay E4 measure. */
static int conn_xfer(ambre_port_conn_t *c, const void *buf, size_t len,
                     void *reply, size_t reply_cap, uint64_t timeout_us,
                     uint64_t *latency_ns)
{
    uint8_t ack[AMBRE_PORT_ACK_MAX];
    uint8_t *dst = (reply != NULL) ? (uint8_t *)reply : ack;
    size_t room = (reply != NULL) ? reply_cap : sizeof(ack);
    uint64_t t0;
    ssize_t put;
    ssize_t got;

    if (c == NULL || buf == NULL || len == 0u || dst == NULL || room == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    t0 = ambre_port_now_us();
    put = send(c->fd, buf, len, MSG_NOSIGNAL);
    if (put != (ssize_t)len) {
        return AMBRE_PORT_ERR_IO;
    }
    {
        struct pollfd pfd;

        pfd.fd = c->fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, (int)(timeout_us / 1000u)) <= 0) {
            return AMBRE_PORT_ERR_TIMEOUT;
        }
    }
    got = recv(c->fd, dst, room, 0);
    if (got <= 0) {
        return AMBRE_PORT_ERR_IO;
    }
    if (latency_ns != NULL) {
        *latency_ns = (ambre_port_now_us() - t0) * 1000u;
    }
    return (int)got;
}

int ambre_port_conn_request(ambre_port_conn_t *c, const void *req, size_t req_len,
                            void *reply, size_t reply_cap, uint64_t timeout_us,
                            uint64_t *latency_ns)
{
    return conn_xfer(c, req, req_len, reply, reply_cap, timeout_us, latency_ns);
}

int ambre_port_conn_send(ambre_port_conn_t *c, const void *buf, size_t len,
                         uint64_t timeout_us, uint64_t *latency_ns)
{
    /* Contract: 0 on success. The ack itself carries no payload the caller
     * needs; ambre_port_conn_request() is the one that returns a byte count. */
    int rc = conn_xfer(c, buf, len, NULL, 0u, timeout_us, latency_ns);

    return (rc > 0) ? AMBRE_PORT_OK : rc;
}
