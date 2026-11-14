#!/usr/bin/env python3
"""bench.py — la MACHINE du banc QNX 8.0 réel : démarrer, commander, couper.

Rien ici ne construit l'image (c'est image.py) ni ne compile la SDP (sdp.py) ni
ne façonne les médias (media.py) : ce module ne fait que faire tourner la
machine, piloter sa console et la couper. Trois consommateurs ont besoin
EXACTEMENT de la même machine — la vitrine live (scripts/qnx-vitrine.sh), la
campagne de coupures §7.4 (host/qnx/campaign.py) et le banc interactif
(scripts/qnx-bench.sh) — d'où un seul implémentation, importée, jamais copiée.

Console : le noyau occupe le premier port 8250 (0x3f8, `-D8250` du buildfile),
le bench utilise le second (0x2f8 = /dev/ser2). `PS1` y est vidé et chaque
commande est encadrée par AMBRE-BEGIN / AMBRE-END, donc le lecteur n'a aucun
prompt à deviner — le piège habituel d'un shell amorcé sans /etc/passwd.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(os.path.dirname(_HERE))
sys.path.insert(0, os.path.join(_ROOT, "host", "powercut"))

from vm_util import SerialConsole, wait_for  # noqa: E402

from media import BenchError, run  # noqa: E402
from image import prepare  # noqa: E402

QEMU = os.environ.get("QEMU_BINARY", "qemu-system-x86_64")
BEGIN, END = "AMBRE-BEGIN", "AMBRE-END"
class Target:
    """Une machine QNX 8.0 : démarrée, commandée, coupée, jamais orpheline."""

    def __init__(self, workdir, archive=True, vga=True, mem_mb=1024, cpus=2):
        self.workdir = workdir
        self.archive = archive
        self.vga = vga
        self.mem_mb, self.cpus = mem_mb, cpus
        self.ifs = os.path.join(workdir, "ifs.bin")
        self.journal = os.path.join(workdir, "journal.raw")
        self.shell_sock = os.path.join(workdir, "shell.sock")
        self.console_sock = os.path.join(workdir, "console.sock")
        self.mon = os.path.join(workdir, "mon.sock")
        self.log = os.path.join(workdir, "console.log")
        self.proc, self.console = None, None
        self.boot_s = 0.0

    def qemu_argv(self):
        """La ligne QEMU éprouvée : KVM, IDE cache=none, deux ports série."""
        argv = [QEMU, "-enable-kvm", "-cpu", "host", "-smp", str(self.cpus),
                "-m", str(self.mem_mb), "-kernel", self.ifs,
                "-drive", "file=%s,format=raw,if=ide,cache=none,id=drv0" % self.journal]
        if self.archive and os.path.exists(os.path.join(self.workdir, "archive.raw")):
            argv += ["-drive", "file=%s,format=raw,if=ide,cache=none,id=drv1"
                     % os.path.join(self.workdir, "archive.raw")]
        argv += ["-object", "rng-random,filename=/dev/urandom,id=rng0",
                 "-device", "virtio-rng-pci,rng=rng0",
                 "-display", "none", "-vga", "std" if self.vga else "none",
                 # Le port du NOYAU (0x3f8) est écrit dans un fichier : un
                 # chardev socket côté hôte ne met rien en tampon, donc la
                 # sortie d'amorce est perdue si le client se connecte après
                 # — et on se retrouve avec un banc « muet » sans aucune trace.
                 "-chardev", "file,id=s0,path=%s,append=on" % self.log,
                 "-serial", "chardev:s0",
                 # Le port du BENCH (0x2f8 = /dev/ser2) est un socket : bidirectionnel,
                 # l'hôte y envoie les commandes et relit la sortie.
                 "-chardev", "socket,id=s1,path=%s,server=on,wait=off" % self.shell_sock,
                 "-serial", "chardev:s1",
                 "-monitor", "unix:%s,server,nowait" % self.mon,
                 "-pidfile", os.path.join(self.workdir, "qemu.pid")]
        return argv

    def boot(self, timeout_s=90.0):
        """Démarre la machine et attend que le shell de bench réponde."""
        for stale in (self.mon, self.shell_sock, self.console_sock):
            try:
                os.unlink(stale)
            except OSError:
                pass
        started = time.monotonic()
        with open(os.path.join(self.workdir, "qemu.log"), "a") as err:
            self.proc = subprocess.Popen(self.qemu_argv(), stdout=err, stderr=err)
        self.boot_s = time.monotonic() - started
        if not self.wait_shell(timeout_s):
            raise BenchError("shell de bench muet après %.0fs (rc=%s, vois %s)"
                             % (timeout_s, self.proc.poll(), os.path.join(self.workdir, "qemu.log")))
        return self.boot_s

    def wait_shell(self, timeout_s=90.0):
        """Le shell répond-il ? (PS1 vidé, donc cadres AMBRE-BEGIN/END.)"""
        def attempt():
            if self.console is None:
                self.console = SerialConsole(self.shell_sock, prompt=END, strict_prompt=True)
                if not self.console.connect(min(timeout_s, 10.0)):
                    return False
                self.cmd("PS1=''", timeout_s=5.0)
            return self.cmd("echo BENCH-OK", timeout_s=8.0)[0] == 0
        try:
            return wait_for(attempt, timeout_s, 0.5, "shell de bench")
        except TimeoutError:
            # wait_for lève : boot() doit rendre BenchError avec le chemin du
            # log, pas un traceback — c'est là que se lit un échec d'amorce.
            return False

    def cmd(self, line, timeout_s=30.0):
        """Une commande dans le guest ; rend (rc, sortie propre, stderr)."""
        if self.console is None and not self.wait_shell(timeout_s):
            return 125, "", "console absente"
        # Le `&` final d'une mise en arrière-plan ne peut pas être suivi de `;`
        # (ksh : « syntax error: `;' unexpected ») : la graine est donc emballée
        # sans point-virgule dans ce cas-là.
        body = "{ %s }" % line if line.rstrip().endswith("&") else "{ %s; }" % line
        payload = "echo %s; %s 2>&1; echo RC=$?; echo %s" % (BEGIN, body, END)
        res = self.console.run(payload, timeout_s)
        return self._parse(res.stdout, res)

    @staticmethod
    def _parse(raw, res):
        """Jette l'écho de la commande, lit RC= et la vraie sortie du guest.

        Le shell renvoie la ligne tapée telle quelle : sans ce tri, la sortie
        d'un appel contiendrait la commande elle-même, et un script hôte ne
        pourrait rien en tirer de stable. 125 = le guest n'a pas rendu RC=, donc
        rien de ce qu'il dit n'est exploitable.
        """
        out, rc, echoing = [], None, True
        for line in (raw or "").replace("\r\n", "\n").split("\n"):
            stripped = line.strip()
            if echoing and BEGIN in stripped and END in stripped:
                echoing = False           # this line is the echo: drop it
                continue
            if stripped == BEGIN:
                continue            # l'affichage du marqueur d'ouverture
            if stripped == END:
                break
            if stripped.startswith("RC="):
                rc = stripped[3:].strip()
                continue
            out.append(line)
        if rc is None:
            return (0 if res.ok else 125), "\n".join(out).strip(), res.stderr
        return int(rc), "\n".join(out).strip(), res.stderr

    def power_cut(self):
        """La coupure d'alimentation : SIGKILL sur QEMU, le 12 V est parti."""
        started = time.monotonic()
        if self.proc is not None:
            try:
                self.proc.kill()
                self.proc.wait(timeout=10)
            except (OSError, subprocess.TimeoutExpired):
                pass
        if os.path.exists(os.path.join(self.workdir, "qemu.pid")):
            try:
                os.unlink(os.path.join(self.workdir, "qemu.pid"))
            except OSError:
                pass
        if self.console is not None:
            self.console.close()
            self.console = None
        self.proc = None
        return time.monotonic() - started

    def kill_stale(self):
        """Tue tout QEMU qui tiendrait encore ce journal (verrou d'écriture)."""
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open("/proc/%s/cmdline" % entry, "rb") as fh:
                    cmd = fh.read().decode("utf-8", "replace")
            except OSError:
                continue
            if "qemu-system" in cmd and self.journal in cmd:
                # SIGKILL, pas terminate : c'est la sémantique même de la coupure
                # (le 12 V ne se coupe pas poliment) et cela libère le verrou
                # d'écriture du disque tant que la prochaine amorce attend.
                try:
                    os.kill(int(entry), signal.SIGKILL)
                except OSError:
                    pass  # déjà mort entre le répertoire et le signal
        for sock in (self.mon, self.shell_sock, self.console_sock):
            try:
                os.unlink(sock)
            except OSError:
                pass

    def close(self):
        """Rien d'orphelin : la machine est éteinte, les sockets effacés."""
        if self.proc is not None:
            self.power_cut()
        self.kill_stale()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        self.close()
        return False


def repl(target, timeout_s=30.0):
    """Lit des commandes sur stdin, les exécute dans le guest, rends le rc."""
    worst = 0
    for line in sys.stdin:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        rc, out, err = target.cmd(line, timeout_s)
        if out:
            print(out)
        if err:
            print(err, file=sys.stderr)
        print("BENCH-RC %d" % rc)
        worst = max(worst, 0 if rc == 0 else 1)
    return worst


def main(argv=None):
    """CLI du banc : préparer l'image puis la piloter depuis stdin."""
    ap = argparse.ArgumentParser(prog="bench.py", description="banc QNX 8.0 réel")
    ap.add_argument("what", choices=("prepare", "repl"))
    ap.add_argument("--workdir", default=os.path.join(_ROOT, "build", "qnx-bench"))
    ap.add_argument("--no-archive", action="store_true")
    ap.add_argument("--secpol", action="store_true",
                    help="compile/push la politique QNX (non démontrée à l'exécution)")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--demo", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)
    task = ("on -d -t /dev/ser2 /proc/boot/demo-qnx.sh" if a.demo else
            "on -d -t /dev/ser2 -e HOME=/proc/boot /proc/boot/ksh")
    archive = not a.no_archive
    if a.what == "prepare":
        from image import prepare
        info = prepare(a.workdir, archive=archive, secpol=a.secpol, task=task,
                       verbose=not a.quiet)
        print("prepared %s (archive=%s secpol=%s)" % (info["ifs"], archive, a.secpol))
        return 0
    from image import prepare
    if not os.path.exists(os.path.join(a.workdir, "ifs.bin")):
        prepare(a.workdir, archive=archive, secpol=a.secpol, task=task,
                verbose=not a.quiet)
    with Target(a.workdir, archive=archive) as target:
        boot_s = target.boot(timeout_s=120.0)
        if not a.quiet:
            print("bench prêt en %.1fs" % boot_s)
        return repl(target, timeout_s=a.timeout)


if __name__ == "__main__":
    sys.exit(main())


