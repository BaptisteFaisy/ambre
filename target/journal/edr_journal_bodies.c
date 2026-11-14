/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * edr_journal_bodies.c — les corps de réponse du journal, partagés par les deux
 * transports (IPC et resource manager). Voir l'en-tête pour le pourquoi : deux
 * implémentations d'un même corps voudrait dire deux bugs possibles.
 *
 * Ce fichier est une EXTRACTION de edr_journal_svc_handlers.c, pas une
 * réécriture : les lignes déplacées sont celles qui remplissaient déjà
 * STATUS_REPLY et LIST_REPLY, et qui comptaient les blocs d'un événement. Le
 * comportement de l'IPC est donc inchangé, et les suites du PC
 * (test_journal_svc, 8 277 vérifications) continuent de le garantir.
 */

#include "edr_journal_bodies.h"

#include "ambre_journal.h"

#include <string.h>

/* ------------------------------------------------------------------ status -- */

void ambre_edr_fill_status(const ambre_edr_service_t *svc,
                           ambre_ipc_status_body_t *out)
{
    const ambre_journal_stats_t *s = ambre_journal_stats(&svc->journal);
    const ambre_alloc_table_t *t = ambre_journal_table(&svc->journal);

    memset(out, 0, sizeof(*out));
    out->mode              = svc->cfg.continuous_mode ? 1u : 0u;
    out->health            = svc->health;
    out->backend           = (uint8_t)ambre_port_backend();
    out->supervised        = ambre_port_supervised() ? 1u : 0u;
    out->current_seg       = t->current_seg;
    out->current_block     = t->current_block;
    out->open_events       = s->open_events;
    out->acquired_events   = s->acquired_events;
    out->sealed_events     = s->sealed_events;
    out->refused_no_space  = s->refused_no_space;
    out->bytes_written     = s->bytes_written;
    out->flush_count       = s->flush_count;
    out->uptime_ms         = (ambre_port_now_us() - svc->start_us) / 1000u;
    out->last_freeze_us    = svc->last_freeze_us;
    out->worst_freeze_us   = svc->worst_freeze_us;
}

/* -------------------------------------------------------------------- list -- */

uint32_t ambre_edr_list_events(const ambre_edr_service_t *svc,
                               ambre_ipc_event_line_t *out, uint32_t max)
{
    const ambre_alloc_table_t *t = ambre_journal_table(&svc->journal);
    uint32_t count = 0u;
    uint32_t i;

    for (i = 0u; i < t->event_count && count < max; i++) {
        const ambre_ev_desc_t *e = &t->events[i];

        if (e->state == (uint8_t)AMBRE_EV_EMPTY) {
            continue;
        }
        out[count].event_id      = e->event_id;
        out[count].state         = e->state;
        out[count].locked        = e->locked;
        out[count].file_complete = e->file_complete;
        out[count].trig_reason   = e->trig_reason;
        out[count].trig_ts_us    = e->trig_ts_us;
        out[count].seq_emerg     = e->seq_emerg;
        out[count].start_seg     = e->start_seg;
        out[count].start_block   = e->start_block;
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ events -- */

const ambre_ev_desc_t *ambre_edr_find_event(const ambre_edr_service_t *svc,
                                            uint32_t event_id)
{
    return ambre_table_find_event_const(ambre_journal_table(&svc->journal),
                                        event_id);
}

uint32_t ambre_edr_event_blocks(const ambre_ev_desc_t *e)
{
    if (e->end_seg == e->start_seg) {
        return (e->end_block >= e->start_block)
                   ? (e->end_block - e->start_block + 1u) : 0u;
    }
    return (e->end_seg - e->start_seg) * AMBRE_BLOCKS_PER_SEGMENT +
           e->end_block + 1u - e->start_block;
}

void ambre_edr_event_block_at(const ambre_ev_desc_t *e, uint32_t index,
                              uint32_t *seg, uint32_t *blk)
{
    uint32_t s = e->start_seg;
    uint32_t b = e->start_block + index;

    while (b >= AMBRE_BLOCKS_PER_SEGMENT && s < e->end_seg) {
        b -= AMBRE_BLOCKS_PER_SEGMENT;
        s++;
    }
    *seg = s;
    *blk = b;
}

int ambre_edr_read_event_block(const ambre_edr_service_t *svc,
                               const ambre_ev_desc_t *e, uint32_t index,
                               uint8_t raw[AMBRE_BLOCK_SIZE])
{
    uint32_t seg;
    uint32_t blk;

    ambre_edr_event_block_at(e, index, &seg, &blk);
    return (svc->vt.read(svc->vt.ctx, ambre_block_offset(seg, blk), raw,
                         AMBRE_BLOCK_SIZE) == 0) ? AMBRE_PORT_OK
                                                 : AMBRE_PORT_ERR_IO;
}
