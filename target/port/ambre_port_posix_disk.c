/*
 * ambre_port_posix_disk.c — POSIX backend: a regular file as the raw journal
 * partition (STO-01 on target is a real partition; here it is a file so the
 * whole chain can be exercised, cut and recovered on the PC).
 *
 * Honesty rule: this backend cannot tell whether a flush reached a physical
 * platter, so ambre_port_disk_flush_probe() returns UNSUPPORTED rather than
 * reporting a fake "yes". Essay E1 needs the QNX backend or the faulty block
 * server in host/powercut/, which does log the FLUSH commands it receives.
 */

#define _POSIX_C_SOURCE 200809L

#include "ambre_port.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ambre_port_disk {
    int      fd;
    uint64_t size;
    ambre_disk_t vtable;
};

static int disk_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    ambre_port_disk_t *d = (ambre_port_disk_t *)ctx;
    ssize_t got;

    if (d == NULL || buf == NULL || off + len > d->size) {
        return -1;
    }
    got = pread(d->fd, buf, len, (off_t)off);
    if (got != (ssize_t)len) {
        /* A short read past the end of file reads as zeroes, like a freshly
         * formatted partition would. */
        if (got < 0 || off + (uint64_t)got > d->size) {
            return -1;
        }
        memset((uint8_t *)buf + got, 0, len - (size_t)got);
    }
    return 0;
}

static int disk_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    ambre_port_disk_t *d = (ambre_port_disk_t *)ctx;
    ssize_t put;

    if (d == NULL || buf == NULL || off + len > d->size) {
        return -1;
    }
    put = pwrite(d->fd, buf, len, (off_t)off);
    return (put == (ssize_t)len) ? 0 : -1;
}

static int disk_flush(void *ctx)
{
    ambre_port_disk_t *d = (ambre_port_disk_t *)ctx;

    if (d == NULL) {
        return -1;
    }
    /* fdatasync is the closest POSIX equivalent of a device cache flush: it
     * pushes the data without the metadata round trip. */
    return (fdatasync(d->fd) == 0) ? 0 : -1;
}

int ambre_port_disk_open(const char *spec, uint64_t size, ambre_port_disk_t **out)
{
    ambre_port_disk_t *d;
    int fd;

    if (spec == NULL || out == NULL || size == 0u) {
        return AMBRE_PORT_ERR_PARAM;
    }
    fd = open(spec, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        return AMBRE_PORT_ERR_IO;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
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
    d->vtable.read = disk_read;
    d->vtable.write = disk_write;
    d->vtable.flush = disk_flush;
    d->vtable.mark = NULL;             /* marks exist only for the explorer */
    *out = d;
    return AMBRE_PORT_OK;
}

void ambre_port_disk_close(ambre_port_disk_t *d)
{
    if (d == NULL) {
        return;
    }
    if (d->fd >= 0) {
        (void)fdatasync(d->fd);
        close(d->fd);
    }
    free(d);
}

const ambre_disk_t *ambre_port_disk_vtable(ambre_port_disk_t *d)
{
    return (d != NULL) ? &d->vtable : NULL;
}

int ambre_port_disk_flush_probe(ambre_port_disk_t *d, bool *flush_seen,
                                uint64_t *flush_ns)
{
    uint64_t t0;
    int rc;

    if (d == NULL || flush_seen == NULL || flush_ns == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    /* The cost is measurable here; whether the order reached a device is not.
     * Report the cost and say plainly that propagation is unproven. */
    t0 = ambre_port_now_us();
    rc = disk_flush(d);
    *flush_ns = (ambre_port_now_us() - t0) * 1000u;
    *flush_seen = false;
    return (rc == 0) ? AMBRE_PORT_ERR_UNSUPPORTED : AMBRE_PORT_ERR_IO;
}
