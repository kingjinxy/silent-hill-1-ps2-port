#!/usr/bin/env python3
"""Minimal ISO9660 (level 1) image writer for the PS2 port's DVD image.

    mkiso.py <out.iso> <volume id> <ISO name>=<host file> ...

Flat root directory only; files are written in the order given. 2048-byte sectors (DVD media).

Layout follows retail PS2 DVDs (checked against Silent Hill 2): L/M path tables at sectors 257/259,
root directory at 261, and the root directory's recorded size is the bytes actually used rather than
a whole sector. Both matter:
  - On DVD media the BIOS reads the path table at 257 (seen with CdvdVerboseReads: sectors 16, 257).
    With path tables at 18/19 it fails with "open fail name SYSTEM.CNF;1".
  - PCSX2 takes an ISO as DVD media when the u16 at PVD offset 166 differs from the u16 at 171
    (CDVDcommon.cpp FindDiskType; images > 452849 sectors are DVD regardless). Those overlap the LE
    and BE copies of the root directory size; they differ for retail-style sizes (e.g. 468 = 0x1D4),
    but match for a whole-sector size like 2048.
Sectors 18-256 (the UDF bridge on retail discs) are left empty; the BIOS doesn't need them.
All fields are standard ISO9660.
"""
import os
import struct
import sys
import time

SECTOR = 2048
ROOT_SECTORS = 1


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dir_date(t):
    return bytes([t.tm_year - 1900, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0])


def vol_date(t):
    return time.strftime("%Y%m%d%H%M%S00", t).encode() + b"\0"


def dir_record(name, lba, size, is_dir, t):
    n = len(name)
    rec = bytearray(33 + n + (1 - n % 2))
    rec[0] = len(rec)
    rec[2:10] = both32(lba)
    rec[10:18] = both32(size)
    rec[18:25] = dir_date(t)
    rec[25] = 2 if is_dir else 0
    rec[28:32] = both16(1)
    rec[32] = n
    rec[33:33 + n] = name
    return bytes(rec)


def strfield(s, n):
    return s.encode("ascii").ljust(n, b" ")[:n]


def main():
    out, volid, entries = sys.argv[1], sys.argv[2], sys.argv[3:]
    t = time.gmtime()
    files = []
    for e in entries:
        name, path = e.split("=", 1)
        files.append((name.encode("ascii"), path, os.path.getsize(path)))

    # Layout of retail PS2 DVDs (checked against Silent Hill 2): L path table at 257, M at 259, root
    # directory at 261 (sectors 18-256 hold the UDF bridge there). On DVD media the BIOS reads the
    # path table at 257.
    path_table_lba, root_lba = 257, 261
    lba = root_lba + ROOT_SECTORS
    layout = []
    for name, path, size in files:
        layout.append((name, path, size, lba))
        lba += (size + SECTOR - 1) // SECTOR
    total = lba

    # The root directory's recorded size is the bytes actually used, as on retail DVDs (Sony's
    # mastering doesn't round it to a sector); computed below, patched into the "." records and PVD.
    root = bytearray()
    root += dir_record(b"\0", root_lba, 0, True, t)
    root += dir_record(b"\1", root_lba, 0, True, t)
    for name, path, size, flba in sorted(layout, key=lambda x: x[0]):
        rec = dir_record(name, flba, size, False, t)
        if len(root) // SECTOR != (len(root) + len(rec) - 1) // SECTOR:  # records don't cross sectors
            root += b"\0" * (-len(root) % SECTOR)
        root += rec
    if len(root) > ROOT_SECTORS * SECTOR:
        sys.exit("too many files for the root directory")
    root_size = len(root)
    root[0:root[0]] = dir_record(b"\0", root_lba, root_size, True, t)
    root[root[0]:root[0] + root[root[0]]] = dir_record(b"\1", root_lba, root_size, True, t)
    root += b"\0" * (ROOT_SECTORS * SECTOR - len(root))

    pt_entry = bytes([1, 0]) + struct.pack("<I", root_lba) + struct.pack("<H", 1) + b"\0\0"
    pt_entry_m = bytes([1, 0]) + struct.pack(">I", root_lba) + struct.pack(">H", 1) + b"\0\0"

    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = strfield("PLAYSTATION", 32)
    pvd[40:72] = strfield(volid, 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(len(pt_entry))
    pvd[140:144] = struct.pack("<I", path_table_lba)
    pvd[148:152] = struct.pack(">I", path_table_lba + 1)
    pvd[156:190] = dir_record(b"\0", root_lba, root_size, True, t)
    pvd[190:318] = strfield("", 128)
    pvd[318:446] = strfield("", 128)
    pvd[446:574] = strfield("", 128)
    pvd[574:702] = strfield("PLAYSTATION", 128)
    pvd[702:813] = strfield("", 111)
    for off in (813, 830):
        pvd[off:off + 17] = vol_date(t)
    pvd[847:864] = b"0000000000000000\0"
    pvd[864:881] = b"0000000000000000\0"
    pvd[881] = 1

    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1

    with open(out, "wb") as f:
        f.write(b"\0" * (16 * SECTOR))
        f.write(pvd)
        f.write(term)
        f.write(b"\0" * ((path_table_lba - 18) * SECTOR))
        f.write(pt_entry.ljust(2 * SECTOR, b"\0"))
        f.write(pt_entry_m.ljust(2 * SECTOR, b"\0"))
        assert f.tell() == root_lba * SECTOR
        f.write(root)
        for name, path, size, flba in layout:
            assert f.tell() == flba * SECTOR
            with open(path, "rb") as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
            f.write(b"\0" * (-size % SECTOR))
    print("%s: %d sectors" % (out, total))


if __name__ == "__main__":
    main()
