#!/usr/bin/env python3
"""media.py — les médias du banc QNX, et le glue sous-processus qui va avec.

Deux médias, deux contrats différents, et c'est toute la distinction STO-01 /
STO-07 du cahier des charges :

  journal.raw   partition BRUTE, type 177 (0xB1), aucun système de fichiers.
                devb-eide ne publie /dev/hd0t177 que si une table de partitions
                existe : sans elle, edr-journal ne démarre pas et le banc
                imprimerait un faux « READY » sans rien sur le média. La table
                est écrite par scripts/qnx-mbr.py (stdlib seule, puis RELUE),
                parce que sfdisk est ABSENT de certaines machines du parc.

  archive.raw   partition FAT32 (type 0x0C) que fs-dos.so monte sur /arch. Le
                système de fichiers est façonné côté hôte (`mkfs.vfat -C`) puis
                recopié dans la partition : c'est ce qui permet de RELIRE les
                MCAP écrits par le guest sans rien monter ni aucun privilège, et
                de les vérifier avec le propre lecteur du projet. Un média QNX6
                (fs-qnx6.so) serait illisible côté hôte sans outil dédié : le
                FAT est un choix de banc, pas de produit — dit comme tel dans
                docs/QNX-DEMO.md.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
QNX_MBR = os.path.join(_ROOT, "scripts", "qnx-mbr.py")


class BenchError(RuntimeError):
    """Le banc ne peut pas préparer la cible : on s'arrête, on ne simule pas."""


def run(argv, env=None, cwd=_ROOT, timeout=600, log=None):
    """Une commande, un rc, aucune exception : le banc rend des rapports."""
    proc = subprocess.run(argv, cwd=cwd, env=env, timeout=timeout,
                          capture_output=True, text=True, check=False)
    if log is not None:
        with open(log, "a") as fh:
            fh.write("$ %s\n%s\n" % (" ".join(argv), proc.stdout + proc.stderr))
    return proc.returncode, proc.stdout, proc.stderr


def write_mbr(disk, size, lba, type_hex):
    """La table de partitions, écrite et RELUE par scripts/qnx-mbr.py."""
    rc, out, err = run([sys.executable, QNX_MBR, disk, str(size), str(lba), type_hex],
                       timeout=120)
    if rc != 0:
        raise BenchError("qnx-mbr rc=%d : %s" % (rc, (out + err)[-500:]))
    return out.strip()


def make_journal_disk(path, size_mb=64, lba=2048):
    """Le média du journal : brut, type 177, aucun fichier système (STO-01)."""
    if os.path.exists(path):
        os.unlink(path)
    return write_mbr(path, "%dM" % size_mb, lba, "0xB1")


def make_archive_disk(path, size_mb=64, lba=2048):
    """Le média d'archive : FAT32 formaté côté hôte, dans sa partition."""
    sectors = size_mb * 1024 * 1024 // 512 - lba
    part = path + ".part"
    if os.path.exists(path):
        os.unlink(path)
    rc, out, err = run(["mkfs.vfat", "-F", "32", "-C", part, str(sectors // 2)],
                       timeout=180)
    if rc != 0:
        raise BenchError("mkfs.vfat rc=%d : %s" % (rc, (out + err)[-500:]))
    with open(path, "wb") as fh:
        fh.truncate(size_mb * 1024 * 1024)
    write_mbr(path, "%dM" % size_mb, lba, "0x0C")
    with open(part, "rb") as src, open(path, "r+b") as dst:
        dst.seek(lba * 512)
        shutil.copyfileobj(src, dst)
    os.unlink(part)
    return path


def archive_bytes(path, lba=2048):
    """Les octets de la partition elle-même (sans le MBR) : lecture hôte du
    média écrit par le guest, base de la vérification des MCAP."""
    with open(path, "rb") as fh:
        fh.seek(lba * 512)
        return fh.read()
