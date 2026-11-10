/*
 * ambre_explore.h — Exhaustive exploration of power-cut states (spec 7.3).
 *
 * Method:
 *   1. run the journal against a simdisk over a scenario, logging every write,
 *      flush and durability mark;
 *   2. for each prefix of that trace, materialize the disk image;
 *   3. add variants: torn last write at each sector boundary, subsets of the
 *      unflushed writes dropped (volatile disk cache), one flipped bit, table
 *      copies wiped;
 *   4. run recovery on the image and check invariants I1..I8 against the
 *      injected ground truth;
 *   5. replay the same exploration on the recovery's own trace (I8).
 *
 * A violating image is kept on disk so it becomes a regression test (spec 7.7).
 */

#ifndef AMBRE_EXPLORE_H
#define AMBRE_EXPLORE_H

#include "../../fmt/ambre_journal.h"
#include "../simdisk/ambre_simdisk.h"

#define AMBRE_TRUTH_MAX_EVENTS 32u

typedef struct {
    uint32_t event_id;
    uint32_t order;
    bool     triggered;            /* an EMERG record was submitted          */
    bool     sealed;               /* a SEAL record was submitted            */
    bool     locked;
    bool     file_complete;        /* value carried by the injected SEAL     */
    long     emerg_op;             /* op index of the EMERG write, -1        */
    long     acquired_op;          /* op index of MARK_ACQUIRED, -1          */
    long     sealed_op;            /* op index of MARK_SEALED, -1            */
    uint64_t t0_ts_us;
    uint64_t trig_ts_us;
    uint64_t first_sample_ts_us;
    uint64_t last_sample_ts_us;
    ambre_emerg_t emerg;           /* byte-exact injected class A snapshot   */
    ambre_seal_t  seal;            /* byte-exact injected seal               */
} ambre_truth_event_t;

typedef struct {
    ambre_truth_event_t events[AMBRE_TRUTH_MAX_EVENTS];
    uint32_t event_count;
    uint32_t triggered_count;
    /* injected class B stream, for invariant I4 (exact sample comparison) */
    ambre_sample_t *samples;
    uint64_t       *sample_ts_us;  /* ts of the DATA record carrying it      */
    uint32_t        sample_count;
    uint64_t        last_ts_us;    /* last timestamp the scenario reached    */
} ambre_truth_t;

typedef enum {
    AMBRE_I1_TERMINATES   = 0,
    AMBRE_I2_ACQUIRED     = 1,
    AMBRE_I3_NO_PHANTOM   = 2,
    AMBRE_I4_SAMPLES      = 3,
    AMBRE_I5_RECENCY      = 4,
    AMBRE_I6_LOCKED       = 5,
    AMBRE_I7_COMPLETE     = 6,
    AMBRE_I8_REPEATABLE   = 7,
    AMBRE_INVARIANT_COUNT = 8
} ambre_invariant_t;

typedef struct {
    uint32_t violated_mask;        /* bit AMBRE_Ix_* set when violated       */
    char     detail[224];          /* human readable first violation        */
} ambre_invariant_report_t;

typedef struct {
    const char *scenario_path;     /* scenario file, see docs/FORMAT.md §11 */
    uint64_t seed;                 /* used when scenario_path is NULL       */
    uint32_t segment_limit;        /* small journal = fast exploration      */
    uint32_t flush_period_us;      /* continuous mode cadence               */
    uint32_t event_flush_period_us;
    uint32_t duration_ms;
    uint32_t crash_at_ms;
    uint32_t max_states;           /* 0 = every reachable state             */
    uint32_t jobs;                 /* parallel workers, 0 = nproc           */
    bool     variant_torn;
    bool     variant_volatile_cache;
    bool     variant_bitflip;
    bool     variant_table_wipe;
    bool     nested_recovery;      /* cut during recovery (I8)              */
    const char *report_json;       /* NULL = no report                      */
    const char *violation_dir;     /* NULL = do not keep images             */
} ambre_explore_cfg_t;

typedef struct {
    uint64_t states_explored;
    uint64_t states_by_variant[6]; /* prefix, torn, cache, bitflip, table, nested */
    uint64_t violations;
    uint64_t per_invariant[AMBRE_INVARIANT_COUNT];
    uint64_t events_acquired;
    uint64_t events_phantom;
    uint64_t recoveries_run;
    double   seconds;
    char     first_violation[256];
} ambre_explore_stats_t;

void ambre_explore_cfg_default(ambre_explore_cfg_t *cfg);

/* Run the whole exploration. Returns 0 when no invariant was violated. */
int ambre_explore_run(const ambre_explore_cfg_t *cfg, ambre_explore_stats_t *out);

/* One scenario run without any cut: produces the op trace and the ground
 * truth used by the invariant checker. */
int ambre_explore_record_truth(const ambre_explore_cfg_t *cfg,
                               ambre_simdisk_t *sd, ambre_truth_t *truth);

/* Check I1..I8 on one recovered image. `prefix` and the op trace give the
 * checker what was durable before the cut. */
int ambre_check_invariants(const ambre_truth_t *truth,
                           const ambre_recovery_result_t *rec,
                           const ambre_disk_t *recovered_disk,
                           uint32_t prefix, const ambre_simdisk_t *sd,
                           ambre_invariant_report_t *report);

void ambre_truth_free(ambre_truth_t *truth);

#endif /* AMBRE_EXPLORE_H */
