#!/bin/sh
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"  # repo root
# Compile mk64's libultra (src/os) with IDO into an archive used as a fuzzy-match reference.
cd ~
[ -d mk64 ] || git clone -q --depth 1 https://github.com/n64decomp/mk64.git
cd mk64
CC=~/ultralib/tools/ido/cc
OUT=/tmp/mk64os_O2; mkdir -p $OUT
ok=0; fail=0
for f in src/os/*.c; do
  b=$(basename $f .c)
  OPT="-O2"
  case $b in gu*|al*|_Printf|_Litob|_Ldtob|osSyncPrintf) OPT=-O3;; ldiv|string) OPT=-O2;; __osLeoInterrupt) OPT=-O1;; esac
  if cpp -P -undef -nostdinc -Wno-trigraphs -Iinclude -Iinclude/libc -Isrc -Isrc/os -I. -D__sgi -D_LANGUAGE_C -D_MIPS_SZLONG=32 -D_MIPS_SZINT=32 -D_MIPS_SIM=1 -DVERSION_US=1 -DTARGET_N64 -DF3DEX_GBI -o $OUT/$b.i $f >/tmp/mk64os_$b.log 2>&1 &&      $CC -c -G 0 -non_shared -Wab,-r4300_mul $OPT -mips2 -woff 649,838,712,516 -o $OUT/$b.o $OUT/$b.i >>/tmp/mk64os_$b.log 2>&1; then ok=$((ok+1)); else fail=$((fail+1)); echo "FAIL $b: $(head -c 200 /tmp/mk64os_$b.log | tr '\n' ' ')"; fi
done
rm -f /tmp/libultra_mk64_O2.a
mips-linux-gnu-ar rcs /tmp/libultra_mk64_O2.a $OUT/*.o
cp /tmp/libultra_mk64_O2.a "$ROOT"/build/sigs/
echo "compiled $ok, failed $fail"
