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
# libgs: the objects not yet in C (src/port/libgs_port.c); port_link.py links the recompiled code as an
# archive, so only what's used is pulled in.
LIBGS_C="gs_001 gs_002 gs_003 gs_007 gs_008 gs_009 gs_010 gs_104 gs_105 gs_106 gs_111 gs_113 gs_114 gs_121 gs_127 gs_107 gs_108 gs_110 matrix"
for o in lib/libgs/*.o; do
    case " $LIBGS_C " in *" $(basename "$o" .o) "*) continue;; esac
    python3 tools/port/recomp.py "$o" "$OUT" --prefix "libgs_$(basename "$o" .o)" --protos $PROTOS 2>&1 \
        | grep -vE '^lib/libgs/.*->|no prototype for (gte_init|GsInitCoord2param|print_|Gssub_make_matrix)' || true
done
# libgpu: TIM/TMD parsing (tmd.o). The primitive and drawing-setting builders are C
# (src/port/libgpu_packets.c), the GPU system layer is src/port/libgpu_port.c.
for o in tmd; do
    python3 tools/port/recomp.py "lib/libgpu/$o.o" "$OUT" --prefix "libgpu_$o" --protos $PROTOS 2>&1 \
        | grep -vE '^lib/libgpu/.*->' || true
done
python3 tools/port/recomp_bridges.py "$OUT"
