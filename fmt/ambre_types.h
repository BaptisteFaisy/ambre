/*
 * ambre_types.h — Ambre QNX EDR: on-disk structures and shared constants.
 *
 * Portable C11. Compiled identically for PC (host tools, explorer) and QNX 8.0.
 * Every structure here is an ON-DISK structure: packed, explicit widths, and
 * only ever read/written through the codec in ambre_fmt.h (memcpy, no casts).
 *
 * House rules: files < 500 lines, functions < 50 lines, no duplication.
 */

#ifndef AMBRE_TYPES_H
#define AMBRE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- layout -- */

#define AMBRE_MAGIC_BLOCK       0x414D4252u /* "AMBR" */
#define AMBRE_MAGIC_TABLE       0x414D5442u /* "AMTB" */
#define AMBRE_FORMAT_VERSION    1u

#define AMBRE_BLOCK_SIZE        512u         /* one sector                    */
#define AMBRE_BLOCK_HDR_SIZE    48u
#define AMBRE_BLOCK_CRC_SIZE    4u
#define AMBRE_BLOCK_PAYLOAD_MAX (AMBRE_BLOCK_SIZE - AMBRE_BLOCK_HDR_SIZE \
                                 - AMBRE_BLOCK_CRC_SIZE)   /* 460             */

#define AMBRE_REC_MAX_BLOCKS    16u          /* one record spans <= 16 sectors */

#define AMBRE_SEGMENT_SIZE      (2u * 1024u * 1024u)   /* 2 MiB per segment   */
#define AMBRE_SEGMENT_COUNT     24u                    /* 48 MiB journal      */
#define AMBRE_BLOCKS_PER_SEGMENT (AMBRE_SEGMENT_SIZE / AMBRE_BLOCK_SIZE)

/* Sector 0 = allocation table copy A, sector 1 = copy B, data starts at 2. */
#define AMBRE_TABLE_A_OFFSET    0u
#define AMBRE_TABLE_B_OFFSET    AMBRE_BLOCK_SIZE
#define AMBRE_FIRST_DATA_OFFSET (2u * AMBRE_BLOCK_SIZE)

#define AMBRE_JOURNAL_BYTES     (AMBRE_FIRST_DATA_OFFSET + \
                                 (uint64_t)AMBRE_SEGMENT_COUNT * AMBRE_SEGMENT_SIZE)

#define AMBRE_FL_CONTINUATION   0x01u        /* block continues previous one  */

/* ------------------------------------------------------------ enumerations - */

typedef enum {                    /* record types (spec 4.5)                  */
    AMBRE_REC_DATA  = 0x01,       /* class B samples, 8 bytes each            */
    AMBRE_REC_EMERG = 0x02,       /* class A pre-crash snapshot               */
    AMBRE_REC_CONT  = 0x03,       /* class A during-crash continuation        */
    AMBRE_REC_SEAL  = 0x04,       /* end-of-event summary                     */
    AMBRE_REC_GAP   = 0x05        /* capture interruption                     */
} ambre_rec_type_t;

typedef enum {                    /* segment states (spec 4.2)                */
    AMBRE_SEG_FREE     = 0,
    AMBRE_SEG_CURRENT  = 1,       /* being written now                        */
    AMBRE_SEG_FROZEN   = 2,       /* pinned by an acquired event              */
    AMBRE_SEG_LOCKED   = 3,       /* holds a locked event, never reused       */
    AMBRE_SEG_ARCHIVED = 4
} ambre_seg_state_t;

typedef enum {                    /* event descriptor states                  */
    AMBRE_EV_EMPTY  = 0,
    AMBRE_EV_OPEN   = 1,          /* triggered, not sealed yet                */
    AMBRE_EV_SEALED = 2
} ambre_ev_state_t;

typedef enum {                    /* why an event was sealed                  */
    AMBRE_SEAL_NORMAL    = 0,     /* end of event reached                     */
    AMBRE_SEAL_POWERCUT  = 1,     /* still open when recovery ran (REP-03)    */
    AMBRE_SEAL_VOLTAGE   = 2,     /* supply voltage drop (GEL-09)             */
    AMBRE_SEAL_OVERWRITE = 3      /* recycled for lack of space (STO-06)      */
} ambre_seal_cause_t;

typedef enum {                    /* trigger reasons (UN R160 5.3.1 / 5.3.2)  */
    AMBRE_TRIG_NONE        = 0,
    AMBRE_TRIG_DV_LONG     = 1,  /* delta-v long. > 8 km/h in <= 150 ms       */
    AMBRE_TRIG_DV_LAT      = 2,  /* delta-v lat.  > 8 km/h in <= 150 ms       */
    AMBRE_TRIG_RESTRAINT   = 3,  /* non-reversible restraint deployed         */
    AMBRE_TRIG_DV25_LOCK   = 4,  /* delta-v > 25 km/h in <= 150 ms            */
    AMBRE_TRIG_VRU         = 5,  /* vulnerable road user protection           */
    AMBRE_TRIG_EMERG_BRAKE = 6,  /* off-profile, never locked                 */
    AMBRE_TRIG_AEB         = 7,  /* off-profile, never locked                 */
    AMBRE_TRIG_SERVICE     = 8   /* off-profile, never locked                 */
} ambre_trig_reason_t;

/* ------------------------------------------------------------- structures - */

#pragma pack(push, 1)

/* Block header, 48 bytes. A record of N blocks shares seq/chunk_count. */
typedef struct {
    uint32_t magic;            /* AMBRE_MAGIC_BLOCK                           */
    uint16_t version;          /* AMBRE_FORMAT_VERSION                        */
    uint8_t  rec_type;         /* ambre_rec_type_t                            */
    uint8_t  flags;            /* AMBRE_FL_CONTINUATION                       */
    uint64_t seq;              /* record sequence number, monotonic per boot  */
    uint64_t session_id;       /* boot session id, flags stale data           */
    uint64_t first_ts_us;      /* monotonic microseconds, relative (VIE-02)   */
    uint16_t chunk_index;      /* 0-based block index inside the record       */
    uint16_t chunk_count;      /* total blocks in the record, 1..MAX_BLOCKS   */
    uint32_t payload_len;      /* valid bytes in THIS block's payload         */
    uint32_t total_len;        /* valid bytes in the WHOLE record payload     */
    uint32_t payload_crc;      /* CRC-32C over this block's payload_len bytes */
} ambre_block_hdr_t;

/* One 512-byte block: header + payload + whole-block CRC. */
typedef struct {
    ambre_block_hdr_t hdr;                                  /*  48          */
    uint8_t payload[AMBRE_BLOCK_PAYLOAD_MAX];               /* 460          */
    uint32_t block_crc;   /* CRC-32C over the 508 preceding bytes */        /* 4 */
} ambre_block_t;

/* Class B sample, 8 bytes (spec 4.5, 6.2). */
typedef struct {
    uint16_t signal_id;        /* whitelisted signal id (ACQ-04, VIE-01)      */
    uint8_t  quality;          /* 0 valid, 1 frozen, 2 out of range, 3 fault  */
    uint8_t  pad;
    int32_t  value;            /* fixed point, see docs/FORMAT.md             */
} ambre_sample_t;

/* Class A pre-crash point at 2 Hz (spec 6.1), 20 bytes. */
typedef struct {
    uint16_t speed_kmh_x4;     /* 0..250 km/h, resolution 0.25 km/h           */
    uint8_t  throttle_pct;     /* 0..100                                      */
    uint8_t  brake_active;
    uint16_t engine_rpm;
    int16_t  steer_angle_deg_x2;
    int16_t  accel_long_mm_s2; /* mm/s^2                                      */
    int16_t  accel_lat_mm_s2;
    uint8_t  abs_active;
    uint8_t  esc_active;
    uint8_t  belt_driver;
    uint8_t  airbag_lamp;
    int16_t  yaw_rate_deg_s_x10;
    uint16_t rel_time_ms;      /* milliseconds before T0, 0..5000             */
} ambre_a_point_t;

/* EMERG payload: class A snapshot, written in ONE contiguous write (GEL-01). */
typedef struct {
    uint32_t event_id;
    uint8_t  trig_reason;      /* ambre_trig_reason_t                         */
    uint8_t  locked;
    uint8_t  event_number;     /* 1-based index inside the same crash (DEC-09)*/
    uint8_t  ignition_cycle;
    uint64_t t0_ts_us;
    uint64_t trig_ts_us;
    uint32_t crash_order;      /* monotonic counter, oldest-first overwrite   */
    uint16_t point_count;      /* valid entries in points[]                   */
    uint16_t reserved;
    ambre_a_point_t points[21]; /* 5 s at 2 Hz = 10, margin for the US profile */
} ambre_emerg_t;

/* CONT payload: class A during the crash, one record per 10 ms (GEL-04). */
typedef struct {
    uint32_t event_id;
    uint16_t sample_count;
    uint16_t rel_time_ms;      /* offset from T0                              */
    int32_t  dv_long_mm_s;     /* cumulative delta-v at this instant          */
    int32_t  dv_lat_mm_s;
    ambre_a_point_t points[4];
} ambre_cont_t;

/* SEAL payload (GEL-05, REP-03, invariant I7). */
typedef struct {
    uint32_t event_id;
    uint8_t  file_complete;    /* 1 = "oui", 0 = "non"                        */
    uint8_t  seal_cause;       /* ambre_seal_cause_t                          */
    uint8_t  locked;
    uint8_t  event_number;
    uint32_t time_since_first_ms;
    int32_t  max_dv_long_mm_s;
    uint16_t max_dv_long_at_ms;
    int32_t  max_dv_lat_mm_s;
    uint16_t max_dv_lat_at_ms;
    uint16_t restraint_deploy_ms;
    uint16_t vru_deploy_ms;
    uint32_t ignition_cycle_at_read;
} ambre_seal_t;

/* GAP payload (ACQ-06). */
typedef struct {
    uint64_t start_ts_us;
    uint64_t end_ts_us;
    uint32_t reason;
} ambre_gap_t;

/* Event descriptor, 52 bytes, stored in the allocation table. */
typedef struct {
    uint32_t event_id;
    uint32_t order;            /* monotonic, oldest-first overwrite (STO-06)  */
    uint8_t  state;            /* ambre_ev_state_t                            */
    uint8_t  locked;
    uint8_t  file_complete;
    uint8_t  trig_reason;
    uint32_t start_seg;
    uint32_t start_block;      /* block index of the EMERG record             */
    uint32_t end_seg;
    uint32_t end_block;        /* last accepted block index, inclusive        */
    uint64_t trig_ts_us;
    uint64_t seq_emerg;
    uint64_t seq_last;
} ambre_ev_desc_t;

#define AMBRE_MAX_EVENTS 8u

/* Allocation table, exactly one sector, two alternating copies (STO-04). */
typedef struct {
    uint32_t magic;            /* AMBRE_MAGIC_TABLE                           */
    uint32_t version;
    uint32_t generation;       /* monotonic; highest valid generation wins    */
    uint32_t flags;
    uint64_t session_id;
    uint64_t last_seq;         /* highest seq committed by a table update     */
    uint64_t next_event_id;
    uint32_t current_seg;
    uint32_t current_block;    /* next free block index inside current_seg    */
    uint32_t event_count;      /* entries used in events[]                    */
    uint32_t sealed_count;
    uint8_t  seg_states[AMBRE_SEGMENT_COUNT];
    ambre_ev_desc_t events[AMBRE_MAX_EVENTS];         /* 8 * 52 = 416         */
    uint8_t  reserved[12];
    uint32_t crc;              /* CRC-32C over the 508 preceding bytes        */
} ambre_alloc_table_t;

#pragma pack(pop)

_Static_assert(sizeof(ambre_block_hdr_t) == AMBRE_BLOCK_HDR_SIZE, "hdr must be 48");
_Static_assert(sizeof(ambre_block_t) == AMBRE_BLOCK_SIZE, "block must be 512");
_Static_assert(sizeof(ambre_sample_t) == 8, "class B sample must be 8");
_Static_assert(sizeof(ambre_a_point_t) == 20, "class A point must be 20");
_Static_assert(sizeof(ambre_emerg_t) <= AMBRE_BLOCK_PAYLOAD_MAX, "EMERG: one block");
_Static_assert(sizeof(ambre_cont_t) <= AMBRE_BLOCK_PAYLOAD_MAX, "CONT: one block");
_Static_assert(sizeof(ambre_seal_t) <= AMBRE_BLOCK_PAYLOAD_MAX, "SEAL: one block");
_Static_assert(sizeof(ambre_ev_desc_t) == 52, "event descriptor must be 52");
_Static_assert(sizeof(ambre_alloc_table_t) == AMBRE_BLOCK_SIZE, "table must be 512");

#endif /* AMBRE_TYPES_H */
