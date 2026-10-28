#!/usr/bin/env python3
"""Remote control for the game on a real PS2 (booted over Ethernet with Neutrino): sends a command to
sh1agent.irx (src/port/iop/sh1agent, UDP port 62968) and prints its answer.

    python3 tools/port/ps2_ctl.py ping
    python3 tools/port/ps2_ctl.py restart      # restart the game (same build: the image it booted)
    python3 tools/port/ps2_ctl.py deploy       # restart into the newest build (needs udpfs_serve.py)
    python3 tools/port/ps2_ctl.py osd          # exit to the PS2 browser (as RESET does for retail games)
    python3 tools/port/ps2_ctl.py md g_SysWork 0x400 [--out sys.bin]   # read memory (hex in the log)
    python3 tools/port/ps2_ctl.py where        # where the game was at the last 16 vertical blanks
    python3 tools/port/ps2_ctl.py demo "map3_s00 func_800D0CF8"   # play a Demo menu cutscene (name or number)
    python3 tools/port/ps2_ctl.py newgame      # start a New Game (normal difficulty), from anywhere
    python3 tools/port/ps2_ctl.py rec route1   # record from the next save loaded (8 chars; "rec stop" stops)
    python3 tools/port/ps2_ctl.py replay docks1  # send rec/docks1.dem and play it (title: REPLAY, Start)
    python3 tools/port/ps2_ctl.py options 3    # Debug Options rendering: bit 0 Resolution x2, bit 1 32-bit, bit 2 480p

md takes an address (hex) or a symbol of the running build (build/port/sh1.elf, e.g. g_WorldMapWork or
g_WorldMapWork+0x138) and a length (at most 64 KB per command); it waits for the game's "md" lines in
build/port/ps2.log (tools/port/ps2_log.py must be running), prints them as a hex dump, and with --out
writes the bytes to a file. It works while the game sits in a kept crashed state (Triangle held).

The PS2's address comes from --ip (default 192.168.1.10, Neutrino's config/bsd-udpfs.toml).
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FLAG = os.path.join(HERE, "..", "..", "build", "port", ".udpfs_refresh")  # udpfs_serve.py
LOG = os.path.join(HERE, "..", "..", "build", "port", "ps2.log")             # ps2_log.py
ELF = os.path.join(HERE, "..", "..", "build", "port", "sh1.elf")
NM = os.path.join(os.environ.get("PS2DEV", os.path.expanduser("~/ps2dev")), "ee", "bin", "mips64r5900el-ps2-elf-nm")

PORT = 62968
COMMANDS = {"ping": b"PI", "restart": b"RS", "deploy": b"RS", "osd": b"OS", "md": b"MD", "where": b"WH",
            "demo": b"DM", "newgame": b"NG", "options": b"OP", "rec": b"RC",
            "upload": b"UP", "replay": b"RP"}
REC_DIR = os.path.join(HERE, "..", "..", "build", "port", "rec")  # tools/port/rec_extract.py
UPLOAD_MAX = 1024  # the IOP agent's UPLOAD_MAX


def address(text):
    """Hex address, or symbol[+offset] of the running build."""
    m = re.match(r"^([A-Za-z_]\w*)(?:\+(0x[0-9a-fA-F]+|\d+))?$", text)
    if not m:
        return int(text, 16)
    out = subprocess.run([NM, ELF], capture_output=True, text=True).stdout
    syms = {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}
    if m.group(1) not in syms:
        sys.exit("ps2_ctl: no symbol %s in %s" % (m.group(1), ELF))
    return syms[m.group(1)] + (int(m.group(2), 0) if m.group(2) else 0)


def memory(ip, addr, length, timeout=20):
    """Reads PS2 memory through the game's log ("md" lines); returns the bytes."""
    start = os.path.getsize(LOG) if os.path.exists(LOG) else 0
    if send(ip, "md", payload=struct.pack("<II", addr, length), tries=1, timeout=2.0) is None:
        sys.exit("ps2_ctl: no answer from %s" % ip)
    data, end = {}, time.time() + timeout
    while time.time() < end:
        with open(LOG, errors="replace") as f:
            f.seek(start)
            text = f.read()
        for m in re.finditer(r"md ([0-9a-f]{8}):([0-9a-f]*)", text):
            data[int(m.group(1), 16)] = bytes.fromhex(m.group(2))
        err = re.search(r"md error: (.*)", text)
        if err:
            sys.exit("ps2_ctl: " + err.group(1))
        if "md end" in text:
            break
        time.sleep(0.1)
    else:
        sys.exit("ps2_ctl: no complete answer in the log (is tools/port/ps2_log.py running?)")
    return b"".join(data[a] for a in sorted(data) if addr <= a < addr + length)


def log_wait(start, pattern, timeout):
    """Waits for `pattern` in the log after byte `start`; returns the match, or None."""
    end = time.time() + timeout
    while time.time() < end:
        with open(LOG, errors="replace") as f:
            f.seek(start)
            m = re.search(pattern, f.read())
        if m:
            return m
        time.sleep(0.02)
    return None


def replay(ip, name):
    """Uploads rec/NAME.dem a chunk at a time (each acknowledged in the log), then starts it: the game
    plays it once the REPLAY screen is waiting (Start there)."""
    data = open(os.path.join(REC_DIR, name + ".dem"), "rb").read()
    for off in range(0, len(data), UPLOAD_MAX):
        chunk = data[off:off + UPLOAD_MAX]
        for _ in range(3):
            start = os.path.getsize(LOG)
            if send(ip, "upload", payload=struct.pack("<II", off, len(chunk)) + chunk, tries=1, timeout=2.0) is None:
                continue
            m = log_wait(start, r"rec upload %d %d\b|rec: upload at %d refused \((.*)\)" % (off, len(chunk), off), 3)
            if m and m.group(1):
                sys.exit("ps2_ctl: upload refused: " + m.group(1))
            if m:
                break
        else:
            sys.exit("ps2_ctl: no acknowledgement for the upload at %d (is tools/port/ps2_log.py running?)" % off)
        print("\rps2_ctl: sent %d of %d bytes" % (off + len(chunk), len(data)), end="", flush=True)
    print()
    start = os.path.getsize(LOG)
    if send(ip, "replay", payload=name.encode().ljust(8, b"\0"), tries=1, timeout=2.0) is None:
        sys.exit("ps2_ctl: no answer from %s" % ip)
    m = log_wait(start, r"rec: received \S+: (\d+) bytes, sum ([0-9A-F]{8})|rec: replay \S+ refused \((.*)\)", 5)
    if not m or m.group(3):
        sys.exit("ps2_ctl: replay not accepted: %s" % (m.group(3) if m else "no answer in the log"))
    total = 0
    for b in data:
        total = (total * 31 + b) & 0xFFFFFFFF
    if int(m.group(1)) != len(data) or int(m.group(2), 16) != total:
        sys.exit("ps2_ctl: the console received %s bytes, sum %s; sent %d, sum %08X" % (m.group(1), m.group(2), len(data), total))
    print("ps2_ctl: %s received intact (%d bytes); it plays once the REPLAY screen waits (Start there)" % (name, len(data)))
    return 0


def send(ip, command, timeout=1.0, tries=3, payload=b""):
    """Sends `command` (a COMMANDS key); returns the agent's answer, or None without one."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        for _ in range(tries):
            sock.sendto(COMMANDS[command] + payload, (ip, PORT))
            try:
                data, _ = sock.recvfrom(2048)
                return data.lstrip(b" ").decode("latin-1").strip()
            except socket.timeout:
                continue
        return None
    finally:
        sock.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=sorted(COMMANDS))
    ap.add_argument("--ip", default="192.168.1.10")
    ap.add_argument("addr", nargs="?", help="md: address (hex) or symbol[+offset]")
    ap.add_argument("length", nargs="?", default="0x100", help="md: bytes (at most 0x10000)")
    ap.add_argument("--out", help="md: write the bytes to this file")
    args = ap.parse_args()
    if args.command == "where":
        start = os.path.getsize(LOG) if os.path.exists(LOG) else 0
        if send(args.ip, "where", tries=1, timeout=2.0) is None:
            sys.exit("ps2_ctl: no answer from %s" % args.ip)
        end = time.time() + 10
        text = ""
        while time.time() < end and "where end" not in text:
            time.sleep(0.1)
            with open(LOG, errors="replace") as f:
                f.seek(start)
                text = f.read()
        out = subprocess.run([NM, "-n", ELF], capture_output=True, text=True).stdout
        syms = sorted((int(p[0], 16), p[2]) for p in (l.split() for l in out.splitlines()) if len(p) == 3 and p[1] in "tTwW")
        import bisect
        addrs = [a for a, _ in syms]
        for m in re.finditer(r"where ([0-9a-f]{8})", text):
            pc = int(m.group(1), 16)
            i = bisect.bisect_right(addrs, pc) - 1
            print("%08x  %s+0x%x" % (pc, syms[i][1], pc - syms[i][0]) if i >= 0 else "%08x" % pc)
        return 0
    if args.command == "options":
        answer = send(args.ip, "options", payload=struct.pack("<II", int(args.addr or "0", 0) & 0x1F, 0), tries=1,
                      timeout=2.0)
        print(answer if answer is not None else "ps2_ctl: no answer from %s" % args.ip)
        sys.exit(0 if answer is not None else 1)
    if args.command == "rec":
        name = (args.addr or "").encode()
        if name == b"stop":
            name = b""
        if len(name) > 8 or not all(chr(c).isalnum() or chr(c) in "_-" for c in name):
            sys.exit("ps2_ctl: a recording name is up to 8 letters, digits, _ or -")
        answer = send(args.ip, "rec", payload=name.ljust(8, b"\0"), tries=1, timeout=2.0)
        print(answer if answer is not None else "ps2_ctl: no answer from %s" % args.ip)
        return 0 if answer is not None else 1
    if args.command == "replay":
        name = args.addr or ""
        if not name or len(name) > 8 or not all(c.isalnum() or c in "_-" for c in name):
            sys.exit("ps2_ctl: a recording name is up to 8 letters, digits, _ or -")
        return replay(args.ip, name)
    if args.command == "demo":
        sys.path.insert(0, HERE)
        import cutscene_list
        names = [cutscene_list.label(c) for c in cutscene_list.cutscenes()]
        idx = int(args.addr) if args.addr and args.addr.isdigit() else (
            names.index(args.addr) if args.addr in names else
            next((i for i, c in enumerate(cutscene_list.cutscenes()) if c["func"] == args.addr), None))
        if idx is None:
            sys.exit("ps2_ctl: no cutscene %r; the list:\n  %s" % (args.addr, "\n  ".join(names)))
        answer = send(args.ip, "demo", payload=struct.pack("<II", idx, 0), tries=1, timeout=2.0)
        print("ps2_ctl: %s (%d: %s)" % (answer or "no answer", idx, names[idx]))
        return 0 if answer else 1
    if args.command == "md":
        a = address(args.addr)
        data = memory(args.ip, a, int(args.length, 0))
        if args.out:
            open(args.out, "wb").write(data)
            print("ps2_ctl: %d bytes from %08x written to %s" % (len(data), a, args.out))
        else:
            for i in range(0, len(data), 16):
                row = data[i:i + 16]
                print("%08x: %-48s %s" % (a + i, " ".join("%02x" % b for b in row),
                                          "".join(chr(b) if 32 <= b < 127 else "." for b in row)))
        return 0
    if args.command == "deploy":
        # The UDPFS server switches the open image to the new build, then the game restarts into it.
        open(FLAG, "w").close()
        end = time.time() + 3
        while os.path.exists(FLAG) and time.time() < end:
            time.sleep(0.05)
        if os.path.exists(FLAG):
            os.remove(FLAG)
            print("ps2_ctl: the UDPFS server didn't switch the image (is tools/port/udpfs_serve.py running?)")
            return 1
    answer = send(args.ip, args.command, timeout=2.0 if args.command != "ping" else 1.0,
                  tries=3 if args.command == "ping" else 1)  # a repeated restart would restart twice
    if answer is None:
        print("ps2_ctl: no answer from %s:%d (game not running, or crashed with interrupts off)" % (args.ip, PORT))
        return 1
    print("ps2_ctl: %s" % answer)
    return 0


if __name__ == "__main__":
    sys.exit(main())
