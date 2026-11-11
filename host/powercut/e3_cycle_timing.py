"""Essay E3 — kill the machine, restart it, find the service again: how long?

Question (spec section 10, E3): *can QEMU be killed and restarted in a loop,
without intervention, in less than ten seconds per cycle?*  If not: shrink the
boot image and run several machines in parallel.

One cycle is measured as: **power cut (SIGKILL) → restart → guest answers →
service reachable**.  The service probe runs a guest command (``edrctl list
--json`` by default) after the machine is up, which is what "finding the
service again" means for the campaign.

Runs against the real QEMU backend or the dry-run fake VM, so the harness is
testable today without a QNX 8.0 image.

Usage::

    python3 e3_cycle_timing.py --backend dry-run --cycles 5
    python3 e3_cycle_timing.py --image disk.img --cycles 50 --report e3.json

English only, Python 3 standard library only.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import qemu_vm  # noqa: E402

#: Target of essay E3: one full cycle must stay under this many seconds.
CYCLE_TARGET_S = 10.0


def measure_cycle(vm, list_cmd, guest_timeout):
    """Cut, restart, wait for the service; return the phase timings."""
    cut_s = vm.power_cut()
    boot_s = vm.restart()
    started = time.monotonic()
    result = vm.run_guest(list_cmd, timeout_s=guest_timeout)
    service_s = time.monotonic() - started
    return {"cut_s": cut_s, "boot_s": boot_s, "service_s": service_s,
            "total_s": cut_s + boot_s + service_s, "service_ok": result.ok}


def run_cycles(args):
    """Run ``args.cycles`` measured cycles and return the per-cycle records."""
    records = []
    with qemu_vm.QemuVM(image=args.image, backend=args.backend,
                        boot_timeout_s=args.boot_timeout, mem_mb=args.mem,
                        cpus=args.cpus, workdir=args.workdir,
                        keep_workdir=args.keep_workdir) as vm:
        vm.start()
        for index in range(args.cycles):
            record = measure_cycle(vm, args.list_cmd, args.guest_timeout)
            record["cycle"] = index
            records.append(record)
            if args.verbose:
                print("  cycle %d: cut %.3fs + boot %.3fs + service %.3fs = %.3fs"
                      % (index, record["cut_s"], record["boot_s"],
                         record["service_s"], record["total_s"]))
    return records


def summarise(records):
    """Aggregate the cycle timings and answer the E3 question."""
    totals = [record["total_s"] for record in records]
    summary = {
        "cycles": len(records),
        "total_s": qemu_vm.stats(totals),
        "cut_s": qemu_vm.stats([record["cut_s"] for record in records]),
        "boot_s": qemu_vm.stats([record["boot_s"] for record in records]),
        "service_s": qemu_vm.stats([record["service_s"] for record in records]),
        "service_failures": sum(1 for record in records if not record["service_ok"]),
        "target_s": CYCLE_TARGET_S,
    }
    summary["under_target"] = bool(totals) and summary["total_s"]["max"] < CYCLE_TARGET_S
    summary["answer"] = ("yes: every cycle is below %.0f s" % CYCLE_TARGET_S
                         if summary["under_target"] else
                         "no: at least one cycle exceeds %.0f s" % CYCLE_TARGET_S)
    return summary


def build_parser():
    parser = argparse.ArgumentParser(description="Essay E3: kill/restart cycle time")
    parser.add_argument("--image", default=None, help="guest disk image")
    parser.add_argument("--backend", choices=("qemu", "dry-run"), default="qemu")
    parser.add_argument("--cycles", type=int, default=5)
    parser.add_argument("--mem", type=int, default=512)
    parser.add_argument("--cpus", type=int, default=2)
    parser.add_argument("--boot-timeout", type=float,
                        default=qemu_vm.DEFAULT_BOOT_TIMEOUT_S)
    parser.add_argument("--guest-timeout", type=float, default=30.0)
    parser.add_argument("--list-cmd", default=qemu_vm.DEFAULT_LIST_CMD,
                        help="guest command proving the service answers")
    parser.add_argument("--workdir", default=None)
    parser.add_argument("--keep-workdir", action="store_true")
    parser.add_argument("--report", default=None, help="write the result as JSON")
    parser.add_argument("--verbose", action="store_true")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.backend == "qemu" and not args.image:
        raise SystemExit("--image is required with --backend qemu")
    args.workdir = args.workdir or tempfile.mkdtemp(prefix="ambre-e3-")
    records = run_cycles(args)
    summary = summarise(records)
    if args.report:
        with open(args.report, "w") as handle:
            json.dump({"tool": "e3_cycle_timing", "backend": args.backend,
                       "summary": summary, "cycles": records},
                      handle, indent=2, sort_keys=True)
            handle.write("\n")
    print("E3 cycle timing — backend=%s, %d cycle(s)" % (args.backend, len(records)))
    for phase in ("cut_s", "boot_s", "service_s", "total_s"):
        stats = summary[phase]
        if stats.get("count"):
            print("  %-10s min %.3f  median %.3f  p99 %.3f  max %.3f  mean %.3f"
                  % (phase, stats["min"], stats["median"], stats["p99"],
                     stats["max"], stats["mean"]))
    print("  service failures: %d" % summary["service_failures"])
    print(qemu_vm.histogram_text([record["total_s"] for record in records],
                                 "cycle time histogram"), end="")  # shared helper
    print("  under %.0f s per cycle? %s" % (CYCLE_TARGET_S, summary["answer"]))
    ok = summary["under_target"] and summary["service_failures"] == 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
