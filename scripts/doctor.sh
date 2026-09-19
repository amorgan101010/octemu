#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# What this computer has, what it is missing, and what each thing is for — read
# from the Brewfile, so the list exists in exactly one place. Exit 1 if
# anything required is absent; an absent optional tool gates one named feature
# and is reported, not fatal.
set -uo pipefail
cd "$(dirname "$0")/.."

ok=1
pad="                "

report() {                      # kind cmd formula why...
    local kind=$1 cmd=$2 formula=$3 found=1
    shift 3
    local why="$*"
    case $cmd in
        # sdl2 ships no command worth trusting; pkg-config is the real test.
        sdl2-config) pkg-config --exists sdl2 2>/dev/null || found=0 ;;
        *)           command -v "$cmd" >/dev/null 2>&1 || found=0 ;;
    esac
    if [ "$found" = 1 ]; then
        printf '  ok       %-16s %s\n' "$cmd" "$why"
    else
        printf '  MISSING  %-16s %s\n' "$cmd" "$why"
        printf '  %s brew install %s\n' "$pad" "$formula"
        [ "$kind" = need ] && ok=0
    fi
}

# The Brewfile carries one `need:<cmd>` or `want:<cmd>` annotation per formula.
parse() {   # kind
    awk -v want="$1" '
        /^brew "/ {
            formula = $0; sub(/^brew "/, "", formula); sub(/".*/, "", formula)
            ann = $0; sub(/^[^#]*# */, "", ann)
            split(ann, f, ":"); kind = f[1]
            rest = ann; sub(/^[^:]*: */, "", rest)
            cmd = rest; sub(/ .*/, "", cmd)
            why = rest; sub(/^[^ ]+ +/, "", why)
            if (kind == want) print kind, cmd, formula, why
        }' Brewfile
}

echo "required — nothing builds without these:"
parse need | while read -r kind cmd formula why; do
    report "$kind" "$cmd" "$formula" "$why"
done > /tmp/doctor.need.$$ || true
cat /tmp/doctor.need.$$
grep -q MISSING /tmp/doctor.need.$$ && ok=0
rm -f /tmp/doctor.need.$$

# Two things the Brewfile cannot express as a command name.
if ! python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)' 2>/dev/null; then
    printf '  MISSING  %-16s %s\n' "python >= 3.11" "tomllib, used by tests/emac-conform.py"
    printf '  %s brew install python@3.12\n' "$pad"
    ok=0
fi
if ! command -v git >/dev/null 2>&1; then
    printf '  MISSING  %-16s %s\n' git "fetches QEMU, the DSP core and the firmware tool"
    printf '  %s xcode-select --install\n' "$pad"
    ok=0
fi

echo "optional — each gates one feature:"
parse want | while read -r kind cmd formula why; do
    report "$kind" "$cmd" "$formula" "$why"
done

[ "$(uname)" = Darwin ] || cat <<MSG
  note: this is $(uname). The emulator is macOS-only for two reasons — the USB
        DISK MODE host mount and --midi (CoreMIDI), both in src/platform/.
MSG

if [ "$ok" = 1 ]; then
    echo "ok: everything required is here"
else
    echo
    echo "'brew bundle' installs the whole Brewfile at once."
    exit 1
fi
