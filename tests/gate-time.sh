#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# gate-time.sh — run one gate and print its wall-clock cost, so a slow suite
# is a list of numbers rather than a feeling. Exit status is the gate's.
#   tests/gate-time.sh tests/usb-audio-test.sh audio-validate
set -u
t0=$(date +%s)
"$@"
rc=$?
echo "== $(( $(date +%s) - t0 )) s: $* (exit $rc)"
exit $rc
