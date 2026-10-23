#!/bin/bash
# Recompile the PS1 library code the port needs (tools/port/recomp.py) into build/port/recomp/.
# libgte and libkmath are C (src/port/libgte_port.c, libkmath_port.c); calls from recompiled code to C go
# through bridges (recomp_bridges.py).
set -e
cd "$(dirname "$0")/../.."
OUT=build/port/recomp
rm -rf "$OUT"; mkdir -p "$OUT"
PROTOS="include/psyq/libgte.h include/psyq/libgs.h include/psyq/libgpu.h include/gpu.h include/bodyprog/math/math.h tools/port/recomp_protos.h"
# libgs: all C (src/port/libgs_port.c).
# libgpu: all C (src/port/libgpu_packets.c, libgpu_port.c).
python3 tools/port/recomp_bridges.py "$OUT"
