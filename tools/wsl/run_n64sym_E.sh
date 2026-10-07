#!/bin/sh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"  # repo root
cd "$ROOT"/build
"${N64SYM:-n64sym}" ram_image.bin -l sigs -t -h 0x80000000 > n64sym_ram_E.txt 2>&1
wc -l n64sym_ram_E.txt
