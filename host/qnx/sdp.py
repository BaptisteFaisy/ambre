#!/usr/bin/env python3
"""sdp.py — l'environnement de la SDP 8.0 et les binaires de la cible.

Isolé dans son propre module pour que la chaîne soit acyclique :
  sdp.py    (environnement, compilation)  <- media.py, image.py
  media.py  (les deux médias du banc)
  image.py  (la construction de l'IFS de banc)
  bench.py  (la machine : QEMU, console, coupure)

Le point délicat est le PATH : `qnxsdp-env.sh` REMPLACE le PATH par les seuls
outils de la SDP. C'est suffisant pour qcc et mkifs, pas pour `make` (qui a
besoin de mkdir, basename, file, cut) — l'environnement hôte est donc remis
derrière, la SDP en premier.
"""

from __future__ import annotations

import os
import re
import subprocess

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))

SDP_ENV = os.path.expanduser("~/qnx800/qnxsdp-env.sh")


class BenchError(RuntimeError):
    """La SDP ou la compilation du target est indisponible : on s'arrête."""


def run(argv, env=None, cwd=_ROOT, timeout=600, log=None):
    """Une commande, un rc, aucune exception : le banc rend des rapports."""
    proc = subprocess.run(argv, cwd=cwd, env=env, timeout=timeout,
                          capture_output=True, text=True, check=False)
    if log is not None:
        with open(log, "a") as fh:
            fh.write("$ %s\n%s\n" % (" ".join(argv), proc.stdout + proc.stderr))
    return proc.returncode, proc.stdout, proc.stderr


def sdp_env(force=False):
    """L'environnement de la SDP 8.0 (qcc, mkifs, secpolcompile, QNX_TARGET)."""
    cached = getattr(sdp_env, "_cache", None)
    if cached is not None and not force:
        return cached
    if not os.path.exists(SDP_ENV):
        raise BenchError("QNX SDP 8.0 absente : %s introuvable" % SDP_ENV)
    proc = subprocess.run(["bash", "-c", ". %s >/dev/null; env" % SDP_ENV],
                          capture_output=True, text=True, timeout=60, check=False)
    env = dict(os.environ)
    for line in proc.stdout.splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", key):
                env[key] = value
    if not env.get("QNX_TARGET"):
        raise BenchError("QNX_TARGET absent de l'environnement SDP (%s)" % proc.stderr[:200])
    host_path = os.environ.get("PATH", "")
    if host_path:
        env["PATH"] = env.get("PATH", "") + ":" + host_path
    sdp_env._cache = env
    return env


def make_binaries(env=None, verbose=False):
    """Les cinq processus du target + les deux sondes, compilés par le vrai qcc."""
    env = env or sdp_env()
    rc, out, err = run(["make", "-s", "qnx", "qnx-probes"], env=env, timeout=900)
    if rc != 0:
        raise BenchError("make qnx/qnx-probes rc=%d\n%s" % (rc, (out + err)[-2000:]))
    if verbose:
        print(out[-400:])
    return rc


def compile_policy(env=None):
    """`secpolcompile` : l'acceptance test de image/ambre.sec (SEC-01..06).

    Avant la SDP, ce fichier portait la mention UNVERIFIED : le compilateur est
    la seule preuve possible de sa grammaire. rc=0 veut dire « politique
    compilable », rien de plus — l'application est démontrée par secpol_probe.
    """
    env = env or sdp_env()
    out = os.path.join(_ROOT, "build", "qnx", "secpol.bin")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    rc, _, err = run(["secpolcompile", "-o", out, "image/ambre.sec"], env=env, timeout=180)
    if rc != 0:
        raise BenchError("secpolcompile rc=%d : %s" % (rc, err[-1500:]))
    return out
