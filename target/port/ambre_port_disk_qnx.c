/*
 * ambre_port_disk_qnx.c — QNX 8.0 disk backend of the portability layer.
 *
 * Opens the dedicated raw journal partition (e.g. /dev/hd0t177 exposed by
 * io-blk, STO-01), performs pread/pwrite sector I/O, and issues explicit
 * cache-flush requests with devctl(fd, DCMD_FLUSH_CACHE, ...) (sys/dcmd_blk.h).
 *
 * Essay E1 (docs/cahier-des-charges.md §10): the caller writes one block
 * through the vtable, then ambre_port_disk_flush_probe() orders a cache flush
 * with devctl(DCMD_FLUSH_CACHE) and reports whether io-blk ACCEPTED the order
 * and what it cost. Whether the order physically reached the medium is only
 * observable on target (QEMU host block trace or the faulty block server) —
 * this file reports acceptance, never claims propagation.
 *
 * Syntax-checked on PC against target/qnx-stub/include; verified on QNX 8.0.
 */

#define _POSIX_C_SOURCE 200809L   /* pread/pwrite/fdatasync on the PC host */

#include "ambre_port.h"

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dcmd_blk.h>
#include <sys/neutrino.h>
#include <unistd.h>

struct ambre_port_disk {
    int          fd;
    uint64_t     size;
    ambre_disk_t vtable;
};

/* QNX-only: verified on target, syntax-checked on PC */
static int qnx_disk_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    ambre_port_disk_t *d = (ambre_port_disk_t *)ctx;
    ssize_t got;

    if (d == NULL || buf == NULL || off + len > d->size) {
        return -1;
    }
    got = pread(d->fd, buf, len, (off_t)off);
    if (got != (ssize_t)len) {
        if (got < 0 || off + (uint64_t)got > d->size) {
            return -1;
        }
        memset((uint8_t *)buf + got, 0, len - (size_t)got);
    }
    return 0;
}

/* QNX-only: verified on target, syntax-checked on PC */
static int qnx_disk_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    ambre_port_disk_t *d = (ambre_port_disk_t *)ctx;
    ssize_t put;

    if (d == NULL || buf == NULL || off + len > d->size) {
        return -1;
    }
    put = pwrite(d->fd, buf, len, (off_t)off);
    return (put == (ssize_t)len) ? 0 : -1;
}

/* QNX-only: verified on target, syntax-checked on PC */
static int qnx_disk_flush(void *ctx)
{
    ambre_port_disk_t *d = (ambre_port_disk_t *)ctx;
    int info = 0;
    int err;

    if (d == NULL) {
        return -1;
    }
    /* STO-01 / GEL-01: explicit io-blk cache flush order. */
    err = devctl(d->fd, DCMD_FLUSH_CACHE, NULL, 0u, &info);
    if (err == EOK) {
        return 0;
    }
    return (fdatasync(d->fd) == 0) ? 0 : -1;
}

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_disk_open(const char *spec, uint64_t size, ambre_port_disk_t **out)
{
    ambre_port_disk_t *d;
    int fd;

    if (spec == NULL || out == NULL || size == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    /* STO-01: open the raw block partition exposed by io-blk (/dev/hd0tNNN). */
    fd = open(spec, O_RDWR);
    if (fd < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    d = (ambre_port_disk_t *)calloc(1, sizeof(*d));
    if (d == NULL) {
        close(fd);
        return AMBRE_PORT_ERR_IO;
    }
    d->fd = fd;
    d->size = size;
    d->vtable.ctx = d;
    d->vtable.size = size;
    d->vtable.sector_size = AMBRE_BLOCK_SIZE;
    d->vtable.read = qnx_disk_read;
    d->vtable.write = qnx_disk_write;
    d->vtable.flush = qnx_disk_flush;
    d->vtable.mark = NULL;               /* marks exist only for the explorer */
    *out = d;
    return AMBRE_PORT_OK;
}

/* QNX-only: verified on target, syntax-checked on PC */
void ambre_port_disk_close(ambre_port_disk_t *d)
{
    if (d == NULL) {
        return;
    }
    if (d->fd >= 0) {
        (void)qnx_disk_flush(d);
        close(d->fd);
    }
    free(d);
}

/* QNX-only: verified on target, syntax-checked on PC */
const ambre_disk_t *ambre_port_disk_vtable(ambre_port_disk_t *d)
{
    return (d != NULL) ? &d->vtable : NULL;
}

/* QNX-only: verified on target, syntax-checked on PC */
int ambre_port_disk_flush_probe(ambre_port_disk_t *d, bool *flush_seen,
                                uint64_t *flush_ns)
{
    uint64_t c0;
    uint64_t c1;
    int info = 0;
    int err;

    if (d == NULL || flush_seen == NULL || flush_ns == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    /* Feasibility essay E1: issue DCMD_FLUSH_CACHE directly to io-blk and
     * log whether the device driver accepted the flush command (EOK) and its
     * duration. Physical platter arrival is confirmed on QEMU by the host
     * block trace / faulty block server. */
    c0 = ClockTime(CLOCK_MONOTONIC, NULL);
    err = devctl(d->fd, DCMD_FLUSH_CACHE, NULL, 0u, &info);
    c1 = ClockTime(CLOCK_MONOTONIC, NULL);
    *flush_ns = (c1 >= c0) ? (c1 - c0) : 0u;
    *flush_seen = (err == EOK);
    ambre_port_log(AMBRE_LOG_INFO, "e1-probe",
                   "DCMD_FLUSH_CACHE %s (err=%d, info=%d, %" PRIu64 " ns)",
                   *flush_seen ? "accepted" : "rejected", err, info, *flush_ns);
    return (*flush_seen) ? AMBRE_PORT_OK : AMBRE_PORT_ERR_IO;
}
