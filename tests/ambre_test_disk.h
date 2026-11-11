/*
 * ambre_test_disk.h - Small in-memory fake disk for codec unit tests.
 *
 * Header-only (static inline) so both isolated builds and the top-level
 * Makefile (`$(BUILD)/test_%: tests/test_%.c tests/ambre_test_util.c $(LIB)`)
 * link cleanly without adding extra objects, and without depending on
 * host/simdisk.
 */

#ifndef AMBRE_TEST_DISK_H
#define AMBRE_TEST_DISK_H

#include "ambre_fmt.h"
#include <string.h>

typedef struct {
    uint8_t     *mem;
    uint64_t     size;
    uint64_t     write_count;
    uint64_t     last_write_off;
    uint32_t     last_write_len;
    bool         fail_read;
    bool         fail_write;
    ambre_disk_t disk;
} ambre_test_disk_t;

static inline int ambre_test_disk_read(void *ctx, uint64_t off,
                                       void *buf, size_t len)
{
    ambre_test_disk_t *td = (ambre_test_disk_t *)ctx;

    if (td == NULL || td->fail_read || buf == NULL) {
        return -1;
    }
    if (len > td->size || off > td->size - len) {
        return -1;
    }
    memcpy(buf, td->mem + off, len);
    return 0;
}

static inline int ambre_test_disk_write(void *ctx, uint64_t off,
                                        const void *buf, size_t len)
{
    ambre_test_disk_t *td = (ambre_test_disk_t *)ctx;

    if (td == NULL || td->fail_write || buf == NULL) {
        return -1;
    }
    if (len > td->size || off > td->size - len) {
        return -1;
    }
    memcpy(td->mem + off, buf, len);
    td->write_count++;
    td->last_write_off = off;
    td->last_write_len = (uint32_t)len;
    return 0;
}

static inline int ambre_test_disk_flush(void *ctx)
{
    (void)ctx;
    return 0;
}

static inline void ambre_test_disk_init(ambre_test_disk_t *td,
                                        uint8_t *backing, uint64_t size)
{
    memset(td, 0, sizeof(*td));
    memset(backing, 0, (size_t)size);
    td->mem              = backing;
    td->size             = size;
    td->disk.ctx         = td;
    td->disk.size        = size;
    td->disk.sector_size = AMBRE_BLOCK_SIZE;
    td->disk.read        = ambre_test_disk_read;
    td->disk.write       = ambre_test_disk_write;
    td->disk.flush       = ambre_test_disk_flush;
    td->disk.mark        = NULL;
}

#endif /* AMBRE_TEST_DISK_H */
