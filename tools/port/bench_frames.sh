#!/bin/bash
# Deterministic renderer comparison: builds a benchmark build (SH1_BENCH=1: the attract demo uncapped
# with its own timing, frame dumps at demo frames 99, 199, ... 999), runs the first demo in PCSX2 and
# collects the dumps into OUTDIR. Compare two OUTDIRs with tools/port/compare_dumps.py.
#   tools/port/bench_frames.sh OUTDIR
set -e
cd "$(dirname "$0")/../.."
OUT="$1"; [ -n "$OUT" ] || { echo "usage: $0 OUTDIR"; exit 1; }
mkdir -p "$OUT"
source .venv/bin/activate
export SH1_FPS=60 SH1_BENCH=1 SH1_NO_RECOMP=1
unset SH1_PROF SH1_EXTRA_CFLAGS
bash tools/port/ee_compile_check.sh >/dev/null 2>&1
python3 tools/port/port_link.py >/dev/null && bash tools/port/make_iso.sh >/dev/null
find build/port -maxdepth 1 -name "frame_*.ppm" -delete
rm -f build/port/input.txt
timeout 900 python3 tools/port/pcsx2_run.py build/port/sh1_ps2.iso --seconds 60 --stall 30 --until "demo trace: step 1020" >/dev/null 2>&1 || true
n=0; for f in $(ls build/port/frame_*.ppm | sort -V); do mv "$f" "$OUT/dump_$(printf %02d $n).ppm"; n=$((n+1)); done
echo "$n dumps in $OUT"
