#!/usr/bin/env python3
"""First PS2 link of the whole game, to inventory what the HAL must provide.

Inputs (from the USA linker scripts in linkers/, so run `make setup` first):
  - C files: the EE objects from tools/port/ee_compile_check.sh (build/ee_check/).
  - splat data asm (asm/USA/**/data/*.s): assembled here with the EE toolchain.
  - src/port/*.c: compiled here.
  - Left out on purpose: Sony's prebuilt PS1 libraries (lib/*.o, replaced by the HAL), the PS-EXE
    header and the footer_data padding blobs.

Linking follows tools/port/static_link_poc.py: each map is merged on its own, its g_MapOverlayHdr
renamed g_MapOverlayHdr_<map> and everything else made local; screen overlays keep only the symbols
other parts reference. Then everything is linked against ps2sdk's crt0/libc, and the undefined
symbols are reported, grouped by the Sony library (lib/<name>/) that defined them on PS1.

Output: build/port/ (objects, all.o, link log). Exit status 0 only if the link succeeds.
"""
import collections
import glob
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

PS2DEV = os.environ.get("PS2DEV", os.path.expanduser("~/ps2dev"))
PS2SDK = os.environ.get("PS2SDK", os.path.join(PS2DEV, "ps2sdk"))
GSKIT = os.environ.get("GSKIT", os.path.join(PS2DEV, "gsKit"))
EE = os.path.join(PS2DEV, "ee", "bin", "mips64r5900el-ps2-elf-")
CC, LD, OBJCOPY, NM = EE + "gcc", EE + "ld", EE + "objcopy", EE + "nm"
OUT = "build/port"
EE_C_OBJS = "build/ee_check"
# -mno-check-zero-division: as on the PS1, an integer division by zero must not trap (the game
# relies on it, e.g. sound falloff 0 in map4_s02's demo); MIPS divides return a fixed quotient.
# -fno-strict-aliasing: decompiled code type-puns freely (e.g. GTE macros read s16 matrices as
# words); the PS1's GCC 2.8 never assumed otherwise.
PORT_CFLAGS = ["-O2", "-G0", "-fno-toplevel-reorder", "-mno-check-zero-division", "-fno-strict-aliasing", "-std=gnu89", "-nostdinc", "-Wa,-Iinclude", "-Iinclude", "-Iinclude/psyq", "-Iinclude/decomp",
               "-D_LANGUAGE_C", "-DVER_USA", "-DSH_PORT", "-DNON_MATCHING"]


def run(cmd, check=True):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit("FAILED: %s\n%s" % (" ".join(cmd[:4]), r.stderr[-4000:]))
    return r


def ld_objects(ld_script):
    seen, objs = set(), []
    for o in re.findall(r"([\w./-]+?\.o)\(", open(ld_script).read()):
        if o not in seen:
            seen.add(o)
            objs.append(o)
    return objs


# Hand-written asm in src/: PS1 startup (replaced by ps2sdk's crt0) and Konami's math library (has
# GTE instructions; needs a C version). Left out; libkmath's symbols are reported as their own group.
HAND_ASM = {"build/USA/src/main/libsn/snmain.s.o": "snmain", "build/USA/src/bodyprog/libkmath/libkmath.s.o": "libkmath"}


def ee_object(ps1_obj):
    """PS1 object path from a linker script -> EE object path, or None if it's left out."""
    if ps1_obj.startswith("lib/") or ps1_obj.endswith(".bin.o") or ps1_obj.endswith("main/header.s.o"):
        return None
    if ps1_obj in HAND_ASM:
        return None
    m = re.match(r"build/USA/(src/.+\.c)\.o$", ps1_obj)
    if m:
        p = os.path.join(EE_C_OBJS, m.group(1).replace("/", "_") + ".o")
        if not os.path.exists(p):
            sys.exit("missing EE object %s (run tools/port/ee_compile_check.sh)" % p)
        return p
    m = re.match(r"build/USA/(asm/.+\.s)\.o$", ps1_obj)
    if m:
        return os.path.join(OUT, m.group(1) + ".o")
    sys.exit("unknown object kind: " + ps1_obj)


def assemble(src_obj_pairs):
    def one(pair):
        src, obj = pair
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        if not os.path.exists(obj) or os.path.getmtime(obj) < os.path.getmtime(src):
            run([CC, "-c", "-G0", "-Wa,-Iinclude", "-x", "assembler", src, "-o", obj])
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        list(ex.map(one, src_obj_pairs))


def nm_set(args):
    return {l.split()[-1] for l in run([NM, *args]).stdout.splitlines() if l.strip() and not l.endswith(":")}


def defined(objs):
    return {f[2] for f in (l.split() for l in run([NM, "--defined-only", *objs]).stdout.splitlines()) if len(f) == 3}


# name -> (anchor, offset) from configs/USA/relative_syms.ld
ALIASES = {m.group(1): (m.group(2), int(m.group(3), 16)) for m in re.finditer(
    r'PROVIDE\("(\w+)" = "([\w.]+)" \+ (0x[0-9A-Fa-f]+)\)', open("configs/USA/relative_syms.ld").read())}


def rename_refs(obj, renames, outdir):
    und = nm_set(["-u", obj])
    hits = {a: b for a, b in renames.items() if a in und}
    if not hits:
        return obj
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, os.path.basename(obj))
    run([OBJCOPY, *[x for a, b in hits.items() for x in ("--redefine-sym", "%s=%s" % (a, b))], obj, out])
    return out


def sony_library_index():
    """Symbol -> Sony library name, from the prebuilt PS1 objects the matching build links."""
    index = {}
    for o in sorted(glob.glob("lib/*/*.o")):
        lib = o.split("/")[1]
        out = subprocess.run(["mips-linux-gnu-nm", "--defined-only", o], capture_output=True, text=True).stdout
        for f in (l.split() for l in out.splitlines()):
            if len(f) == 3 and f[1].isupper():
                index.setdefault(f[2], lib)
    for o, name in HAND_ASM.items():
        src = o[len("build/USA/"):-2]
        for sym in re.findall(r"^\s*glabel\s+(\w+)", open(src).read(), re.M):
            index.setdefault(sym, name)
    return index


def main():
    os.makedirs(OUT, exist_ok=True)
    scripts = {"main": "linkers/USA/main.ld", "bodyprog": "linkers/USA/bodyprog.ld"}
    for ld in sorted(glob.glob("linkers/USA/screens/*.ld")) + sorted(glob.glob("linkers/USA/maps/map*_s*.ld")):
        scripts[os.path.basename(ld)[:-3]] = ld

    # Assemble every data .s any binary uses.
    pairs = {}
    for ld in scripts.values():
        for o in ld_objects(ld):
            m = re.match(r"build/USA/(asm/.+\.s)\.o$", o)
            if m and not o.endswith("main/header.s.o"):
                pairs[m.group(1)] = ee_object(o)
    assemble(sorted(pairs.items()))

    objs = {name: [p for p in (ee_object(o) for o in ld_objects(ld)) if p] for name, ld in scripts.items()}

    # Port runtime sources, and Sony/Konami library code recompiled by tools/port/recomp_all.sh.
    port_objs = []
    # Renderer: SH1_GPU=gs (default), soft, or compare (both; debug dumps compare them).
    gpu = {"soft": 0, "gs": 1, "compare": 2}[os.environ.get("SH1_GPU", "gs")]
    # Profiling counters (include/port/prof.h): SH1_PROF=1.
    prof = ["-DSH_PORT_PROF"] if os.environ.get("SH1_PROF") else []
    for src in sorted(glob.glob("src/port/*.c")):
        obj = os.path.join(OUT, "port", os.path.basename(src) + ".o")
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        run([CC, "-c", *PORT_CFLAGS, *prof, "-DSH_PORT_GPU=%d" % gpu, src, "-o", obj])
        port_objs.append(obj)
    # PS2-side HAL code (src/port/ps2/): compiled against ps2sdk's headers, not the game's.
    for src in sorted(glob.glob("src/port/ps2/*.c")):
        obj = os.path.join(OUT, "port", "ps2_" + os.path.basename(src) + ".o")
        run([CC, "-c", "-O2", "-G0", "-D_EE", "-Wall", *prof, "-I" + os.path.join(PS2SDK, "ee", "include"),
             "-I" + os.path.join(PS2SDK, "common", "include"), "-I" + os.path.join(GSKIT, "include"),
             "-iquote", "include", src, "-o", obj])
        port_objs.append(obj)
    if not os.environ.get("SH1_NO_RECOMP"):
        run(["tools/port/recomp_all.sh"])
    recomp_objs = []
    for src in sorted(glob.glob(OUT + "/recomp/*.c")):
        run([CC, "-c", "-O2", "-G0", "-std=gnu89", "-fno-builtin", "-fno-strict-aliasing", "-nostdinc", "-Iinclude", *prof, src,
             "-o", src[:-2] + ".o"])
        run([CC, "-c", "-G0", src[:-2] + ".data.s", "-o", src[:-2] + ".data.o"])
        recomp_objs += [src[:-2] + ".o", src[:-2] + ".data.o"]
    # As an archive: only the recompiled objects something references are linked.
    recomp_lib = OUT + "/librecomp.a"
    if os.path.exists(recomp_lib):
        os.remove(recomp_lib)
    run([EE + "ar", "rcs", recomp_lib, *recomp_objs])

    # Overlay data reset (src/port/overlay.c): marker objects around each overlay's objects give the
    # bounds of its .data and .bss once merged (ld -r keeps input order within a section).
    def markers(name):
        out = []
        for kind in ("start", "end"):
            asm = "%s/markers/%s_%s.s" % (OUT, name, kind)
            os.makedirs(os.path.dirname(asm), exist_ok=True)
            open(asm, "w").write("".join(
                "\t.section %s\n\t.globl __ovl_%s_%s_%s\n__ovl_%s_%s_%s:\n" % (sec, name, tag, kind, name, tag, kind)
                for sec, tag in ((".data", "data"), (".bss", "bss"))))
            obj = asm[:-2] + ".o"
            run([CC, "-c", asm, "-o", obj])
            out.append(obj)
        return out

    OVERLAY_FILES = {"b_konami": "FILE_1ST_B_KONAMI_BIN", "options": "FILE_VIN_OPTION_BIN",
                     "saveload": "FILE_VIN_SAVELOAD_BIN", "credits": "FILE_VIN_STF_ROLL_BIN",
                     "stream": "FILE_VIN_STREAM_BIN"}
    overlays = []

    parts = []
    for name in sorted(n for n in objs if n.startswith("map")):
        o = objs[name]
        d = defined(o)
        renames = {a: b for a, (b, off) in ALIASES.items() if off == 0 and b in d}
        if renames:
            o = [rename_refs(x, renames, "%s/renamed/%s" % (OUT, name)) for x in o]
        merged = "%s/merged/%s.o" % (OUT, name)
        os.makedirs(os.path.dirname(merged), exist_ok=True)
        start, end = markers(name)
        run([LD, "-r", "-o", merged, start, *o, end])
        hdr = "g_MapOverlayHdr_" + name
        keepmarks = [a for k in ("data", "bss") for e in ("start", "end") for a in ("--keep-global-symbol", "__ovl_%s_%s_%s" % (name, k, e))]
        run([OBJCOPY, "--redefine-sym", "g_MapOverlayHdr=" + hdr, "--keep-global-symbol", hdr, *keepmarks, merged])
        overlays.append((name, "FILE_VIN_%s_BIN" % name.upper()))
        parts.append(merged)

    core = objs["main"] + objs["bodyprog"]
    wanted = nm_set(["-u", *core, *parts])
    for name in sorted(n for n in objs if n not in ("main", "bodyprog") and not n.startswith("map")):
        merged = "%s/merged/%s.o" % (OUT, name)
        start, end = markers(name)
        run([LD, "-r", "-o", merged, start, *objs[name], end])
        keep = sorted(defined([merged]) & wanted)
        keep += ["__ovl_%s_%s_%s" % (name, k, e) for k in ("data", "bss") for e in ("start", "end")]
        run([OBJCOPY, *[a for k in keep for a in ("--keep-global-symbol", k)], merged])
        parts.append(merged)
        if name in OVERLAY_FILES:
            overlays.append((name, OVERLAY_FILES[name]))

    # Table of overlay data ranges for src/port/overlay.c.
    table = OUT + "/overlay_ranges.c"
    with open(table, "w") as f:
        f.write("/* Generated by tools/port/port_link.py: overlay .data/.bss ranges (src/port/overlay.c). */\n")
        f.write('#include "game.h"\n#include "main/fileinfo.h"\n#include "port/overlay.h"\n\n')
        for name, _ in overlays:
            f.write("extern char " + ", ".join("__ovl_%s_%s_%s[]" % (name, k, e) for k in ("data", "bss") for e in ("start", "end")) + ";\n")
        f.write("\nconst Port_OverlayRange g_PortOverlayRanges[] = {\n")
        for name, fid in overlays:
            f.write('    { %s, "%s", __ovl_%s_data_start, __ovl_%s_data_end, __ovl_%s_bss_start, __ovl_%s_bss_end },\n'
                    % (fid, name, name, name, name, name))
        f.write("};\nconst int g_PortOverlayRangeCount = %d;\n" % len(overlays))
    run([CC, "-c", *PORT_CFLAGS, table, "-o", table[:-2] + ".o"])
    port_objs.append(table[:-2] + ".o")

    combined = OUT + "/all.o"
    r = run([LD, "-r", "-o", combined, *core, *parts], check=False)
    dups = sorted(set(re.findall(r"multiple definition of `([^']+)'", r.stderr)))
    if r.returncode:
        print("game objects don't link together: %d duplicate definitions" % len(dups))
        print("  " + "\n  ".join(dups[:50]) if dups else r.stderr[-3000:])
        return 1
    # The game's PS1 main() is called by the port's main() (src/port/port_main.c).
    run([OBJCOPY, "--redefine-sym", "main=Game_PsxMain", combined])

    # Final link against ps2sdk's runtime.
    elf = OUT + "/sh1.elf"

    def final_link(extra):
        return run([CC, "-T", os.path.join(PS2SDK, "ee", "startup", "linkfile"), "-L", os.path.join(PS2SDK, "ee", "lib"),
                    "-Wl,-zmax-page-size=128", "-Wl,--unresolved-symbols=report-all", "-o", elf,
                    combined, *port_objs, *extra, "-Wl,--start-group", recomp_lib, "-Wl,--end-group",
                    "configs/USA/relative_syms.ld", "configs/USA/port_relative_syms.ld", "configs/USA/port_syms.ld",
                    "-L" + os.path.join(GSKIT, "lib"), "-lgskit", "-ldmakit", "-lcdvd", "-lpad", "-leedebug"],
                   check=False)

    r = final_link([])
    open(OUT + "/link.log", "w").write(r.stderr)
    und = sorted(set(re.findall(r"undefined reference to `([^']+)'", r.stderr)))
    dups = sorted(set(re.findall(r"multiple definition of `([^']+)'", r.stderr)))
    other = [l for l in r.stderr.splitlines() if "error" in l.lower() and "undefined reference" not in l
             and "multiple definition" not in l and "ld returned 1 exit status" not in l]

    sony = sony_library_index()
    by_lib = collections.defaultdict(list)
    for u in und:
        by_lib[sony.get(u, "(not a Sony library symbol)")].append(u)
    print("undefined symbols: %d" % len(und))
    for lib in sorted(by_lib, key=lambda k: (-len(by_lib[k]), k)):
        print("  %-28s %3d  %s" % (lib, len(by_lib[lib]), " ".join(by_lib[lib])))
    if dups:
        print("duplicate definitions against ps2sdk/libc: %d\n  %s" % (len(dups), " ".join(dups)))
    if other:
        print("other errors:\n  " + "\n  ".join(other[:20]))
    print("full log: %s/link.log" % OUT)
    if r.returncode == 0:
        print("linked: " + elf)
        return 0
    if dups or other or not und:
        return 1

    # Stub every remaining HAL function so the game links and can be run: each logs its first calls.
    stubs = OUT + "/hal_stubs.c"
    with open(stubs, "w") as f:
        f.write("/* Generated by tools/port/port_link.py: HAL functions not implemented yet. */\n"
                "extern int printf(const char* fmt, ...);\n"
                "static void hal_stub_log(const char* name, int* count)\n{\n"
                "    if (*count < 3) printf(\"HAL stub: %s\\n\", name);\n    (*count)++;\n}\n"
                "#define STUB(name) unsigned int name() { static int n; hal_stub_log(#name, &n); return 0; }\n")
        for u in und:
            f.write("STUB(%s)\n" % u)
    run([CC, "-c", "-O2", "-G0", "-std=gnu89", "-fno-builtin", stubs, "-o", stubs[:-2] + ".o"])
    r = final_link([stubs[:-2] + ".o"])
    open(OUT + "/link.log", "w").write(r.stderr)
    if r.returncode:
        print("link with stubs failed:\n" + r.stderr[-3000:])
        return 1
    print("linked with %d HAL stubs: %s" % (len(und), elf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
