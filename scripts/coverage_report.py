#!/usr/bin/env python3
"""Line coverage report for the Ambre portable format library (spec 12.10).

The CI image has `gcov` but neither `lcov` nor `gcovr`, so this script reads
the `.gcov` annotation files that gcov produces and turns them into a report:

  * total executable / covered lines for fmt/ (the critical library);
  * one line per source file;
  * every uncovered line, with the source text of that line;
  * a JSON report for the other tools.

The `.gcov` format is `count:line_number:source_text`, where `count` is

  * a decimal number  -> the line was executed that many times;
  * `#####`           -> the line is executable but was never executed;
  * `=====`           -> the line is not executable (skipped block);
  * `-`               -> the line carries no code (comment, declaration).

Usage:
    python3 scripts/coverage_report.py --gcov-dir build-cov/gcov \
            [--source-dir fmt] [--out-json build-cov/coverage.json] \
            [--min 95.0] [--list-uncovered]
"""

import argparse
import json
import os
import sys

LINE_UNCOVERED = "#####"
LINE_NOT_CODE = "-"
LINE_SKIPPED = "====="


class FileCoverage(object):
    """Coverage of one C source file, line by line."""

    def __init__(self, source):
        self.source = source
        self.executable = 0
        self.covered = 0
        self.uncovered = []          # [(line_number, source_text)]

    @property
    def percent(self):
        if self.executable == 0:
            return 0.0
        return 100.0 * self.covered / self.executable

    def as_dict(self):
        return {
            "source": self.source,
            "executable_lines": self.executable,
            "covered_lines": self.covered,
            "uncovered_lines": len(self.uncovered),
            "coverage_percent": round(self.percent, 2),
            "uncovered": [{"line": number, "text": text}
                          for number, text in self.uncovered],
        }


def parse_gcov(path):
    """Yield (line_number, count_text, source_text) of one .gcov file."""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            parts = raw.split(":", 2)
            if len(parts) < 3:
                continue
            count, number, text = parts[0].strip(), parts[1].strip(), parts[2]
            if not number.isdigit():
                continue
            yield int(number), count, text.rstrip("\n")


def count_field(count_text):
    """Return the numeric execution count, or None when the line has no code."""
    if count_text == LINE_NOT_CODE:
        return None
    cleaned = count_text.rstrip("*")
    if cleaned == LINE_UNCOVERED:
        return 0
    if cleaned == LINE_SKIPPED:
        return None
    try:
        return int(cleaned)
    except ValueError:
        return None


def analyse_file(gcov_path, source_name):
    """Build the FileCoverage of one .gcov file."""
    coverage = FileCoverage(source_name)
    for number, count_text, text in parse_gcov(gcov_path):
        if number == 0:
            continue
        count = count_field(count_text)
        if count is None:
            continue
        coverage.executable += 1
        if count > 0:
            coverage.covered += 1
        else:
            coverage.uncovered.append((number, text.strip()))
    return coverage


def collect(gcov_dir, source_dir):
    """Analyse every .gcov file of `gcov_dir` and return the list."""
    results = []
    for name in sorted(os.listdir(gcov_dir)):
        if not name.endswith(".gcov"):
            continue
        path = os.path.join(gcov_dir, name)
        source = os.path.join(source_dir, name[:-len(".gcov")])
        results.append(analyse_file(path, source))
    return results


def source_line(source_dir, source_name, number):
    """Return the text of one line of a source file (best effort)."""
    path = os.path.join(source_dir, os.path.basename(source_name))
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for index, raw in enumerate(handle, start=1):
                if index == number:
                    return raw.strip()
    except OSError:
        pass
    return ""


def print_report(results, show_uncovered, source_dir):
    """Print the human readable summary."""
    total_exec = sum(item.executable for item in results)
    total_cov = sum(item.covered for item in results)
    print("")
    print("  Line coverage of the critical library (fmt/)")
    print("  %-34s %8s %8s %8s" % ("file", "covered", "lines", "percent"))
    print("  " + "-" * 60)
    for item in results:
        print("  %-34s %8d %8d %7.2f%%"
              % (os.path.basename(item.source), item.covered, item.executable,
                 item.percent))
    print("  " + "-" * 60)
    print("  %-34s %8d %8d %7.2f%%"
          % ("TOTAL", total_cov, total_exec,
             (100.0 * total_cov / total_exec) if total_exec else 0.0))
    if show_uncovered:
        print("")
        print("  Uncovered lines")
        for item in results:
            for number, text in item.uncovered:
                extra = source_line(source_dir, item.source, number)
                print("    %-30s %4d  %s" % (os.path.basename(item.source),
                                             number, extra or text))
    return total_exec, total_cov


def build_json(results, total_exec, total_cov, gcov_dir):
    """Build the JSON report."""
    return {
        "tool": "scripts/coverage_report.py",
        "gcov_dir": gcov_dir,
        "scope": "fmt/",
        "executable_lines": total_exec,
        "covered_lines": total_cov,
        "coverage_percent": round(100.0 * total_cov / total_exec, 2) if total_exec else 0.0,
        "files": [item.as_dict() for item in results],
    }


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcov-dir", default="build-cov/gcov",
                        help="directory holding the .gcov annotation files")
    parser.add_argument("--source-dir", default="fmt",
                        help="directory holding the instrumented sources")
    parser.add_argument("--out-json", default=None,
                        help="write the JSON report to this file")
    parser.add_argument("--min", type=float, default=0.0,
                        help="fail when the total coverage is below this percent")
    parser.add_argument("--list-uncovered", action="store_true",
                        help="print every uncovered line with its source text")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.gcov_dir):
        print("coverage: no gcov directory %s" % args.gcov_dir, file=sys.stderr)
        return 2
    results = collect(args.gcov_dir, args.source_dir)
    if not results:
        print("coverage: no .gcov file in %s" % args.gcov_dir, file=sys.stderr)
        return 2
    total_exec, total_cov = print_report(results, args.list_uncovered,
                                         args.source_dir)
    percent = (100.0 * total_cov / total_exec) if total_exec else 0.0
    if args.out_json:
        report = build_json(results, total_exec, total_cov, args.gcov_dir)
        directory = os.path.dirname(os.path.abspath(args.out_json))
        if directory and not os.path.isdir(directory):
            os.makedirs(directory)
        with open(args.out_json, "w", encoding="utf-8") as handle:
            json.dump(report, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print("  JSON report: %s" % args.out_json)
    if args.min > 0.0 and percent < args.min:
        print("  FAIL: coverage %.2f%% is below the %.2f%% target"
              % (percent, args.min), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
