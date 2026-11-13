#!/usr/bin/env python3
"""qnx-mbr.py — table de partitions MBR du disque brut du journal.

Le journal d'Ambre écrit dans une partition BRUTE de type 177 (0xB1) : sans
table de partitions, devb-eide ne publie pas /dev/hd0t177 et le journal
refuse de démarrer (« il n'y a pas de partition à ouvrir »). sfdisk n'est pas
installé partout — il est ABSENT du r630, où le disque restait donc vide de
table et la démo jouait le faux « READY » sans jamais enregistrer d'événement.

La table est donc écrite ici, en stdlib seule, puis RELUE pour vérifier : si
l'écriture échoue, le script s'arrête au lieu de continuer sur un disque muet.

    qnx-mbr.py <disque> [taille] [début_lba] [type]
    taille : 67108864, 64M, 64MiB... (défaut 64M)
"""
import os
import struct
import sys

SECTEUR = 512
TETES = 255
PISTES = 63


def chs(lba):
    """Convertit un LBA en triplet CHS (tête, secteur, cylindre)."""
    cyl = lba // (TETES * PISTES)
    reste = lba % (TETES * PISTES)
    return bytes([(reste // PISTES) & 0xFF, (reste % PISTES) + 1, cyl & 0xFF])


def taille_octets(texte):
    """Lit une taille avec suffixe (64M, 512K, 2G) ou en octets."""
    t = str(texte).strip().upper()
    for suffixe, mult in (("MIB", 1024 ** 2), ("M", 1024 ** 2), ("KIB", 1024),
                          ("K", 1024), ("G", 1024 ** 3)):
        if t.endswith(suffixe):
            return int(float(t[:-len(suffixe)]) * mult)
    return int(t)


def ecrire(disque, taille, debut, type_mbr):
    """Écrit le MBR (une partition de type 177 qui va jusqu'au bout du disque)."""
    nb = taille // SECTEUR - debut
    entree = struct.pack("<B3sB3sII", 0x00, chs(debut), type_mbr,
                          chs(debut + nb - 1), debut, nb)
    mbr = bytearray(SECTEUR)
    mbr[446:462] = entree
    mbr[510:512] = b"\x55\xaa"
    with open(disque, "r+b") as fh:
        fh.seek(0)
        fh.write(bytes(mbr))
    return debut, nb


def relire(disque):
    """Relit la première partition du MBR. None si le disque n'en a pas."""
    with open(disque, "rb") as fh:
        mbr = fh.read(SECTEUR)
    if mbr[510:512] != b"\x55\xaa":
        return None
    for i in range(4):
        entree = mbr[446 + 16 * i:446 + 16 * (i + 1)]
        _, _, typ, _, lba, cnt = struct.unpack("<B3sB3sII", entree)
        if typ or lba or cnt:
            return {"type": typ, "lba": lba, "secteurs": cnt}
    return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    disque = argv[1]
    taille = taille_octets(argv[2]) if len(argv) > 2 else 64 * 1024 * 1024
    debut = int(argv[3]) if len(argv) > 3 else 2048
    type_mbr = int(argv[4], 0) if len(argv) > 4 else 0xB1
    if not os.path.exists(disque):
        with open(disque, "wb") as fh:
            fh.truncate(taille)
    debut, nb = ecrire(disque, taille, debut, type_mbr)
    relu = relire(disque)
    if not relu or relu["lba"] != debut or relu["secteurs"] != nb:
        print("qnx-mbr: ECHEC — table de partitions non écrite sur %s" % disque,
              file=sys.stderr)
        return 1
    print("qnx-mbr: %s — type 0x%02X, LBA %d, %d secteurs (%d Mo)"
          % (disque, type_mbr, debut, nb, nb * SECTEUR // (1024 * 1024)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
