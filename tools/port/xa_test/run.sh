#!/bin/bash
# Compares sh1spu's XA decoder (src/port/iop/sh1spu/xa.c, built natively) with the reference
# (xa_ref.py) on one voice line from the disc: tools/port/xa_test/run.sh INDEX FILE CHAN
set -e
cd "$(dirname "$0")/../../.."
T=build/port/xa_test; mkdir -p $T/src
cp src/port/iop/sh1spu/xa.c src/port/iop/sh1spu/gauss_table.h $T/src/
cp tools/port/xa_test/host/irx_imports.h $T/src/
gcc -O1 -w -I$T/src tools/port/xa_test/xa_host.c -o $T/xa_host
$T/xa_host rom/USA/HILL. "$1" "$2" "$3" $T/host_48k.raw
python3 tools/port/xa_test/xa_ref.py --index "$1" --file "$2" --chan "$3" --out $T/ref.wav --out48 $T/ref_48k.wav
python3 tools/port/xa_test/compare.py $T/ref_48k.wav $T/host_48k.raw
