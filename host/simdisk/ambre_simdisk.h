/*
 * ambre_simdisk.h — PC-side simulated raw partition with a full op log.
 *
 * The explorer needs three things from a disk:
 *   1. every write and every flush, in order (the op trace);
 *   2. the ability to materialize the image after any PREFIX of that trace;
 *   3. crash variants: torn writes, a volatile cache that loses unflushed
 *      writes, and single-bit corruption (spec 7.3, 7.6, ROB-07).
 *
 * The image lives in host memory. Size is configurable so tests can run on a
 * small journal (segment_limit) instead of the full 48 MiB.
 */

#ifndef AMBRE_SIMDISK_H
#define AMBRE_SIMDISK_H

#include "../../fmt/ambre_fmt.h"

/* Both bounds are overridable at build time. The browser build (host/web/)
 * compiles the same sources with a smaller op-log capacity: a 400 000-entry
 * log would reserve more memory than a web page should. Exceeding a bound is
 * never silent — the write is refused with AMBRE_ERR_TOO_BIG. */
#ifndef AMBRE_SIMDISK_MAX_OPS
#define AMBRE_SIMDISK_MAX_OPS   400000u
#endif
#ifndef AMBRE_SIMDISK_MAX_DATA
#define AMBRE_SIMDISK_MAX_DATA  8192u   /* bytes kept per logged write       */
#endif

typedef enum {
    AMBRE_OP_WRITE = 1,
    AMBRE_OP_FLUSH = 2,
    AMBRE_OP_MARK  = 3,
    AMBRE_OP_READ  = 4              /* logged but never replayed             */
} ambre_op_kind_t;

typedef struct {
    uint32_t index;                 /* position in the trace                 */
    uint8_t  kind;                  /* ambre_op_kind_t                       */
    uint8_t  pad[3];
    uint64_t off;                   /* write/read offset                     */
    uint32_t len;                   /* write/read length                     */
    uint32_t mark_kind;             /* for AMBRE_OP_MARK                     */
    uint64_t mark_value;
    uint64_t mark_aux;
    uint64_t data_id;               /* index into the data pool, writes only */
} ambre_op_t;

/* Cut variants applied when materializing a prefix. All fields optional. */
typedef struct {
    uint32_t prefix;                /* apply ops [0, prefix); 0 = all        */
    bool     torn_last_write;       /* keep only torn_bytes of the last write*/
    uint32_t torn_bytes;            /* sector multiple                       */
    bool     drop_unflushed;        /* volatile disk cache model (spec 7.4)  */
    uint64_t keep_mask;             /* bit i: keep unflushed write #i        */
    bool     corrupt_bit;           /* flip one bit after the cut (ROB-07)   */
    uint64_t corrupt_off;
    uint8_t  corrupt_bit_index;     /* 0..7                                  */
    bool     wipe_table_a;          /* destroy table copy A (REP-02)         */
    bool     wipe_both_tables;      /* force the full-scan rebuild path      */
} ambre_cut_variant_t;

typedef struct ambre_simdisk {
    uint8_t   *image;
    uint64_t   size;
    ambre_op_t *ops;
    uint32_t   op_count;
    uint32_t   op_cap;
    uint8_t  **data_pool;           /* per-write payload copies              */
    uint32_t  *data_len;
    uint64_t   data_id_next;
    uint64_t   data_pool_cap;
    bool       log_reads;
    ambre_disk_t disk;              /* vtable pointing at this struct        */
} ambre_simdisk_t;

int  ambre_simdisk_create(ambre_simdisk_t *sd, uint64_t size);
void ambre_simdisk_destroy(ambre_simdisk_t *sd);

/* ambre_disk_t vtable bound to this simdisk (write/flush/read/mark logged). */
const ambre_disk_t *ambre_simdisk_disk(ambre_simdisk_t *sd);

uint32_t ambre_simdisk_op_count(const ambre_simdisk_t *sd);
const ambre_op_t *ambre_simdisk_op(const ambre_simdisk_t *sd, uint32_t i);

/* Index of the last op with the given mark kind and value, or -1. */
long ambre_simdisk_find_mark(const ambre_simdisk_t *sd, uint32_t mark_kind,
                             uint64_t value);

/* Materialize the image after `prefix` ops into dst (dst_size must equal the
 * simdisk size). var may be NULL for a plain prefix. */
int ambre_simdisk_snapshot(const ambre_simdisk_t *sd, uint32_t prefix,
                           const ambre_cut_variant_t *var,
                           uint8_t *dst, uint64_t dst_size);

/* Same, but mutate the simdisk's own image in place (fast path for the
 * explorer: it snapshots once per prefix, then tries cheap variants). */
int ambre_simdisk_apply_prefix(ambre_simdisk_t *sd, uint32_t prefix);

/* Restore the simdisk image from a snapshot buffer. */
void ambre_simdisk_restore(ambre_simdisk_t *sd, const uint8_t *src);

/* Index of the last FLUSH op strictly before `prefix`, or -1. */
long ambre_simdisk_last_flush_before(const ambre_simdisk_t *sd, uint32_t prefix);

#endif /* AMBRE_SIMDISK_H */
