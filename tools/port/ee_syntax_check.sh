#!/bin/bash
# Syntax-check every compilable C file with the PS2 EE compiler under SH_PORT.
# Files directly under src/maps/ (and characters/shared/common) are only #included into maps, so skipped.
# Usage: tools/port/ee_syntax_check.sh [outdir]   (diagnostics per file in outdir, default build/ee_check)
set -u
OUT=${1:-build/ee_check}
CC=${PS2DEV:-$HOME/ps2dev}/ee/bin/mips64r5900el-ps2-elf-gcc
mkdir -p "$OUT"
check() {
    f=$1
    m=$(echo "$f" | sed -nE 's|src/maps/(map[0-9]_s[0-9]+)/.*|\1|p' | tr a-z A-Z)
    "$CC" -fsyntax-only -std=gnu89 -nostdinc -Iinclude -Ibuild/USA -Iinclude/psyq -Iinclude/decomp \
        -D_LANGUAGE_C -DVER_USA -DSH_PORT -DNON_MATCHING ${m:+-D$m} \
        -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
        -Wno-error=incompatible-pointer-types -Wno-error=implicit-int -Wno-error=return-mismatch \
        "$f" > "$OUT/$(echo "$f" | tr / _).txt" 2>&1 || echo "$f"
}
export -f check; export CC OUT
find src -name '*.c' | grep -vE '^src/port/|^src/maps/[^/]+\.c$|^src/maps/(characters|shared|common)/' \
    | sort | xargs -P "$(nproc)" -I{} bash -c 'check {}' > "$OUT/failed.txt"
total=$(find src -name '*.c' | grep -vcE '^src/port/|^src/maps/[^/]+\.c$|^src/maps/(characters|shared|common)/')
echo "failed: $(wc -l < "$OUT/failed.txt") of $total"; cat "$OUT/failed.txt"
cat "$OUT"/*.txt | grep -oE '\[-W[a-z0-9-]+\]' | sort | uniq -c | sort -rn | sed 's/^/  warnings /'
[ ! -s "$OUT/failed.txt" ]
