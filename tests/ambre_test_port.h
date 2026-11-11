/*
 * ambre_test_port.h — Header-only fixtures for the portability-layer tests.
 *
 * Same pattern as ambre_test_disk.h: everything is `static inline`, so both
 * isolated builds and the top-level Makefile
 * (`$(BUILD)/test_%: tests/test_%.c tests/ambre_test_util.c $(LIB)`) link
 * cleanly without extra objects. Shared by every test that exercises
 * target/port/ on the PC, so the fork/pipe plumbing exists exactly once.
 *
 * Rules for the forked helpers:
 *   - paths are unique per process and live in /tmp (never in the repo);
 *   - the child signals "I am listening" through a pipe, never by polling;
 *   - the child reports its own failures through its exit status, because its
 *     stdout buffer is inherited and must not be flushed twice;
 *   - the parent always reaps with ambre_test_reap(), so no orphan survives.
 */

#ifndef AMBRE_TEST_PORT_H
#define AMBRE_TEST_PORT_H

#include "ambre_port.h"

#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define AMBRE_TEST_PATH_MAX        128u
#define AMBRE_TEST_SERVER_US    3000000u   /* a forked server waits 3 s      */
#define AMBRE_TEST_CLIENT_US    2000000u   /* a client waits 2 s for its ack */

/* A unique /tmp path for this process, so parallel runs never collide. */
static inline void ambre_test_tmp_path(char *out, size_t cap, const char *tag)
{
    (void)snprintf(out, cap, "/tmp/ambre-test-%ld-%s", (long)getpid(), tag);
}

/* Wait for the forked server to announce that its channel is listening. */
static inline bool ambre_test_wait_ready(int fd, int timeout_ms)
{
    struct pollfd pfd;
    char c = 0;

    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) != 1) {
        return false;
    }
    return read(fd, &c, 1u) == 1 && c == 'R';
}

/* Reap a forked child and return its exit status (0 = the child was happy). */
static inline int ambre_test_reap(pid_t pid)
{
    int status = 0;

    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status)) {
        return -2;
    }
    return WEXITSTATUS(status);
}

/* Child side: serve `nframes` frames of `frame_bytes` bytes and echo each one
 * back as the ack. Frame i is expected to be filled with the byte
 * (uint8_t)(i + 1); anything else, or a frame of the wrong length, is a
 * failure of the transport and shows up in the exit status. */
static inline void ambre_test_child_echo_server(const char *path, int nframes,
                                                size_t frame_bytes,
                                                int ready_fd)
{
    ambre_port_chan_t *chan = NULL;
    uint8_t buf[AMBRE_TEST_PATH_MAX * 32u];
    char ready = 'R';

    if (ambre_port_chan_open(path, &chan) != AMBRE_PORT_OK) {
        _exit(20);
    }
    if (write(ready_fd, &ready, 1u) != 1) {
        _exit(21);
    }
    for (int i = 0; i < nframes; i++) {
        uint32_t peer = 0u;
        int got = ambre_port_chan_recv(chan, buf, frame_bytes,
                                       AMBRE_TEST_SERVER_US, &peer);

        if (got != (int)frame_bytes) {
            _exit(22);
        }
        if (buf[0] != (uint8_t)(i + 1)) {
            _exit(23);
        }
        if (ambre_port_chan_reply(chan, peer, buf, (size_t)got)
            != AMBRE_PORT_OK) {
            _exit(24);
        }
    }
    ambre_port_chan_close(chan);
    _exit(0);
}

/* Fork a server that owns the channel at `path`. Returns its pid (-1 on
 * failure); *ready_fd must be handed to ambre_test_wait_ready() then closed. */
static inline pid_t ambre_test_spawn_echo_server(const char *path, int nframes,
                                                 size_t frame_bytes,
                                                 int *ready_fd)
{
    int fds[2];
    pid_t pid;

    *ready_fd = -1;
    if (pipe(fds) != 0) {
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        ambre_test_child_echo_server(path, nframes, frame_bytes, fds[1]);
        _exit(0);
    }
    close(fds[1]);
    *ready_fd = fds[0];
    return pid;
}

/* Child side: connect, exchange one frame, then close the connection and exit
 * — a producer that dies, which QNX-05 must be detected without polling. */
static inline void ambre_test_child_connect_and_die(const char *path,
                                                    size_t frame_bytes)
{
    ambre_port_conn_t *conn = NULL;
    uint8_t buf[AMBRE_TEST_PATH_MAX * 32u];

    if (ambre_port_conn_open(path, &conn) != AMBRE_PORT_OK) {
        _exit(30);
    }
    memset(buf, 0xA5u, frame_bytes);
    if (ambre_port_conn_send(conn, buf, frame_bytes, AMBRE_TEST_CLIENT_US, NULL)
        != AMBRE_PORT_OK) {
        _exit(31);
    }
    ambre_port_conn_close(conn);
    _exit(0);
}

/* Fork that dying client. The caller must already own the channel at `path`. */
static inline pid_t ambre_test_spawn_dying_client(const char *path,
                                                  size_t frame_bytes)
{
    pid_t pid = fork();

    if (pid == 0) {
        ambre_test_child_connect_and_die(path, frame_bytes);
        _exit(0);
    }
    return pid;
}

#endif /* AMBRE_TEST_PORT_H */
