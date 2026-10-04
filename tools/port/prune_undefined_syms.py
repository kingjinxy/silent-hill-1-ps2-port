#!/usr/bin/env python3
"""Keep the binary's own symbols out of splat's absolute-address symbol lists.

Each binary is linked with linker scripts that assign symbols fixed PS1 addresses
(undefined_{syms,funcs}_auto.*.txt from splat, plus configs/USA/lib_externs.ld). A script assignment
overrides an object's own definition, so anything listed there is ABS: it stays at its PS1 address
even if the code around it moves, which breaks shifting and later static linking.

prune <linker.ld> <relative_syms.ld> <in> <out> [<in> <out> ...]
    Copy each <in> to <out> without the symbols the binary's objects define themselves, or that
    <relative_syms.ld> (configs/USA/relative_syms.ld) defines relative to another symbol.

relativize <linker.ld> <elf> <segment> <out> <undefined.txt> ...
    Maintenance, run on a *matching* build (offsets must come from the PS1 layout): for every ABS symbol the binary's objects reference that lies inside
    <segment>'s own range (e.g. a byte inside a bss variable, or a gap the linker script only
    reserves with `. += N`), write `name = <preceding symbol> + offset;` to <out> and remove the
    name from the given <undefined.txt> files. Relinking then gives identical addresses in the
    matching build, but those symbols move with their neighbours. Merge the results into
    configs/USA/relative_syms.ld as PROVIDE(...) entries.
"""
import os
import re
import subprocess
import sys

NM = os.environ.get("NM", "mips-linux-gnu-nm")
READELF = os.environ.get("READELF", "mips-linux-gnu-readelf")
ASSIGN = re.compile(r"\s*(?:PROVIDE\s*\(\s*)?(\w+)\s*=")


def objects(ld_script):
    return sorted(set(re.findall(r"([\w./-]+?\.o)\(", open(ld_script).read())))


def nm(args):
    return subprocess.run([NM, *args], capture_output=True, text=True, check=True).stdout


def prune(ld_script, relative_syms, pairs):
    defined = {f[2] for f in (l.split() for l in nm(["--defined-only", *objects(ld_script)]).splitlines()) if len(f) == 3}
    defined |= set(re.findall(r'PROVIDE\(\s*"?(\w+)"?\s*=', open(relative_syms).read()))
    for src, dst in zip(pairs[::2], pairs[1::2]):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w") as out:
            for line in open(src):
                m = ASSIGN.match(line)
                if not (m and m.group(1) in defined):
                    out.write(line)


def relativize(ld_script, elf, segment, out_path, undefined_files):
    referenced = {l.split()[-1] for l in nm(["-u", *objects(ld_script)]).splitlines() if l.strip()}
    symtab = subprocess.run([READELF, "-sW", elf], capture_output=True, text=True, check=True).stdout
    syms = []
    for f in (l.split() for l in symtab.splitlines()):
        if len(f) >= 8 and f[0].endswith(":") and f[3] in ("OBJECT", "FUNC", "NOTYPE"):
            syms.append((int(f[1], 16), f[6], f[7]))
    value = {name: addr for addr, _, name in syms}
    lo, hi = value["%s_VRAM" % segment], value["%s_VRAM_END" % segment]
    # Candidate anchors: real (section-relative) symbols of this binary, excluding linker markers.
    anchors = sorted((a, n) for a, ndx, n in syms
                     if ndx not in ("ABS", "UND") and lo <= a < hi
                     and not re.search(r"_(VRAM|ROM|START|END|SIZE)$|\.NON_MATCHING$|^_gp$", n))
    lines, moved = [], set()
    for addr, ndx, name in sorted(syms):
        if ndx != "ABS" or name not in referenced or not (lo <= addr < hi):
            continue
        before = [an for an in anchors if an[0] <= addr]
        if not before:
            continue
        base_addr, base = before[-1]
        lines.append('"%s" = "%s" + 0x%X;\n' % (name, base, addr - base_addr))
        moved.add(name)
    with open(out_path, "w") as out:
        out.writelines(lines)
    for path in undefined_files:
        kept = [l for l in open(path) if not ((m := ASSIGN.match(l)) and m.group(1) in moved)]
        open(path, "w").writelines(kept)


def main():
    mode, args = sys.argv[1], sys.argv[2:]
    if mode == "prune":
        prune(args[0], args[1], args[2:])
    elif mode == "relativize":
        relativize(args[0], args[1], args[2], args[3], args[4:])
    else:
        sys.exit("unknown mode " + mode)


if __name__ == "__main__":
    main()
