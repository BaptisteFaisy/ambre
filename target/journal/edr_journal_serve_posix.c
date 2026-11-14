/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * edr_journal_serve_posix.c — le resource manager n'existe pas sur le banc PC.
 *
 * Le banc POSIX n'a pas de /dev : publier /dev/edr y serait faux. La fonction
 * rend donc AMBRE_PORT_ERR_UNSUPPORTED, et main.c le journalise — c'est le
 * contrat du port (docs/QNX-PORT.md) : un backend qui ne peut pas faire
 * quelque chose le dit, il ne simule pas une réussite.
 *
 * Compilé par les deux builds (il ne se termine pas par `_qnx.c`), c'est la
 * moitié POSIX de l'API déclarée dans edr_journal_serve.h.
 */

#include "edr_journal_serve.h"

int ambre_journal_serve_resmgr(ambre_edr_service_t *svc)
{
    (void)svc;
    return AMBRE_PORT_ERR_UNSUPPORTED;
}
