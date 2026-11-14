/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * edr_journal_bodies.h — les corps de réponse du journal, en UN SEUL endroit.
 *
 * Le service répond par deux transports différents, sur la cible :
 *   - l'IPC  (name_open/MsgSend, ce qu'edrctl et edr-capture utilisent) ;
 *   - le resource manager (open/read/write/devctl sur /dev/edr, QNX-01, QNX-03,
 *     ce qu'un client devctl() utilise — OBS-02, EXP-01, EXP-08).
 * Les deux doivent rendre EXACTEMENT les mêmes octets : un corps de STATUS qui
 * différerait entre le devctl et la réponse IPC serait un bug réel, invisible
 * tant qu'un seul transport est exercé. Ces fonctions sont donc partagées, et
 * `edr_journal_svc_handlers.c` (l'IPC) comme `edr_journal_serve_qnx.c` (le
 * resource manager) les appellent — aucune des deux ne recalcule rien.
 *
 * Elles sont portables (aucun en-tête QNX) : la même implémentation sert au banc
 * POSIX et à la cible, ce qui est le principe du port (docs/QNX-PORT.md).
 */

#ifndef EDR_JOURNAL_BODIES_H
#define EDR_JOURNAL_BODIES_H

#include "ambre_port.h"
#include "edr_ipc.h"
#include "edr_journal_svc.h"

#include "ambre_types.h"

/* Le corps de AMBRE_MSG_STATUS_REPLY / DCMD_EDR_STATUS (OBS-02, GEL-08). */
void ambre_edr_fill_status(const ambre_edr_service_t *svc,
                           ambre_ipc_status_body_t *out);

/* Les lignes de AMBRE_MSG_LIST_REPLY / DCMD_EDR_LIST (EXP-01). Rend le nombre
 * de lignes écrites dans `out`, au plus `max`. */
uint32_t ambre_edr_list_events(const ambre_edr_service_t *svc,
                               ambre_ipc_event_line_t *out, uint32_t max);

/* Le descripteur d'événement dans la table d'allocation, NULL s'il n'existe
 * pas (EXP-01, SEC-03 : un événement verrouillé se trouve comme les autres). */
const ambre_ev_desc_t *ambre_edr_find_event(const ambre_edr_service_t *svc,
                                            uint32_t event_id);

/* Nombre de blocs bruts d'un événement : EMERG, les CONT, puis SEAL. Un
 * événement tient normalement dans un segment ; il en déborde sur deux quand le
 * journal a dû pivoter pendant qu'il était ouvert. */
uint32_t ambre_edr_event_blocks(const ambre_ev_desc_t *e);

/* Index linéaire dans l'événement -> (segment, bloc) sur le média. */
void ambre_edr_event_block_at(const ambre_ev_desc_t *e, uint32_t index,
                              uint32_t *seg, uint32_t *blk);

/* Un bloc d'un événement, tel qu'il est sur le média : l'archive et edrctl
 * rebuillent le MCAP avec ces octets (EXP-02). */
int ambre_edr_read_event_block(const ambre_edr_service_t *svc,
                               const ambre_ev_desc_t *e, uint32_t index,
                               uint8_t raw[AMBRE_BLOCK_SIZE]);

#endif /* EDR_JOURNAL_BODIES_H */
