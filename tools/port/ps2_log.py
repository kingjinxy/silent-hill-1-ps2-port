#!/usr/bin/env python3
"""Receives a real PS2's console output over Ethernet: Neutrino's ministack (and the port's network
build) broadcast every IOP tty write as UDP to port 18194 ("udptty"). Lines are printed with
timestamps in PCSX2 log style ("[   12.3456] text", seconds since the first packet) and appended to
build/port/ps2.log, so the tools that read pcsx2_run.py output can read these too.

    python3 tools/port/ps2_log.py [--port 18194] [--log build/port/ps2.log]
"""
import argparse
import os
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=18194)
    ap.add_argument("--log", default=os.path.join(REPO, "build", "port", "ps2.log"))
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", args.port))
    log = open(args.log, "a")
    print("ps2_log: listening on UDP %d, writing %s" % (args.port, args.log), flush=True)
    start, pending, source = None, "", None
    while True:
        data, addr = sock.recvfrom(4096)
        if start is None:
            start = time.time()
        if addr[0] != source:
            source = addr[0]
            line = "ps2_log: output from %s" % source
            print(line, flush=True)
            log.write(line + "\n")
        # ministack's packets start with two padding bytes (spaces) before the text.
        text = data[2:] if data[:2] == b"  " else data
        pending += text.decode("latin-1").replace("\r", "")
        while "\n" in pending:
            line, pending = pending.split("\n", 1)
            out = "[%9.4f] %s" % (time.time() - start, line)
            print(out, flush=True)
            log.write(out + "\n")
            log.flush()


if __name__ == "__main__":
    sys.exit(main())
