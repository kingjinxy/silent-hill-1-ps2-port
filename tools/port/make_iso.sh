#!/bin/bash
# Builds the PS2 port's DVD image: build/port/sh1_ps2.iso (ISO9660, 2048-byte sectors; see
# tools/port/mkiso.py for how PCSX2 is kept on DVD media).
#   SYSTEM.CNF      boots the ELF (title ID below is a placeholder)
#   SHPS_000.01     build/port/sh1.elf
#   SILENT., HILL.  the original game data (rom/USA, from `make setup`), read by the port's CD layer
set -e
cd "$(dirname "$0")/../.."
TITLE=SHPS_000.01
STAGE=build/port/iso
ISO=build/port/sh1_ps2.iso
rm -rf "$STAGE"; mkdir -p "$STAGE"
printf 'BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n' "$TITLE" > "$STAGE/SYSTEM.CNF"
cp build/port/sh1.elf "$STAGE/$TITLE"
# Small files first; the big data files after (tools/port/mkiso.py keeps this order).
# Written to a temporary file, then renamed over the image: a PS2 booted from it over the network
# (tools/port/ps2_log.py, the UDPFS server) keeps reading the old file until it starts again,
# instead of a mix of old and new data.
python3 tools/port/mkiso.py "$ISO.tmp" SH1PS2 \
    "SYSTEM.CNF;1=$STAGE/SYSTEM.CNF" "$TITLE;1=$STAGE/$TITLE" \
    "SILENT.;1=rom/USA/SILENT." "HILL.;1=rom/USA/HILL."
mv -f "$ISO.tmp" "$ISO"
