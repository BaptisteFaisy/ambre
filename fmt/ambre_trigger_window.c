/*
 * ambre_trigger_window.c — Ring buffer and monotonic min/max deque helpers.
 * Moved verbatim from ambre_trigger.c (house rule: files under 500 lines).
 */

#include "ambre_trigger_window.h"

#define RING_SLOTS ((uint16_t)AMBRE_TRIG_RING_SLOTS)

/* ------------------------------------------------------- deques and rings -- */

/* Oldest valid position in the ring (the slot the next push will overwrite). */
uint16_t ambre_trig_ring_oldest(const ambre_trig_ring_t *r)
{
    return r->head;
}

uint16_t ambre_trig_dq_front(const ambre_trig_deque_t *d)
{
    return d->idx[d->head];
}

uint16_t ambre_trig_dq_back(const ambre_trig_deque_t *d)
{
    return d->idx[(uint16_t)((d->head + d->count - 1u) % RING_SLOTS)];
}

void ambre_trig_dq_pop_front(ambre_trig_deque_t *d)
{
    d->head = (uint16_t)((d->head + 1u) % RING_SLOTS);
    d->count--;
}

void ambre_trig_dq_pop_back(ambre_trig_deque_t *d)
{
    d->count--;
}

void ambre_trig_dq_push(ambre_trig_deque_t *d, uint16_t pos)
{
    d->idx[(uint16_t)((d->head + d->count) % RING_SLOTS)] = pos;
    d->count++;
}

/* Drop a queued index that is about to be overwritten by the next ring push.
 * The ring head is the oldest slot, so it can only sit at the deque front. */
void ambre_trig_dq_drop_overwritten(ambre_trig_deque_t *d, const ambre_trig_ring_t *r)
{
    if (r->count == RING_SLOTS && d->count > 0 && ambre_trig_dq_front(d) == ambre_trig_ring_oldest(r)) {
        ambre_trig_dq_pop_front(d);
    }
}

/* Destructive eviction: entries older than cutoff can never re-enter a window
 * (window cutoffs only move forward), so dropping them is final. */
void ambre_trig_dq_evict_before(ambre_trig_deque_t *d, const ambre_trig_ring_t *r,
                            uint64_t cutoff)
{
    while (d->count > 0 && r->slots[ambre_trig_dq_front(d)].ts_us < cutoff) {
        ambre_trig_dq_pop_front(d);
    }
}

/* Non-destructive lookup: the first queued candidate still inside the window.
 * Because deque values are monotonic (decreasing for max, increasing for min),
 * that first survivor carries the extreme of the window. */
bool ambre_trig_dq_first_since(const ambre_trig_deque_t *d, const ambre_trig_ring_t *r,
                           uint64_t cutoff, uint16_t *pos)
{
    for (uint16_t k = 0; k < d->count; k++) {
        uint16_t cand = d->idx[(uint16_t)((d->head + k) % RING_SLOTS)];
        if (r->slots[cand].ts_us >= cutoff) {
            *pos = cand;
            return true;
        }
    }
    return false;
}

void ambre_trig_axis_push(ambre_trig_axis_t ax, uint64_t ts_us, int64_t dv)
{
    ambre_trig_dq_drop_overwritten(ax.dmax, ax.ring);
    ambre_trig_dq_drop_overwritten(ax.dmin, ax.ring);

    uint16_t pos = ax.ring->head;
    ax.ring->slots[pos].ts_us = ts_us;
    ax.ring->slots[pos].dv = dv;
    ax.ring->head = (uint16_t)((ax.ring->head + 1u) % RING_SLOTS);
    if (ax.ring->count < RING_SLOTS) ax.ring->count++;

    while (ax.dmax->count > 0 && ax.ring->slots[ambre_trig_dq_back(ax.dmax)].dv <= dv) {
        ambre_trig_dq_pop_back(ax.dmax);
    }
    ambre_trig_dq_push(ax.dmax, pos);

    while (ax.dmin->count > 0 && ax.ring->slots[ambre_trig_dq_back(ax.dmin)].dv >= dv) {
        ambre_trig_dq_pop_back(ax.dmin);
    }
    ambre_trig_dq_push(ax.dmin, pos);
}

/* Cutoff of the inclusive window of win_ms ending at ts_us (A4). */
uint64_t ambre_trig_window_cutoff(uint64_t ts_us, uint16_t win_ms)
{
    uint64_t span = (uint64_t)win_ms * 1000ULL;
    return (ts_us < span) ? 0 : (ts_us - span);
}

/* Drop every candidate older than the widest window; narrower windows are then
 * answered without touching the deques, so a 20 ms query never destroys what
 * the next sample's 150 ms query still needs. */
void ambre_trig_axis_trim(ambre_trig_axis_t ax, uint64_t ts_us, uint16_t widest_ms)
{
    uint64_t cutoff = ambre_trig_window_cutoff(ts_us, widest_ms);
    ambre_trig_dq_evict_before(ax.dmax, ax.ring, cutoff);
    ambre_trig_dq_evict_before(ax.dmin, ax.ring, cutoff);
}

/* A4: max(dv) - min(dv) over the inclusive window ending at ts_us. */
int64_t ambre_trig_axis_window_delta(ambre_trig_axis_t ax, uint64_t ts_us, uint16_t win_ms)
{
    uint64_t cutoff = ambre_trig_window_cutoff(ts_us, win_ms);
    uint16_t hi, lo;

    if (!ambre_trig_dq_first_since(ax.dmax, ax.ring, cutoff, &hi)) return 0;
    if (!ambre_trig_dq_first_since(ax.dmin, ax.ring, cutoff, &lo)) return 0;

    return ax.ring->slots[hi].dv - ax.ring->slots[lo].dv;
}

ambre_trig_axis_t ambre_trig_axis_long(ambre_trig_state_t *st)
{
    ambre_trig_axis_t ax = { &st->ring_long, &st->max_long, &st->min_long };
    return ax;
}

ambre_trig_axis_t ambre_trig_axis_lat(ambre_trig_state_t *st)
{
    ambre_trig_axis_t ax = { &st->ring_lat, &st->max_lat, &st->min_lat };
    return ax;
}
