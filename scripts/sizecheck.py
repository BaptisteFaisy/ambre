#!/usr/bin/env python3
"""sizecheck.py — house-rule gate: files < 500 lines, functions < 50 lines.

Usage: python3 scripts/sizecheck.py [root ...]
Exit code 0 when every rule holds, 1 otherwise.
"""

import re
import sys
from pathlib import Path

MAX_FILE_LINES = 500
MAX_FUNC_LINES = 50
EXTS = {".c", ".h", ".py"}
SKIP_DIRS = {".git", "build", "__pycache__", ".venv"}


def iter_sources(roots):
    for root in roots:
        path = Path(root)
        if path.is_file():
            yield path
            continue
        for p in sorted(path.rglob("*")):
            if not p.is_file() or p.suffix not in EXTS:
                continue
            if any(part in SKIP_DIRS for part in p.parts):
                continue
            yield p


def python_functions(text):
    """(name, start_line, end_line) for every top-level and nested def."""
    lines = text.splitlines()
    starts = []
    for i, line in enumerate(lines):
        m = re.match(r"^(\s*)def\s+(\w+)", line)
        if m:
            starts.append((i, m.group(2)))
    for idx, (start, name) in enumerate(starts):
        end = len(lines)
        base_indent = len(lines[start]) - len(lines[start].lstrip())
        for j in range(start + 1, len(lines)):
            line = lines[j]
            if not line.strip():
                continue
            indent = len(line) - len(line.lstrip())
            if indent <= base_indent and not line.lstrip().startswith("#"):
                end = j
                break
        else:
            end = len(lines)
        yield name, start + 1, end, end - start


def c_functions(text):
    """(name, start_line, end_line) for every top-level C function definition.

    A signature that ends with ';' before any '{' is a prototype, not a
    definition, and is skipped.
    """
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if re.match(r"^[A-Za-z_][\w \t\*]*\s+\**[A-Za-z_]\w*\s*\([^;]*$", line):
            name = re.search(r"([A-Za-z_]\w*)\s*\(", line)
            name = name.group(1) if name else "?"
            start = i
            depth = 0
            seen = False
            j = i
            while j < len(lines):
                if ";" in lines[j] and not seen:
                    break  # prototype, not a definition
                depth += lines[j].count("{") - lines[j].count("}")
                if "{" in lines[j]:
                    seen = True
                if seen and depth == 0:
                    break
                j += 1
            if seen:
                yield name, start + 1, j + 1, j - start + 1
                i = j + 1
                continue
            i = j + 1
            continue
        i += 1


def main(argv):
    roots = argv[1:] or ["fmt", "host", "tests", "scripts"]
    violations = []
    files = 0
    for path in iter_sources(roots):
        files += 1
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"unreadable {path}: {exc}")
            violations.append((str(path), 0, "unreadable"))
            continue
        n_lines = len(text.splitlines())
        if n_lines >= MAX_FILE_LINES:
            violations.append((str(path), 0, f"file has {n_lines} lines (max {MAX_FILE_LINES - 1})"))
        funcs = python_functions(text) if path.suffix == ".py" else c_functions(text)
        for name, start, end, length in funcs:
            if length > MAX_FUNC_LINES:
                violations.append(
                    (str(path), start, f"function '{name}' spans {length} lines (max {MAX_FUNC_LINES})")
                )
    for path, line, msg in violations:
        where = f"{path}:{line}" if line else path
        print(f"VIOLATION {where}: {msg}")
    print(f"sizecheck: {files} files scanned, {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
