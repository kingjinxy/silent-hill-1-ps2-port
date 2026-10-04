#!/usr/bin/env python3
"""Step 2 proof of concept: link every binary's objects into one relocatable object.

Each map is first linked on its own (`ld -r`), its `g_MapOverlayHdr` renamed to
`g_MapOverlayHdr_<map>`, and every other symbol it defines made local, so the 43 maps' duplicate
names (sharedFunc_*, sharedData_*, Map_WorldObjectsInit, ...) can't collide. Then main, bodyprog,
the screen overlays (keeping only what other parts reference) and all maps are linked together and the remaining undefined symbols listed.

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


# name -> (anchor, offset) from configs/USA/relative_syms.ld
ALIASES = {m.group(1): (m.group(2), int(m.group(3), 16)) for m in re.finditer(
    r'PROVIDE\("(\w+)" = "([\w.]+)" \+ (0x[0-9A-Fa-f]+)\)', open("configs/USA/relative_syms.ld").read())}


def rename_refs(obj, renames, outdir):
    """Copy obj to outdir with undefined references renamed (only if it has any)."""
    und = {l.split()[-1] for l in run([NM, "-u", obj]).stdout.splitlines() if l.strip()}
    hits = {a: b for a, b in renames.items() if a in und}
    if not hits:
        return obj
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, os.path.basename(obj))
    run([OBJCOPY, *[x for a, b in hits.items() for x in ("--redefine-sym", "%s=%s" % (a, b))], obj, out])
    return out


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
        objs = objects(ld)
        # A map's own aliases (offset 0, e.g. map1_s04 D_800CD768_tbl = D_800CD768) must resolve
        # before its symbols are hidden; linker-script expressions turn ABS in an `ld -r` link, so
        # rename the references in copies of the input objects instead.
        defined = {f[2] for f in (l.split() for l in run([NM, "--defined-only", *objs]).stdout.splitlines()) if len(f) == 3}
        renames = {a: b for a, (b, off) in ALIASES.items() if off == 0 and b in defined}
        if renames:
            objs = [rename_refs(o, renames, "%s/%s_objs" % (OUT, name)) for o in objs]
        run([LD, "-EL", "-r", "-o", merged, *objs])
        hdr = "g_MapOverlayHdr_" + name
        run([OBJCOPY, "--redefine-sym", "g_MapOverlayHdr=" + hdr, "--keep-global-symbol", hdr, merged])
        parts.append(merged)
    # Screen overlays (and B_KONAMI) share an address region too: keep only what other parts use
    # (main/bodyprog call their state functions; map7_s03 and map6_s02 call into STF_ROLL).
    core_objs = [o for ld in ("linkers/USA/main.ld", "linkers/USA/bodyprog.ld") for o in objects(ld)]
    wanted = {l.split()[-1] for l in run([NM, "-u", *core_objs, *parts]).stdout.splitlines() if l.strip() and ":" not in l}
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
    # Final (non-relocatable) link with the relative-symbol scripts as implicit scripts, so the
    # default layout is kept and the PROVIDE expressions are computed from the real anchors.
    r = subprocess.run([LD, "-EL", "-o", OUT + "/all.elf", "-e", "0", "--unresolved-symbols=report-all",
                        combined, "configs/USA/relative_syms.ld", "configs/USA/port_relative_syms.ld"],
                       capture_output=True, text=True)
    und = sorted(set(re.findall(r"undefined reference to `([^']+)'", r.stderr)))
    print("undefined symbols in final link: %d" % len(und))
    for u in und:
        print("  " + u)
    return 0


if __name__ == "__main__":
    sys.exit(main())
