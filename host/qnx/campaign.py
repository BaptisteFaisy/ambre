#!/usr/bin/env python3
"""campaign.py — la campagne de coupures d'alimentation sur QNX 8.0 RÉEL.

Niveau 2 du cahier des charges (§7.4, docs/VALIDATION.md §1) : la chaîne
complète tourne dans une machine virtuelle QNX 8.0, et la coupure est un
SIGKILL sur QEMU — le 12 V est parti, ce que le guest n'avoit pas encore fait
passer par le pilote de bloc est perdu, rien de plus. La vitrine live fait la
même chose en boucle, mais sans rapport : ce module en fait une campagne
auditable, avec un verdict PAR cycle, un décalage de coupure choisi par rapport
à l'acquisition, et un cumul vérifiable.

Ce qui change par rapport au simulateur PC (`host/powercut/`) : ici le média
est un vrai disque brut vu par un vrai pilote de bloc QNX, et la reprise après
coupure est le VRAI chemin de récupération (`REP-01`) exécuté par edr-journal
au démarrage suivant. Les invariants I1..I8 sont jugés sur ce que le journal
REND après la coupure, pas sur une trace d'opérations simulée.

Un cycle :
  1. média neuf (partition brute type 177, aucun fichier système — STO-01)
  2. amorce, edr-journal ouvre la partition et court REP-01
  3. edr-capture + edr-replay : 4 s de conduite, choc frontal à 2,5 s
  4. le devctl du journal annonce l'événement ACQUIS (id, trig_ts_us, seq_emerg)
  5. coupure à `cut_ms` APRÈS cette annonce (SIGKILL sur QEMU)
  6. nouvelle amorce : edr-journal seul, REP-01 reconstruit la table
  7. le même devctl rend ce qui survit ; le verdict compare à l'étape 4

Verdicts (§7.2, I1..I8) :
  ok                 l'événement acquis est là, identique (id, trig_ts, seq)
  data_loss          il a disparu, ou il est là mais incomplet/non verrouillé
  phantom_event      un événement NON acquis apparaît (I2 : rien d'inventé)
  inconsistent_state les compteurs du journal contredisent la liste (I7)
  error              le banc n'a pas répondu : cycle non concluant, compté à part

Réutilisation, pas de duplication : la machine vient de host/qnx/bench.py, les
médias de host/qnx/media.py, les helpers de lecture/statistiques de
host/powercut/vm_util.py. Ce module n'ajoute que le jugement.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
sys.path.insert(0, os.path.join(_ROOT, "host", "powercut"))
sys.path.insert(0, _HERE)

from vm_util import stats, wait_for  # noqa: E402

from bench import Target, BEGIN, END  # noqa: E402
from media import make_journal_disk, run  # noqa: E402
from sdp import sdp_env  # noqa: E402

JOURNAL = "/dev/hd0t177"
SERVICE = "/dev/edr/journal"
NODE = "/dev/edr"
CTL = "/proc/boot/devctl_probe"
REPLAY = "/proc/boot/edr-replay"
CAPTURE = "/proc/boot/edr-capture"
JOURNALD = "/proc/boot/edr-journal"
SCENARIO = "/proc/boot/scenario.txt"
# Le service rend le statut à un client du SERVICE (gid 4) et le refuse à un
# producteur (gid 3) : c'est la matrice d'accès SEC-01, mesurée sur la cible.
SVC = "on -u 0:4 "


def guest(target, line, timeout_s=30.0):
    """Une commande dans le guest, sans exception : (rc, sortie, err)."""
    try:
        return target.cmd(line, timeout_s)
    except Exception as exc:                       # socket mort, shell muet
        return 125, "", str(exc)


def events(target, timeout_s=30.0):
    """Les événements que le journal REND par devctl, sous forme de dict."""
    rc, out, _ = guest(target, "%s %s list" % (SVC, CTL), timeout_s)
    if rc != 0:
        return None
    parsed = []
    for line in out.splitlines():
        if not line.startswith("EVENT "):
            continue
        row = {}
        for field in line.split()[1:]:
            key, _, value = field.partition("=")
            row[key] = value
        parsed.append(row)
    return parsed


def status(target, timeout_s=30.0):
    """Le statut du journal (devctl), champ=valeur ; None s'il ne répond pas."""
    rc, out, _ = guest(target, "%s %s status" % (SVC, CTL), timeout_s)
    if rc != 0:
        return None
    row = {}
    for line in out.splitlines():
        if line.startswith("STATUS ") and not line.startswith("STATUS raw="):
            for field in line.split()[1:]:
                key, _, value = field.partition("=")
                row[key] = value
    return row


def start_recorder(target, drive=True):
    """Amorce le service (puis la chaîne de conduite si `drive`)."""
    guest(target, "%s --disk %s --listen %s --mode continuous "
          "--segment-limit 4 &" % (JOURNALD, JOURNAL, SERVICE), 15.0)
    if not wait_for(lambda: status(target, 8.0) is not None, 60.0, 1.0,
                    "journal prêt"):
        return False
    if not drive:
        return True
    guest(target, "%s --journal %s --listen /dev/edr/in --profile eu &"
          % (CAPTURE, SERVICE), 15.0)
    time.sleep(2.0)
    guest(target, "%s --capture /dev/edr/in --scenario %s" % (REPLAY, SCENARIO),
          60.0)
    return wait_for(lambda: bool(events(target, 10.0)), 40.0, 0.5, "événement acquis")


def fingerprint(rows):
    """Ce qui identifie un événement acquis : id, horodatage du choc, séquence.
    Deux cycles comparables se jugent là-dessus, pas sur l'ordre de la liste."""
    return sorted((r.get("id", ""), r.get("trig_ts_us", ""), r.get("seq_emerg", ""))
                  for r in rows or [])


def judge(before, after, st):
    """Le verdict d'un cycle, §7.2 (I1..I8) appliqué à ce que le journal rend."""
    b, a = fingerprint(before), fingerprint(after)
    if a is None or b is None:
        return "error", "le journal n'a pas répondu"
    if not b:
        return "error", "aucun événement acquis avant la coupure"
    missing = [x for x in b if x not in a]
    extra = [x for x in a if x not in b]
    if extra and not missing:
        return "phantom_event", "non acquis avant coupure: %s" % extra
    if missing:
        return "data_loss", "acquis avant coupure, perdu: %s" % missing
    # I3/I4 : un événement verrouillé survit VERROUILLÉ et COMPLET. Les deux
    # drapeaux voyagent avec la ligne d'événement (locked, complete), pas avec
    # les compteurs : `sealed_events` (fmt/ambre_journal_event.c:173) n'est
    # incrémenté QUE par ambre_journal_seal(), c'est un compteur DE SESSION —
    # comparer ce compteur au contenu de la table reconstruite par REP-01
    # signalait à tort un I7 en défaut sur chaque cycle (mesuré 2026-11-14 :
    # `sealed=0` avec 8 événements scellés rendus par le journal).
    bad = [r.get("id") for r in (after or [])
           if r.get("complete") != "1" or r.get("locked") != "1"]
    if bad:
        return "data_loss", "événement là mais incomplet/non verrouillé: %s" % bad
    # I7 : rien d'OUVERT ne traîne après une coupure — un événement acquis avant
    # la coupure est scellé par la récupération, pas relancé.
    open_rows = [r.get("id") for r in (after or []) if r.get("state") != "sealed"]
    if open_rows:
        return "inconsistent_state", "ouvert après récupération: %s" % open_rows
    if st and st.get("open") is not None and int(st.get("open", 0)) != 0:
        return "inconsistent_state", "le statut annonce %s événement(s) ouvert(s) "
        "après récupération" % st.get("open")
    return "ok", "%d événement(s) survécu(s), identiques et scellés" % len(a)


def acquire(target):
    """La phase « conduite » : le guest acquiert l'événement, et on note ce
    que le journal REND (c'est la référence du verdict)."""
    if not start_recorder(target, drive=True):
        return None, None
    return events(target), status(target)


def cut_at(target, uptime_ms, cut_ms):
    """Place la coupure `cut_ms` après l'annonce, sur l'horloge DU GUEST.

    C'est l'horloge du journal qui a un sens pour juger « le guest avait-il déjà
    remis l'écriture au pilote de bloc ? » : l'horloge murale de l'hôte, elle,
    ne dit rien du guest.
    """
    target_uptime = uptime_ms + cut_ms

    def reached():
        st = status(target, timeout_s=8.0)
        return st is not None and int(st.get("uptime_ms", 0)) >= target_uptime

    if cut_ms:
        wait_for(reached, float(cut_ms) / 500.0 + 10.0, 0.05, "decalage de coupure")


def recover(target):
    """La phase « reprise » : le journal repart et rend ce qui survit."""
    if not start_recorder(target, drive=False):
        return None, None
    return events(target), status(target)


def one_cycle(workdir, cut_ms, archive=False, timeout_s=200.0):
    """Un cycle complet : acquisition, coupure à `cut_ms`, reprise, verdict."""
    started = time.monotonic()
    make_journal_disk(os.path.join(workdir, "journal.raw"))
    rec = {"cut_ms": cut_ms, "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ")}
    with Target(workdir, archive=archive) as tgt:
        try:
            tgt.boot(timeout_s=120.0)
        except Exception as exc:
            rec.update(verdict="error", why="amorce: %s" % exc)
            rec["seconds"] = round(time.monotonic() - started, 2)
            return rec
        before, st_before = acquire(tgt)
        if before is None:
            rec.update(verdict="error", why="chaine de conduite non acquise")
            rec["seconds"] = round(time.monotonic() - started, 2)
            return rec
        rec["acquired"] = len(before)
        rec["uptime_at_cut_ms"] = int((st_before or {}).get("uptime_ms", 0))
        cut_at(tgt, rec["uptime_at_cut_ms"], cut_ms)
        cut_at_wall = time.monotonic()
        tgt.power_cut()
        rec["cut_s"] = round(cut_at_wall - started, 2)
        try:
            tgt.boot(timeout_s=120.0)
        except Exception as exc:
            rec.update(verdict="error", why="re-amorce: %s" % exc)
            rec["seconds"] = round(time.monotonic() - started, 2)
            return rec
        after, st = recover(tgt)
        verdict, why = judge(before, after, st)
        rec.update(verdict=verdict, why=why, survived=len(after or []))
    rec["seconds"] = round(time.monotonic() - started, 2)
    return rec


# ------------------------------------------------------------------ rapport --

def write_report(path, records, meta):
    """Le rapport cumulatif : un cycle par ligne, plus les agrégats.

    Il est ÉCRIT À CHAQUE CYCLE (et non à la fin) : une campagne interrompue
    laisse un rapport exploitable, ce qui est le seul moyen de distinguer
    « la campagne a échoué » de « la campagne n'a pas tourné ».
    """
    verdicts = {}
    for rec in records:
        verdicts[rec.get("verdict", "error")] = verdicts.get(rec.get("verdict", "error"), 0) + 1
    cuts = [rec["cut_s"] for rec in records if "cut_s" in rec]
    report = dict(meta)
    report["cycles"] = len(records)
    report["verdicts"] = verdicts
    if cuts:
        report["cycle_seconds"] = stats(cuts)
    report["records"] = records
    tmp = path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(report, fh, indent=2, sort_keys=True)
    os.replace(tmp, path)
    return report


def main(argv=None):
    """CLI : `--cycles N --cut-ms 0,10,20 --report fichier.json`."""
    ap = argparse.ArgumentParser(prog="campaign.py",
                                 description="campagne de coupures QNX 8.0 (niveau 2)")
    ap.add_argument("--cycles", type=int, default=10)
    ap.add_argument("--cut-ms", default="0",
                    help="décalages de coupure après l'acquisition, en ms (liste)")
    ap.add_argument("--workdir", default=os.path.join(_ROOT, "build", "qnx-campaign"))
    ap.add_argument("--report", default=None)
    ap.add_argument("--no-archive", action="store_true")
    a = ap.parse_args(argv)
    offsets = [int(x) for x in a.cut_ms.replace(" ", "").split(",") if x != ""]
    if not offsets:
        offsets = [0]
    os.makedirs(a.workdir, exist_ok=True)
    from image import prepare
    prepare(a.workdir, archive=not a.no_archive, secpol=False, task=None,
            verbose=False)
    path = a.report or os.path.join(a.workdir, "campaign.json")
    records = []
    meta = {"tool": "qnx-campaign", "backend": "qemu (QNX 8.0 réel)",
            "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "cut_offsets_ms": offsets}
    print("campagne : %d cycle(s), décalages %s ms" % (a.cycles, offsets))
    for i in range(a.cycles):
        cut_ms = offsets[i % len(offsets)]
        rec = one_cycle(a.workdir, cut_ms, archive=not a.no_archive)
        rec["index"] = i
        records.append(rec)
        rep = write_report(path, records, meta)
        print("cycle %3d/%d  cut+%3dms  %-18s %s"
              % (i + 1, a.cycles, cut_ms, rec.get("verdict"), rec.get("why", "")))
        print("   → %s (%s cycles)" % (path, rep["verdicts"]))
    rep = write_report(path, records, meta)
    print("campagne terminée : %s" % rep["verdicts"])
    return 0 if rep["verdicts"].get("error", 0) == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
