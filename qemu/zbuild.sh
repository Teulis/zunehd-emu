#!/usr/bin/env bash
# Incremental build of qemu-system-arm with the zunehd machine (MSYS2 UCRT64).
set -o pipefail
cd "$(dirname "$0")/build" || exit 1
ninja -j"$(nproc)" qemu-system-arm.exe > ../build-zunehd.log 2>&1
rc=$?
grep -E 'zunehd|error|warning:|FAILED' ../build-zunehd.log | tail -40
tail -2 ../build-zunehd.log
exit $rc
