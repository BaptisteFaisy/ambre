/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * edr_journal_serve_qnx.c — branche le journal sur /dev/edr (QNX-01, QNX-03).
 *
 * Ce fichier est l'ADAPTATEUR : il relie les poignées de callback attendues par
 * le resource manager (edr_journal_qnx.c, edr_journal_io_qnx.c) au service
 * portable (edr_journal_svc.c). C'est le maillon qui manquait : les handlers
 * étaient écrits, compilés, syntax-vérifiés — et personne ne les appelait, donc
 * aucun devctl n'existait sur la cible.
 *
 * Les corps de réponse viennent d'edr_journal_bodies.c, partagés avec le
 * transport IPC : un STATUS rendu par devctl et un STATUS rendu par MsgSend sont
 * les mêmes octets, au bit près. Aucune des deux voies ne recalcule rien.
 *
 * QNX-only : compilé par `make qnx` contre les vrais en-têtes de la SDP 8.0,
 * exclu du build PC (le suffixe `_qnx.c` est le filtre du Makefile).
 */

#include "edr_journal_qnx_internal.h"
#include "edr_journal_bodies.h"
#include "edr_journal_serve.h"

#include "ambre_journal.h"

/* Le contexte de chaque callback est le service lui-même (une seule instance
 * par processus : c'est le modèle du binaire, pas une hypothèse ajoutée ici). */

static int src_status(void *ctx, ambre_ipc_status_body_t *out)
{
    ambre_edr_fill_status((const ambre_edr_service_t *)ctx, out);
    return 0;
}

static int src_list(void *ctx, ambre_ipc_event_line_t *out, uint32_t max)
{
    return (int)ambre_edr_list_events((const ambre_edr_service_t *)ctx, out, max);
}

static uint32_t src_event_blocks_count(void *ctx, uint32_t event_id)
{
    const ambre_edr_service_t *svc = (const ambre_edr_service_t *)ctx;
    const ambre_ev_desc_t *e = ambre_edr_find_event(svc, event_id);

    return (e != NULL) ? ambre_edr_event_blocks(e) : 0u;
}

static int src_event_blocks(void *ctx, uint32_t event_id, uint32_t first,
                             uint32_t count, void *out)
{
    const ambre_edr_service_t *svc = (const ambre_edr_service_t *)ctx;
    const ambre_ev_desc_t *e = ambre_edr_find_event(svc, event_id);
    uint8_t *dst = (uint8_t *)out;
    uint32_t total, k;

    if (e == NULL || dst == NULL) {
        return -1;
    }
    total = ambre_edr_event_blocks(e);
    if (first >= total || count == 0u) {
        return -1;
    }
    if (count > total - first) {
        count = total - first;
    }
    for (k = 0u; k < count; k++) {
        if (ambre_edr_read_event_block(svc, e, first + k,
                                       dst + (size_t)k * AMBRE_BLOCK_SIZE)
            != AMBRE_PORT_OK) {
            return -1;
        }
    }
    return (int)count;
}

static int src_autotest(void *ctx)
{
    /* OBS-04 : l'auto-test de démarrage du service, le même que celui que la
     * journalisation imprime au lancement — pas une variante devctl. */
    return ambre_edr_service_selftest((ambre_edr_service_t *)ctx);
}

int ambre_journal_serve_resmgr(ambre_edr_service_t *svc)
{
    edr_journal_qnx_src_t src;

    if (svc == NULL) {
        return AMBRE_PORT_ERR_PARAM;
    }
    memset(&src, 0, sizeof(src));
    src.ctx               = svc;
    src.status            = src_status;
    src.list              = src_list;
    src.event_blocks      = src_event_blocks;
    src.event_block_count = src_event_blocks_count;
    src.autotest          = src_autotest;

    edr_journal_qnx_set_source(&src);
    return (edr_journal_qnx_start() == AMBRE_PORT_OK) ? AMBRE_PORT_OK
                                                      : AMBRE_PORT_ERR_IO;
}
