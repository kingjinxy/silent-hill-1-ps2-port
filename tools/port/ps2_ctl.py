#!/usr/bin/env python3
"""Remote control for the game on a real PS2 (booted over Ethernet with Neutrino): sends a command to
sh1agent.irx (src/port/iop/sh1agent, UDP port 62968) and prints its answer.

    python3 tools/port/ps2_ctl.py ping
    python3 tools/port/ps2_ctl.py restart      # restart the game (same build: the image it booted)
    python3 tools/port/ps2_ctl.py deploy       # restart into the newest build (needs udpfs_serve.py)

The PS2's address comes from --ip (default 192.168.1.10, Neutrino's config/bsd-udpfs.toml).
"""
import argparse
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FLAG = os.path.join(HERE, "..", "..", "build", "port", ".udpfs_refresh")  # udpfs_serve.py

PORT = 62968
COMMANDS = {"ping": b"PI", "restart": b"RS", "deploy": b"RS"}


def send(ip, command, timeout=1.0, tries=3):
    """Sends `command` (a COMMANDS key); returns the agent's answer, or None without one."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        for _ in range(tries):
            sock.sendto(COMMANDS[command], (ip, PORT))
            try:
                data, _ = sock.recvfrom(256)
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
    args = ap.parse_args()
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
