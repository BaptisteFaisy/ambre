/*
 * ambre_fmt.h — Journal format codec: blocks, allocation table, recovery.
 *
 * The same code runs on PC (host explorer, tests) and on QNX 8.0 (edr-journal).
 * All disk access goes through the ambre_disk_t vtable, so the crash explorer
 * can replay prefixes of a write trace without touching the library.
 *
 * Error convention: 0 = AMBRE_OK, negative = ambre_err_t.
 */

#ifndef AMBRE_FMT_H
#define AMBRE_FMT_H

#include "ambre_types.h"
#include "ambre_crc32c.h"

typedef enum {
    AMBRE_OK            =   0,
    AMBRE_ERR_IO        =  -1,   /* disk vtable failed                     */
    AMBRE_ERR_BAD_MAGIC =  -2,
    AMBRE_ERR_BAD_VER   =  -3,
    AMBRE_ERR_BAD_CRC   =  -4,
    AMBRE_ERR_TOO_BIG   =  -5,   /* payload > AMBRE_REC_MAX_BLOCKS * PAYLOAD */
    AMBRE_ERR_FULL      =  -6,   /* no free segment, all locked (STO-08)   */
    AMBRE_ERR_STATE     =  -7,   /* call not allowed in current state      */
    AMBRE_ERR_PARAM     =  -8,
    AMBRE_ERR_TORN      =  -9,   /* record incomplete: missing continuation */
    AMBRE_ERR_STALE     = -10    /* block from another session             */
} ambre_err_t;

/* ------------------------------------------------------------ disk vtable - */

typedef struct ambre_disk {
    void    *ctx;
    uint64_t size;               /* bytes; >= AMBRE_JOURNAL_BYTES           */
    uint32_t sector_size;        /* 512 or 4096 (spec 4.5)                  */
    int (*read)(void *ctx, uint64_t off, void *buf, size_t len);
    int (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
    int (*flush)(void *ctx);     /* cache flush / sync: durability point    */
    /* Optional: durability milestones for the crash explorer. The journal
     * emits ambre_mark_kind_t values; the QNX backend leaves this NULL. */
    void (*mark)(void *ctx, uint32_t kind, uint64_t value, uint64_t aux);
} ambre_disk_t;

/* ------------------------------------------------------------- block codec - */

/* Fill a single-block record: header fields, payload copy, both CRCs. */
int ambre_block_build(ambre_block_t *out, ambre_rec_type_t type, uint64_t seq,
                      uint64_t session_id, uint64_t first_ts_us,
                      const void *payload, uint32_t payload_len);

/* Build one block of a multi-block record (chunk_index < chunk_count). */
int ambre_block_build_chunk(ambre_block_t *out, ambre_rec_type_t type,
                            uint64_t seq, uint64_t session_id,
                            uint64_t first_ts_us, uint16_t chunk_index,
                            uint16_t chunk_count, const void *payload,
                            uint32_t chunk_len, uint32_t total_len);

/* Validate magic, version, lengths and both CRCs. Returns AMBRE_OK or error. */
int ambre_block_validate(const ambre_block_t *b);

/* Serialize/deserialize a block to/from a 512-byte buffer (memcpy, no cast). */
void ambre_block_pack(const ambre_block_t *b, uint8_t buf[AMBRE_BLOCK_SIZE]);
void ambre_block_unpack(const uint8_t buf[AMBRE_BLOCK_SIZE], ambre_block_t *b);

int ambre_block_write(const ambre_disk_t *d, uint64_t off, const ambre_block_t *b);
int ambre_block_read(const ambre_disk_t *d, uint64_t off, ambre_block_t *b);

/* Offset of block index `blk` inside segment `seg`. */
uint64_t ambre_block_offset(uint32_t seg, uint32_t blk);
/* Inverse: offset -> seg/blk. Returns false if outside the data area. */
bool ambre_offset_to_segblk(uint64_t off, uint32_t *seg, uint32_t *blk);

/* ------------------------------------------------------------- table codec - */

void ambre_table_init(ambre_alloc_table_t *t, uint64_t session_id);
int  ambre_table_validate(const ambre_alloc_table_t *t);
void ambre_table_pack(const ambre_alloc_table_t *t, uint8_t buf[AMBRE_BLOCK_SIZE]);
void ambre_table_unpack(const uint8_t buf[AMBRE_BLOCK_SIZE], ambre_alloc_table_t *t);

/* Read both copies; keep the valid one with the highest generation (REP-02).
 * *copy_used = 0/1, or -1 when neither copy is valid. */
int ambre_table_read_best(const ambre_disk_t *d, ambre_alloc_table_t *out,
                          int *copy_used);

/* Commit: bump generation, recompute CRC, write the OLDER copy only (STO-04). */
int ambre_table_commit(ambre_disk_t *d, ambre_alloc_table_t *t);

/* Event table helpers (oldest-first overwrite, locked never touched). */
ambre_ev_desc_t *ambre_table_find_event(ambre_alloc_table_t *t, uint32_t event_id);
int ambre_table_add_event(ambre_alloc_table_t *t, const ambre_ev_desc_t *ev);
/* Pick a victim segment for a new event; -AMBRE_ERR_FULL if none (STO-08). */
int ambre_table_pick_victim(ambre_alloc_table_t *t, uint32_t *seg_out);

/* ------------------------------------------------------------- block index - */

/* Outcome of scanning one segment for the last valid block chain. */
typedef struct {
    uint32_t valid_blocks;       /* blocks accepted from the scan start     */
    uint64_t last_seq;
    uint64_t last_ts_us;
    uint32_t next_block;         /* first unusable block index              */
    bool     hit_emerg;
    uint64_t emerg_seq;
    uint32_t emerg_block;
} ambre_scan_result_t;

/* Scan segment `seg` from block `start_blk`, following seq order. Stops at the
 * first block that fails magic/version/CRC/session/seq-continuity. */
int ambre_scan_segment(const ambre_disk_t *d, uint64_t session_id,
                       uint64_t expected_first_seq, uint32_t seg,
                       uint32_t start_blk, ambre_scan_result_t *out);

/* -------------------------------------------------------------- recovery -- */

typedef struct {
    ambre_alloc_table_t table;   /* table as it is after recovery           */
    bool     table_rebuilt;      /* true when both copies were unusable     */
    uint32_t scanned_blocks;
    uint32_t events_before;
    uint32_t events_after;
    uint32_t sealed_incomplete;  /* events sealed with file_complete = 0    */
    uint32_t torn_records;       /* EMERG/CONT records dropped as incomplete */
    uint32_t table_commits;      /* number of ambre_table_commit() calls    */
    char     diag[96];
} ambre_recovery_result_t;

/* Run the whole recovery procedure (spec 4.6, REP-01..REP-05).
 * Idempotent: running it twice, or interrupting it with a power cut and
 * running it again, yields the same final state (REP-04, invariant I8). */
int ambre_recover(ambre_disk_t *d, ambre_recovery_result_t *out);

/* Rebuild a table from scratch by scanning every segment (REP-02 fallback). */
int ambre_rebuild_table(const ambre_disk_t *d, ambre_alloc_table_t *out);

#endif /* AMBRE_FMT_H */
