#!/usr/bin/env python3
"""Runs the GTE checker on the PS2's EE in PCSX2 (check_ee.elf): replays every seed's recorded PS1
results (results_seedN.bin, from run.py) through src/port/gte.c as compiled for the EE, including its
MMI code, and prints the differences and the EE cycles per command."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    subprocess.check_call(["make", "-s", "check_ee.elf"], cwd=HERE)
    out = subprocess.run([sys.executable, os.path.join(HERE, "..", "pcsx2_run.py"), "--seconds", "120", "--stall", "0",
                          "--until", "gte_test_ee:", os.path.join(HERE, "check_ee.elf")], capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if "tests differ" in l or "cycles" in l or "gte_test_ee" in l
             or " hw=" in l or l.split("] ", 1)[-1].startswith("by ")]
    print("\n".join(l.split("] ", 1)[-1] for l in lines))
    return 0 if any("all tests match" in l for l in lines) else 1


if __name__ == "__main__":
    sys.exit(main())
