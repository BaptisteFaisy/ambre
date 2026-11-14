#!/usr/bin/env python3
"""image.py — la construction de l'image de banc (IFS) à partir de celle de la vitrine.

Le banc a besoin de PLUS que la vitrine (une console de shell au lieu de la
démo narrée, un second média monté, la politique poussée, les deux sondes), mais
la vitrine ne doit pas changer d'un octet : c'est elle qui est en ligne. Ce
module lit donc image/vitrine.build TEL QUEL et y INSÈRE ses blocs au niveau
d'ancres explicites ; le fichier live n'est jamais modifié, et une ancre absente
est une erreur franche, pas une insertion silencieuse.

Les trois blocs insérables :
  - la tâche de console      (démo narrée, ou ksh de banc piloté par l'hôte)
  - le montage d'archive     (STO-07 : second média FAT32 sur /arch)
  - le secpolpush            (la politique, avant le premier processus EDR)

Et les fichiers que le banc ajoute à l'IFS : les deux sondes (devctl, intrusion),
la politique compilée, secpollaunch et sa configuration, le module FAT.
"""

from __future__ import annotations

import os
import shutil
import sys

from media import BenchError, make_archive_disk, make_journal_disk, run
from sdp import compile_policy, make_binaries, sdp_env

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))

BUILDFILE = os.path.join(_ROOT, "image", "vitrine.build")
SECPOLLAUNCH_CFG = os.path.join(_ROOT, "image", "secpollaunch-edr_launcher_t.cfg")


# Les ancres du buildfile de la vitrine, sur lesquelles le banc INSÈRE ses
# blocs. Le fichier image/vitrine.build n'est PAS modifié : la vitrine live se
# construit bit-pour-bit comme avant, et le banc ajoute ce dont il a besoin.
ANCHOR_CONSOLE = "    on -d -t /dev/ser2 /proc/boot/demo-qnx.sh"
ANCHOR_STACK = "    waitfor /dev/hd0 10"

# Le montage du média d'archive (STO-07) : second disque FAT32, monté après le
# block stack, sur le répertoire /arch créé dans l'IFS.
ARCHIVE_SETUP = (
    "    waitfor /dev/hd1t12 10\n"
    # Le type du module est `dos`, pas `msdos` : la page SDP de fs-dos.so donne
    # la syntaxe `driver ... dos dos_options ...`, et `mount -t msdos` cherchoit
    # un module fs-msdos.so — d'où « Can't access shared library » sur le banc,
    # avec pourtant fs-dos.so présent à plat dans /proc/boot (mesuré 2026-11-14).
    "    mount -t dos /dev/hd1t12 /arch\n"
)

# Ce que le banc ajoute à la liste des fichiers de l'IFS : les deux sondes, la
# politique compilée, secpollaunch et sa configuration, le module FAT et le
# point de montage. `[+optional]` en tête de buildfile ignore ce qui manque.
BENCH_FILES = """
# --- les fichiers du BANC (host/qnx/bench.py) -------------------------------
# Ils n'existent que dans l'image de banc : la vitrine ne les embarque pas.
devctl_probe
secpol_probe
bin/secpollaunch
[perms=4755] /proc/boot/secpollaunch=secpollaunch
bin/secpolpush
bin/mount
[perms=0444] secpol.bin
lib/dll/fs-dos.so
# edr-archive est C++17 : il lie libc++.so.2 (mesuré, pas supposé — sans lui le
# chargeur du guest imprime « ldd:FATAL: Could not load library libc++.so.2 » et
# l'archive ne démarre jamais, même quand le média et le montage sont bons).
# La bibliothèque vit dans $QNX_TARGET/usr/lib (pas dans $QNX_TARGET/lib) :
# l'entrée est donc un CHEMIN RELATIF à la piste de recherche, et mkifs la pose
# À PLAT dans /proc/boot — comme toutes les autres bibliothèques de cette image
# (dumpifs : proc/boot/libc.so.6, proc/boot/libm.so.3, …). Une destination
# /proc/boot/lib/ ne se charge pas : LD_LIBRARY_PATH=/proc/boot, et le guest
# imprimait encore « ldd:FATAL: Could not load library libc++.so.2 » avec le
# fichier présent dans l'IFS mais au mauvais endroit.
usr/lib/libc++.so.2
[type=dir uid=0 gid=0 perms=0777] /arch=
[perms=0444] /proc/boot/secpollaunch-edr_launcher_t.cfg = {
%s
}
"""


def insert_at(text, anchor, block, what):
    """Insère `block` juste après la première ligne `anchor`; erreur si absente."""
    if anchor not in text:
        raise BenchError("buildfile : ancre %s absente (%s)" % (anchor.strip(), what))
    return text.replace(anchor, anchor + "\n" + block, 1)


def replace_at(text, anchor, block, what):
    """REMPLACE la ligne `anchor` par `block`.

    La console est un choix, pas un ajout : si le banc insérait son ksh APRÈS la
    démo narrée, les DEUX tourneraient sur /dev/ser2 et leurs sorties
    s'entremêleraient (mesuré le 2026-11-14 : les bannières de la démo
    apparaissaient dans le flux de commandes du banc, et deux edr-journal
    coexistaient). Remplacer la ligne est la seule sémantique correcte.
    """
    if anchor not in text:
        raise BenchError("buildfile : ancre %s absente (%s)" % (anchor.strip(), what))
    return text.replace(anchor, block, 1)


def bench_files(cfg_text):
    """Le bloc des fichiers de banc, avec la configuration secpollaunch."""
    return BENCH_FILES % cfg_text


def build_ifs(workdir, archive=False, secpol=False, task=None, env=None,
              quiet=True):
    """image/vitrine.build → ifs.bin de BANC, sans toucher au fichier live.

    Le buildfile de la vitrine est lu TEL QUEL et le banc y INSÈRE ses blocs
    (console de bench, montage d'archive, secpolpush, fichiers de sondes). La
    vitrine se construit donc exactement comme avant : pas de marqueur à
    substituer, donc pas de buildfile malformé par une substitution oubliée.
    Une ancre absente est une erreur franche, jamais une insertion muette.
    """
    env = env or sdp_env()
    os.makedirs(workdir, exist_ok=True)
    target = env["QNX_TARGET"] + "/x86_64"
    parts = [target, target + "/boot/sys", target + "/bin", target + "/usr/bin",
             target + "/sbin", target + "/lib", target + "/lib/dll",
             target + "/usr/lib", os.path.join(_ROOT, "build", "qnx"), workdir]
    cfg = open(SECPOLLAUNCH_CFG).read()
    text = open(BUILDFILE).read()
    text = text.replace("@SEARCH@", ":".join(parts))
    # Par défaut, le banc prend un SHELL (piloté par l'hôte) ; la démo narrée
    # est un choix explicite. La vitrine, elle, construit son IFS à partir du
    # même fichier image/vitrine.build mais sans ce module : elle garde sa démo.
    console = task or ("on -d -t /dev/ser2 -e HOME=/proc/boot /proc/boot/ksh")
    if secpol:
        console = console.replace("on ", "on -u 100:100 ", 1)
    text = replace_at(text, ANCHOR_CONSOLE, "    " + console,
                      "la tâche de console")
    if secpol:
        text = insert_at(text, ANCHOR_STACK,
                         "    secpolpush /proc/boot/secpol.bin",
                         "le secpolpush avant le premier processus EDR")
    if archive:
        text = insert_at(text, ANCHOR_STACK, ARCHIVE_SETUP.rstrip("\n"),
                         "le montage du média d'archive")
    text = text + bench_files(cfg)
    build = os.path.join(workdir, "target.build")
    with open(build, "w") as fh:
        fh.write(text)
    ifs = os.path.join(workdir, "ifs.bin")
    rc, out, err = run(["mkifs", "-v", build, ifs], env=env, timeout=300)
    if rc != 0:
        raise BenchError("mkifs rc=%d : %s" % (rc, (out + err)[-2000:]))
    if not quiet:
        print(out[-400:])
    return ifs


def prepare(workdir, archive=True, secpol=False, task=None, verbose=False):
    """Tout le banc d'un coup : binaires, politique, disques, IFS."""
    env = sdp_env()
    make_binaries(env, verbose=verbose)
    if secpol:
        compile_policy(env)
    os.makedirs(workdir, exist_ok=True)
    make_journal_disk(os.path.join(workdir, "journal.raw"))
    if archive:
        make_archive_disk(os.path.join(workdir, "archive.raw"))
    scenario = os.path.join(workdir, "scenario.txt")
    if not os.path.exists(scenario):
        run([sys.executable, os.path.join(_ROOT, "host", "scenarios", "generate.py"),
              "--preset", "urban-frontal", "--seed", "2026", "--duration-ms", "4000",
              "--crash-at-ms", "2500", "--out", scenario], timeout=180)
    for name in ("demo-qnx.sh", "diagnose.sh"):
        shutil.copy2(os.path.join(_ROOT, "image", name), os.path.join(workdir, name))
    ifs = build_ifs(workdir, archive=archive, secpol=secpol, task=task, env=env)
    return {"ifs": ifs, "workdir": workdir, "env": env}


