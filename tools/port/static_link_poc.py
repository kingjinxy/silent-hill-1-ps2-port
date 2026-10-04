#!/usr/bin/env python3
"""Step 2 proof of concept: link every binary's objects into one relocatable object.

Each map is first linked on its own (`ld -r`), its `g_MapOverlayHdr` renamed to
`g_MapOverlayHdr_<map>`, and every other symbol it defines made local, so the 43 maps' duplicate
names (sharedFunc_*, sharedData_*, Map_WorldObjectsInit, ...) can't collide. Then main, bodyprog,
the screen overlays (keeping only what main/bodyprog reference) and all maps are linked together and the remaining undefined symbols listed.

Uses the PS1 objects from a normal build (run `make build` first); nothing here runs, it only
checks that the pieces fit. Output goes to build/USA/port_poc/.
"""
import glob
import os
import re
import subprocess
import sys

LD, OBJCOPY, NM = "mips-linux-gnu-ld", "mips-linux-gnu-objcopy", "mips-linux-gnu-nm"
OUT = "build/USA/port_poc"


def objects(ld_script):
    seen, objs = set(), []
    for o in re.findall(r"([\w./-]+?\.o)\(", open(ld_script).read()):
        if o not in seen:
            seen.add(o)
            objs.append(o)
    return objs


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("FAILED: %s\n%s" % (" ".join(cmd[:3]), r.stderr[-3000:]))
    return r


def main():
    os.makedirs(OUT, exist_ok=True)
    parts = []
    for ld in sorted(glob.glob("linkers/USA/maps/map*_s*.ld")):
        name = os.path.basename(ld)[:-3]
        merged = "%s/%s.o" % (OUT, name)
        run([LD, "-EL", "-r", "-o", merged, *objects(ld)])
        hdr = "g_MapOverlayHdr_" + name
        run([OBJCOPY, "--redefine-sym", "g_MapOverlayHdr=" + hdr, "--keep-global-symbol", hdr, merged])
        parts.append(merged)
    # Screen overlays (and B_KONAMI) share an address region too: keep only what main/bodyprog use.
    core_objs = [o for ld in ("linkers/USA/main.ld", "linkers/USA/bodyprog.ld") for o in objects(ld)]
    wanted = {l.split()[-1] for l in run([NM, "-u", *core_objs]).stdout.splitlines() if l.strip() and ":" not in l}
    for ld in sorted(glob.glob("linkers/USA/screens/*.ld")):
        name = os.path.basename(ld)[:-3]
        merged = "%s/%s.o" % (OUT, name)
        run([LD, "-EL", "-r", "-o", merged, *objects(ld)])
        defined = {f[2] for f in (l.split() for l in run([NM, "--defined-only", merged]).stdout.splitlines()) if len(f) == 3}
        keep = sorted(defined & wanted)
        run([OBJCOPY, *[a for k in keep for a in ("--keep-global-symbol", k)], merged])
        print("%-9s exports %s" % (name, ", ".join(keep)))
        parts.append(merged)
    combined = OUT + "/all.o"
    r = subprocess.run([LD, "-EL", "-r", "-o", combined, *core_objs, *parts], capture_output=True, text=True)
    dups = sorted(set(re.findall(r"multiple definition of `([^']+)'", r.stderr)))
    print("duplicate definitions: %d" % len(dups))
    for d in dups[:40]:
        print("  " + d)
    if r.returncode and not dups:
        sys.exit(r.stderr[-3000:])
    if r.returncode:
        return 1
    und = sorted({l.split()[-1] for l in run([NM, "-u", combined]).stdout.splitlines() if l.strip()})
    print("undefined symbols left: %d" % len(und))
    for u in und:
        print("  " + u)
    return 0


if __name__ == "__main__":
    sys.exit(main())
