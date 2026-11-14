/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * devctl_probe.c — un VRAI client devctl() du journal, sur la cible QNX 8.0.
 *
 * Ce que ça prouve, et pourquoi ça manquait : /dev/edr est un resource manager
 * (QNX-01) dont les handlers `open/read/write/devctl` étoient compilés et
 * vérifiés syntaxiquement, mais la démo ne parlait AU SERVICE que par
 * name_open()/IPC (edrctl). Les quatre devctl n'étoient donc jamais exercés sur
 * un noyau QNX réel : rien ne prouvait que l'ABI binaire (encodage du code,
 * tailles, packing, longueur de réponse) tienne des deux côtés du fil.
 *
 * Les codes ET les corps viennent de target/edr_dcmd.h, target/edr_ipc.h et
 * target/journal/edr_journal_svc.h : une seule source pour le service et le
 * client, donc aucune divergence possible. C'est le précédent du dépôt
 * (host/qnx_probe/name_probe.c ne redéclarait rien) poussé à son terme.
 *
 * Sortie : `champ=valeur`, une ligne par information, une ligne par événement,
 * stable et parsable par un script hôte. Codes : 0 si toutes les devctl
 * attendues ont répondu avec un corps cohérent, 1 échec fonctionnel (EIO,
 * service absent, événement inconnu, body out of range), 2 erreur d'usage.
 */
#define _POSIX_C_SOURCE 200809L

#include "edr_dcmd.h"

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>          /* struct iovec, pour devctlv()               */
#include <unistd.h>

typedef struct {
    const char *journal;
    const char *what;
    uint32_t event;
    uint32_t block;
} opts_t;

static const char *state_name(uint8_t s)
{
    switch (s) {
    case 0u: return "empty";
    case 1u: return "open";
    case 2u: return "sealed";
    default: return "unknown";
    }
}

/* --------------------------------------------------------------- appels ----- */

/* OBS-02 : le statut binaire du journal. La cohérence des champs bornés est la
 * vérification d'ABI : un code mal encodé rend un tampon intact (tout à zéro,
 * donc indistinguable d'un journal vierge), un corps mal descripté sort de ses
 * domaines. Les deux sont refusés ici, pas imprimés comme un résultat. */
static void print_status(const ambre_ipc_status_body_t *st, int info)
{
    printf("STATUS mode=%u health=%u backend=%u supervised=%u\n",
           st->mode, st->health, st->backend, st->supervised);
    printf("STATUS segment=%u block=%u open=%u acquired=%u sealed=%u refused=%u\n",
           st->current_seg, st->current_block, st->open_events,
           st->acquired_events, st->sealed_events, st->refused_no_space);
    printf("STATUS bytes_written=%llu flushes=%llu uptime_ms=%llu "
           "last_freeze_us=%llu worst_freeze_us=%llu dev_info=%d\n",
           (unsigned long long)st->bytes_written,
           (unsigned long long)st->flush_count,
           (unsigned long long)st->uptime_ms,
           (unsigned long long)st->last_freeze_us,
           (unsigned long long)st->worst_freeze_us, info);
}

static int ask_status(int fd)
{
    ambre_ipc_status_body_t st;
    int info = 0;
    int r;

    memset(&st, 0xAA, sizeof(st));          /* motif non nul : un tampon vide se
                                             * voit, il ne passe pas pour un
                                             * journal à zéro événement */
    /* devctl() rend le code DU SERVEUR, pas -1 : un échec se lit dans la
     * valeur rendue ET dans errno. Imprimer les deux, sinon un refus du service
     * ressemble à un tampon non rempli et on cherche du mauvais côté. */
    r = devctl(fd, (int)DCMD_EDR_STATUS, &st, (int)sizeof(st), &info);
    if (r != 0) {
        fprintf(stderr, "devctl DCMD_EDR_STATUS: rc=%d errno=%d (%s) "
                "code=%d size=%d direction=%d\n", r, errno,
                strerror(errno), get_device_command((int)DCMD_EDR_STATUS),
                get_device_size((int)DCMD_EDR_STATUS),
                get_device_direction((int)DCMD_EDR_STATUS));
        return 1;
    }
    /* La vérification d'ABI se fait sur le CONTENU : le corps est un struct de
     * taille fixe, et ses champs bornés ne peuvent pas sortir de leurs
     * domaines. Un code mal encodé rend un tampon intact (0xAA partout), un
     * corps mal descripté sort des domaines : les deux sont refusés ici. */
    if (st.mode > 1u || st.health > 2u || st.backend > 2u) {
        fprintf(stderr, "devctl DCMD_EDR_STATUS: body out of range "
                "(mode=%u health=%u backend=%u)\n", st.mode, st.health, st.backend);
        return 1;
    }
    print_status(&st, info);
    return 0;
}

/* EXP-01 : la liste des événements, par le corps gelé (compteur + lignes). */
static int ask_list(int fd)
{
    static ambre_edr_list_body_t body;
    int info = 0;
    int r;
    uint32_t i;

    memset(&body, 0xAA, sizeof(body));
    r = devctl(fd, (int)DCMD_EDR_LIST, &body, (int)sizeof(body), &info);
    if (r != 0) {
        fprintf(stderr, "devctl DCMD_EDR_LIST: rc=%d errno=%d (%s)\n",
                r, errno, strerror(errno));
        return 1;
    }
    if (body.count > AMBRE_MAX_EVENTS || body.lines[0].state > 2u) {
        fprintf(stderr, "devctl DCMD_EDR_LIST: body out of range (count=%u)\n",
                body.count);
        return 1;
    }
    for (i = 0u; i < body.count; i++) {
        printf("EVENT id=%u state=%s locked=%u complete=%u reason=%u "
               "trig_ts_us=%llu seq_emerg=%llu start_seg=%u start_block=%u\n",
               body.lines[i].event_id, state_name(body.lines[i].state),
               body.lines[i].locked, body.lines[i].file_complete,
               body.lines[i].trig_reason,
               (unsigned long long)body.lines[i].trig_ts_us,
               (unsigned long long)body.lines[i].seq_emerg,
               body.lines[i].start_seg, body.lines[i].start_block);
    }
    printf("LIST events=%u dev_info=%d line_bytes=%zu\n",
           body.count, info, sizeof(body.lines[0]));
    return 0;
}

/* EXP-01, EXP-08 : les blocs bruts d'un événement, tels qu'ils sont sur le
 * média. devctlv() et pas devctl() : la requête (uint32[2]) et la réponse (le
 * corps puis les secteurs) sont deux tampons distincts. */
static int ask_export(int fd, const opts_t *o)
{
    static uint8_t reply[sizeof(ambre_edr_export_body_t)
                         + DCMD_EDR_EXPORT_BLOCKS * AMBRE_BLOCK_SIZE];
    struct iovec sv[1], rv[1];
    ambre_edr_export_body_t head;
    uint32_t req[2];
    uint8_t *blocks;
    int info = 0;
    int r;
    size_t i;
    uint8_t xor = 0u;

    memset(reply, 0xAA, sizeof(reply));
    memset(&head, 0, sizeof(head));
    req[0] = o->event;
    req[1] = o->block;
    sv[0].iov_base = req;
    sv[0].iov_len = sizeof(req);
    rv[0].iov_base = reply;
    rv[0].iov_len = sizeof(reply);
    r = devctlv(fd, (int)DCMD_EDR_EXPORT, 1, 1, sv, rv, &info);
    if (r != 0) {
        fprintf(stderr, "devctl DCMD_EDR_EXPORT (event=%u block=%u): rc=%d "
                "errno=%d (%s)\n", o->event, o->block, r, errno, strerror(errno));
        return 1;
    }
    memcpy(&head, reply, sizeof(head));
    blocks = reply + sizeof(head);
    if (head.event_id != o->event || head.first_block != o->block
        || head.block_count > DCMD_EDR_EXPORT_BLOCKS) {
        fprintf(stderr, "devctl DCMD_EDR_EXPORT: header inconsistent "
                "(event=%u block=%u count=%u)\n",
                head.event_id, head.first_block, head.block_count);
        return 1;
    }
    for (i = 0u; i < (size_t)head.block_count * AMBRE_BLOCK_SIZE; i++) {
        xor ^= blocks[i];
    }
    printf("EXPORT event=%u block=%u blocks=%u total=%u xor=%02x head=",
           head.event_id, head.first_block, head.block_count, head.total_blocks, xor);
    for (i = 0u; i < 16u && i < (size_t)head.block_count * AMBRE_BLOCK_SIZE; i++) {
        printf("%02x", blocks[i]);
    }
    printf("\n");
    return (head.block_count > 0u) ? 0 : 1;
}

/* OBS-04 : l'auto-test de démarrage, le même que celui que le service
 * journalise au lancement — pas une variante devctl. */
static int ask_autotest(int fd)
{
    int info = 0;
    int r;

    r = devctl(fd, (int)DCMD_EDR_AUTOTEST, NULL, 0, &info);
    if (r != 0) {
        fprintf(stderr, "devctl DCMD_EDR_AUTOTEST: rc=%d errno=%d (%s)\n",
                r, errno, strerror(errno));
        return 1;
    }
    printf("AUTOTEST ok=1 dev_info=%d\n", info);
    return 0;
}

/* ------------------------------------------------------------------ main ---- */

static int parse_args(int argc, char **argv, opts_t *o)
{
    int i;

    o->journal = "/dev/edr";
    o->what = "all";
    o->event = 1u;
    o->block = 0u;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--journal") == 0 && i + 1 < argc) {
            o->journal = argv[++i];
        } else if (strcmp(argv[i], "--event") == 0 && i + 1 < argc) {
            o->event = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--block") == 0 && i + 1 < argc) {
            o->block = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--help") == 0) {
            return 2;
        } else if (argv[i][0] != '-') {
            o->what = argv[i];
        } else {
            return 2;
        }
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--journal <chemin>] [--event N] [--block N] "
            "[status|list|export|autotest|all]\n", prog);
}

int main(int argc, char **argv)
{
    opts_t o;
    int fd, rc = 0;

    if (parse_args(argc, argv, &o) != 0) {
        usage(argv[0]);
        return 2;
    }
    fd = open(o.journal, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: errno=%d (%s)\n", o.journal, errno, strerror(errno));
        return 1;
    }
    if (strcmp(o.what, "status") == 0 || strcmp(o.what, "all") == 0) {
        rc |= ask_status(fd);
    }
    if (strcmp(o.what, "list") == 0 || strcmp(o.what, "all") == 0) {
        rc |= ask_list(fd);
    }
    if (strcmp(o.what, "export") == 0) {
        rc |= ask_export(fd, &o);
    }
    if (strcmp(o.what, "autotest") == 0) {
        rc |= ask_autotest(fd);
    }
    (void)close(fd);
    printf("DEVCTL-PROBE %s what=%s rc=%d\n", (rc == 0) ? "PASS" : "FAIL", o.what, rc);
    return rc;
}
