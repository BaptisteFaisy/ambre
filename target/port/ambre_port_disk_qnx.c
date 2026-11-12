/*
 * ambre_port_disk_qnx.c — QNX 8.0 disk backend of the portability layer.
 *
 * Opens the dedicated raw journal partition (e.g. /dev/hd0t177 exposed by
 * io-blk, STO-01), performs pread/pwrite sector I/O, and forces the media with
 * fsync(fd) — QNX 8.0 has no DCMD_FLUSH_CACHE, see qnx_disk_flush() below.
 *
 * Essay E1 (docs/cahier-des-charges.md §10): the caller writes one block
 * through the vtable, then ambre_port_disk_flush_probe() orders that flush and
 * reports whether io-blk ACCEPTED the order and what it cost. Whether the
 * order physically reached the medium is only observable on target (QEMU host
 * block trace or the faulty block server) — this file reports acceptance,
 * never claims propagation.
 *
 * Compiled with the real QNX SDP 8.0 (qcc 12.2.0, gcc_ntox86_64).
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

/* QNX-only: verified with the real SDP (qcc 12.2.0, nto x86_64).
 *
 * QNX 8.0's <sys/dcmd_blk.h> has NO DCMD_FLUSH_CACHE: the block layer exposes
 * DCMD_FSYS_PGCACHE_CTL with DISCARD/RESIZE only. The supported way to force
 * the media on a raw partition served by io-blk is fsync(fd), which io-blk
 * turns into a cache flush and a SYNCHRONIZE CACHE for the device. That is
 * what this backend does, and what essay E1 measures. */
static int qnx_disk_flush(void *ctx)
{
    ambre_port_disk_t *d = (ambre_port_disk_t *)ctx;

    if (d == NULL) {
        return -1;
    }
    /* STO-01 / GEL-01: the durability point of the write path. */
    if (fsync(d->fd) == 0) {
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

/* QNX-only: verified with the real SDP. */
int ambre_port_disk_flush_probe(ambre_port_disk_t *d, bool *flush_seen,
                                uint64_t *flush_ns)
{
    _Uint64t c0 = 0u;
    _Uint64t c1 = 0u;
    int rc;

    if (d == NULL || flush_seen == NULL || flush_ns == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    /* Feasibility essay E1: order the flush the write path really uses
     * (fsync on the raw partition, see qnx_disk_flush) and log whether io-blk
     * ACCEPTED it and what it cost. Physical arrival on the medium is only
     * observable from the host (QEMU block trace or the faulty block server) —
     * this probe reports acceptance and duration, never propagation. */
    (void)ClockTime(CLOCK_MONOTONIC, NULL, &c0);
    rc = fsync(d->fd);
    (void)ClockTime(CLOCK_MONOTONIC, NULL, &c1);
    *flush_ns = (uint64_t)((c1 >= c0) ? (c1 - c0) : 0u);
    *flush_seen = (rc == 0);
    ambre_port_log(AMBRE_LOG_INFO, "e1-probe",
                   "fsync on the raw partition %s (rc=%d errno=%d, %" PRIu64 " ns)",
                   *flush_seen ? "accepted" : "rejected", rc, *flush_seen ? 0 : errno,
                   *flush_ns);
    return (*flush_seen) ? AMBRE_PORT_OK : AMBRE_PORT_ERR_IO;
}
