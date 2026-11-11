/*
 * ambre_fmt_table.c - Allocation table codec, two-copy commit, victim choice.
 *
 * Implements docs/FORMAT.md section 1 (table updates), section 7 (table
 * layout), section 8 (event descriptor) and section 9 (overwrite policy).
 * Portable C11, no dynamic allocation, memcpy-only serialization.
 */

#include "ambre_fmt.h"
#include "ambre_journal.h"   /* mark kinds only (compile-time constants) */
#include <string.h>

/* Table flags (FORMAT.md section 7). The frozen headers expose no macro for
 * these bits, so each codec translation unit names them locally. */
#define TABLE_FL_FORMATTED 0x01u
#define TABLE_FL_MEM_FULL  0x02u

/* CRC-32C over the first 508 bytes of the packed table. Shared with
 * ambre_fmt_scan.c through an internal linkage declaration (the frozen
 * public header must not be extended), so the logic exists exactly once. */
uint32_t ambre_fmt_table_body_crc(const ambre_alloc_table_t *t)
{
    uint8_t raw[AMBRE_BLOCK_SIZE];
    ambre_table_pack(t, raw);
    return ambre_crc32c(0u, raw, AMBRE_BLOCK_SIZE - AMBRE_BLOCK_CRC_SIZE);
}

void ambre_table_init(ambre_alloc_table_t *t, uint64_t session_id)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    t->magic         = AMBRE_MAGIC_TABLE;
    t->version       = AMBRE_FORMAT_VERSION;
    t->generation    = 1u;
    t->flags         = TABLE_FL_FORMATTED;
    t->session_id    = session_id;
    t->next_event_id = 1u;
    t->seg_states[0] = (uint8_t)AMBRE_SEG_CURRENT;
    /* every other segment stays AMBRE_SEG_FREE (0) via memset */
    t->crc = ambre_fmt_table_body_crc(t);
}

int ambre_table_validate(const ambre_alloc_table_t *t)
{
    if (t == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (t->magic != AMBRE_MAGIC_TABLE) {
        return AMBRE_ERR_BAD_MAGIC;
    }
    if (t->version != AMBRE_FORMAT_VERSION) {
        return AMBRE_ERR_BAD_VER;
    }
    if (t->event_count > AMBRE_MAX_EVENTS ||
        t->current_seg >= AMBRE_SEGMENT_COUNT ||
        t->current_block >= AMBRE_BLOCKS_PER_SEGMENT) {
        return AMBRE_ERR_PARAM;
    }
    if (t->crc != ambre_fmt_table_body_crc(t)) {
        return AMBRE_ERR_BAD_CRC;
    }
    return AMBRE_OK;
}

void ambre_table_pack(const ambre_alloc_table_t *t, uint8_t buf[AMBRE_BLOCK_SIZE])
{
    if (t == NULL || buf == NULL) {
        return;
    }
    memcpy(buf, t, AMBRE_BLOCK_SIZE);
}

void ambre_table_unpack(const uint8_t buf[AMBRE_BLOCK_SIZE], ambre_alloc_table_t *t)
{
    if (buf == NULL || t == NULL) {
        return;
    }
    memcpy(t, buf, AMBRE_BLOCK_SIZE);
}

/* --------------------------------------------------------------------------
 * Shared copy loading: read one copy from disk, validate it.
 * ------------------------------------------------------------------------ */

static bool table_load_copy(const ambre_disk_t *d, uint64_t off,
                            ambre_alloc_table_t *out)
{
    uint8_t buf[AMBRE_BLOCK_SIZE];

    if (d->read(d->ctx, off, buf, AMBRE_BLOCK_SIZE) != 0) {
        return false;
    }
    ambre_table_unpack(buf, out);
    return ambre_table_validate(out) == AMBRE_OK;
}

typedef struct {
    ambre_alloc_table_t a;
    ambre_alloc_table_t b;
    bool valid_a;
    bool valid_b;
} table_pair_t;

static int table_load_pair(const ambre_disk_t *d, table_pair_t *p)
{
    if (d == NULL || d->read == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (d->size < (uint64_t)AMBRE_FIRST_DATA_OFFSET) {
        return AMBRE_ERR_IO;
    }
    p->valid_a = table_load_copy(d, AMBRE_TABLE_A_OFFSET, &p->a);
    p->valid_b = table_load_copy(d, AMBRE_TABLE_B_OFFSET, &p->b);
    return AMBRE_OK;
}

int ambre_table_read_best(const ambre_disk_t *d, ambre_alloc_table_t *out,
                          int *copy_used)
{
    table_pair_t p;
    int rc;

    if (copy_used != NULL) {
        *copy_used = -1;
    }
    if (out == NULL) {
        return AMBRE_ERR_PARAM;
    }
    rc = table_load_pair(d, &p);
    if (rc != AMBRE_OK) {
        return rc;
    }
    if (!p.valid_a && !p.valid_b) {
        return AMBRE_ERR_BAD_CRC;
    }
    /* REP-02: keep the valid copy with the highest generation. */
    if (p.valid_b && (!p.valid_a || p.b.generation > p.a.generation)) {
        *out = p.b;
        if (copy_used != NULL) {
            *copy_used = 1;
        }
    } else {
        *out = p.a;
        if (copy_used != NULL) {
            *copy_used = 0;
        }
    }
    return AMBRE_OK;
}

int ambre_table_commit(ambre_disk_t *d, ambre_alloc_table_t *t)
{
    table_pair_t p;
    uint8_t buf[AMBRE_BLOCK_SIZE];
    uint64_t target;
    int rc;

    if (t == NULL) {
        return AMBRE_ERR_PARAM;
    }
    if (d == NULL || d->write == NULL) {
        return AMBRE_ERR_PARAM;
    }
    rc = table_load_pair(d, &p);
    if (rc != AMBRE_OK) {
        return rc;
    }
    /* STO-04: always overwrite the OLDER copy; an invalid copy counts as
     * older than any valid one. Both invalid -> fall back to copy A. */
    if (p.valid_a && p.valid_b) {
        target = (p.a.generation <= p.b.generation)
                 ? AMBRE_TABLE_A_OFFSET : AMBRE_TABLE_B_OFFSET;
    } else {
        target = p.valid_a ? AMBRE_TABLE_B_OFFSET : AMBRE_TABLE_A_OFFSET;
    }

    t->generation++;
    t->crc = ambre_fmt_table_body_crc(t);
    ambre_table_pack(t, buf);
    if (d->write(d->ctx, target, buf, AMBRE_BLOCK_SIZE) != 0) {
        return AMBRE_ERR_IO;
    }    if (d->mark != NULL) {
        d->mark(d->ctx, AMBRE_MARK_TABLE_COMMIT, t->generation, 0u);
    }
    return AMBRE_OK;
}

/* --------------------------------------------------------------------------
 * Event descriptor helpers (FORMAT.md section 8).
 * ------------------------------------------------------------------------ */

ambre_ev_desc_t *ambre_table_find_event(ambre_alloc_table_t *t, uint32_t event_id)
{
    if (t == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < AMBRE_MAX_EVENTS; i++) {
        if (t->events[i].state != (uint8_t)AMBRE_EV_EMPTY &&
            t->events[i].event_id == event_id) {
            return &t->events[i];
        }
    }
    return NULL;
}

const ambre_ev_desc_t *ambre_table_find_event_const(const ambre_alloc_table_t *t,
                                                     uint32_t event_id)
{
    return ambre_table_find_event((ambre_alloc_table_t *)t, event_id);
}

int ambre_table_add_event(ambre_alloc_table_t *t, const ambre_ev_desc_t *ev)
{
    uint32_t free_slot = AMBRE_MAX_EVENTS;

    if (t == NULL || ev == NULL) {
        return AMBRE_ERR_PARAM;
    }
    for (uint32_t i = 0; i < AMBRE_MAX_EVENTS; i++) {
        if (t->events[i].state == (uint8_t)AMBRE_EV_EMPTY) {
            if (free_slot == AMBRE_MAX_EVENTS) {
                free_slot = i;
            }
        } else if (t->events[i].event_id == ev->event_id) {
            t->events[i] = *ev;   /* update in place */
            return AMBRE_OK;
        }
    }
    if (free_slot == AMBRE_MAX_EVENTS) {
        return AMBRE_ERR_FULL;    /* all 8 slots busy with other events */
    }
    t->events[free_slot] = *ev;
    if (free_slot >= t->event_count) {
        t->event_count = free_slot + 1u;
    }
    return AMBRE_OK;
}

/* --------------------------------------------------------------------------
 * Overwrite policy (FORMAT.md section 9, STO-05/06/08).
 * ------------------------------------------------------------------------ */

/* True when segment `seg` holds at least one event and every event touching
 * it is sealed with locked = 0. *best receives the oldest event key of the
 * segment (order first, seq_emerg as tiebreaker). */
typedef struct {
    uint32_t order;
    uint64_t seq;
} ev_order_key_t;

static bool ev_key_less(ev_order_key_t a, ev_order_key_t b)
{
    return (a.order < b.order) || (a.order == b.order && a.seq < b.seq);
}

static bool seg_reclaimable_sealed(const ambre_alloc_table_t *t, uint32_t seg,
                                   ev_order_key_t *best)
{
    uint32_t count = 0u;
    ev_order_key_t min_key = { UINT32_MAX, UINT64_MAX };

    if (t->seg_states[seg] == (uint8_t)AMBRE_SEG_LOCKED ||
        t->seg_states[seg] == (uint8_t)AMBRE_SEG_CURRENT) {
        return false;
    }
    for (uint32_t i = 0; i < AMBRE_MAX_EVENTS; i++) {
        const ambre_ev_desc_t *e = &t->events[i];
        ev_order_key_t k;
        if (e->state == (uint8_t)AMBRE_EV_EMPTY || e->start_seg > seg ||
            seg > e->end_seg) {
            continue;
        }
        if (e->locked != 0u || e->state != (uint8_t)AMBRE_EV_SEALED) {
            return false;
        }
        count++;
        k.order = e->order;
        k.seq = e->seq_emerg;
        if (ev_key_less(k, min_key)) {
            min_key = k;
        }
    }
    if (count == 0u) {
        return false;
    }
    *best = min_key;
    return true;
}

int ambre_table_pick_victim(ambre_alloc_table_t *t, uint32_t *seg_out)
{
    uint32_t best_seg = UINT32_MAX;
    ev_order_key_t best_key = { UINT32_MAX, UINT64_MAX };

    if (t == NULL || seg_out == NULL) {
        return AMBRE_ERR_PARAM;
    }
    /* 1. FREE segment, oldest in ring order first. */
    for (uint32_t i = 0; i < AMBRE_SEGMENT_COUNT; i++) {
        uint32_t seg = (t->current_seg + i) % AMBRE_SEGMENT_COUNT;
        if (t->seg_states[seg] == (uint8_t)AMBRE_SEG_FREE) {
            *seg_out = seg;
            return AMBRE_OK;
        }
    }
    /* 2. ARCHIVED segment, same ring order. */
    for (uint32_t i = 0; i < AMBRE_SEGMENT_COUNT; i++) {
        uint32_t seg = (t->current_seg + i) % AMBRE_SEGMENT_COUNT;
        if (t->seg_states[seg] == (uint8_t)AMBRE_SEG_ARCHIVED) {
            *seg_out = seg;
            return AMBRE_OK;
        }
    }
    /* 3. Segment fully sealed + unlocked, oldest event first. */
    for (uint32_t seg = 0; seg < AMBRE_SEGMENT_COUNT; seg++) {
        ev_order_key_t k = { UINT32_MAX, UINT64_MAX };
        if (seg_reclaimable_sealed(t, seg, &k) && ev_key_less(k, best_key)) {
            best_key = k;
            best_seg = seg;
        }
    }
    if (best_seg != UINT32_MAX) {
        *seg_out = best_seg;
        return AMBRE_OK;
    }
    /* 4. Nothing reclaimable: set memory-full and refuse (STO-08). */
    t->flags |= TABLE_FL_MEM_FULL;
    return AMBRE_ERR_FULL;
}
