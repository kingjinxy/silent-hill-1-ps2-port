#!/usr/bin/env python3
"""Runs gte_test.exe in DuckStation, dumps the recorded GTE results to results.bin via gdb, and runs
./check on them. DuckStation settings are patched for the run and restored (see warp_sweep.py)."""
import os
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "gdb"))
import warp_sweep  # noqa: E402  (settings patching, port wait)


def sym(name):
    out = subprocess.run(["mipsel-linux-gnu-nm", os.path.join(HERE, "gte_test.elf")], capture_output=True, text=True).stdout
    return int(next(l.split()[0] for l in out.splitlines() if l.split()[-1] == name), 16)


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    subprocess.check_call(["make", "-s", "-B", "SEED=%d" % seed, "gte_test.exe", "check"], cwd=HERE, stderr=subprocess.DEVNULL)
    exe = os.path.join(HERE, "gte_test.exe")
    results = os.path.join(HERE, "results.bin")
    num = int(next(l.split()[2] for l in open(os.path.join(HERE, "cmds.h")) if "GTE_TEST_NUM_CMDS" in l and "#define" in l))
    size = num * 8 * 64 * 4
    start, done = sym("gte_test_results"), sym("gte_test_done")

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    backup = warp_sweep.patch_settings()
    ds = None
    try:
        ds = subprocess.Popen([os.path.expanduser("~/Downloads/DuckStation-x64.AppImage"), "-batch", "-nofullscreen", "--", exe],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
        if not warp_sweep.wait_port(2345, 30):
            sys.exit("GDB server didn't start")
        time.sleep(1)
        out = subprocess.run(["gdb-multiarch", "-q", "-batch", "-ex", "set architecture mips:3000", "-ex", "target remote localhost:2345",
                              "-ex", "break *0x%08X" % done, "-ex", "continue",
                              "-ex", "dump binary memory %s 0x%08X 0x%08X" % (results, start, start + size), "-ex", "kill"],
                             capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL).stdout
    finally:
        if ds:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(ds.pid, sig)
                    ds.wait(5)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
        import shutil
        shutil.copy2(backup, warp_sweep.SETTINGS)
        os.remove(backup)
    if not os.path.exists(results) or os.path.getsize(results) != size:
        sys.exit("no results dumped")
    print("seed %d:" % seed, end=" ", flush=True)
    return subprocess.call([os.path.join(HERE, "check"), results])


if __name__ == "__main__":
    sys.exit(main())
