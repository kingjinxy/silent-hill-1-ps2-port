#!/usr/bin/env python3
"""Field layout of a game struct, from DWARF: compiles a one-line file declaring it with the port's
EE compiler (-g) and walks the debug info. Used by compare_frames.py to name differing bytes.

    python3 tools/port/struct_layout.py s_SysWork
"""
import os
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
CC = os.path.join(os.environ.get("PS2DEV", os.path.expanduser("~/ps2dev")), "ee", "bin", "mips64r5900el-ps2-elf-gcc")


def layout(type_name, headers=("game.h", "bodyprog/bodyprog.h")):
    """[(offset, size, "field.path")] for every leaf field of `type_name`."""
    with tempfile.TemporaryDirectory() as tmp:
        src, obj = os.path.join(tmp, "t.c"), os.path.join(tmp, "t.o")
        open(src, "w").write("".join('#include "%s"\n' % h for h in headers) + "%s g_LayoutProbe;\n" % type_name)
        subprocess.check_call([CC, "-c", "-g", "-O0", "-G0", "-std=gnu89", "-nostdinc", "-Iinclude", "-Ibuild/USA",
                               "-Iinclude/psyq", "-Iinclude/decomp", "-Wa,-Iinclude", "-D_LANGUAGE_C", "-DVER_USA",
                               "-DSH_PORT", "-DNON_MATCHING", "-w", src, "-o", obj], cwd=REPO)
        elf = ELFFile(open(obj, "rb"))
        dwarf = elf.get_dwarf_info()
        dies = {}
        for cu in dwarf.iter_CUs():
            for die in cu.iter_DIEs():
                dies[die.offset] = die

        def target(die):
            return dies[die.attributes["DW_AT_type"].value + die.cu.cu_offset] if "DW_AT_type" in die.attributes else None

        def strip(t):
            while t is not None and t.tag in ("DW_TAG_typedef", "DW_TAG_volatile_type", "DW_TAG_const_type"):
                t = target(t)
            return t

        def size(t):
            t = strip(t)
            if t is None:
                return 0
            if "DW_AT_byte_size" in t.attributes:
                return t.attributes["DW_AT_byte_size"].value
            if t.tag == "DW_TAG_array_type":
                n = 1
                for sub in t.iter_children():
                    if "DW_AT_upper_bound" in sub.attributes:
                        n *= sub.attributes["DW_AT_upper_bound"].value + 1
                return n * size(target(t))
            return 4

        out = []

        def walk(t, base, path):
            t = strip(t)
            if t is None:
                return
            if t.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") and t.tag != "DW_TAG_union_type":
                for m in t.iter_children():
                    if m.tag != "DW_TAG_member":
                        continue
                    off = m.attributes["DW_AT_data_member_location"].value if "DW_AT_data_member_location" in m.attributes else 0
                    name = m.attributes["DW_AT_name"].value.decode() if "DW_AT_name" in m.attributes else "?"
                    walk(target(m), base + off, path + "." + name if path else name)
            elif t.tag == "DW_TAG_array_type" and size(target(t)) and size(t) // size(target(t)) <= 64:
                el = size(target(t))
                for i in range(size(t) // el):
                    walk(target(t), base + i * el, "%s[%d]" % (path, i))
            else:
                out.append((base, size(t), path))

        for die in dies.values():
            if die.tag == "DW_TAG_variable" and die.attributes.get("DW_AT_name") and \
                    die.attributes["DW_AT_name"].value == b"g_LayoutProbe":
                walk(target(die), 0, "")
                break
        return sorted(out)


if __name__ == "__main__":
    for off, sz, path in layout(sys.argv[1] if len(sys.argv) > 1 else "s_SysWork"):
        print("%05X %4d %s" % (off, sz, path))
