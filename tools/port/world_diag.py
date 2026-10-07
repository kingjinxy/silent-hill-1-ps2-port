#!/usr/bin/env python3
"""World streaming diagnosis from a memory image: the active map chunks, the chunk texture slots
(name, file queue entry, reference count) and every model material without a texture (which keeps a
map load waiting forever: WorldGfx_ChunkInitCheck).

Memory images:
    --ps2-state FILE   PCSX2 save state (.p2s; ~/.config/PCSX2/sstates/), port build
    --ps2-ram FILE     host:ramdump.bin written by the port's load watchdog (src/port/ps2/debug_ps2.c)
    --ps1-ram FILE     2 MB PS1 RAM (tools/port/duckstation_world_watch.py writes these)

    python3 tools/port/world_diag.py --ps2-state "~/.config/PCSX2/sstates/SHPS-00001 (8924831E).01.p2s"

PS2 symbol addresses come from build/port/sh1.elf (the image must come from that build); PS1 ones
from configs/USA/sym.*.txt.
"""
import argparse
import os
import struct
import subprocess
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
NM = os.path.join(os.environ.get("PS2DEV", os.path.expanduser("~/ps2dev")), "ee", "bin", "mips64r5900el-ps2-elf-nm")

PS1_SYMS = {"g_WorldMapWork": 0x800C1020, "g_FsQueue": 0x80022C98, "g_SysWork": 0x800B9FC0,
            "g_GameWork": 0x800BC728}

# s_WorldMapWork (include/bodyprog/map/terrain.h)
WM_GLOBAL_LM, WM_GLOBAL_QUEUE, WM_TAG = 0x138, 0x140, 0x144
WM_CHUNK_COUNT, WM_CHUNKS, CHUNK_SIZE = 0x158, 0x15C, 28
WM_TEXTURES = 0x430   # s_ChunkTextures: fullPage {count, textures[10]}, halfPage, then the slots
WM_POS = 0x578
TEXTURE_SIZE = 0x18   # s_Texture: imageDesc 8, name 8, queueIdx 4, refCount 1
MATERIAL_SIZE = 0x18  # s_Material: name 8, texture 4, field_C 1


class Image:
    def __init__(self, data, mask, syms, ptr_ok):
        self.m, self.mask, self.syms, self.ptr_ok = data, mask, syms, ptr_ok

    def u8(self, a):
        return self.m[a & self.mask]

    def s8(self, a):
        return struct.unpack_from("<b", self.m, a & self.mask)[0]

    def u32(self, a):
        return struct.unpack_from("<I", self.m, a & self.mask)[0]

    def s32(self, a):
        return struct.unpack_from("<i", self.m, a & self.mask)[0]

    def name(self, a):
        return self.m[a & self.mask:(a & self.mask) + 8].split(b"\0")[0].decode(errors="replace")


def zstd_member(path, member):
    """A member of a PCSX2 save state (zip; members are zstd compressed, which zipfile can't read)."""
    info = zipfile.ZipFile(path).getinfo(member)
    with open(path, "rb") as f:
        f.seek(info.header_offset)
        h = f.read(30)
        f.seek(info.header_offset + 30 + int.from_bytes(h[26:28], "little") + int.from_bytes(h[28:30], "little"))
        data = f.read(info.compress_size)
    if info.compress_type == 0:
        return data
    return subprocess.run(["zstd", "-d", "-c"], input=data, capture_output=True, check=True).stdout


def ps2_syms():
    out = subprocess.run([NM, os.path.join(REPO, "build", "port", "sh1.elf")], capture_output=True, text=True).stdout
    return {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}


FS_POST_LOAD_IDX = 0x410  # s_FsQueue.postLoad.idx (main/fsqueue.h; tools/port/struct_layout.py s_FsQueue)


def fs_post_load_idx(img):
    """g_FsQueue.postLoad.idx (entries below it are loaded: Fs_QueueIsEntryLoaded)."""
    return img.s32(img.syms["g_FsQueue"] + FS_POST_LOAD_IDX)


def diagnose(img):
    wm = img.syms["g_WorldMapWork"]
    post = fs_post_load_idx(img)
    lines = []
    tag = img.m[(wm + WM_TAG) & img.mask:(wm + WM_TAG + 4) & img.mask].split(b"\0")[0].decode(errors="replace")
    lines.append("map %s, world sample position (%.2f, %.2f); file queue entries below %d are loaded" % (
        tag, img.s32(wm + WM_POS) / 4096, img.s32(wm + WM_POS + 4) / 4096, post))
    gw = img.syms.get("g_GameWork")
    slots = {}
    for i in range(10):
        a = wm + WM_TEXTURES + 0x58 + i * TEXTURE_SIZE
        slots[a & img.mask] = i
    refs = {}
    missing = []

    def materials(lm, owner):
        if not img.ptr_ok(lm):
            return
        for k in range(img.u8(lm + 3)):
            mat = img.u32(lm + 4) + k * MATERIAL_SIZE
            tex = img.u32(mat + 8)
            if img.u8(mat + 0xC):
                continue
            if tex == 0:
                missing.append("%s: material %s has no texture" % (owner, img.name(mat)))
            elif (tex & img.mask) in slots:
                refs.setdefault(tex & img.mask, []).append(owner)
                if not img.u32(tex + 0x10) < post:
                    missing.append("%s: material %s's texture not loaded yet (queue entry %d)" % (
                        owner, img.name(mat), img.u32(tex + 0x10)))

    lm = img.u32(wm + WM_GLOBAL_LM)
    lines.append("global model file: queue entry %d (%s)" % (
        img.s32(wm + WM_GLOBAL_QUEUE), "loaded" if img.s32(wm + WM_GLOBAL_QUEUE) < post else "NOT loaded"))
    materials(lm, "global")
    n = img.s32(wm + WM_CHUNK_COUNT)
    for i in range(4):
        c = wm + WM_CHUNKS + i * CHUNK_SIZE
        ipd, q = img.u32(c), img.s32(c + 4)
        cx, cz = struct.unpack_from("<hh", img.m, (c + 8) & img.mask)
        d0, d1 = img.s32(c + 12) / 4096, img.s32(c + 16) / 4096
        state = "unused" if q == -1 else ("loaded" if q < post else "loading")
        loaded = img.ptr_ok(ipd) and q != -1 and q < post and img.u8(ipd + 1)
        lines.append("chunk %d%s: (%d, %d) queue entry %d %s, padded distance %.2f / %.2f%s" % (
            i, "" if i < n else " (beyond active count)", cx, cz, q, state, d0, d1,
            "" if loaded or q == -1 else ", header not set up"))
        if loaded:
            materials(img.u32(ipd + 4), "chunk %d (%d, %d)" % (i, cx, cz))
    for label, off in (("full-page", 0), ("half-page", 0x2C)):
        base = wm + WM_TEXTURES + off
        cnt = img.s32(base)
        lines.append("%s texture slots: %d in use" % (label, cnt))
        for k in range(min(max(cnt, 0), 10)):
            t = img.u32(base + 4 + 4 * k)
            lines.append("  %-8s queue entry %3d, %s, refCount %d, used by %s" % (
                img.name(t + 8), img.u32(t + 0x10), "loaded" if img.u32(t + 0x10) < post else "loading",
                img.s8(t + 0x14), ", ".join(refs.get(t & img.mask, [])) or "nothing"))
    lines.append("materials without a usable texture: %s" % ("none" if not missing else ""))
    lines += ["  " + m for m in missing]
    if gw is not None:
        lines.insert(0, "gameState %d, step %d" % (img.s32(gw + 0x594), img.s32(gw + 0x598)))
    return "\n".join(lines)


def load(args):
    if args.ps2_state:
        return Image(zstd_member(os.path.expanduser(args.ps2_state), "eeMemory.bin"), 0x1FFFFFF, ps2_syms(),
                     lambda a: 0x100000 <= (a & 0x1FFFFFF) < 0x2000000)
    if args.ps2_ram:
        return Image(open(args.ps2_ram, "rb").read(), 0x1FFFFFF, ps2_syms(),
                     lambda a: 0x100000 <= (a & 0x1FFFFFF) < 0x2000000)
    return Image(open(args.ps1_ram, "rb").read(), 0x1FFFFF, PS1_SYMS,
                 lambda a: (a >> 24) in (0x00, 0x80) and (a & 0xFFFFFF) < 0x200000 and a & 0x1FFFFF)


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--ps2-state")
    g.add_argument("--ps2-ram")
    g.add_argument("--ps1-ram")
    print(diagnose(load(ap.parse_args())))


if __name__ == "__main__":
    main()
