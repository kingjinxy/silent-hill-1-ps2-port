#!/usr/bin/env python3
"""Runs the libgte/libkmath equivalence test: the PS1 build in DuckStation (results dumped via gdb),
then the EE build in PCSX2 with those results embedded; prints the EE side's report."""
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "gdb"))
import warp_sweep  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


def sym(elf, name):
    out = subprocess.run(["mipsel-linux-gnu-nm", elf], capture_output=True, text=True).stdout
    return int(next(l.split()[0] for l in out.splitlines() if l.split()[-1] == name), 16)


def run_ps1():
    elf, exe = os.path.join(HERE, "lib_test_ps1.elf"), os.path.join(HERE, "lib_test_ps1.exe")
    hdr = open(os.path.join(HERE, "lib_tests.h")).read()
    nfuncs = int(re.search(r"LIB_TEST_NUM_FUNCS (\d+)", hdr).group(1))
    size = nfuncs * 8 * (4 + 256 + 2048)
    start, done = sym(elf, "lib_test_results"), sym(elf, "lib_test_done")
    out_bin = os.path.join(HERE, "ps1_results.bin")
    backup = warp_sweep.patch_settings()
    ds = None
    try:
        ds = subprocess.Popen([os.path.expanduser("~/Downloads/DuckStation-x64.AppImage"), "-batch", "-nofullscreen", "--", exe],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL, start_new_session=True)
        if not warp_sweep.wait_port(2345, 30):
            sys.exit("GDB server didn't start")
        time.sleep(1)
        subprocess.run(["gdb-multiarch", "-q", "-batch", "-ex", "set architecture mips:3000", "-ex", "target remote localhost:2345",
                        "-ex", "break *0x%08X" % done, "-ex", "continue",
                        "-ex", "dump binary memory %s 0x%08X 0x%08X" % (out_bin, start, start + size), "-ex", "kill"],
                       capture_output=True, text=True, timeout=300, stdin=subprocess.DEVNULL)
    finally:
        if ds:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(ds.pid, sig)
                    ds.wait(5)
                    break
                except (ProcessLookupError, subprocess.TimeoutExpired):
                    pass
        shutil.copy2(backup, warp_sweep.SETTINGS)
        os.remove(backup)
    if not os.path.exists(out_bin) or os.path.getsize(out_bin) != size:
        sys.exit("no PS1 results")


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    subprocess.check_call([os.path.join(ROOT, "tools", "port", "recomp_all.sh")], stdout=subprocess.DEVNULL)
    subprocess.check_call(["make", "-s", "-C", HERE, "lib_test_ps1.exe"], stderr=subprocess.DEVNULL)
    run_ps1()
    subprocess.check_call(["make", "-s", "-C", HERE, "-B", "lib_test_ee.elf"], stderr=subprocess.DEVNULL)
    out = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "port", "pcsx2_run.py"), os.path.join(HERE, "lib_test_ee.elf"),
                          "--until", "lib_test done", "--seconds", "60"], capture_output=True, text=True).stdout
    lines = [l.split("] ", 1)[-1] for l in out.splitlines() if re.search(r"MISMATCH|\b(ok|DIFFERS)$|lib_test done", l)]
    print("\n".join(lines))
    return 0 if any("lib_test done: 0 of" in l for l in lines) else 1


if __name__ == "__main__":
    sys.exit(main())
