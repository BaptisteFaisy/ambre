/*
 * ambre_journal.h — Journal engine: the write path, from samples to "acquis".
 *
 * Portable C11, no dynamic allocation on the critical path (GEL-03): the
 * journal state is a fixed-size struct, all buffers are pre-allocated.
 * Runs unchanged on PC (explorer, tests) and on QNX 8.0 (edr-journal).
 *
 * Durability sequence for a trigger (GEL-01, GEL-02):
 *   1. one contiguous write of the EMERG record blocks
 *   2. one allocation-table commit (older copy, generation + 1)
 *   3. one disk flush -> on ack the event is ACQUIRED and marked as such
 * Steps 1-3 are the only writes on the critical path.
 */

#ifndef AMBRE_JOURNAL_H
#define AMBRE_JOURNAL_H

#include "ambre_fmt.h"

/* Marks pushed through ambre_disk_t.mark() so the crash explorer knows what
 * had already been made durable before a given point in the op trace. */
typedef enum {
    AMBRE_MARK_EMERG_WRITTEN = 1,  /* value = event_id                      */
    AMBRE_MARK_ACQUIRED      = 2,  /* value = event_id (flush acked)        */
    AMBRE_MARK_SEALED        = 3,  /* value = event_id, complete flag in aux*/
    AMBRE_MARK_DATA_FLUSHED  = 4,  /* value = last seq flushed              */
    AMBRE_MARK_TABLE_COMMIT  = 5   /* value = generation                    */
} ambre_mark_kind_t;

typedef struct {
    uint64_t session_id;
    uint32_t segment_limit;          /* segments used, <= AMBRE_SEGMENT_COUNT */
    uint32_t flush_period_us;        /* continuous mode cadence (GEL-07)      */
    uint32_t event_flush_period_us;  /* during-event cadence, 10 ms (GEL-04)  */
    bool     continuous_mode;        /* false = event-only mode (GEL-08)      */
    uint8_t  ignition_cycle;
    uint8_t  reserved[3];
} ambre_journal_cfg_t;

typedef struct {
    uint32_t open_events;            /* events triggered, not sealed          */
    uint32_t acquired_events;        /* events that reached "acquis"          */
    uint32_t sealed_events;
    uint32_t refused_no_space;       /* STO-08 counter                        */
    uint32_t refused_busy;
    uint64_t bytes_written;          /* STO-09                                */
    uint64_t flush_count;            /* STO-09                                */
    uint64_t blocks_written;
    uint32_t torn_dropped;
} ambre_journal_stats_t;

typedef struct {
    ambre_disk_t     *disk;
    ambre_journal_cfg_t cfg;
    ambre_alloc_table_t table;       /* in-memory image of the table          */
    uint64_t          next_seq;
    uint64_t          last_flush_ts_us;
    uint64_t          last_event_flush_ts_us;
    bool              recovered;
    ambre_journal_stats_t stats;
    /* pre-allocated staging buffers: the critical path never allocates */
    uint8_t           rec_buf[AMBRE_REC_MAX_BLOCKS * AMBRE_BLOCK_SIZE];
    ambre_block_t     readback;
    ambre_recovery_result_t last_recovery;
} ambre_journal_t;

void ambre_journal_cfg_default(ambre_journal_cfg_t *cfg);

/* Write a fresh, empty journal layout (tables + zeroed segments). */
int ambre_journal_format(ambre_disk_t *disk, const ambre_journal_cfg_t *cfg);

/* Open an existing journal. Runs ambre_recover() first (REP-01) unless the
 * caller states the partition was just formatted. */
int ambre_journal_open(ambre_disk_t *disk, const ambre_journal_cfg_t *cfg,
                       bool run_recovery, ambre_journal_t *j);

/* Continuous class-B capture: appends DATA records, no flush (GEL-07 cadence
 * is handled by ambre_journal_tick). */
int ambre_journal_append_data(ambre_journal_t *j, uint64_t ts_us,
                              const ambre_sample_t *samples, uint32_t count);

int ambre_journal_append_gap(ambre_journal_t *j, const ambre_gap_t *gap);

/* Trigger path: EMERG in one contiguous write + table commit + flush.
 * Returns AMBRE_OK once the event is ACQUIRED (GEL-02). */
int ambre_journal_trigger(ambre_journal_t *j, const ambre_emerg_t *emerg);

/* During-event continuation: one CONT record + flush (GEL-04). */
int ambre_journal_continue(ambre_journal_t *j, const ambre_cont_t *cont);

/* Seal an event: SEAL record + table commit + flush (GEL-05). */
int ambre_journal_seal(ambre_journal_t *j, const ambre_seal_t *seal);

/* Periodic maintenance: flush in continuous mode when the cadence is due,
 * advance segments, apply the overwrite policy (STO-06). */
int ambre_journal_tick(ambre_journal_t *j, uint64_t now_us);

int ambre_journal_flush(ambre_journal_t *j, uint64_t now_us);

const ambre_alloc_table_t *ambre_journal_table(const ambre_journal_t *j);
const ambre_journal_stats_t *ambre_journal_stats(const ambre_journal_t *j);
const ambre_recovery_result_t *ambre_journal_recovery(const ambre_journal_t *j);

/* Read back one event's records (used by the MCAP exporter and the tests). */
typedef struct {
    ambre_emerg_t emerg;
    bool          have_emerg;
    ambre_seal_t  seal;
    bool          have_seal;
    uint32_t      cont_count;
    uint32_t      data_blocks;
    uint64_t      first_ts_us;
    uint64_t      last_ts_us;
} ambre_event_view_t;

int ambre_journal_read_event(const ambre_journal_t *j, uint32_t event_id,
                             ambre_event_view_t *view);

#endif /* AMBRE_JOURNAL_H */
