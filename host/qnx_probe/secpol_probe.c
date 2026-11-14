/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * secpol_probe.c — la sonde d'intrusion de la politique de sécurité noyau.
 *
 * image/ambre.sec déclarait des règles que rien n'avoit jamais compilées, et
 * donc jamais appliquées. Cette sonde est le test d'acceptation : elle est
 * lancée SANS type de sécurité (elle atterrit sur le type du lanceur, celui qui
 * n'a aucun droit EDR) et tente, une par une, les quatre choses qu'un processus
 * non habilité ne doit PAS pouvoir faire. Chaque tentative rends un verdict ;
 * la sonde ne touche JAMAIS au média (aucune écriture sur /dev/hd0t177, même
 * accidentelle : l'ouverture se fait en lecture seule).
 *
 *   a) ouvrir la partition brute du journal /dev/hd0t177
 *        STO-01, SEC-03 : edr-journal est le seul à écrire le média, donc aucun
 *        autre processus ne doit même pouvoir l'ouvrir.
 *   b) name_attach() sur /dev/edr/journal
 *        SEC-01 : usurper le service (le noyau refuse par allow_attach).
 *   c) name_open() + MsgSend() vers /dev/edr/journal
 *        SEC-01 : se connecter au canal du journal (refusé par la matrice
 *        `allow ... edr_journal_t:channel connect`).
 *   d) ouvrir un socket réseau
 *        VIE-03 : boucle fermée. Si la pile réseau n'est pas dans le build,
 *        l'imprimer « absent du build » — jamais le compter comme une réussite.
 *
 * Sortie : une ligne `INTRUSION ...` par tentative, puis
 * `SECPOL-PROBE PASS n/N` ou `SECPOL-PROBE FAIL n/N`.
 * Codes : 0 tout est refusé comme attendu, 1 une action a réussi (faille),
 * 2 erreur d'usage.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700       /* __EXT_UNIX_MISC : dlopen/dlsym dans dlfcn.h */

#include <dlfcn.h>            /* dlopen/dlsym : la sonde ne lie pas libsocket */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/socket.h>
#include <unistd.h>

#define JOURNAL_PATH  "/dev/edr/journal"
#define RAW_PART      "/dev/hd0t177"
#define PROBE_MSG     "INTRUSION"

typedef struct {
    const char *name;     /* ce qu'on tente                                  */
    const char *expect;   /* ce que la politique doit obtenir                */
    int denied;           /* 1 = refusé comme attendu                        */
    int got;              /* 0 = action réussie, sinon errno                 */
    int counted;          /* 0 = non compté (capability absente du build)    */
} attempt_t;

static void verdict(const attempt_t *a)
{
    if (!a->counted) {
        printf("INTRUSION %s : attendu=%s obtenu=absent-from-build verdict=SKIP\n",
               a->name, a->expect);
        return;
    }
    printf("INTRUSION %s : attendu=%s obtenu=%s errno=%d (%s) verdict=%s\n",
           a->name, a->expect, (a->denied ? "refused" : "allowed"),
           a->got, strerror(a->got), (a->denied ? "PASS" : "FAIL"));
}

/* a) la partition brute du journal (STO-01, SEC-03). Lecture seule seulement :
 * une sonde qui écrirait le média détruirait la preuve qu'elle vient de faire. */
static attempt_t try_raw_partition(void)
{
    attempt_t a = {"raw-partition " RAW_PART, "refused", 0, 0, 1};
    int fd = open(RAW_PART, O_RDONLY);

    if (fd < 0) {
        a.denied = 1;
        a.got = errno;
    } else {
        (void)close(fd);   /* ouvert : la lecture n'est pas bloquée, on le dit */
    }
    return a;
}

/* b) usurper le service (SEC-01, par allow_attach). */
static attempt_t try_attach_service(void)
{
    attempt_t a = {"name-attach " JOURNAL_PATH, "refused", 0, 0, 1};
    name_attach_t *attach = name_attach(NULL, JOURNAL_PATH, 0);

    if (attach == NULL) {
        a.denied = 1;
        a.got = errno;
    } else {
        (void)name_detach(attach, 0u);  /* rien volé : le nom est rendu tout de suite */
    }
    return a;
}

/* c) se connecter au canal du journal (SEC-01, matrice de connexion). */
static attempt_t try_connect_channel(void)
{
    attempt_t a = {"channel-connect " JOURNAL_PATH, "refused", 0, 0, 1};
    char reply[16];
    int coid, rc;

    coid = name_open(JOURNAL_PATH, 0);
    if (coid < 0) {
        a.denied = 1;
        a.got = errno;
        return a;
    }
    memset(reply, 0, sizeof(reply));
    rc = MsgSend(coid, PROBE_MSG, (int)strlen(PROBE_MSG), reply, sizeof(reply));
    a.got = (rc < 0) ? errno : 0;
    a.denied = (rc < 0) ? 1 : 0;
    (void)name_close(coid);
    return a;
}

/* d) un socket réseau (VIE-03, boucle fermée).
 *
 * La sonde ne lie PAS libsocket : l'image d'Ambre n'en contient aucune (VIE-03
 * est aussi une absence dans le build). Elle résout donc `socket()` à la
 * volée ; si la bibliothèque n'est pas là, le verdict est « absent du build »,
 * compté à part — une absence n'est pas une victoire de la politique. */
static attempt_t try_network(void)
{
    attempt_t a = {"socket-inet", "refused", 0, 0, 1};
    int (*sock_fn)(int, int, int) = NULL;
    void *handle = dlopen("libsocket.so", RTLD_LAZY);
    int fd;

    if (handle == NULL) {
        handle = dlopen("libsocket.so.4", RTLD_LAZY);
    }
    if (handle == NULL) {
        a.counted = 0;      /* aucune pile réseau dans l'image : SKIP honnête */
        return a;
    }
    /* L'union est le type-punning autorisé par C11 (6.5.2.3) : la conversion
     * directe d'un void* en pointeur de fonction est prohibée par le C strict
     * et le dépôt compile avec -Wpedantic -Werror. */
    union {
        void *obj;
        int (*fn)(int, int, int);
    } cast;

    fd = -1;
    cast.obj = dlsym(handle, "socket");
    sock_fn = cast.fn;
    if (sock_fn == NULL) {
        (void)dlclose(handle);
        a.counted = 0;
        return a;
    }
    fd = sock_fn(AF_INET, SOCK_DGRAM, 0);
    (void)dlclose(handle);
    if (fd < 0) {
        a.denied = 1;
        a.got = errno;
    } else {
        (void)close(fd);
    }
    return a;
}

int main(int argc, char **argv)
{
    attempt_t attempts[4];
    int i, n = 0, passed = 0, failed = 0;

    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        fprintf(stderr, "Usage: %s   (aucun argument : les quatre tentatives)\n", argv[0]);
        return 2;
    }
    attempts[0] = try_raw_partition();
    attempts[1] = try_attach_service();
    attempts[2] = try_connect_channel();
    attempts[3] = try_network();
    for (i = 0; i < 4; i++) {
        verdict(&attempts[i]);
        if (!attempts[i].counted) {
            continue;
        }
        n++;
        if (attempts[i].denied) {
            passed++;
        } else {
            failed++;
        }
    }
    printf("SECPOL-PROBE %s %d/%d\n", (failed == 0) ? "PASS" : "FAIL", passed, n);
    fflush(stdout);
    return (failed == 0) ? 0 : 1;
}
