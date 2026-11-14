# Ambre — banc QNX 8.0 live, et ce qu'il prouve

Le SDP 8.0 est installé (`~/qnx800`, qcc 12.2.0) et l'image **démarre** sous
QEMU : la chaîne complète — conduite, déclenchement UN R160, acquisition sur
média brut, coupure d'alimentation, récupération — tourne dans le guest, et la
vitrine en ligne le montre en continu (https://tradoooo.github.io/ambre/emulator.html).

Cette page remplace celle du 12/11, qui affirmait que « `edr-journal` ne peut
pas ouvrir `/dev/hd0` ». C'est faux depuis le 12/11 au soir, et les lignes qui
suivent sont mesurées, pas déduites.

---

## 1. Ce qui est démontré sur la cible réelle

| Ce que la cible prouve | Preuve (reproductible) |
|---|---|
| les cinq processus se compilent pour `nto x86_64` | `make qnx` (qcc 12.2.0, aucun avertissement, `-Werror`) |
| l'IFS démarre, le block stack publie le média | `ls /dev/hd0t177` dans le guest ; `devb-eide` + `io-blk.so` + `cam-disk.so` |
| le journal ouvre la partition et court la récupération au démarrage (REP-01) | `slog2info` : `edr-journal: recovery done: 8 event(s)`, `self-test: OK (OBS-04)`, `serving /dev/edr/journal (backend qnx, prio 62)` |
| la conduite déclenche et le journal ACQUIERT l'événement sur le média | `edrctl list` : `event 1 sealed locked=yes complete_file=yes reason=restraint` |
| la coupure d'alimentation (SIGKILL sur QEMU) ne perd pas l'événement | le même `event_id`, le même `trig_ts_us`, le même `seq_emerg` après re-amorce |
| **le devctl du journal répond** (QNX-01, QNX-03, OBS-02, EXP-01, EXP-08) | `host/qnx_probe/devctl_probe.c` : STATUS, LIST, EXPORT, AUTOTEST — et les mêmes valeurs que le transport IPC sur le même journal |
| **la matrice d'accès du service est appliquée sur la cible** (SEC-01) | `on -u 0:4 devctl_probe status` rend le statut ; `on -u 0:3` (producteur) reçoit `EACCES` (rc=13) |
| **l'archive écrit sur un SECOND média** (STO-07) | partition FAT32 (`/dev/hd1t12`) montée sur `/arch` ; `edr-archive --once` y pose `event-000001-000001.mcap` |
| **ce MCAP est vérifiable hors du guest** | extrait de l'image brute côté hôte, puis `host/verify/edr_verify.py` : 10 des 11 vérifications passent (la onzième ci-dessous) |
| **la campagne de coupures niveau 2 tourne et se juge** | `host/qnx/campaign.py` → `build/qnx-campaign/campaign.json`, un verdict par cycle, I1..I8 jugés sur ce que le journal rend |

## 2. Le banc, en un commande

Tout ce qui touche la cible vit dans `host/qnx/` — une seule implémentation,
trois consommateurs (la vitrine, la campagne, le banc interactif) :

```sh
source ~/qnx800/qnxsdp-env.sh
python3 host/qnx/bench.py prepare --workdir build/qnx-bench   # binaires, médias, IFS
python3 host/qnx/bench.py repl    --workdir build/qnx-bench    # shell de bench sur la 2e série
python3 host/qnx/campaign.py --cycles 24 --cut-ms 0,5,10,20,50,120
```

`image/vitrine.build` n'est **pas** modifié : le banc lit ce buildfile tel quel
et y **insère** ses blocs (console de bench, montage d'archive, fichiers de
sondes) au niveau d'ancres. La vitrine live se construit donc bit-pour-bit comme
avant — c'est le point : aucun changement de l'image en ligne n'est nécessaire
pour faire avancer le banc.

Console : le noyau occupe le premier port 8250 (0x3f8), le bench le second
(0x2f8 = `/dev/ser2`), où `PS1` est vidé et chaque commande encadrée par
`AMBRE-BEGIN`/`AMBRE-END` — un shell amorcé sans `/etc/passwd` n'a pas de prompt
fiable, et le lecteur ne doit pas en deviner un.

## 3. Ce que le banc a révélé (huit défauts réels, tous corrigés)

Un banc qui sert la cible sert d'abord à ça : les huit points ci-dessous
n'étoient visibles nulle part ailleurs, et six d'entre eux étoient dans le
chemin de la démo live.

1. **`resmgr_attach()` : `flags` et `ftype` inversés.** La signature de la SDP
   est `(dpp, attr, path, flags, ftype, …)` ; le dépôt passait
   `(_FTYPE_ANY, _RESMGR_FLAG_DIR)`, donc des flags invalides au noyau — un
   `SIGABRT` au démarrage du service, **sur la cible seulement** : la doublure
   (`target/qnx-stub`) a la même signature que la SDP, et la vérification
   syntaxique ne pouvait rien voir.
2. **`thread_pool_start()` avec `POOL_FLAG_EXIT_SELF` ne revient pas** (documenté :
   « the function may or may not return, depending on the flags »). Le processus
   devenait le pool et la boucle IPC — celle à qui `edrctl` et `edr-capture`
   parlent — ne tournait jamais : le service s'attachait mais ne répondait plus.
3. **Les codes devctl n'étoient pas encodés** (taille 0, classe hors table QNX) :
   le noyau ne remontait rien au client. Voir `docs/QNX-ABI.md` §3.
4. **La réponse d'un devctl s'écrit dans le payload du message**, pas dans un
   tampon séparé : `_RESMGR_PTR(ctp, &corps, …)` fait lire les 16 premiers
   octets du corps comme l'en-tête `_io_devctl_reply`, et le client reçoit le
   corps **décalé de 16 octets** — 512 dans `open_events`, 1 dans
   `sealed_events`. Des valeurs plausibles, fausses, et donc le genre de défaut
   qu'aucun regard ne trouve : ce sont les octets bruts qui l'ont montré.
5. **`_RESMGR_PTR()` ne copie pas** : il enregistre un pointeur, et le noyau
   répond après le retour du handler. Un tampon local (le corps de STATUS, le
   texte de statut de `read()`) est déjà mort à ce moment-là. Les deux sont
   devenus statiques.
6. **`image/ambre.build` embarquait le mauvais runtime C++.** `edr-archive` lie
   `libc++.so.2` (`readelf -d`), pas `libstdc++.so.6` : le chargeur du guest
   imprimait `ldd:FATAL: Could not load library libc++.so.2` et l'archive ne
   démarroit jamais, même avec un média et un montage corrects.
7. **Le type du module FAT est `dos`, pas `msdos`** (la page SDP de `fs-dos.so`
   donne la syntaxe `driver … dos dos_options …`). `mount -t msdos` cherchoit
   `fs-msdos.so` → « Can't access shared library », avec le module présent.
8. **Un nœud de partition se nomme d'après l'octet de type MBR, en décimal** :
   une partition FAT32 (0x0C) est `/dev/hd1t12`, pas `/dev/hd1t177` (réservé à
   0xB1, le QNX6 du média d'archive produit). Monter `hd1t177` sur une
   partition 0x0C ne donne rien.

Deux défauts d'outillage côté hôte, au passage :

9. **`vm_util.GuestResult` n'étoit pas défini là où `run_capture()` et
   `SerialConsole` le construisent** : tout chemin d'erreur (pas de prompt,
   timeout, socket absent) levait un `NameError` au lieu de rendre un résultat —
   un banc muet se transformoit en plantage, sans rapport. La classe vit
   désormais dans `vm_util`, et `qemu_vm` l'importe.
10. **`SerialConsole` clôtoit sa lecture sur `endswith(prompt)`** : quand la
    commande envoyée se termine elle-même par le marqueur (le cas d'un shell
    sans prompt), l'**écho** de la ligne tapée déclenchoit la fin de lecture —
    le banc rendoit un rc plausible et une sortie vide. Le mode strict
    (`strict_prompt=True`, utilisé par le banc) juge sur la dernière ligne non
    vide ; le défaut reste l'ancien comportement pour les campagnes qui ont un
    vrai prompt.

## 4. Ce que la cible ne prouve PAS encore

Écrit ici pour qu'aucune phrase du dépôt ne puisse le lire comme une réussite.

| Ouvert | Pourquoi, mesuré |
|---|---|
| **la politique de sécurité appliquée dans le noyau** (SEC-01/02/03/06, VIE-03, QNX-06) | `image/ambre.sec` **compile** (`make secpol`, rc=0, aucun avertissement) — la grammaire est vraie. Son **application** ne l'est pas : pousser cette politique dans cette image casse l'amorce, QUELLES QUE SOIENT les règles données au lanceur. Trois essais, mêmes refus (`secpol_resmgr_attach() failed: Permission denied` pour pci-server, devc-con, devc-ser8250, pipe, devb-eide, slogger2) : aucune règle, règles énumérées, puis la forme documentée du blanc-seing (`allow_attach … /...;`) avec un `self:ability` large. La raison est dans la documentation SDP elle-même : « Processes started from the ifs start-up script are not automatically fully controlled by a security policy » — un processus amorcé sans `-T` ne tombe sur le type d'aucune politique poussée. Le remède est connu (amorcer la base avec `slm`, ou `mkqnximage` en mode sécurité qui génère la politique avec `secpolgenerate`, puis typer chaque service) ; il n'est pas fait. Ce qui EST démontré sur la cible, c'est la matrice d'accès du SERVICE (`edr_gate`), par gid : un client du mauvais type reçoit `EACCES`. |
| **les latences sur la cible** (PERF-01..PERF-06) | rien de mesuré sur QNX ; le `backend=posix` reste publié avec ses défauts (PERF-04, PERF-05 non atteints). Le guest rend `worst_freeze_us` par devctl, mais aucune campagne n'a été courue dessus. |
| **le trace noyau** (PERF-07, QNX-02, QNX-04) | `tracelogger` jamais lancé ; l'héritage de priorité à travers le pilote de bloc et `mlockall()` ne sont pas démontrés. |
| **la supervision** (QNX-06) | `supervised=0` sur la cible : `image/ambre-supervise.sh` est un substitut, HAM n'est pas configuré. |
| **la lecture d'un événement par chemin** (`/dev/edr/events/<id>`, EXP-08) | l'OCB est désormais celui de la bibliothèque QNX et ne porte pas d'identifiant ; les blocs bruts se lisent par `DCMD_EDR_EXPORT`, qui est mesuré. |
| **`class_a.fidelity` sur le MCAP de la cible** | 10 des 11 vérifications passent ; celle-ci échoue sur la base de temps : le MCAP ancre T0 sur l'horloge monotonique du guest (mesuré `T0=10555926` µs, soit ~10,5 s après l'amorce) alors que le vérificateur indexe le scénario depuis 0 (`no scenario sample at t_us=8023000`). Ce n'est pas une perte de données : les points sont là, complets, verrouillés, et les dix autres vérifications passent. Une normalisation par décalage d'horloge reste à écrire dans `host/verify/edr_verify.py`. |
| **le niveau 3, Raspberry Pi 5** (§7.7 extrême, 500 coupures électriques) | `BLOCKED(no bench)` : rien ici ne vaut un relais et une vraie ligne 12 V. |
| **le balayage de 2 000 coupures et les 10 000 avec cache volatil** (§7.4) | la campagne niveau 2 tourne (`host/qnx/campaign.py`) avec un rapport cumulatif ; les 2 000 et le cache volatil (serveur de blocs NBD, `host/powercut/faulty_block_server.py`) n'ont pas été courus. |

## 5. Ce que la vitrine en ligne montre

`scripts/qnx-vitrine.sh` (service user `ambre-vitrine-qemu.service`) boucle :
amorce → conduite → acquisition → **SIGKILL sur QEMU** → re-amorce →
récupération, et `image/demo-qnx.sh` publie les chiffres réels du guest toutes
les 2 s (`AMBRE-STATUS`, `AMBRE-EVENT`) ; `ambre-vitrine-web.service` les sert
sur la page. Ces chiffres sont ceux de la machine qui tourne, pas une copie
d'ailleurs — c'est le guest qui les produit en interrogeant son propre journal.

La boucle de la vitrine n'étoit pas auditable : elle ne gardoit rien d'un cycle
à l'autre (`: > "$LOG"` à chaque tour). `host/qnx/campaign.py` fait la même
chose et **écrit un rapport à chaque cycle** — une campagne interrompue laisse
un rapport exploitable, ce qui distingue « la campagne a échoué » de « la
campagne n'a pas tourné ».

## 6. Honnêteté

Le média du journal est brut, sans fichier système (STO-01) ; la coupure est un
`SIGKILL` sur QEMU, `cache=none` sur un disque IDE virtuel. Ce que le guest
n'avoit pas remis au pilote de bloc est perdu, exactement comme le dit l'essai
de niveau 2 — rien ici n'imite la ligne 12 V, et aucun chiffre de cette page
ne doit être lu comme un résultat de matériel réel.
