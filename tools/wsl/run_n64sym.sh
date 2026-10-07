#!/bin/sh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"  # repo root
cd "$ROOT"/build
"${N64SYM:-n64sym}" ram_image.bin -s -h 0x80000000 > n64sym_ram.txt 2>&1
"${N64SYM:-n64sym}" ram_image.bin -s -t -h 0x80000000 > n64sym_ram_t.txt 2>&1
wc -l n64sym_ram.txt n64sym_ram_t.txt
