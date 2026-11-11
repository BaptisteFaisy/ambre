/*
 * ambre_trigger_window.h — Ring buffer and monotonic min/max deque helpers
 * used by the trigger engine (fmt/ambre_trigger.c).
 *
 * Split out of ambre_trigger.c to keep every file under 500 lines (house
 * rule). Internal API: not part of the public trigger contract.
 */

#ifndef AMBRE_TRIGGER_WINDOW_H
#define AMBRE_TRIGGER_WINDOW_H

#include "ambre_trigger.h"

#define AMBRE_TRIG_WINDOW_OK 0

/* One axis: its ring plus the two monotonic deques tracking its extrema. */
typedef struct {
    ambre_trig_ring_t  *ring;
    ambre_trig_deque_t *dmax;
    ambre_trig_deque_t *dmin;
} ambre_trig_axis_t;

uint16_t ambre_trig_ring_oldest(const ambre_trig_ring_t *r);

uint16_t ambre_trig_dq_front(const ambre_trig_deque_t *d);
uint16_t ambre_trig_dq_back(const ambre_trig_deque_t *d);
void     ambre_trig_dq_pop_front(ambre_trig_deque_t *d);
void     ambre_trig_dq_pop_back(ambre_trig_deque_t *d);
void     ambre_trig_dq_push(ambre_trig_deque_t *d, uint16_t pos);
void     ambre_trig_dq_drop_overwritten(ambre_trig_deque_t *d,
                                        const ambre_trig_ring_t *r);
void     ambre_trig_dq_evict_before(ambre_trig_deque_t *d,
                                    const ambre_trig_ring_t *r, uint64_t cutoff);
bool     ambre_trig_dq_first_since(const ambre_trig_deque_t *d,
                                   const ambre_trig_ring_t *r,
                                   uint64_t cutoff, uint16_t *pos);

void     ambre_trig_axis_push(ambre_trig_axis_t ax, uint64_t ts_us, int64_t dv);
void     ambre_trig_axis_trim(ambre_trig_axis_t ax, uint64_t ts_us,
                              uint16_t widest_ms);
int64_t  ambre_trig_axis_window_delta(ambre_trig_axis_t ax, uint64_t ts_us,
                                      uint16_t win_ms);
uint64_t ambre_trig_window_cutoff(uint64_t ts_us, uint16_t win_ms);

ambre_trig_axis_t ambre_trig_axis_long(ambre_trig_state_t *st);
ambre_trig_axis_t ambre_trig_axis_lat(ambre_trig_state_t *st);

#endif /* AMBRE_TRIGGER_WINDOW_H */
