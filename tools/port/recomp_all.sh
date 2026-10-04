#!/bin/bash
# Recompile the PS1 library code the port needs (tools/port/recomp.py) into build/port/recomp/.
# libgte: the objects defining the functions the game links (msc00.o = InitGeom is hand-written in
# src/port/libgte_port.c: it patches the PS1 kernel). libkmath: the PS1 build's object (run `make` first).
set -e
cd "$(dirname "$0")/../.."
OUT=build/port/recomp
rm -rf "$OUT"; mkdir -p "$OUT"
PROTOS="include/psyq/libgte.h include/psyq/libgs.h include/psyq/libgpu.h include/gpu.h include/bodyprog/math/math.h tools/port/recomp_protos.h"
LIBGTE="mtx_05 mtx_004 mtx_06 mtx_005 mtx_01 mtx_003 g3 g4 gt3 gt4 msc06 smp_00 mtx_03 mtx_000 mtx_04 mtx_001
        smp mtx_006 reg08 reg09 mtx_007 smp_04 smp_02 smp_03 cmb_00 sqrtbl mtx_08 reg10 mtx_11 reg12 reg13
        mtx_10 mtx_02 mtx_09 mtx_12 msc01 msc09 mtx_07 fgo_00 msc02 ratan
        reg11"  # reg11 = SetFarColor, used by libgs
for o in $LIBGTE; do
    python3 tools/port/recomp.py "lib/libgte/$o.o" "$OUT" --prefix "libgte_$o" --protos $PROTOS 2>&1 \
        | grep -vE '^lib/libgte/.*->|no prototype for (GsTMDfast|AverageSZ)' || true
done
python3 tools/port/recomp.py build/USA/src/bodyprog/libkmath/libkmath.s.o "$OUT" --prefix libkmath --protos $PROTOS
# libgs: all objects (port_link.py links the recompiled code as an archive, so only what's used is pulled in).
for o in lib/libgs/*.o; do
    python3 tools/port/recomp.py "$o" "$OUT" --prefix "libgs_$(basename "$o" .o)" --protos $PROTOS 2>&1 \
        | grep -vE '^lib/libgs/.*->|no prototype for (gte_init|GsInitCoord2param|print_|Gssub_make_matrix)' || true
done
python3 tools/port/recomp_bridges.py "$OUT"
