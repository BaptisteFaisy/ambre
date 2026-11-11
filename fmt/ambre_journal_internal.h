/*
 * ambre_journal_internal.h — Internal helpers shared by the journal engine.
 *
 * Not part of the public API: the engine is split into ambre_journal_core.c
 * (lifecycle, capture, flush, segment rotation) and ambre_journal_event.c
 * (trigger, continuation, sealing, read back) to keep both files small.
 */

#ifndef AMBRE_JOURNAL_INTERNAL_H
#define AMBRE_JOURNAL_INTERNAL_H

#include "ambre_journal.h"

/* Write one record (1..AMBRE_REC_MAX_BLOCKS blocks) with a single contiguous
 * disk write (GEL-01). The record is assembled in j->rec_buf. */
int ambre_journal_write_record(ambre_journal_t *j, uint8_t rec_type,
                               uint64_t ts_us, const void *payload,
                               uint32_t payload_len, uint64_t *seq_used,
                               uint32_t *blocks_used);

/* Move the write pointer forward and bump the sequence number. */
void ambre_journal_advance(ambre_journal_t *j, uint32_t blocks, uint64_t seq);

/* Offset of the next free block in the current segment. */
uint64_t ambre_journal_write_offset(const ambre_journal_t *j);

/* Find a segment to write into next: a free one, else the oldest segment
 * whose events are all sealed and unlocked (STO-05/06). Returns
 * AMBRE_ERR_FULL when every segment holds a locked event (STO-08). */
int ambre_journal_pick_segment(ambre_journal_t *j, uint32_t *seg_out);

/* Mark the events of a recycled segment as overwritten (FORMAT.md section 9). */
int ambre_journal_overwrite_segment(ambre_journal_t *j, uint32_t seg);

/* Register an event in the allocation table and commit it. */
int ambre_journal_register_event(ambre_journal_t *j, const ambre_ev_desc_t *desc);

/* Commit the table after a segment change (no event is registered). */
int ambre_journal_register_event_marker(ambre_journal_t *j);

#endif /* AMBRE_JOURNAL_INTERNAL_H */
