#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Optimized builds of the two hot components, QEMU and the dsp56300 archives
# linked into it (the emulator's time is ~all on the vCPU thread: TCG, the
# ColdFire EMAC helpers and the DSP56300 JIT/peripherals).
#
#   scripts/build-opt.sh lto    -O3 dsp56300 + GCC LTO across QEMU and it
#   scripts/build-opt.sh pgo    lto + GCC profile-guided optimization:
#                               instrumented build, a training run over the
#                               emulator's real workloads, rebuild with it
#   scripts/build-opt.sh pgo-use  redo only the optimized half, reusing the
#                               profile in out/pgo-profile
#   scripts/build-opt.sh off    back to the stock flags
#
# The choice is written to opt.env, which scripts/build-dsp.sh and
# scripts/build-qemu.sh read, so a later `make qemu` keeps the same flags
# instead of silently replacing the optimized binary.
#
# ☠ PGO generates and uses in the SAME build directories. GCC names each .gcda
# after its object's absolute path, so a use build in another directory finds
# no profile — and -Wno-missing-profile would hide that. This script checks
# with -Wmissing-profile instead and refuses a use build that missed.
set -euo pipefail
cd "$(dirname "$0")/.."
MODE=${1:?usage: build-opt.sh lto|pgo|off}
export JOBS=${JOBS:-6}
LTO_JOBS=${LTO_JOBS:-6}
PROF=$PWD/out/pgo-profile

write_env() {   # cflags ldflags
    cat > opt.env <<EOF
# written by scripts/build-opt.sh $MODE — delete (or build-opt.sh off) for stock flags
OT_DSP_BUILD_TYPE=Release
OT_OPT_CFLAGS="$1"
OT_OPT_LDFLAGS="$2"
OT_QEMU_CONFIGURE="-Db_lto=true -Db_lto_threads=$LTO_JOBS"
EOF
}

build() {       # full logs in out/opt-{dsp,qemu}.log
    mkdir -p out
    echo "== dsp56300 archives"
    scripts/build-dsp.sh > out/opt-dsp.log 2>&1 || { tail -20 out/opt-dsp.log; exit 1; }
    tail -1 out/opt-dsp.log
    echo "== qemu"
    scripts/build-qemu.sh > out/opt-qemu.log 2>&1 || { tail -30 out/opt-qemu.log; exit 1; }
    tail -1 out/opt-qemu.log
}

use_build() {    # the -fprofile-use half, against the profile in $PROF
    echo "== 3/3 optimized build"
    write_env "$LTO -fprofile-use=$PROF -fprofile-partial-training -fprofile-correction -Wmissing-profile -Wno-error=missing-profile -Wno-error=coverage-mismatch" ""
    build
    # Plenty of TUs legitimately have no profile (asmjit's ARM backend, QAPI
    # for other targets: compiled, never linked or run). What must not miss
    # is the hot path; a path mismatch would take these down with the rest.
    miss=$(cat out/opt-dsp.log out/opt-qemu.log | grep 'Wmissing-profile' | grep -o '#[^#]*\.gcda' | sort -u)
    echo "   $(echo "$miss" | grep -c . ) translation units found no profile"
    hot=$(echo "$miss" | grep -E '#(translate|cpu-exec|cputlb|ot-board|ot-dsp-shim|ot-dsp56k|ot-ata|dsp|jitops|jitblock|dma|hdi08|esai|peripherals)\.' || true)
    [ -z "$hot" ] || { echo "hot translation units missed their profile — paths differ?" >&2; echo "$hot" >&2; exit 1; }
}
LTO="-flto=$LTO_JOBS -ffat-lto-objects"
case $MODE in
off)
    rm -f opt.env
    build
    ;;
lto)
    write_env "$LTO" ""
    build
    ;;
pgo)
    echo "== 1/3 instrumented build"
    rm -rf "$PROF"; mkdir -p "$PROF"
    write_env "$LTO -fprofile-generate=$PROF -fprofile-update=single" \
              "-fprofile-generate=$PROF"
    build
    echo "== 2/3 training"
    scripts/pgo-train.sh
    n=$(find "$PROF" -name '*.gcda' | wc -l)
    echo "   profile: $n .gcda files"
    [ "$n" -gt 100 ] || { echo "profile looks empty; not building the use step" >&2; exit 1; }
    use_build
    ;;
pgo-use)
    [ -d "$PROF" ] || { echo "no profile in $PROF; run build-opt.sh pgo" >&2; exit 1; }
    use_build
    ;;
*)
    echo "usage: build-opt.sh lto|pgo|pgo-use|off" >&2; exit 2 ;;
esac
make octdsp >/dev/null
echo "ok: $MODE build in place (opt.env: $( [ -f opt.env ] && echo present || echo absent ))"
