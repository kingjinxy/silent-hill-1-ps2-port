#!/usr/bin/env python3
"""Neutrino's UDPFS server (udpfs_server.py from the Neutrino release, unchanged) serving build/port/
to a real PS2, plus deploying new builds to a running game.

A running game keeps reading the image it booted from (make_iso.sh renames a new image over it, and
the server's open handle still points to the old file). When build/port/.udpfs_refresh appears
(tools/port/ps2_ctl.py deploy creates it right before sending "restart"), every open file whose path
now names a different file is reopened, then the flag is removed: the restarted game reads the new
image. Crash restarts don't set the flag, so they keep the build that crashed.

    python3 tools/port/udpfs_serve.py [--server ~/ps2tools/neutrino_v1.8.0/udpfs_server/udpfs_server.py]
"""
import argparse
import importlib.util
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
ROOT = os.path.join(REPO, "build", "port")
FLAG = os.path.join(ROOT, ".udpfs_refresh")
DEFAULT_SERVER = os.path.expanduser("~/ps2tools/neutrino_v1.8.0/udpfs_server/udpfs_server.py")


def load_server(path):
    sys.path.insert(0, os.path.dirname(path))  # its compressed_iso package
    spec = importlib.util.spec_from_file_location("udpfs_server", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def refresh_handles(server):
    """Reopens open files whose path now names another file; returns how many."""
    n = 0
    for handle_id, fh in list(server.handles.items()):
        f = fh.obj
        if fh.is_dir or not hasattr(f, "fileno") or not hasattr(f, "name") or not isinstance(f.name, str):
            continue
        try:
            if os.stat(f.name).st_ino == os.fstat(f.fileno()).st_ino:
                continue
            fh.obj = open(f.name, f.mode)
            f.close()
            n += 1
            print("udpfs_serve: handle %d now reads the new %s" % (handle_id, os.path.relpath(f.name, ROOT)), flush=True)
        except OSError as e:
            print("udpfs_serve: handle %d: %s" % (handle_id, e), flush=True)
    return n


def watch(server):
    while True:
        if os.path.exists(FLAG):
            n = refresh_handles(server)
            print("udpfs_serve: deploy: %d open file(s) switched to the new build" % n, flush=True)
            try:
                os.remove(FLAG)
            except OSError:
                pass
        time.sleep(0.1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", default=DEFAULT_SERVER, help="Neutrino's udpfs_server.py")
    args, rest = ap.parse_known_args()
    module = load_server(args.server)
    created = []
    original_init = module.UdpfsServer.__init__

    def init(self, *a, **kw):
        original_init(self, *a, **kw)
        created.append(self)
        threading.Thread(target=watch, args=(self,), daemon=True).start()

    module.UdpfsServer.__init__ = init
    if os.path.exists(FLAG):
        os.remove(FLAG)
    sys.argv = [args.server, "-d", ROOT] + rest
    return module.main()


if __name__ == "__main__":
    sys.exit(main())
