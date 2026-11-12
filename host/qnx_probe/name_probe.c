/*
 * name_probe.c — does a name_open() connection carry a reply?
 *
 * QNX 8.0 has two ways to reach a server:
 *   - ConnectAttach(pid, chid, ...)  -> a regular connection, replies come back;
 *   - name_open(name, flags)         -> a SIDE CHANNEL (the returned coid is
 *     0x40000000 | chid, see the QNX Library Reference for name_open()).
 *
 * This probe answers the question on the target, with no Ambre code involved:
 *   server mode : name_attach() a name, receive one message, answer it
 *   client mode : name_open() the name, send one message, print what came back
 *
 * The expected outcomes, and what each one means for the Ambre transport:
 *   - the client gets the reply   -> the side channel carries replies, so the
 *     bug is in the Ambre port layer;
 *   - the client gets nothing     -> a side channel is send-only, and the
 *     client must open a regular connection (ConnectAttach with the server's
 *     pid and chid) to read any answer.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#include <sys/procmgr.h>
#include <unistd.h>

#define PROBE_NAME "ambre-probe"

static int run_server(void)
{
    name_attach_t *attach;
    char buf[64];
    rcvid_t rcvid;

    /* The name must be PUBLIC, otherwise only the server can find it. */
    if (procmgr_ability(0, PROCMGR_AID_PUBLIC_CHANNEL | PROCMGR_AOP_ALLOW |
                        PROCMGR_ADN_ROOT | PROCMGR_ADN_NONROOT,
                        PROCMGR_AID_EOL) != 0) {
        fprintf(stderr, "server: procmgr_ability failed (errno=%d)\n", errno);
    }
    attach = name_attach(NULL, PROBE_NAME, 0);
    if (attach == NULL) {
        fprintf(stderr, "server: name_attach failed (errno=%d)\n", errno);
        return 1;
    }
    fprintf(stderr, "server: name_attach ok, chid=%d pid=%ld\n",
            attach->chid, (long)getpid());
    rcvid = MsgReceive(attach->chid, buf, sizeof(buf), NULL);
    if (rcvid < 0) {
        fprintf(stderr, "server: MsgReceive failed\n");
        return 1;
    }
    fprintf(stderr, "server: got '%s', replying HELLO\n", buf);
    if (MsgReply(rcvid, 0, "HELLO", 5) != 0) {
        fprintf(stderr, "server: MsgReply failed\n");
        return 1;
    }
    return 0;
}

static int run_client(void)
{
    char reply[64];
    int coid;
    int rc;

    coid = -1;
    for (int attempt = 0; attempt < 200 && coid < 0; attempt++) {
        coid = name_open(PROBE_NAME, 0);
        if (coid < 0) {
            usleep(50000);          /* the server may not have attached yet */
        }
    }
    if (coid < 0) {
        fprintf(stderr, "client: name_open failed (errno=%d)\n", errno);
        return 1;
    }
    fprintf(stderr, "client: name_open ok, coid=%d\n", coid);
    memset(reply, 0, sizeof(reply));
    rc = MsgSend(coid, "PING", 4, reply, sizeof(reply));
    fprintf(stderr, "client: MsgSend -> rc=%d errno=%d, reply='%s'\n",
            rc, errno, reply);
    return (rc > 0 && strncmp(reply, "HELLO", 5) == 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
    int server = (argc > 1 && strcmp(argv[1], "server") == 0);

    if (server) {
        return run_server();
    }
    return run_client();
}
