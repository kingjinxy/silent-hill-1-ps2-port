#!/bin/bash
# Compile every C file the USA build links (taken from linkers/USA/*.ld, so run `make setup` first)
# with the PS2 EE compiler under SH_PORT, to object files in build/ee_check/. Lists failures.
# Usage: tools/port/ee_compile_check.sh [outdir]
# Environment: SH1_FPS=60 (gameplay at 60 fps instead of 30), SH1_BENCH=1 (also the attract demo, at
# one frame per vertical blank, for measuring speed: the demo then desyncs), SH1_PROF=1 (profiling
# counters, include/port/prof.h), SH1_EXTRA_CFLAGS.
# NULL reads stay reads (-fno-delete-null-pointer-checks, -fno-isolate-erroneous-paths-dereference):
# the PS1 code reads through NULL in places and gets 0 (crash_ps2.c maps address 0 to zeros); GCC
# otherwise turned such paths into traps (map6_s04's Chara_AnimPlaybackStateGet hung on one).
set -u
OUT=${1:-build/ee_check}
CC=${PS2DEV:-$HOME/ps2dev}/ee/bin/mips64r5900el-ps2-elf-gcc
mkdir -p "$OUT"
check() {
    f=$1
    m=$(echo "$f" | sed -nE 's|src/maps/(map[0-9]_s[0-9]+)/.*|\1|p' | tr a-z A-Z)
    o="$OUT/$(echo "$f" | tr / _)"
    "$CC" -c -O2 ${SH1_EXTRA_CFLAGS:-} ${SH1_FPS:+-DSH_PORT_FPS=$SH1_FPS} ${SH1_BENCH:+-DSH_PORT_BENCH} ${SH1_PROF:+-DSH_PORT_PROF} -G0 -fno-toplevel-reorder -fno-zero-initialized-in-bss -mno-check-zero-division -fno-strict-aliasing -fno-delete-null-pointer-checks -fno-isolate-erroneous-paths-dereference -std=gnu89 -nostdinc -Iinclude -Ibuild/USA -Iinclude/psyq -Iinclude/decomp -Wa,-Iinclude \
        -D_LANGUAGE_C -DVER_USA -DSH_PORT -DNON_MATCHING ${m:+-D$m -DSH_MAP_OVERLAY} \
        -Wno-error=implicit-function-declaration -Wno-error=int-conversion \
        -Wno-error=incompatible-pointer-types -Wno-error=implicit-int -Wno-error=return-mismatch \
        "$f" -o "$o.o" > "$o.txt" 2>&1 || echo "$f"
}
export -f check; export CC OUT SH1_EXTRA_CFLAGS SH1_FPS SH1_BENCH SH1_PROF
cat linkers/USA/*.ld linkers/USA/*/*.ld | grep -oE 'build/USA/src/[^ ()]+\.c\.o' | sed -E 's|^build/USA/||; s|\.o$||' \
    | sort -u > "$OUT/files.txt"
xargs -P "$(nproc)" -I{} bash -c 'check {}' < "$OUT/files.txt" > "$OUT/failed.txt"
echo "failed: $(wc -l < "$OUT/failed.txt") of $(wc -l < "$OUT/files.txt")"
cat "$OUT/failed.txt"
cat "$OUT"/*.txt | grep -oE '\[-W[a-z0-9-]+\]' | sort | uniq -c | sort -rn | sed 's/^/  warnings /'
[ ! -s "$OUT/failed.txt" ]
