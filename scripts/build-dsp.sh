#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Configure and build the three dsp56300 archives the emulator links, in the
# tree scripts/setup.sh cloned and patched. Honours opt.env (see
# scripts/build-opt.sh) so an optimized build stays optimized across rebuilds.
set -euo pipefail
cd "$(dirname "$0")/.."

# shellcheck source=/dev/null
[ -f opt.env ] && . ./opt.env

DSP=vendor/dsp56300
[ -d "$DSP/source" ] || { echo "run 'make setup' first" >&2; exit 1; }

# RelWithDebInfo, never Release, on macOS: base.cmake appends -flto to Release
# there, which puts LLVM bitcode in the archives instead of object code. On
# Linux Release is plain -O3 and opt.env may ask for it.
BUILD_TYPE=${OT_DSP_BUILD_TYPE:-RelWithDebInfo}
args=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE")
if [ -n "${OT_OPT_CFLAGS:-}" ]; then
    args+=(-DCMAKE_C_FLAGS="$OT_OPT_CFLAGS" -DCMAKE_CXX_FLAGS="$OT_OPT_CFLAGS")
else
    args+=(-DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS=)
fi
# GCC LTO objects need the plugin-aware archiver for their symbol index.
if [[ "${OT_OPT_CFLAGS:-}" == *-flto* ]]; then
    args+=(-DCMAKE_AR="$(command -v gcc-ar)" -DCMAKE_RANLIB="$(command -v gcc-ranlib)")
else
    args+=(-DCMAKE_AR="$(command -v ar)" -DCMAKE_RANLIB="$(command -v ranlib)")
fi

cmake -S "$DSP" -B "$DSP/build" "${args[@]}" >/dev/null
cmake --build "$DSP/build" --target dsp56kEmu -j "${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"
for a in dsp56kEmu/libdsp56kEmu dsp56kBase/libdsp56kBase asmjit/libasmjit; do
    f=$DSP/build/source/$a.a
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
    m=$(ar t "$f" | grep -m1 '\.o$')
    ar p "$f" "$m" | file -b - | grep -qi bitcode && { echo "$f is LLVM bitcode — rebuild RelWithDebInfo" >&2; exit 1; }
done
echo "   ok: three archives, object code ($BUILD_TYPE${OT_OPT_CFLAGS:+, $OT_OPT_CFLAGS})"
