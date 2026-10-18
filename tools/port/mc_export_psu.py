#!/usr/bin/env python3
"""Exports a save folder from a PS2 memory card image (PCSX2's .ps2 files) to a .psu file, the format
uLaunchELF writes and reads ("psu Paste"), to copy it to a real memory card.

    python3 tools/port/mc_export_psu.py ~/.config/PCSX2/memcards/Mcd001.ps2 BASLUS-00707SH1 out.psu
    python3 tools/port/mc_export_psu.py CARD.ps2 --list

The card's file system: 512-byte pages (528 with the 16-byte ECC area, as PCSX2 stores them), 1 KB
clusters, a superblock in page 0, a FAT reached through a list of indirect FAT clusters, and
directories made of 512-byte entries. A .psu is the folder's entry (its length = the number of
entries, "." and ".." included), the "." and ".." entries, then each file's entry followed by its data
padded to 1 KB; entries keep the card's modes and timestamps, as uLaunchELF copies them.
"""
import argparse
import struct
import sys

DIRENT = struct.Struct("<HHI8sII8sI28x32s416x")  # mode, unused, length, created, cluster, dir_entry, modified, attr, name
DF_DIRECTORY = 0x0020
DF_EXISTS = 0x8000


class Card:
    def __init__(self, path):
        self.raw = open(path, "rb").read()
        sb = self.raw[:340]
        if not sb.startswith(b"Sony PS2 Memory Card Format "):
            sys.exit("%s: not a formatted PS2 memory card" % path)
        self.page_len, self.pages_per_cluster = struct.unpack_from("<HH", sb, 0x28)
        self.alloc_offset, self.alloc_end, self.root_cluster = struct.unpack_from("<III", sb, 0x34)
        self.ifc_list = struct.unpack_from("<32I", sb, 0x50)
        self.raw_page = self.page_len + (16 if len(self.raw) % (self.page_len + 16) == 0 else 0)
        self.cluster_len = self.page_len * self.pages_per_cluster
        self.per_cluster = self.cluster_len // 4

    def cluster(self, n):
        """Absolute cluster n's data (ECC areas removed)."""
        out = b""
        for p in range(n * self.pages_per_cluster, (n + 1) * self.pages_per_cluster):
            out += self.raw[p * self.raw_page:p * self.raw_page + self.page_len]
        return out

    def fat(self, n):
        """FAT entry of allocatable cluster n (relative to alloc_offset)."""
        fat_cluster_idx, entry = divmod(n, self.per_cluster)
        ifc_idx, ifc_entry = divmod(fat_cluster_idx, self.per_cluster)
        fat_cluster = struct.unpack_from("<I", self.cluster(self.ifc_list[ifc_idx]), ifc_entry * 4)[0]
        return struct.unpack_from("<I", self.cluster(fat_cluster), entry * 4)[0]

    def chain(self, first):
        n, seen = first, set()
        while n not in seen:
            seen.add(n)
            yield n
            e = self.fat(n)
            if not e & 0x80000000 or (e & 0x7FFFFFFF) == 0x7FFFFFFF:
                return
            n = e & 0x7FFFFFFF

    def read(self, first, length):
        data = b"".join(self.cluster(c + self.alloc_offset) for c in self.chain(first))
        return data[:length]

    def entries(self, first, count):
        data = self.read(first, count * 512)
        return [data[i:i + 512] for i in range(0, count * 512, 512)]

    def root(self):
        dot = self.cluster(self.root_cluster + self.alloc_offset)[:512]
        return self.entries(self.root_cluster, DIRENT.unpack(dot)[2])


def name_of(ent):
    return DIRENT.unpack(ent)[8].split(b"\0")[0].decode("ascii", "replace")


def psu_entry(ent, length=None):
    """A .psu header: the card's entry with its cluster and directory index cleared (as uLaunchELF writes)."""
    mode, unused, size, created, cluster, dir_entry, modified, attr, name = DIRENT.unpack(ent)
    return DIRENT.pack(mode, 0, size if length is None else length, created, 0, 0, modified, 0, name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("card")
    ap.add_argument("folder", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--list", action="store_true", help="list the card's root folders")
    args = ap.parse_args()
    card = Card(args.card)
    root = card.root()
    if args.list or not args.folder:
        for ent in root[2:]:
            mode, _, length = DIRENT.unpack(ent)[:3]
            if mode & DF_EXISTS:
                print("%-32s %s %d" % (name_of(ent), "dir " if mode & DF_DIRECTORY else "file", length))
        return
    folder = next((e for e in root if DIRENT.unpack(e)[0] & DF_EXISTS and name_of(e) == args.folder), None)
    if folder is None:
        sys.exit("%s: no folder %s on the card" % (args.card, args.folder))
    mode, _, count, _, cluster = DIRENT.unpack(folder)[:5]
    ents = card.entries(cluster, count)
    live = [e for e in ents[2:] if DIRENT.unpack(e)[0] & DF_EXISTS]
    out = bytearray()
    out += psu_entry(folder, len(live) + 2)
    out += psu_entry(ents[0])  # "."
    out += psu_entry(ents[1])  # ".."
    for e in live:
        fmode, _, length, _, fcluster = DIRENT.unpack(e)[:5]
        if fmode & DF_DIRECTORY:
            sys.exit("%s: subfolder %s: not supported in a .psu" % (args.folder, name_of(e)))
        data = card.read(fcluster, length)
        if len(data) != length:
            sys.exit("%s: %s is %d bytes, read %d" % (args.folder, name_of(e), length, len(data)))
        out += psu_entry(e)
        out += data + b"\0" * (-length % 1024)
        print("  %-20s %7d bytes" % (name_of(e), length))
    out_path = args.out or args.folder + ".psu"
    open(out_path, "wb").write(out)
    print("%s: %d files, %d bytes" % (out_path, len(live), len(out)))


if __name__ == "__main__":
    main()
