/*
 * ambre_explore_internal.h — Internal helpers shared by the explorer modules.
 *
 * Not part of the frozen public API: only the explorer sources and the
 * explorer tests include this file.
 */

#ifndef AMBRE_EXPLORE_INTERNAL_H
#define AMBRE_EXPLORE_INTERNAL_H

#include "ambre_explore.h"

/* State variant kinds (ambre_explore_state_t.variant_type). */
#define AMBRE_VAR_PREFIX   0   /* plain cut after `prefix` ops              */
#define AMBRE_VAR_TORN     1   /* last write torn at a sector boundary      */
#define AMBRE_VAR_CACHE    2   /* volatile disk cache: unflushed writes lost */
#define AMBRE_VAR_BITFLIP  3   /* one flipped bit anywhere in the data area */
#define AMBRE_VAR_TABLE    4   /* allocation table copies destroyed         */
#define AMBRE_VAR_NESTED   5   /* cut inside the recovery itself (I8)       */

/* ------------------------------------------------------------- mem disk -- */

#define AMBRE_MEMDISK_MAX_OPS    256u
#define AMBRE_MEMDISK_MAX_WRITE  8192u
#define AMBRE_MEMDISK_MAX_DIRTY  4096u

/* Minimal in-memory disk: optional op log (to cut inside recovery, I8) and
 * dirty-block tracking (to roll back cheaply between exploration states). */
typedef struct {
    uint8_t     *image;
    uint64_t     size;
    ambre_disk_t disk;
    bool         log_ops;
    uint32_t     op_count;
    ambre_op_t   ops[AMBRE_MEMDISK_MAX_OPS];
    uint8_t     *payloads;      /* MAX_OPS * MAX_WRITE arena */
    uint64_t     dirty[AMBRE_MEMDISK_MAX_DIRTY];
    uint32_t     dirty_n;
    bool         dirty_overflow;
    bool         synced;        /* image already derived from a base  */
} ambre_memdisk_t;

int  ambre_memdisk_init(ambre_memdisk_t *md, uint64_t size);
void ambre_memdisk_destroy(ambre_memdisk_t *md);
void ambre_memdisk_reset(ambre_memdisk_t *md);
int  ambre_memdisk_copy_from(ambre_memdisk_t *md, const uint8_t *src);
int  ambre_memdisk_sync_from(ambre_memdisk_t *md, const uint8_t *base);
int  ambre_memdisk_apply_log(const ambre_memdisk_t *src, uint32_t count,
                             ambre_memdisk_t *dst, const uint8_t *base);

/* ------------------------------------------------------------- scenario -- */

typedef struct {
    uint64_t ts_us;
    int32_t  a_long_mm_s2;
    int32_t  a_lat_mm_s2;
    uint16_t speed_kmh_x4;
    uint8_t  throttle_pct;
    uint8_t  brake_active;
    uint16_t engine_rpm;
    int16_t  steer_deg_x2;
    uint8_t  ext_flags;
} ambre_scen_sample_t;

typedef struct {
    ambre_scen_sample_t *s;
    uint32_t             count;
    uint32_t             crash_at_ms;
    uint32_t             duration_ms;
} ambre_scenario_t;

int  ambre_scenario_load(const ambre_explore_cfg_t *cfg, ambre_scenario_t *sc);
void ambre_scenario_free(ambre_scenario_t *sc);

/* --------------------------------------------------------------- states -- */

typedef struct {
    uint32_t            prefix;
    ambre_cut_variant_t variant;
    int                 variant_type;   /* 0 prefix, 1 torn, 2 cache,
                                           3 bitflip, 4 table, 5 nested */
} ambre_explore_state_t;

typedef struct {
    ambre_explore_state_t *items;
    size_t                 count;
    size_t                 cap;
} ambre_state_queue_t;

int  ambre_explore_gen_states(const ambre_simdisk_t *sd,
                              const ambre_explore_cfg_t *cfg,
                              ambre_state_queue_t *q);
void ambre_state_queue_free(ambre_state_queue_t *q);

/* Per-worker scratch space: allocated once, reused for every state. */
typedef struct {
    uint64_t        size;
    uint8_t        *plain;      /* image of the current prefix (all writes)  */
    uint8_t        *empty;      /* zeroed image, base for the cache variants */
    uint32_t        plain_prefix;
    ambre_memdisk_t work;       /* image the recovery runs on                */
    ambre_memdisk_t nested;     /* base + part of the recovery's own writes  */
    bool            nested_used; /* nested recovery ran on ws->nested        */
} ambre_explore_ws_t;

int  ambre_explore_ws_init(ambre_explore_ws_t *ws, uint64_t size);
void ambre_explore_ws_free(ambre_explore_ws_t *ws);
int  ambre_explore_ws_test(ambre_explore_ws_t *ws,
                           const ambre_explore_cfg_t *cfg,
                           const ambre_simdisk_t *sd,
                           const ambre_truth_t *truth,
                           const ambre_explore_state_t *st,
                           ambre_invariant_report_t *rep);

/* The three steps ambre_explore_ws_test() is made of. The browser emulator
 * (host/web/) needs the crash image itself, before the recovery rewrites it,
 * so it drives the same steps instead of re-implementing them. */
int  ambre_explore_build_image(ambre_explore_ws_t *ws, const ambre_simdisk_t *sd,
                               const ambre_explore_state_t *st);
/* Returns 1 when an invariant was violated, 0 when the state is clean,
 * negative on an internal error. rec_out (may be NULL) receives the recovery
 * result the verdict was judged on. */
int  ambre_explore_judge(ambre_explore_ws_t *ws, const ambre_truth_t *truth,
                         const ambre_explore_state_t *st, const ambre_simdisk_t *sd,
                         ambre_invariant_report_t *rep,
                         ambre_recovery_result_t *rec_out);
/* Disk holding the recovered image of the state judged last: ws->work, except
 * for a nested cut whose verdict comes from the second recovery on ws->nested. */
const ambre_disk_t *ambre_explore_recovered_disk(const ambre_explore_ws_t *ws,
                                                 const ambre_explore_state_t *st);

/* Counters shared by the PC driver and the browser sweep, so both report the
 * exploration exactly the same way. */
void ambre_explore_merge_stats(ambre_explore_stats_t *dst,
                               const ambre_explore_stats_t *src);
uint32_t ambre_explore_acquired_before(const ambre_truth_t *truth,
                                       uint32_t prefix);

void ambre_save_violation(const ambre_explore_cfg_t *cfg,
                          const ambre_explore_state_t *st,
                          const uint8_t *img, uint64_t size,
                          const ambre_invariant_report_t *rep);

/* Table equality used by I8 (generation and CRC excluded on purpose). */
int  ambre_tables_equivalent(const ambre_alloc_table_t *a,
                             const ambre_alloc_table_t *b);

/* True when the image holds a formatted journal (valid table or valid block):
 * a pristine image must not be reported as a recovery failure. */
bool ambre_image_has_journal(const ambre_disk_t *d);

#endif /* AMBRE_EXPLORE_INTERNAL_H */
