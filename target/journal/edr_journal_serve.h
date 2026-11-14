/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * edr_journal_serve.h — expose le journal comme un resource manager (QNX-01).
 *
 * Le service a DEUX visages sur la cible :
 *   - l'IPC  (name_attach/name_open) : ce que edrctl et edr-capture utilisent
 *     déjà, et ce que la vitrine démontre depuis le 12/11 ;
 *   - le resource manager (/dev/edr, open/read/write/devctl) : QNX-01, QNX-03,
 *     OBS-02, EXP-01, EXP-08.
 * Le second n'était pas branché : `edr_journal_qnx_start()` n'était appelée par
 * personne dans le dépôt (vérifié par grep le 2026-11-14), donc les quatre
 * devctl n'existaient que dans le code compilé. Ce point d'entrée est ce qui
 * manque pour les exercer sur un noyau QNX réel — host/qnx_probe/devctl_probe.c
 * est le client qui le fait.
 *
 * Backend POSIX : la fonction rend AMBRE_PORT_ERR_UNSUPPORTED (le banc PC n'a
 * pas de /dev), rapporté plutôt que simulé — c'est le contrat du port
 * (docs/QNX-PORT.md) et l'exact opposé d'un faux « OK ».
 */

#ifndef EDR_JOURNAL_SERVE_H
#define EDR_JOURNAL_SERVE_H

#include "ambre_port.h"
#include "edr_journal_svc.h"

/* Attache le chemin du resource manager et démarre son pool de threads. `path`
 * est le préfixe publié (EDR_PATH="/dev/edr" sur la cible) ; les clients
 * ouvrent ensuite /dev/edr/journal (statut, liste, export) ou
 * /dev/edr/events/<id> (blocs bruts d'un événement).
 *
 * Rend AMBRE_PORT_OK quand l'interface est servie, un code négatif sinon.
 * Appeler cette fonction ne remplace pas l'IPC : les deux coexistent, et c'est
 * voulu — le transport figé du protocole est un MsgSend par trame (ACQ-01), pas
 * un devctl. */
int ambre_journal_serve_resmgr(ambre_edr_service_t *svc);

#endif /* EDR_JOURNAL_SERVE_H */
