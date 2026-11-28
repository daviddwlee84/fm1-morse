#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# core.wasm from MORSE's own learn.c, morse.c and content.c (unchanged) and core.c. Needs clang with the wasm32
# target and wasm-ld (lld): apt install clang lld, or Xcode's clang plus `brew install lld`. No Emscripten.
set -eu
cd "$(dirname "$0")"
${CLANG:-clang} --target=wasm32 -O2 -std=c99 -Wall -Wextra -nostdlib -ffreestanding \
  -Wl,--no-entry -Wl,--export-dynamic -Wl,--strip-all -I../.. \
  core.c ../../learn.c ../../morse.c ../../content.c -o core.wasm
echo "core.wasm: $(wc -c < core.wasm) bytes"
