# Agent brief — projet Ambre (à lire avant d'écrire du code)

## Contexte

« Ambre » est un enregistreur de données d'événement (EDR, « boîte noire »
automobile) pour QNX 8.0, candidat au QNX Global Hackathon (soumission
15/12/2026). L'objectif de la semaine 1 : **bibliothèque de format portable,
reprise après coupure, tests unitaires, exploration exhaustive des états de
coupure sur PC**.

- Dépôt local : `/home/baptiste/Bureau/ambre` (git initialisé, **aucun remote :
  ne jamais pusher, ne jamais créer de dépôt distant**).
- Cahier des charges : `docs/cahier-des-charges.md` (source de vérité fonctionnelle).
- Format disque normatif : `docs/FORMAT.md` (source de vérité binaire).

## Fichiers FIGÉS — lecture obligatoire, modification interdite

```
fmt/ambre_types.h          structures on-disk + constantes
fmt/ambre_fmt.h            API codec blocs/table/scan/reprise + vtable disque
fmt/ambre_trigger.h        contrat arithmétique du déclenchement (A1..A9)
fmt/ambre_journal.h        moteur de journal (chemin d'écriture, marques)
host/simdisk/ambre_simdisk.h  disque simulé + journal des opérations
host/explore/ambre_explore.h  explorateur + invariants I1..I8
tests/ambre_test.h         harnais de test
tests/ambre_test_util.c
Makefile
docs/FORMAT.md
```

Si un header te semble faux ou bloquant : **ne le modifie pas**, termine ce qui
est possible et signale le problème précisément dans ta réponse finale (fichier,
ligne, pourquoi, correction proposée).

## Règles dues au user (non négociables)

1. **Fichiers < 500 lignes, fonctions < 50 lignes.** Vérifie avec `wc -l` avant
   de finir ; découpe en modules/helpers si besoin.
2. **Zéro duplication** : un seul endroit par logique, helpers partagés.
3. **C11 portable** : les mêmes sources doivent compiler sur PC (gcc) et sur
   QNX 8.0. Pas de dépendance système hors libc (pas d'API Linux spécifiques
   dans `fmt/`). Pas d'allocation dynamique sur les chemins critiques
   (GEL-03) ; les structures sont à taille fixe.
4. **Aucun warning** : `-std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpedantic -Werror`.
5. **N'écris que les fichiers qui te sont attribués** — d'autres agents
   travaillent en parallèle dans le même arbre.
6. **Build isolé** : utilise toujours ton propre répertoire de build, par
   exemple `make BUILD=build-a1 …`. Jamais `build/` (partagé).
7. Pas de réseau, pas de commande destructrice (`trash` plutôt que `rm`), pas de
   `git commit` / `git push` (l'agent pilote s'en occupe), **pas de délégation** :
   fais le travail toi-même.
8. **Bornes** : après 3 essais infructueux sur la même erreur, arrête-toi et
   réponds `KO` avec l'erreur exacte et ce que tu as déjà produit.
9. Commentaires et identifiants **en anglais** (le dépôt est en anglais),
   messages de test en anglais.

## Compilation / test

Le Makefile découvre les sources par wildcard, donc il peut embarquer le fichier
d'un autre agent en cours d'écriture. En cas d'échec de link non lié à ton code,
compile directement, par exemple :

```sh
cd /home/baptiste/Bureau/ambre
mkdir -p build-a1
gcc -std=c11 -O2 -g -Wall -Wextra -Wshadow -Wpedantic -Werror -Ifmt -I. -Itests \
    tests/test_block.c tests/ambre_test_util.c \
    fmt/ambre_crc32c.c fmt/ambre_fmt_block.c fmt/ambre_fmt_table.c fmt/ambre_fmt_scan.c \
    -o build-a1/test_block
./build-a1/test_block
```

Le code de retour d'un binaire de test = nombre d'échecs (0 = vert).

Si tu dois attendre le fichier d'un autre agent, utilise une attente bornée :

```sh
timeout 300 bash -c 'until [ -f fmt/ambre_fmt_block.c ]; do sleep 10; done'
```

(jamais de `sleep` fixe > 30 s).

## Interfaces CLI figées (à respecter exactement)

```
build/ambre-explore --scenario <file.txt> [--seed S] [--duration-ms N]
                    [--crash-at-ms N] [--segment-limit N] [--states N]
                    [--jobs N] [--variants all|prefix,torn,cache,bitflip,table]
                    [--nested] [--report out.json] [--violation-dir DIR]
                    [--export-image out.img]
    → stdout: résumé humain ; code de retour 0 si aucune violation d'invariant.

build/ambre-dump-truth --vectors <in.vec>          → stdout: décisions (.dec)
python3 host/scenarios/generate.py --seed S --duration-ms N --crash-at-ms N
                                   --out <file.txt> [--dv-kmh X] [--rate-hz 1000]
python3 host/verify/gen_vectors.py --scenarios N --seed S --out <file.vec>
python3 host/verify/trigger_crosscheck.py --vectors <in.vec> --c-output <c.dec>
python3 host/mcap/export_mcap.py --image <journal.img> --out <event.mcap> [--event-id N]
python3 host/verify/edr_verify.py --mcap <event.mcap> --scenario <scenario.txt>
```

## Format des vecteurs de déclenchement (`.vec` / `.dec`)

Texte brut, `#` = commentaire, un scénario par bloc. Ce format évite un
analyseur JSON en C ; il est utilisé par le jumeau Python (DEC-12).

```
S <id> <mode> <dv_trigger_mm_s> <dv_lock_mm_s> <dv_t0_mm_s> <win_trigger_ms> \
  <win_t0_long_ms> <win_t0_lat_ms> <has_front_restraint> <same_crash_window_ms>
A <t_us> <a_long_mm_s2> <a_lat_mm_s2> <ext_flags>
A ...
E
```

`ext_flags` : bit0 retenue déployée, bit1 VRU, bit2 freinage d'urgence,
bit3 AEB, bit4 commande de service, bit5 échantillon invalide.

Décisions, une ligne par échantillon, dans l'ordre :

```
D <id> <i> <fired> <end> <locked> <reason> <t0_us> <event_number> \
  <time_since_first_ms> <dv_long> <dv_lat> <dv_since_t0_long> <dv_since_t0_lat> \
  <max_dv_long> <max_dv_lat> <max_dv_long_at_ms> <max_dv_lat_at_ms> \
  <win_long> <win_lat>
```

Booléens = `0`/`1`. Tous les entiers en base 10, séparés par **un seul espace**,
aucun espace final, `\n` unix. Les deux implémentations doivent produire des
fichiers `.dec` **octet pour octet identiques** (comparaison par `diff`).

## Répartition des fichiers par agent

| Agent | Fichiers |
|---|---|
| codec | `fmt/ambre_crc32c.c`, `fmt/ambre_fmt_block.c`, `fmt/ambre_fmt_table.c`, `fmt/ambre_fmt_scan.c`, `tests/test_crc32c.c`, `tests/test_block.c`, `tests/test_table.c`, `tests/test_scan.c` |
| trigger-c | `fmt/ambre_trigger.c`, `tests/test_trigger.c`, `tests/dump_truth.c` |
| trigger-py | `host/verify/ambre_trigger.py`, `host/verify/gen_vectors.py`, `host/verify/trigger_crosscheck.py`, `tests/test_trigger_py.py` |
| simdisk | `host/simdisk/ambre_simdisk.c`, `tests/test_simdisk.c` |
| recovery | `fmt/ambre_fmt_recovery.c`, `tests/test_recovery.c` |
| journal | `fmt/ambre_journal.c`, `tests/test_journal.c` |
| explorer | `host/explore/ambre_explore.c`, `host/explore/ambre_invariants.c`, `host/explore/main.c`, `tests/test_explore.c` |
| mcap | `host/mcap/export_mcap.py`, `host/verify/edr_verify.py`, `foxglove/ambre-layout.json`, `tests/test_mcap_py.py` |
| docs-ci | `README.md`, `LICENSE`, `NOTICE`, `.github/workflows/*.yml`, `host/scenarios/generate.py`, `host/scenarios/*.txt`, `docs/ARCHITECTURE.md`, `docs/VALIDATION.md`, `codelab/README.md`, `scripts/demo.sh` |
