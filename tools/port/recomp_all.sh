#!/bin/bash
# Recompile the PS1 library code the port needs (tools/port/recomp.py) into build/port/recomp/.
# libgte: the objects defining the functions the game links that aren't C yet (src/port/libgte_port.c;
# calls from recompiled code to those go through bridges, recomp_bridges.py). libkmath is C (src/port/libkmath_port.c).
set -e
cd "$(dirname "$0")/../.."
OUT=build/port/recomp
rm -rf "$OUT"; mkdir -p "$OUT"
PROTOS="include/psyq/libgte.h include/psyq/libgs.h include/psyq/libgpu.h include/gpu.h include/bodyprog/math/math.h tools/port/recomp_protos.h"
LIBGTE="g3 g4 gt3 gt4"
for o in $LIBGTE; do
    python3 tools/port/recomp.py "lib/libgte/$o.o" "$OUT" --prefix "libgte_$o" --protos $PROTOS 2>&1 \
        | grep -vE '^lib/libgte/.*->|no prototype for (GsTMDfast|AverageSZ)' || true
done
# libgs: all objects (port_link.py links the recompiled code as an archive, so only what's used is pulled in).
for o in lib/libgs/*.o; do
    python3 tools/port/recomp.py "$o" "$OUT" --prefix "libgs_$(basename "$o" .o)" --protos $PROTOS 2>&1 \
        | grep -vE '^lib/libgs/.*->|no prototype for (gte_init|GsInitCoord2param|print_|Gssub_make_matrix)' || true
done
# libgpu: only the objects without hardware access (primitive builders, TIM parsing). sys.o (the GPU
# system layer) is reimplemented in src/port/libgpu_port.c.
for o in p06 p09 p14 p16 p17 p18 p26 p33 p34 tmd; do
    python3 tools/port/recomp.py "lib/libgpu/$o.o" "$OUT" --prefix "libgpu_$o" --protos $PROTOS 2>&1 \
        | grep -vE '^lib/libgpu/.*->' || true
done
# sys.o: recompiled for its pure packet builders only (the hardware layer is src/port/libgpu_port.c).
python3 tools/port/recomp.py lib/libgpu/sys.o "$OUT" --prefix libgpu_sys --protos $PROTOS \
    --exports SetDrawMode SetDrawArea SetDrawOffset SetDrawStp SetDrawEnv SetPriority SetTexWindow GetGraphDebug \
    2>&1 | grep -vE '^lib/libgpu/.*->|no prototype' || true
python3 tools/port/recomp_bridges.py "$OUT"
