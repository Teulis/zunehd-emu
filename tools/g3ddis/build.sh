#!/bin/bash
# Build g3ddis (host tool) with MSYS2 UCRT64 gcc. Run via:
#   $env:MSYSTEM='UCRT64'; & C:\msys64\usr\bin\bash.exe -l <repo>/tools/g3ddis/build.sh
set -e
cd "$(dirname "$0")"
G=../../ext/grate
gcc -O2 -std=gnu11 -w -o g3ddis.exe g3ddis.c \
    $G/src/libwrap/disasm.c \
    $G/src/libgrate/vertex_disasm.c $G/src/libgrate/fragment_disasm.c $G/src/libgrate/linker_disasm.c \
    -I$G/include -I$G/src/libgrate -I$G/src/libwrap -I$G/src
echo built g3ddis.exe
