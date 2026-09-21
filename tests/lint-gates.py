#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# lint-gates.py — static checks over the gate scripts, so the 8-minute
# emulator suite is not used as a syntax checker.
#
# ☠ Written after six consecutive full-suite runs each found exactly one
# defect of these two kinds: a shell variable used but never set (fatal under
# `set -u`, and invisible until that line runs), and a build script invoked
# without an argument it requires.
import glob, re, sys

SAFE = {"PATH","HOME","PWD","OSTYPE","IFS","SECONDS","RANDOM","LINENO","PPID"}

def shell_vars(path):
    s = open(path).read()
    s = re.sub(r'\\\n', ' ', s)                       # join continuations
    # ☠ Deliberately loose: any NAME= anywhere counts as assigned. The defect
    # this exists to catch is a name never assigned AT ALL (used under set -u,
    # fatal only when that line runs). Being stricter produced false positives
    # on `local x; x=$(...)` and made the lint worth ignoring.
    assigned = set(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)=', s))
    assigned |= set(re.findall(r'\bfor\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\b', s))
    assigned |= set(re.findall(r'\bread\s+(?:-\w+\s+)*([A-Za-z_][A-Za-z0-9_]*)', s))
    used = set()
    for m in re.finditer(r'\$\{?([A-Za-z_][A-Za-z0-9_]*)(\}|:[-=?+]|[^A-Za-z0-9_}]|$)', s):
        name, nxt = m.group(1), m.group(2)
        if nxt.startswith(":") or nxt in ("-", "=", "+"):   # ${x:-default} is safe
            continue
        used.add(name)
    # a bare $X inside ${X:+...} only expands when X is already set
    guarded = set(re.findall(r'\$\{([A-Za-z_][A-Za-z0-9_]*):[+-]', s))
    return sorted(u for u in used - assigned - SAFE - guarded if not u.startswith("BASH"))

def required_args():
    req = {}
    for p in glob.glob("custom/*.py"):
        src = open(p).read()
        req[p] = set(re.findall(r'add_argument\(\s*"(--[a-z-]+)"[^)]*required=True', src, re.S))
    return req

def main():
    bad = 0
    for f in sorted(glob.glob("tests/*.sh")):
        miss = shell_vars(f)
        if miss:
            print(f"  ☠ {f}: uses unset {miss}"); bad = 1
    req = required_args()
    for f in sorted(glob.glob("tests/*.sh")) + ["Makefile"]:
        s = re.sub(r'\\\n', ' ', open(f).read())
        for m in re.finditer(r'python3 (custom/[a-z-]+\.py)([^\n]*)', s):
            need = req.get(m.group(1), set())
            missing = [a for a in need if a not in m.group(2)]
            if missing:
                print(f"  ☠ {f}: {m.group(1)} invoked without {missing}"); bad = 1
    print("  gates lint clean" if not bad else "")
    return bad

sys.exit(main())
