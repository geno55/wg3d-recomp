#!/bin/sh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"  # repo root
set -e
cd ~/ultralib/build/E
for T in libultra_rom libultra; do
  n=$(find $T -name "*.o" | wc -l)
  rm -f /tmp/$T.E.a
  find $T -name "*.o" | sort | xargs mips-linux-gnu-ar rcs /tmp/$T.E.a
  echo "$T: $n objects"
done
mkdir -p "$ROOT"/build/sigs
cp /tmp/libultra_rom.E.a /tmp/libultra.E.a "$ROOT"/build/sigs/
ls -la "$ROOT"/build/sigs/
