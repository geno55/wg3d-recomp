#!/bin/sh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"  # repo root
set -e
cd ~
[ -d ultralib ] || git clone -q --depth 1 https://github.com/decompals/ultralib.git
cd ultralib
for T in libultra_rom libultra; do
  make VERSION=E TARGET=$T COMPARE=0 setup > /tmp/ul_setup_$T.log 2>&1 || { tail -20 /tmp/ul_setup_$T.log; exit 1; }
  make VERSION=E TARGET=$T COMPARE=0 -j8 > /tmp/ul_build_$T.log 2>&1 || { tail -30 /tmp/ul_build_$T.log; exit 1; }
done
find build -name "*.a" | xargs ls -la
