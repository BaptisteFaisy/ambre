# Boîte noire automobile sur QNX 8.0 — cahier des charges

**Nom de travail : « Ambre »** (l'instant de l'accident, figé et conservé intact).
Version 0.1 — 10 novembre 2026 — projet candidat au QNX Global Hackathon, catégorie Automobile & Mobilité.
Date limite de soumission : mardi 15 décembre 2026 à 20 h, heure de Paris.

---

## 0. Lecture rapide

**Ce que c'est.** Un enregistreur de données d'événement (EDR, la « boîte noire » d'une voiture) qui tourne sur QNX 8.0. Il capture en continu les signaux du véhicule et, quand un choc est détecté, il rend l'enregistrement durable sur disque dans un délai borné et mesuré, même si l'alimentation tombe quelques dizaines de millisecondes plus tard.

**La promesse, en une phrase.** Passé un délai de gel annoncé (cible : 20 ms après le déclenchement), les données réglementaires d'avant le choc survivent à n'importe quelle coupure, et cette affirmation est démontrée par des essais, pas seulement affirmée.

**Trois idées qui portent le projet.**

1. **Sauver d'abord l'essentiel.** Le jeu de données minimal exigé par la réglementation (5 s avant le choc à 2 points par seconde) pèse environ 1 Ko. Il part sur le disque en une seule écriture, avant tout le reste.
2. **Un journal continu sur partition brute.** Les données haute résolution sont écrites au fil de l'eau dans un journal circulaire, hors de tout système de fichiers. Au déclenchement, il ne reste qu'à « épingler » ce qui est déjà sur le disque : deux écritures et un vidage de cache.
3. **La preuve par l'exploration.** Le code de reprise après coupure est testé contre tous les instants de coupure possibles d'un scénario (des centaines de milliers d'états de disque), puis contre des milliers de coupures réelles de la machine virtuelle.

**Trois niveaux d'ambition.** Chaque exigence porte un niveau : **S** (socle, indispensable à une soumission), **C** (complet, la version visée), **X** (extrême, ce qui pousse l'idée au bout).

**À valider avant d'écrire la moindre ligne de produit** (section 10, semaine 1) : quatre essais de faisabilité, dont le plus important vérifie qu'un ordre de vidage émis depuis QNX atteint bien le disque virtuel.

**Statut des affirmations.** La section 14 sépare ce qui a été vérifié sur les textes et la documentation de ce qui reste une hypothèse de conception.

---

## 1. Contexte

### 1.1 Qu'est-ce qu'un enregistreur de données d'événement

Un EDR ne filme rien et n'enregistre pas en permanence sur un support durable. Il garde en mémoire vive les dernières secondes de quelques dizaines de signaux (vitesse, frein, accélérateur, ceintures, accélérations). Quand un choc franchit un seuil, il fige cette fenêtre et la copie en mémoire non volatile, avec ce qui se passe pendant le choc. Les données servent ensuite à la reconstitution d'accidents.

La difficulté technique est précisément celle que ce projet attaque : l'instant où il faut écrire est celui où la voiture se déforme et où l'alimentation peut disparaître.

### 1.2 Cadre réglementaire

| Texte | Ce qu'il faut en retenir |
|---|---|
| **Règlement (UE) 2019/2144**, article 6 §4-5 et annexe II | EDR obligatoire. Voitures et utilitaires légers (M1, N1) : nouveaux types depuis le 6 juillet 2022, tous les véhicules neufs depuis le 7 juillet 2024. Bus et poids lourds (M2, M3, N2, N3) : 7 janvier 2026 et 7 janvier 2029. L'EDR ne peut pas être désactivé, fonctionne en boucle fermée, ses données sont anonymisées et protégées contre la manipulation, et ne sont remises aux autorités que pour la recherche et l'analyse des accidents, par une interface normalisée. Il ne doit pas pouvoir enregistrer les quatre derniers caractères du numéro de série du véhicule (VIN), ni rien qui identifie le véhicule, son propriétaire ou son détenteur. |
| **Règlement délégué (UE) 2022/545** | Prescriptions techniques pour M1 et N1, applicable depuis le 6 juillet 2022. Il s'appuie sur le règlement ONU n° 160 et y ajoute des exigences de lecture des données, de vie privée et de sécurité. |
| **Règlement ONU n° 160** | Liste des éléments de données, règles de déclenchement et de verrouillage, capacité, tenue aux essais de choc. Détail ci-dessous. |
| **49 CFR Part 563** (États-Unis) | Règle du 18 décembre 2024 : la fenêtre avant choc passe de 5 s à 2 Hz à **20 s à 10 Hz**. Le calendrier a été repoussé en mai 2026 : montée en charge sur quatre ans à partir du 1er septembre 2028. |

**Règlement ONU n° 160, points utiles à la conception** (numéros de paragraphes relevés sur la version publiée au Journal officiel de l'UE, à relire avant de les citer dans la soumission) :

- **§1.3 — exclus du périmètre** : VIN, détails associés du véhicule, localisation, informations sur le conducteur, date et heure de l'événement.
- **§2.3 et §2.28** : « capturer » désigne la mise en tampon en mémoire volatile, mise à jour en continu ; « enregistrer » désigne la sauvegarde en mémoire non volatile.
- **§5.3** : la mémoire non volatile doit contenir les données d'**au moins trois événements**.
- **§5.3.1 — déclenchement** : variation de vitesse longitudinale de plus de 8 km/h en 150 ms ou moins ; même seuil en latéral ; activation d'un système de retenue non réversible (airbag) ; activation d'un système de protection des usagers vulnérables, si le véhicule en a un.
- **§5.3.2 — verrouillage** (l'événement ne peut plus être écrasé) : déploiement d'une retenue non réversible ; en choc frontal sans retenue frontale non réversible, variation de vitesse de plus de 25 km/h en 150 ms ou moins ; activation de la protection des usagers vulnérables.
- **§5.3.3 — instant zéro (T0)** : pour un algorithme qui tourne en continu, premier instant où la variation de vitesse cumulée dépasse 0,8 km/h en 20 ms (5 ms en latéral).
- **§2.7 — fin d'événement** : la variation de vitesse cumulée sur 20 ms retombe à 0,8 km/h ou moins.
- **§5.3.4 — écrasement** : sans emplacement libre, les événements non verrouillés sont écrasés du plus ancien au plus récent.
- **§5.3.5** : les données en mémoire non volatile sont conservées après une perte d'alimentation.
- **§5.4 — essais de choc** : après l'essai, les éléments requis existent et l'élément « fichier complet enregistré » vaut « oui ».
- **§5.5** : l'EDR ne peut pas être désactivé.

### 1.3 Pourquoi un système temps réel à micronoyau

- **Le délai de gel est une échéance dure.** Entre le choc et la perte d'alimentation, la marge se compte en dizaines de millisecondes. Un système généraliste ne borne ni le délai de réveil d'une tâche ni le temps passé dans le pilote de disque.
- **Les pilotes sont des processus ordinaires.** Sur QNX, quand une tâche prioritaire envoie une écriture au pilote de disque, le pilote traite la requête à la priorité de l'appelant (héritage de priorité par passage de messages). Le chemin critique reste prioritaire de bout en bout, ce qui se montre sur une trace noyau.
- **L'isolation limite les dégâts.** L'acquisition, le déclenchement, le journal et l'archivage sont des processus séparés. Le plantage de l'un ne détruit pas les données des autres, et le noyau signale la mort d'un processus à ses correspondants.
- **Le système de fichiers Power-Safe** de QNX garantit la cohérence de l'archive après coupure, sans vérificateur au démarrage.

### 1.4 Cadre du hackathon

- **Catégorie visée** : Automobile & Mobilité. Dans les dépôts publics visibles au 10 novembre 2026, cette catégorie compte des tableaux de bord, un régulateur de vitesse, un garde-fou de signaux, mais aucun EDR.
- **Critères du jury**, à poids égal : réalisation technique, produit complet et cohérent, impact potentiel (problème réel dans un marché QNX, et pourquoi le temps réel), innovation.
- **À fournir** : dépôt public neuf sous licence libre, description, instructions de construction précisant la version de QNX, la cible et la chaîne de compilation, vidéo de moins de trois minutes sur YouTube, le tout en anglais.
- **Prix accessibles** : catégorie (4 000 $ et 2 000 $), grand prix (10 000 $), et deux bonus cumulables de 3 500 $ : meilleur usage des technologies propres à QNX, meilleur tutoriel.

| Critère du jury | Réponse du projet |
|---|---|
| Réalisation technique | Journal brut à écriture unique, reprise idempotente, exploration exhaustive des états de coupure, mesures de latence sur trace noyau |
| Produit complet | De la capture à la lecture dans un visualiseur standard, avec outil de service, rapport et tutoriel |
| Impact | Équipement obligatoire sur tous les véhicules neufs de l'UE ; durcissement en cours aux États-Unis |
| Innovation | Priorité aux données réglementaires dans le premier kilo-octet ; preuve par exploration plutôt que par quelques essais |

---

## 2. Positionnement par rapport à l'existant

J'avais d'abord classé cette idée comme « déjà couverte » par un dépôt concurrent. Lecture faite, le recouvrement est partiel.

| Projet (GitHub, public) | Ce qu'il fait | Ce qu'il ne fait pas |
|---|---|---|
| `vinit1009/last-second` (hackathon, 9 nov. 2026) | Enregistreur générique pour boucles temps réel de robots et d'appareils médicaux. Garde 30 s à 1 kHz en mémoire, fige sur échéance manquée ou mort du processus, sauve en CSV par un fil de basse priorité, politique de sécurité, deux redémarrages avec QEMU tué. | Pas de domaine automobile ni de règles de déclenchement sur la variation de vitesse. Pas d'échéance sur l'écriture disque (elle est volontairement différée). Pas de campagne de coupures. |
| `ayswarya-nachinarkinian/Real-Time-Automotive-Event-Recorder` (sept. 2026) | Projet étudiant sur Raspberry Pi : capteurs réels, détection de choc, de retournement et d'incendie par seuils. | Antérieur au hackathon. Pas d'alignement réglementaire, pas de garantie de durabilité. |
| `santhoshbala23/real-time-automotive-event-recorder` (août 2026) | Tampon circulaire partagé et files de messages. | Dépôt sans documentation. |
| `princeraj2572/QNX-Black-Box` (hackathon) | Outil côté PC qui explique pourquoi une échéance a été manquée à partir d'une trace. | Rien à voir avec un EDR malgré le nom. |

**Ce qui doit distinguer Ambre, sans ambiguïté :**

1. L'alignement sur le règlement ONU n° 160 : éléments de données, seuils, T0, verrouillage, capacité, « fichier complet ».
2. Une échéance sur la **durabilité**, pas seulement sur la détection.
3. Une campagne de coupures d'une rigueur inhabituelle, avec exploration exhaustive.
4. Un format de sortie standard (MCAP) lisible dans les visualiseurs courants.
5. La vie privée traitée comme une exigence vérifiable.

**Règle de prudence.** Le projet se présente comme un démonstrateur « aligné sur » le règlement n° 160. Il ne se dit jamais conforme ni homologable.

---

## 3. Périmètre

### 3.1 Objectifs

- Capturer, figer, enregistrer, reprendre après coupure et restituer des événements de choc.
- Tenir une échéance de gel mesurée, sous charge.
- Démontrer la tenue aux coupures par des essais automatisés et reproductibles par le jury.
- Fonctionner entièrement sous QEMU, sans matériel.

### 3.2 Hors périmètre

- Détection de choc de qualité série : l'algorithme de déclenchement applique les seuils du règlement, il ne remplace pas un calculateur d'airbag.
- Certification de sûreté de fonctionnement (ISO 26262) et homologation.
- Tenue mécanique et thermique du boîtier.
- Vidéo, audio, position GPS : exclus par conception (section 6.3).

### 3.3 Hypothèses

- QNX SDP 8.0 obtenu par QNX Everywhere, image construite avec `mkqnximage`, cible QEMU x86_64. Cible secondaire facultative : Raspberry Pi 5.
- Le disque virtuel honore les ordres de vidage de cache. Le banc d'essai le vérifie au lieu de le supposer.
- Les signaux du véhicule arrivent déjà décodés. Le décodage CAN est une extension.

---

## 4. Conception proposée

Cette section décrit une solution. Elle n'a pas valeur d'exigence : seules les sections 5 à 7 engagent.

### 4.1 Deux classes de données

| | Classe A — réglementaire | Classe B — haute résolution |
|---|---|---|
| Contenu | Éléments de l'annexe 4 du règlement n° 160 | Accélérations à 1 kHz, signaux dynamiques à 100 Hz, états à 10 Hz |
| Fenêtre | 5 s avant T0 à 2 points/s, puis le choc | 20 s avant T0, 5 s après |
| Volume par événement | Environ 1 Ko avant choc | Environ 1,1 Mo |
| Priorité d'écriture | Première, en une seule écriture | Déjà sur disque grâce au journal continu |

Le profil américain de 2024 (20 s à 10 Hz) reste petit : environ 16 Ko pour 20 signaux, soit quatre blocs de 4 Ko.

### 4.2 Deux étages de stockage

**Étage 1 — le journal brut.** Une partition dédiée, sans système de fichiers, découpée en segments de 2 Mo (environ 45 s de données). Le processus journal y écrit des blocs de la taille d'un secteur, à la suite, chacun avec un numéro de séquence et un CRC. Un bloc écrit n'est jamais réécrit avant la libération de son segment. Une table d'allocation en deux copies alternées indique l'état de chaque segment : libre, courant, figé, verrouillé, archivé.

**Étage 2 — l'archive MCAP.** Une seconde partition, en système de fichiers Power-Safe, reçoit un fichier MCAP par événement. Un processus de basse priorité le fabrique après coup à partir du journal.

**Pourquoi ne pas écrire directement dans le système de fichiers.** La documentation QNX 8.0 est explicite : la synchronisation du système Power-Safe se fait au niveau du système de fichiers entier et non du fichier, `fsync()` est « potentiellement coûteux », et l'option `O_SYNC` est ignorée. Chaque modification de données peut entraîner la copie d'une douzaine de blocs. C'est excellent pour la cohérence, mauvais pour une échéance de 20 ms.

### 4.3 Chronologie d'un événement

```
        avant T0            T0    déclenchement              fin + 30 ms
  ─────────────────────────┬──────────┬───────────────────────────┬──────────────
  journal continu          │          │ t = 0                     │
  (blocs + vidage/100 ms)  │          │  1. enregistrement        │ 4. scellement
                           │          │     d'urgence (classe A)  │    « complet = oui »
                           │          │  2. table d'allocation    │
                           │          │  3. vidage  ──► ACQUIS    │ 5. plus tard, au calme :
                           │          │     (cible ≤ 20 ms)       │    archive MCAP
                           │          │  puis toutes les 10 ms :  │
                           │          │  suite + vidage           │
```

| Coupure d'alimentation… | Ce qui survit |
|---|---|
| avant le déclenchement | Rien à garder : il n'y a pas d'événement |
| entre le déclenchement et « acquis » | Non garanti. C'est la fenêtre de risque assumée, égale au délai de gel |
| après « acquis », avant le scellement | Tout l'avant-choc, et le choc jusqu'au dernier vidage (10 ms de perte au plus). L'événement est restitué avec « fichier complet = non » |
| après le scellement | Tout, avec « fichier complet = oui » |

### 4.4 Architecture logicielle

```
  producteurs                 QNX 8.0 (un processus par boîte)
 ┌────────────┐  trames 10 ms  ┌──────────────┐ déclenchement┌───────────────┐
 │ edr-replay │───────────────►│ edr-capture  │─────────────►│  edr-journal  │
 │ (ou CAN,   │  /dev/edr/in   │  + détection │ + instantané │  gestionnaire │
 │  zenoh)    │                │  (prio 60-61)│  trames      │  de ressources│
 └────────────┘                └──────────────┘─────────────►│  (prio 62)    │
                                                             └──────┬────────┘
 ┌────────────┐  /dev/edr/ctl, status                               │ écritures brutes
 │  edrctl    │◄───────────────────────────────────────────────┐    ▼
 │ (service)  │                                                │ ┌─────────────────┐
 └────────────┘      ┌──────────────┐  lit les segments figés  │ │ partition brute │
                     │ edr-archive  │◄─────────────────────────┴─│ (journal)       │
                     │  (prio 8)    │──► fichiers MCAP ──►       └─────────────────┘
                     └──────────────┘    partition Power-Safe
```

| Processus | Rôle | Priorité | Peut accéder à |
|---|---|---|---|
| `edr-capture` | Reçoit les trames, horodate, calcule le déclenchement, tient à jour les tampons de classe A. Au déclenchement, envoie au journal un message qui contient l'instantané de classe A | 60 à 61 | Le canal du journal |
| `edr-journal` | Seul propriétaire de la partition brute ; écrit, épingle, scelle, reprend au démarrage | 62 | La partition brute |
| `edr-archive` | Convertit les événements scellés en MCAP ; gère la rétention | 8 | L'archive, le journal en lecture |
| `edrctl` | Outil de service : état, liste, export, vérification | 10 | Les interfaces de service |
| `edr-replay` | Producteur de démonstration : rejoue un scénario | 50 | L'entrée seulement |

### 4.5 Format du journal

- **Bloc** (512 ou 4 096 octets selon le secteur du support) : nombre magique, version de format, numéro de séquence sur 64 bits, identifiant de session, type, instant du premier échantillon, longueur utile, CRC-32C du bloc entier.
- **Types d'enregistrement** : `DONNEES` (échantillons de classe B, 8 octets chacun), `URGENCE` (instantané de classe A avant choc), `SUITE` (classe A pendant le choc), `SCELLE` (résumé de fin), `TROU` (interruption de capture).
- **Table d'allocation** : deux copies à des emplacements fixes, compteur de génération, CRC. Chaque mise à jour écrit la copie la plus ancienne. Elle contient l'état des segments et un descripteur par événement.
- **Auto-description** : un enregistrement `URGENCE` suffit à prouver qu'un événement a eu lieu. Si les deux copies de la table sont illisibles, la reprise la reconstruit en balayant la partition.

**Dimensionnement indicatif.** Environ 5 500 échantillons par seconde, marge comprise, à 8 octets chacun font 44 Ko/s, soit 1,1 Mo pour une fenêtre de 25 s. Avec 24 segments de 2 Mo (48 Mo), le journal tient six événements figés dans le pire cas, plus la capture en cours. À une heure de conduite par jour, il s'écrit environ 58 Go par an, soit sept réécritures complètes d'un support de 8 Go : négligeable sur un support qui répartit l'usure.

### 4.6 Reprise après coupure

1. Lire les deux copies de la table ; garder la copie valide de génération la plus haute.
2. Dans le segment courant, trouver le dernier bloc valide (séquence croissante, CRC correct).
3. Chercher un enregistrement `URGENCE` que la table ne connaît pas encore, et l'y inscrire.
4. Sceller tout événement resté ouvert avec « fichier complet = non » et la cause.
5. Relancer les archives manquantes.

La reprise n'écrit que des ajouts et des mises à jour de table alternées. Elle peut donc être interrompue à son tour et rejouée sans dommage.

### 4.7 Archive MCAP

Écriture sous un nom temporaire, synchronisation, renommage, synchronisation du répertoire. Le segment du journal n'est libéré qu'après relecture réussie du fichier. Le détail des canaux est en section 6.4.

---

## 5. Exigences

Niveau : **S** socle, **C** complet, **X** extrême. Vérification : **T** test automatique, **M** mesure, **I** inspection du code, **D** démonstration.

### 5.1 Acquisition

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| ACQ-01 | Le système accepte des trames d'échantillons de plusieurs producteurs par `/dev/edr/in`. Une trame couvre au plus 10 ms et pèse au plus 4 Ko. | S | T |
| ACQ-02 | Chaque trame est horodatée à la réception sur l'horloge monotone, avec une résolution de 100 µs ou mieux. Aucun horodatage absolu n'est produit. | S | T, I |
| ACQ-03 | Cadences prises en charge : 1 kHz (accélérations), 100 Hz (signaux dynamiques), 10 Hz (états). La liste des signaux est fixée à la construction. | S | T |
| ACQ-04 | Un signal absent de la liste blanche est rejeté et compté. | S | T |
| ACQ-05 | Un producteur de rejeu lit un fichier de scénario et l'injecte au rythme réel ou en accéléré. | S | D |
| ACQ-06 | Une interruption de capture de plus de trois périodes est marquée dans le flux par un enregistrement `TROU`. | S | T |
| ACQ-07 | Chaque échantillon porte un indicateur de qualité (valide, figé, hors plage, capteur en défaut), conservé jusqu'à l'export. | C | T |
| ACQ-08 | Un producteur lit un bus CAN virtuel relié au PC et décode les trames à partir d'un fichier DBC. | C | D |
| ACQ-09 | Un producteur s'abonne à des signaux publiés par d'autres calculateurs via zenoh. | X | D |

### 5.2 Déclenchement

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| DEC-01 | Déclenchement quand la variation de vitesse longitudinale dépasse 8 km/h sur un intervalle de 150 ms ou moins. | S | T |
| DEC-02 | Même règle sur l'axe latéral. | S | T |
| DEC-03 | Déclenchement sur signal externe « retenue non réversible déployée ». L'événement est verrouillé. | S | T |
| DEC-04 | Verrouillage quand la variation de vitesse longitudinale dépasse 25 km/h en 150 ms ou moins, si la configuration déclare l'absence de retenue frontale non réversible. | S | T |
| DEC-05 | T0 est le premier instant où la variation de vitesse cumulée dépasse 0,8 km/h en 20 ms (5 ms en latéral). Pour un signal externe, T0 est l'instant du signal. | S | T |
| DEC-06 | La fin d'événement est l'instant où la variation de vitesse cumulée sur 20 ms retombe à 0,8 km/h ou moins. | S | T |
| DEC-07 | Le calcul s'exécute en temps borné et en mémoire fixe : sommes cumulées, minimum et maximum glissants, arithmétique entière. | S | I, M |
| DEC-08 | Les cas limites sont couverts : 7,9 km/h en 150 ms ne déclenche pas, 8,1 km/h en 150 ms déclenche, 8,1 km/h atteints en 151 ms ne déclenchent pas. | S | T |
| DEC-09 | Deux événements dont les T0 sont séparés de moins de 5 s (paramètre) appartiennent au même accident : numéro d'événement et temps écoulé depuis le premier sont enregistrés. | C | T |
| DEC-10 | Déclenchement sur signal « protection des usagers vulnérables activée ». L'événement est verrouillé. | C | T |
| DEC-11 | Déclencheurs hors profil réglementaire (freinage d'urgence, intervention du freinage automatique, commande de service), marqués comme tels et jamais verrouillés. | C | T |
| DEC-12 | Une seconde implémentation, indépendante et écrite en Python, donne les mêmes décisions sur au moins 10 000 scénarios aléatoires. | C | T |
| DEC-13 | Un échantillon marqué invalide est exclu du calcul de variation de vitesse. | X | T |

### 5.3 Gel et enregistrement

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| GEL-01 | Au déclenchement, l'enregistrement d'urgence (classe A avant choc) est écrit en une seule écriture contiguë. | S | T, I |
| GEL-02 | L'écriture d'urgence est suivie de la mise à jour de la table d'allocation, puis d'un vidage du cache du support. L'événement est dit **acquis** à l'acquittement de ce vidage. | S | T |
| GEL-03 | Le chemin du déclenchement à « acquis » ne fait ni allocation mémoire, ni défaut de page, ni attente sur un processus moins prioritaire. | S | I, M |
| GEL-04 | Pendant l'événement, un enregistrement de suite est écrit et vidé toutes les 10 ms. | S | T |
| GEL-05 | À la fin de l'événement, un enregistrement de scellement porte la variation de vitesse maximale, son instant, les temps de déploiement et « fichier complet = oui ». | S | T |
| GEL-06 | Aucune écriture d'archive n'a lieu tant qu'un événement est ouvert. | S | T |
| GEL-07 | En mode journal continu, les données de classe B d'avant le déclenchement sont déjà écrites. La perte possible avant « acquis » ne dépasse pas la période de vidage plus un bloc. | C | T |
| GEL-08 | Un mode « sur événement » garde la capture en mémoire vive seule, comme la définition réglementaire de « capturer », et n'écrit qu'au déclenchement. Le choix du mode est fixé à la construction. | C | T |
| GEL-09 | Sur chute de la tension d'alimentation simulée, l'événement est scellé immédiatement avec « fichier complet = non, cause = tension ». | X | D |

### 5.4 Stockage et capacité

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| STO-01 | Le chemin critique écrit sur une partition brute dédiée, sans passer par un système de fichiers. | S | I |
| STO-02 | Chaque bloc porte un nombre magique, une version, un numéro de séquence sur 64 bits, un identifiant de session, un type, une longueur et un CRC-32C. | S | T |
| STO-03 | Un bloc acquitté n'est pas réécrit avant la libération de son segment. | S | T, I |
| STO-04 | La table d'allocation existe en deux copies avec compteur de génération et CRC. Une mise à jour ne touche qu'une copie. | S | T |
| STO-05 | Le journal contient au moins trois événements figés en même temps. Cible : six. | S | T |
| STO-06 | Faute de place, les événements non verrouillés sont écrasés du plus ancien au plus récent. Un événement verrouillé n'est jamais écrasé. | S | T |
| STO-07 | L'archive conserve au moins 16 événements sur une partition Power-Safe réservée à l'enregistreur. | C | T |
| STO-08 | Si tous les emplacements sont verrouillés, un nouvel événement est refusé et l'état « mémoire pleine » est signalé et journalisé. | C | T |
| STO-09 | Le volume écrit et le nombre de vidages par heure sont mesurés, documentés et exposés par un compteur. | C | M |
| STO-10 | Le journal peut être dupliqué sur deux supports. La perte d'un support ne perd aucun événement acquis. | X | T |

### 5.5 Reprise après coupure

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| REP-01 | Au démarrage, la reprise s'exécute avant toute nouvelle capture. | S | T |
| REP-02 | La reprise retient la copie de table valide la plus récente. Si aucune n'est valide, elle reconstruit la table par balayage. | S | T |
| REP-03 | Tout événement resté ouvert est scellé avec « fichier complet = non » et la cause. | S | T |
| REP-04 | La reprise donne le même résultat si elle est exécutée deux fois, ou interrompue par une nouvelle coupure puis relancée. | S | T |
| REP-05 | La reprise ne fait apparaître aucun événement qui n'a pas été déclenché, et ne restitue aucune donnée au CRC invalide. | S | T |
| REP-06 | La reprise d'un journal de 48 Mo dure moins de 500 ms. | C | M |
| REP-07 | Une archive interrompue est refaite depuis le journal. Le segment n'est libéré qu'après relecture réussie de l'archive. | C | T |

### 5.6 Restitution

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| EXP-01 | `edrctl status` et `edrctl list` affichent l'état, les événements, leur verrouillage et leur complétude. | S | D |
| EXP-02 | Chaque événement s'exporte en un fichier MCAP unique et valide : résumé, index, CRC des blocs. | S | T |
| EXP-03 | Le fichier s'ouvre dans Foxglove sans extension. Une disposition d'écran est fournie. | S | D |
| EXP-04 | Le fichier porte en métadonnées le résumé de l'événement, le type, la variante et la version du véhicule, le profil, la version logicielle et l'empreinte de la configuration. | C | T |
| EXP-05 | Un rapport HTML reprend le tableau des éléments de données avec leurs plages et résolutions. | C | D |
| EXP-06 | Chaque lecture incrémente le compteur « cycle d'allumage au téléchargement » et s'inscrit dans un journal des lectures. | C | T |
| EXP-07 | Un outil sur PC, `edr-verify`, contrôle l'intégrité d'un export et le compare aux données injectées. | C | T |
| EXP-08 | Les événements apparaissent comme des fichiers sous `/dev/edr/events/`, servis par le gestionnaire de ressources. | X | D |

### 5.7 Temps réel

Les valeurs sont des cibles initiales, à ajuster après les essais de faisabilité. Elles s'entendent comme **pire cas mesuré**, pas comme pire cas prouvé.

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| PERF-01 | Du dernier échantillon qui franchit le seuil à l'impulsion de déclenchement : 2 ms au plus. | S | M |
| PERF-02 | Du déclenchement à « acquis » : 20 ms au plus sur le matériel de référence. | S | M |
| PERF-03 | La part logicielle de ce délai, hors temps du support, ne dépasse pas 500 µs. | S | M |
| PERF-04 | Aucun dépassement sur 1 000 déclenchements (S), 10 000 sous charge processeur et disque (C). | S, C | M |
| PERF-05 | L'envoi d'une trame coûte au producteur moins de 50 µs au 99e centile. | S | M |
| PERF-06 | Le système capture moins d'une seconde après le lancement du noyau, reprise comprise. | C | M |
| PERF-07 | Les trois pires cas de PERF-02 sont expliqués par une trace noyau qui montre où le temps est passé. | C | D |
| PERF-08 | Les mêmes mesures sont refaites sur Raspberry Pi 5 avec le support de stockage précisé. | X | M |

Sous QEMU, les durées ne représentent pas un matériel réel. Les mesures y sont faites en mode comptage d'instructions pour être reproductibles, et présentées comme émulées.

### 5.8 Robustesse

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| ROB-01 | Une coupure d'alimentation à n'importe quel instant respecte les invariants de la section 7.2. | S | T |
| ROB-02 | Le plantage de `edr-archive` ou de `edrctl` n'a aucun effet sur la capture ni sur le gel. | S | T |
| ROB-03 | Le plantage de `edr-capture` est détecté par le journal, marqué par un `TROU`, et le processus est relancé. | C | T |
| ROB-04 | Le plantage de `edr-journal` est suivi d'une relance et d'une reprise en moins de 100 ms. | C | M |
| ROB-05 | Une erreur d'écriture du support est retentée un nombre borné de fois, puis signalée par l'état « défaut de stockage ». | C | T |
| ROB-06 | Une surcharge d'entrée (dix fois le débit nominal) ne bloque aucun producteur plus de 50 µs. Les trames en trop sont comptées. | C | T |
| ROB-07 | Un bloc corrompu (bit inversé) est détecté et écarté, sans affecter les blocs voisins. | S | T |
| ROB-08 | L'analyseur de journal ne plante sur aucune entrée, vérifié par 24 h de test à données aléatoires. | X | T |

### 5.9 Sécurité et vie privée

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| VIE-01 | Le système ne peut enregistrer ni VIN, ni position, ni date ou heure absolue, ni identité du conducteur. La liste blanche des signaux l'impose. | S | T, I |
| VIE-02 | Le temps est exprimé relativement à T0 et en cycles d'allumage. | S | T |
| VIE-03 | Les processus de l'enregistreur n'ouvrent aucune connexion réseau (boucle fermée). | C | T |
| SEC-01 | Une politique de sécurité QNX attribue un type à chaque processus. Seul le type des producteurs peut écrire sur l'entrée, seul le type de service peut lire, seul le journal accède à la partition brute. | C | T |
| SEC-02 | Aucune commande ne désactive l'enregistreur. Un arrêt forcé est suivi d'une relance, et l'interruption est enregistrée. | C | T |
| SEC-03 | Un événement verrouillé ne peut être effacé par aucune commande applicative. | C | T |
| SEC-04 | Chaque export porte une chaîne d'empreintes SHA-256 sur ses enregistrements et une signature. La clé est propre au type de véhicule, pas au véhicule, pour rester compatible avec l'anonymisation. | C | T |
| SEC-05 | Une trame malformée est rejetée et comptée, sans effet sur le service. | S | T |
| SEC-06 | La configuration est en lecture seule dans l'image de démarrage. Son empreinte figure dans chaque événement. | C | I |

### 5.10 Observabilité

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| OBS-01 | Les processus écrivent dans le journal système de QNX (slog2). | S | D |
| OBS-02 | `/dev/edr/status` donne l'état en texte : mode, segment courant, événements, compteurs, santé. | S | D |
| OBS-03 | Des événements utilisateur sont insérés dans la trace noyau au déclenchement, à la soumission de l'écriture, à l'acquittement du vidage et au scellement. | S | M |
| OBS-04 | Un autotest au démarrage écrit et relit un bloc d'essai et contrôle la cohérence de la configuration. | C | T |

### 5.11 Usage des technologies propres à QNX

| ID | Exigence | Niv. | Vérif. |
|---|---|---|---|
| QNX-01 | Le journal est un gestionnaire de ressources ; son interface se réduit à `open`, `write`, `read` et `devctl`. | S | I |
| QNX-02 | Une trace noyau montre que le pilote de disque traite l'écriture d'urgence à la priorité du journal. | S | D |
| QNX-03 | Les temporisations et les notifications entre processus utilisent des impulsions. | S | I |
| QNX-04 | Les processus critiques verrouillent leur mémoire au démarrage. | S | I |
| QNX-05 | La mort d'un producteur est détectée par la fermeture de sa connexion, sans interrogation périodique. | C | T |
| QNX-06 | Les processus sont lancés et relancés par le service de supervision de QNX. | C | D |
| QNX-07 | L'enregistreur démarre depuis l'image de démarrage, avant le montage des systèmes de fichiers. | C | M |
| QNX-08 | L'enregistreur tourne dans un invité QNX sous l'hyperviseur QNX et continue d'enregistrer quand l'invité voisin plante. | X | D |

---

## 6. Modèle de données

### 6.1 Classe A — éléments alignés sur l'annexe 4 du règlement n° 160

Les intervalles et cadences sont ceux relevés dans la version publiée au Journal officiel de l'UE. Le caractère obligatoire de chaque élément dépend de la série d'amendements et doit être relu sur le texte.

| Élément | Intervalle par rapport à T0 | Cadence | Source dans la démo |
|---|---|---|---|
| Variation de vitesse, longitudinale et latérale | 0 à 250 ms, ou jusqu'à la fin d'événement + 30 ms | 100 /s | Intégration des accélérations |
| Variation de vitesse maximale, et son instant | 0 à 300 ms | — | Calcul |
| Vitesse indiquée | −5,0 à 0 s | 2 /s | Scénario |
| Accélérateur (% de la course) | −5,0 à 0 s | 2 /s | Scénario |
| Frein de service (actif ou non) | −5,0 à 0 s | 2 /s | Scénario |
| Régime moteur | −5,0 à 0 s | 2 /s | Scénario |
| Activité de l'ABS, contrôle de stabilité | −5,0 à 0 s | 2 /s | Scénario |
| Angle du volant, vitesse de lacet | −5,0 à 0 s | 2 /s | Scénario |
| Accélérations longitudinale et latérale avant choc | −5,0 à 0 s | 2 /s | Scénario |
| États des aides à la conduite (freinage d'urgence, régulateur, maintien de voie) | −5,0 à 0 s | 2 /s | Scénario |
| Cycle d'allumage au moment du choc | −1,0 s | — | Compteur |
| Cycle d'allumage au téléchargement | À la lecture | — | Compteur |
| État des ceintures, témoin d'airbag | −1,0 s | — | Scénario |
| Temps de déploiement des airbags | Événement | — | Signal externe |
| Nombre d'événements, temps entre événements 1 et 2 | Événement | — | Calcul |
| Fichier complet enregistré (oui ou non) | Après les autres données | — | Scellement |
| Accélérations pendant le choc (si enregistrées) | 0 à 250 ms | 500 /s | Classe B |
| Accélération verticale, angle et vitesse de roulis (si enregistrés) | −1,0 à 5,0 s | 10 /s | Classe B |

Formats relevés : variation de vitesse de −100 à +100 km/h, précision ±10 %, résolution 1 km/h ; vitesse de 0 à 250 km/h, précision ±1 km/h ; instant du maximum de 0 à 300 ms, précision ±3 ms, résolution 2,5 ms.

### 6.2 Classe B — haute résolution

| Groupe | Signaux | Cadence | Fenêtre |
|---|---|---|---|
| Accélérations | x, y, z | 1 kHz | −20 s à +5 s |
| Dynamique | Vitesses des quatre roues, angle du volant, pédales, lacet, roulis, régime | 100 Hz | −20 s à +5 s |
| États | Aides à la conduite, ceintures, témoins, rapport engagé | 10 Hz | −20 s à +5 s |

Le profil américain (20 s à 10 Hz) se déduit de la classe B par sous-échantillonnage.

### 6.3 Données exclues par conception

VIN complet ou partiel, position, date et heure absolues, identité du conducteur, image, son. Un producteur qui tente d'envoyer un signal hors liste blanche voit sa trame rejetée (ACQ-04, VIE-01).

### 6.4 Correspondance MCAP

| Canal | Contenu | Cadence |
|---|---|---|
| `/edr/a/precrash` | Une ligne de classe A par instant | 2 /s |
| `/edr/a/delta_v` | Variation de vitesse longitudinale et latérale | 100 /s |
| `/edr/a/event` | Résumé : type de déclenchement, verrouillage, maximum, déploiements | 1 par événement |
| `/edr/b/accel` | Accélérations x, y, z | 1 kHz |
| `/edr/b/vehicle` | Signaux dynamiques | 100 Hz |
| `/edr/b/status` | États | 10 Hz |
| `/edr/meta/gaps` | Interruptions de capture | Au besoin |

- **Encodage** : JSON avec schéma JSON au niveau socle, Protobuf au niveau complet.
- **Temps** : T0 est placé à 60 s dans le fichier. Aucun horodatage absolu n'y figure.
- **Métadonnées** : `edr.event` (identifiant, type, verrouillage, fichier complet, cycle d'allumage, numéro dans l'accident), `edr.vehicle_type` (type, variante, version), `edr.build` (version, profil, empreinte de configuration), `edr.integrity` (empreinte de tête, signature).
- **Pièce jointe** facultative : l'extrait brut du journal, pour revérification.
- **Blocs** : CRC activé, compression zstd ou aucune, section de résumé et statistiques présentes.

---

## 7. Plan de validation

### 7.1 Quatre niveaux d'essai

| Niveau | Où | Ce qui est testé |
|---|---|---|
| 0 — unitaire | PC | Encodage des blocs, choix de la copie de table, machine d'états de reprise, calcul du déclenchement |
| 1 — exploration exhaustive | PC | Tous les états de disque atteignables par une coupure pendant un scénario |
| 2 — coupures réelles | QEMU | La chaîne complète, machine virtuelle tuée puis redémarrée |
| 3 — matériel | Raspberry Pi 5 | Coupures électriques réelles, latences réelles |

La bibliothèque de format et de reprise est écrite en C portable et compilée à l'identique pour le PC et pour QNX. C'est ce qui rend le niveau 1 possible.

### 7.2 Invariants à tenir après toute coupure

| ID | Invariant |
|---|---|
| I1 | La reprise se termine, en un nombre borné d'étapes, sans plantage. |
| I2 | Tout événement acquis avant la coupure est présent, et ses données de classe A d'avant choc sont identiques à celles injectées. |
| I3 | Aucun événement n'apparaît s'il n'a pas été déclenché. |
| I4 | Chaque échantillon restitué est identique à l'échantillon injecté de même instant. |
| I5 | Pour un événement acquis, le dernier échantillon restitué date de moins d'une période de vidage plus un bloc avant la coupure. |
| I6 | Un événement verrouillé survit à toute suite d'événements et de coupures ultérieurs. |
| I7 | « Fichier complet » vaut « oui » si et seulement si l'événement a été scellé avant la coupure. |
| I8 | Une coupure pendant la reprise ou pendant l'archivage, suivie d'une nouvelle reprise, donne le même état final. |

### 7.3 Exploration exhaustive des états de coupure

1. Le journal s'exécute sur PC contre un disque simulé qui enregistre, dans l'ordre, chaque écriture et chaque vidage.
2. Pour chaque préfixe de cette trace, l'outil construit l'image disque correspondante.
3. Il ajoute les variantes : dernière écriture déchirée à chaque limite de secteur ; sous-ensembles des écritures non encore vidées supprimés, pour modéliser un cache de disque volatil.
4. Sur chaque image, il lance la reprise puis vérifie les invariants I1 à I8 contre les données injectées.
5. Il rejoue ensuite la même exploration sur la trace de la reprise elle-même (I8).

Un scénario de 30 s produit environ 2 600 écritures. Avec les variantes, l'exploration couvre de l'ordre de 10 000 états au niveau socle et d'un million au niveau complet, en quelques minutes.

### 7.4 Campagne de coupures sous QEMU

Un orchestrateur sur PC démarre QEMU, lance le scénario, tue le processus QEMU à un instant choisi, redémarre, exporte les événements et les compare aux données injectées.

- **Balayage** : un instant de coupure par milliseconde, de 50 ms avant le déclenchement à 500 ms après, puis des instants aléatoires.
- **Limite connue** : tuer QEMU ne fait pas perdre les écritures déjà remises au système hôte. Ce niveau teste l'ordre et l'interruption des écritures, pas la perte d'un cache volatil, que couvre le niveau 1.
- **Niveau extrême** : le disque de la machine virtuelle est servi par un petit serveur de blocs en réseau (NBD) écrit pour le projet. Il retient les écritures non vidées et en jette une partie à la coupure. Il journalise aussi les ordres de vidage reçus, ce qui vérifie que QNX les émet bien.

### 7.5 Campagne de latence

- Déclenchements répétés sous trois charges : processeur saturé par des tâches moins prioritaires, écritures intensives sur l'archive par un autre processus, pression mémoire.
- Mesure par les événements de trace noyau (OBS-03).
- Résultat publié : histogramme, médiane, 99e centile, 99,9e centile, maximum, et les trois pires cas commentés.

### 7.6 Injection d'autres fautes

- Arrêt forcé de chaque processus à des instants aléatoires.
- Inversion de bits dans le journal, table d'allocation détruite, partition remplie d'événements verrouillés.
- Trames malformées, signaux interdits, débit multiplié par dix.

### 7.7 Seuils de réussite par niveau

| Mesure | Socle | Complet | Extrême |
|---|---|---|---|
| États de coupure explorés | 10 000 | 1 000 000, coupures imbriquées comprises | Idem, plus traces relevées sur la cible |
| Coupures de QEMU | 200 | 2 000 balayées | 10 000 avec cache volatil simulé |
| Déclenchements mesurés | 1 000 | 10 000 sous charge | Idem sur Raspberry Pi 5 |
| Arrêts forcés de processus | — | 500 | 5 000 |
| Coupures électriques réelles | — | — | 500 |
| Violations d'invariant tolérées | 0 | 0 | 0 |

Toute violation rencontrée en cours de développement est conservée comme test de non-régression, avec l'image disque qui la reproduit.

---

## 8. Démonstration (vidéo de moins de trois minutes)

| Temps | À l'écran | Message |
|---|---|---|
| 0:00 – 0:20 | Une phrase et un schéma | Toute voiture neuve en Europe embarque un EDR. Il doit écrire au pire moment possible. |
| 0:20 – 0:45 | Schéma d'architecture | Deux classes de données, un journal brut, une archive MCAP. |
| 0:45 – 1:35 | Terminal et QEMU | Rejeu d'un trajet. Choc. « Acquis en N ms ». QEMU est tué 30 ms après l'impact. Redémarrage : `edrctl list` montre l'événement, verrouillé, « fichier complet = non ». |
| 1:35 – 2:00 | Foxglove | Les cinq secondes d'avant choc, la courbe de variation de vitesse qui s'arrête net à la coupure. |
| 2:00 – 2:25 | Rapport de campagne | Nombre de coupures, zéro perte ; nombre d'états explorés ; histogramme de latence. |
| 2:25 – 2:45 | Trace noyau et terminal | Le pilote de disque à la priorité du journal. Un processus intrus se voit refuser `/dev/edr`. |
| 2:45 – 3:00 | Dépôt | Licence, commande unique pour rejouer la démo, tutoriel. |

**Données du trajet.** Trois sources, par ordre de repli :

1. Un générateur synthétique à graine fixe : conduite urbaine, freinage, impulsion de choc paramétrable. Toujours disponible, il sert aussi aux cas limites.
2. Des enregistrements réels de conduite, par exemple le jeu comma2k19 (licence MIT d'après sa fiche, à confirmer).
3. Des impulsions de choc réelles tirées de la base publique des essais de choc de la NHTSA (format à vérifier).

---

## 9. Livrables et dépôt

```
ambre/
├── README.md              anglais : pitch, démo en une commande, preuves d'usage de QNX
├── LICENSE                Apache-2.0
├── NOTICE                 attribution des jeux de données et bibliothèques
├── fmt/                   bibliothèque C portable : blocs, table, reprise
├── target/                processus QNX : capture, journal, archive, edrctl, replay
├── image/                 fragments pour mkqnximage, politique de sécurité
├── host/
│   ├── explore/           exploration des états de coupure
│   ├── powercut/          orchestrateur de coupures QEMU, serveur de blocs fautif
│   ├── verify/            edr-verify
│   └── scenarios/         générateur et scénarios de référence
├── docs/                  ce cahier des charges, format du journal, résultats de campagne
├── codelab/               tutoriel pas à pas
└── foxglove/              disposition d'écran
```

- **Langages** : C11 pour la bibliothèque de format et les processus critiques ; C++17 pour l'archive, avec la bibliothèque MCAP déjà portée sur QNX ; Python 3 pour les outils sur PC.
- **Instructions de construction** : version de QNX, cible (QEMU x86_64), chaîne de compilation, comme le règlement l'exige.
- **Preuves d'usage de QNX** à pointer dans la description : gestionnaire de ressources, passage de messages et héritage de priorité, impulsions, politique de sécurité, système Power-Safe, trace noyau.
- **Tutoriel** candidat au bonus : « Écrire un journal qui survit aux coupures sur QNX 8.0, et le prouver ».
- **Tout en anglais** dans le dépôt et la vidéo.

---

## 10. Planning

Cinq semaines exactement séparent le mardi 10 novembre du mardi 15 décembre.

| Semaine | Dates | Contenu | Jalon |
|---|---|---|---|
| 1 | 10 – 16 nov. | Essais de faisabilité E1 à E4. Bibliothèque de format, reprise, tests unitaires. Exploration exhaustive, première version. | L'exploration passe sur PC |
| 2 | 17 – 23 nov. | Journal en gestionnaire de ressources, capture, déclenchement, rejeu. Premier événement de bout en bout sous QEMU. | Un événement survit à un redémarrage |
| 3 | 24 – 30 nov. | Archive MCAP, disposition Foxglove, reprise au démarrage, orchestrateur de coupures. | **Socle atteint** : 200 coupures sans perte |
| 4 | 1 – 7 déc. | Verrouillage, multi-événements, écrasement. Politique de sécurité, supervision. Campagne de latence et traces. Données réelles. | **Complet atteint** |
| 5 | 8 – 14 déc. | README, tutoriel, vidéo, résultats de campagne. Un seul élément extrême au choix. Soumission le 14 au soir. | Soumis, un jour de marge |

**Essais de faisabilité (trois premiers jours).**

| Essai | Question | Si la réponse est non |
|---|---|---|
| E1 | Depuis QNX sous QEMU, une écriture sur partition brute suivie d'une demande de vidage atteint-elle le disque virtuel comme un ordre de vidage ? | Chercher la commande de vidage propre au pilote ; à défaut, partition Power-Safe dédiée et `fsync()`, avec un délai de gel plus long |
| E2 | La bibliothèque MCAP portée se lie-t-elle à un programme QNX, et Foxglove lit-il un fichier JSON ainsi produit ? | Écrire un générateur MCAP minimal ; en dernier recours, produire le MCAP sur PC |
| E3 | Peut-on tuer et redémarrer QEMU en boucle, sans intervention, en moins de dix secondes par cycle ? | Réduire l'image de démarrage ; lancer plusieurs machines en parallèle |
| E4 | L'envoi d'une trame de 4 Ko par passage de messages tient-il les 50 µs ? | Passer à une mémoire partagée avec impulsions |

**Ce qui saute en premier en cas de retard** : le serveur de blocs fautif, le bus CAN, la signature, le rapport HTML. Le socle et la campagne de coupures ne sautent pas.

---

## 11. Risques

| Risque | Gravité | Parade |
|---|---|---|
| Le vidage de cache sur partition brute ne se comporte pas comme prévu sous QNX | Forte | Essai E1 dès le premier jour ; solution de repli décrite |
| Un projet concurrent proche (`last-second`) fait une démo qui ressemble | Moyenne | Différenciation de la section 2 ; consulter la galerie avant de soumettre |
| Les durées sous QEMU sont contestées | Moyenne | Mode comptage d'instructions, mention « émulé », séparation part logicielle et part support ; Raspberry Pi 5 si disponible |
| Le périmètre déborde | Forte | Niveaux S, C, X ; jalon socle en fin de semaine 3 |
| Une écriture de secteur n'est pas atomique sur le support | Moyenne | CRC par bloc, table en deux copies, variantes d'écriture déchirée dans l'exploration |
| Une écriture d'archive retarde le gel | Moyenne | Pas d'archive pendant un événement, écritures d'archive de taille bornée, mesure sous charge disque |
| Les jeux de données réels posent un problème de licence ou de format | Faible | Générateur synthétique toujours disponible |
| Une revendication de conformité réglementaire est jugée abusive | Moyenne | Formulation « aligné sur », jamais « conforme » |
| Le coût de `fsync()` du système Power-Safe pèse sur l'archive | Faible | Partition dédiée, basse priorité, hors chemin critique |

---

## 12. Pousser l'idée au bout

Au-delà du niveau complet, par ordre d'intérêt pour un jury :

1. **Disque à cache volatil simulé** (7.4) : la campagne de coupures devient plus dure que ce qu'un matériel correct infligerait.
2. **Modèle formel du protocole de validation.** La table en deux copies et la reprise sont décrites dans un langage de spécification (TLA+ par exemple) et vérifiées par exploration de modèle. Le code est relié au modèle par les mêmes invariants.
3. **Coupures électriques réelles** sur Raspberry Pi 5, pilotées par un relais, sur carte SD puis sur NVMe, avec comparaison des latences.
4. **Modèle de réserve d'énergie.** Une entrée « tension » décroît comme un condensateur qui se vide. La démo montre que le gel se termine avant le seuil d'arrêt, et quelle capacité il faut : à 5 W, 20 ms demandent 0,1 J.
5. **Isolation par hyperviseur.** L'enregistreur vit dans un invité QNX, à côté d'un invité qui joue l'info-divertissement. On fait planter le second ; le premier enregistre.
6. **Journal des interactions de conduite automatisée** : activations, demandes de reprise en main, reprises par le conducteur, sur un canal séparé.
7. **Profil américain** sélectionnable (20 s à 10 Hz).
8. **Plusieurs calculateurs** : signaux reçus par zenoh, avec alignement des horloges.
9. **Dossier de sûreté** réduit : analyse des modes de défaillance, et matrice de traçabilité des exigences vers les tests, générée automatiquement.
10. **Qualité du code critique** : couverture de la bibliothèque de format au-dessus de 95 % des lignes, analyse statique, test à données aléatoires de 24 h.
11. **Démarrage rapide** : capture active en moins de 500 ms après la mise sous tension.
12. **Rapport de lecture** au format d'un rapport d'expertise, avec le tableau réglementaire rempli.

---

## 13. Critères d'acceptation

**Socle — soumission possible.**

- [ ] Toutes les exigences de niveau S sont vérifiées.
- [ ] Une seule commande rejoue la démonstration sous QEMU.
- [ ] 10 000 états de coupure explorés et 200 coupures de QEMU, zéro violation.
- [ ] 1 000 déclenchements mesurés, zéro dépassement du délai de gel annoncé.
- [ ] Un événement exporté s'ouvre dans Foxglove.
- [ ] Dépôt public, licence, README en anglais, vidéo de moins de trois minutes.

**Complet — version visée.**

- [ ] Toutes les exigences de niveau C sont vérifiées.
- [ ] Un million d'états explorés, coupures pendant la reprise comprises ; 2 000 coupures de QEMU.
- [ ] 10 000 déclenchements sous charge ; trois pires cas expliqués par une trace.
- [ ] Un processus intrus est refusé par la politique de sécurité.
- [ ] Tutoriel publié.

**Extrême.**

- [ ] Au moins deux éléments de la section 12 réalisés et mesurés.

---

## 14. Ce qui est vérifié et ce qui reste à valider

| Affirmation | Statut |
|---|---|
| Dates d'application de l'EDR dans l'UE ; exigences de l'article 6 | Vérifié sur le texte du règlement (UE) 2019/2144 |
| Seuils, T0, verrouillage, capacité, éléments de données du règlement n° 160 | Relevés par extraction automatique de la version publiée au Journal officiel de l'UE en 2021. La série 01 d'amendements, celle qu'exige l'UE, n'a pas pu être consultée (accès refusé au document de la CEE-ONU). **À relire.** |
| Règle américaine de 2024 et report de 2026 | Vérifié sur le Federal Register |
| Comportement du système Power-Safe (`fsync()` global, `O_SYNC` ignoré, instantané toutes les 10 s) | Vérifié dans la documentation QNX 8.0 |
| Niveaux de validation d'`io-blk.so`, partitions brutes exposées sous `/dev` | Vérifié dans la documentation QNX 8.0 |
| Format MCAP : enregistrements, CRC, encodages | Vérifié sur la spécification |
| Bibliothèque MCAP présente dans le tableau des ports QNX | Vérifié |
| Contenu des dépôts concurrents | Lu le 10 novembre 2026 ; ils évoluent |
| Un vidage demandé sur partition brute atteint le support | **Hypothèse**, essai E1 |
| L'héritage de priorité s'applique au pilote de disque sur ce chemin | **Hypothèse** fondée sur le fonctionnement général de QNX, à montrer par trace (QNX-02) |
| Tuer QEMU conserve les écritures déjà remises à l'hôte | Raisonnement sur le cache de l'hôte, non testé |
| Délai de gel de 20 ms | **Cible**, à mesurer |
| Outils `mcap doctor` et `mcap recover` ; lecture du JSON par Foxglove | De mémoire, essai E2 |
| Service de supervision et génération de politiques de sécurité sous QNX 8.0 | Vus dans des dépôts concurrents, à confirmer dans la documentation |
| Licence MIT de comma2k19 ; format des données NHTSA | À confirmer |
| Travaux de référence sur le test de cohérence après coupure (CrashMonkey, ALICE, dm-log-writes) | Cités de mémoire comme inspiration, non revérifiés |

---

## 15. Glossaire

| Terme | Sens |
|---|---|
| EDR | Enregistreur de données d'événement, la « boîte noire » d'une voiture |
| T0 | Instant zéro : référence de temps de l'événement |
| Variation de vitesse (Δv) | Changement de vitesse du véhicule pendant le choc, obtenu en intégrant l'accélération |
| Capturer, enregistrer | Mettre en tampon en mémoire vive ; sauvegarder en mémoire non volatile |
| Acquis | État d'un événement dont l'écriture d'urgence et la table ont été vidées sur le support |
| Verrouillé | Événement qui ne peut plus être écrasé |
| Scellé | Événement terminé, avec son résumé de fin |
| Vidage | Ordre donné au support d'écrire réellement ce qu'il garde dans son cache |
| Partition brute | Zone de disque utilisée directement, sans système de fichiers |
| Gestionnaire de ressources | Processus QNX qui apparaît comme un fichier ou un répertoire, par exemple sous `/dev` |
| Impulsion | Petit message QNX non bloquant, utilisé pour les notifications |
| Politique de sécurité | Règles QNX qui fixent quels types de processus peuvent se parler |
| Power-Safe | Système de fichiers QNX à copie sur écriture, cohérent après coupure |
| MCAP | Format de fichier ouvert pour données horodatées de robotique et d'automobile |
| M1, N1 | Catégories réglementaires : voitures particulières, utilitaires légers |

---

## 16. Sources

**Réglementation**

- Règlement (UE) 2019/2144 : https://eur-lex.europa.eu/legal-content/EN/TXT/HTML/?uri=CELEX:32019R2144
- Règlement ONU n° 160, version publiée au JO de l'UE : https://eur-lex.europa.eu/legal-content/EN/TXT/HTML/?uri=CELEX:42021X1215
- Règlement ONU n° 160, révision 1 (non consultée, accès refusé) : https://unece.org/sites/default/files/2024-02/R160r1e.pdf
- Règlement délégué (UE) 2022/545 : https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX%3A32022R0545
- États-Unis, règle finale du 18 décembre 2024 : https://www.govinfo.gov/content/pkg/FR-2024-12-18/html/2024-29862.htm
- États-Unis, report du 18 mai 2026 : https://www.govinfo.gov/content/pkg/FR-2026-05-18/html/2026-09849.htm

**QNX**

- Système de fichiers Power-Safe, guide de l'utilisateur : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.user_guide/topic/fsystems_QNX6_filesystem.html
- Power-Safe, performances : https://www.qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.sys_arch/topic/fsys_QNX6_performance.html
- `fs-qnx6.so` : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/f/fs-qnx6.so.html
- `io-blk.so` : https://qnx.com/developers/docs/8.0/com.qnx.doc.neutrino.utilities/topic/i/io-blk.so.html
- Tableau des ports open source : https://oss.qnx.com/

**Format et outils**

- Spécification MCAP : https://mcap.dev/spec
- Registre MCAP (encodages) : https://mcap.dev/spec/registry
- QEMU, injection d'erreurs de blocs : https://qemu.googlesource.com/qemu/+/refs/heads/master/docs/devel/testing/blkdebug.rst

**Données**

- Base des essais de choc de la NHTSA, documentation de l'interface : https://nrd-static.nhtsa.dot.gov/
- comma2k19 : https://github.com/commaai/comma2k19

**Hackathon et projets voisins**

- Page et règlement : https://qnx.devpost.com/ et https://qnx.devpost.com/rules
- https://github.com/vinit1009/last-second
- https://github.com/ayswarya-nachinarkinian/Real-Time-Automotive-Event-Recorder
- https://github.com/santhoshbala23/real-time-automotive-event-recorder
- https://github.com/princeraj2572/QNX-Black-Box
