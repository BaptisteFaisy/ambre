/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * edr_dcmd.h — les devctl d'Ambre sur /dev/edr, encodés selon l'ABI QNX 8.0.
 *
 * SOURCE UNIQUE : le service (target/journal/edr_journal_io_qnx.c) et les
 * clients (host/qnx_probe/devctl_probe.c, edrctl, edr-archive) prennent leurs
 * codes ICI. Deux copies d'un même code numérique est le genre de chose qui
 * marche jusqu'au jour où l'une des deux moitiés est recompilée.
 *
 * POURQUOI LES NUMÉROS BRUTS DU DÉPÔT NE POUVOIENT PAS MARCHER SUR LA CIBLE.
 * Les codes projet (0x45000001..0x45000004, docs/QNX-ABI.md §3) encodent, dans
 * les deux bits de poids fort, une DIRECTION et, dans les octets 16-31, une
 * TAILLE (devctl.h : get_device_size() = ((cmd & ~DEVDIR_TOFROM) >> 16)). Sur
 * 0x45000001 la taille encodée vaut 0 et la classe 0x45 n'existe pas dans la
 * table QNX (0x000..0x12ff). Le noyau ne copiait donc RIEN de la réponse vers
 * le client : `devctl()` rendait EOK avec un tampon intact, et la sonde voyait
 * des champs à zéro. Mesuré sur la cible le 2026-11-14 :
 *     devctl DCMD_EDR_STATUS: errno=0 (No error)   puis   DEVCTL-PROBE FAIL
 * L'en-tête du dépôt l'avoit anticipé sans pouvoir le faire (« re-encode with
 * QNX's __DIOTF() macros from <devctl.h> on target », docs/QNX-ABI.md §3) :
 * c'est ce que ce fichier fait, avec la classe recommandée pour un devctl non
 * QNX (`_DCMD_MISC`, devctl.h) et les corps réels.
 *
 * Sens des directions (devctl.h) :
 *   _POSIX_DEVDIR_FROM  la donnée vient DU serveur (réponse seule)
 *   _POSIX_DEVDIR_TO    la donnée va VERS le serveur (requête seule)
 *   _POSIX_DEVDIR_TOFROM les deux (requête + réponse dans le même tampon)
 */

#ifndef EDR_DCMD_H
#define EDR_DCMD_H

#include <devctl.h>

#include "edr_ipc.h"
#include "journal/edr_journal_svc.h"

/* OBS-02, EXP-01 : le client ne demande rien, le serveur rend un corps. */
#define DCMD_EDR_STATUS    __DIOF(_DCMD_MISC, 1, ambre_ipc_status_body_t)
#define DCMD_EDR_LIST      __DIOF(_DCMD_MISC, 2, ambre_edr_list_body_t)

/* EXP-01, EXP-08 : `uint32_t[2]` (event_id, premier bloc) en entre, les blocs
 * bruts en sortie — donc TOFROM. Le client passe les deux tampons séparés par
 * devctlv(), la taille encodée reste celle de la requête. */
#define DCMD_EDR_EXPORT    __DIOTF(_DCMD_MISC, 3, uint32_t[2])

/* OBS-04 : l'auto-test de démarrage, sans corps (EOK ou EIO seulement). */
#define DCMD_EDR_AUTOTEST  __DION(_DCMD_MISC, 4)

/* La réponse de DCMD_EDR_EXPORT : le corps gelé puis les blocs (au plus
 * DCMD_EDR_EXPORT_BLOCKS secteurs de AMBRE_BLOCK_SIZE octets). */
#define DCMD_EDR_EXPORT_BLOCKS  7u

#endif /* EDR_DCMD_H */
