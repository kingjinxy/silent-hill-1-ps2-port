#!/usr/bin/env python3
"""SH1's GTE macro headers through the HAL's generator (../psx-to-ps2-HAL/tools/gen_gte_inline.py).
    python3 tools/port/gen_gte_inline.py   (rerun after editing any of the source headers)"""
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HAL = os.path.normpath(os.path.join(ROOT, os.environ.get("SH1_HAL", "../psx-to-ps2-HAL")))
SOURCES = {
    "include/psyq/inline_c.h": HAL + "/include/port/gte_from_inline_c.h",
    "include/inline_no_dmpsx.h": HAL + "/include/port/gte_from_inline_no_dmpsx.h",
    "include/gpu.h": HAL + "/include/port/gte_from_gpu.h",
}
os.chdir(ROOT)
sys.exit(subprocess.call([sys.executable, os.path.join(HAL, "tools", "gen_gte_inline.py")] + ["%s=%s" % kv for kv in SOURCES.items()]))
