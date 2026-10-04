#!/usr/bin/env python3
"""Wraps a flat binary (loaded at 0x80010000) in a PS-EXE header. Usage: mkexe.py in.bin entry out.exe"""
import struct, sys
data = open(sys.argv[1], "rb").read()
data += b"\0" * (-len(data) % 0x800)
hdr = bytearray(0x800)
hdr[0:8] = b"PS-X EXE"
struct.pack_into("<IIIIIIIIIIII", hdr, 0x10, int(sys.argv[2], 0), 0, 0x80010000, len(data), 0, 0, 0, 0, 0x801FFF00, 0, 0, 0)
open(sys.argv[3], "wb").write(bytes(hdr) + data)
