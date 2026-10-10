#!/bin/bash
# Deterministic renderer comparison in a Demo cutscene: builds a benchmark build (SH1_BENCH=1: Demo
# cutscenes run uncapped with a fixed step of one vertical blank a frame; frame dumps every 200 frames
# after the event starts, up to 2000), plays Demo cutscene N in PCSX2 (host:demo.txt) and collects the
# dumps into OUTDIR. Compare two OUTDIRs with tools/port/compare_dumps.py. Extra build options come
# from the environment (e.g. SH1_NO_FOG_PAIRS=1).
#   tools/port/bench_cutscene.sh N OUTDIR
set -e
cd "$(dirname "$0")/../.."
N="$1"; OUT="$2"; [ -n "$OUT" ] || { echo "usage: $0 N OUTDIR"; exit 1; }
mkdir -p "$OUT"
source .venv/bin/activate
export SH1_FPS=60 SH1_BENCH=1 SH1_NO_RECOMP=1
unset SH1_PROF SH1_EXTRA_CFLAGS
bash tools/port/ee_compile_check.sh >/dev/null 2>&1
touch src/port/ps2/gpu_gs.c src/port/demo_menu.c
python3 tools/port/port_link.py >/dev/null && bash tools/port/make_iso.sh >/dev/null
find build/port -maxdepth 1 -name "frame_*.ppm" -delete
rm -f build/port/input.txt
echo "sweep $N 300 $N" > build/port/demo.txt
timeout 900 python3 tools/port/pcsx2_run.py build/port/sh1_ps2.iso --seconds 600 --stall 30 --until "demo sweep: done" >/dev/null 2>&1 || true
rm -f build/port/demo.txt
n=0; for f in $(ls build/port/frame_*.ppm 2>/dev/null | sort -V); do mv "$f" "$OUT/dump_$(printf %02d $n).ppm"; n=$((n+1)); done
echo "$n dumps in $OUT"
