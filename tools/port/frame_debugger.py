#!/usr/bin/env python3
"""Frame debugger for captures from tools/port/compare_frames.py: serves tools/port/frame_debugger/
(the page) and build/port/cap/ (the captures) on http://127.0.0.1:8765/ — open that in a browser.

    python3 tools/port/frame_debugger.py [--port 8765]
"""
import argparse
import functools
import http.server
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
PAGE = os.path.join(HERE, "frame_debugger")
CAP = os.path.join(REPO, "build", "port", "cap")


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        path = path.split("?", 1)[0]
        if path.startswith("/cap/"):
            return os.path.join(CAP, os.path.basename(path))
        return os.path.join(PAGE, os.path.basename(path) or "index.html")

    def do_GET(self):
        if self.path.split("?", 1)[0] == "/frames.json":
            frames = set()
            for name in os.listdir(CAP) if os.path.isdir(CAP) else []:
                m = re.match(r"ps1_(\d+)_gp0\.bin$", name)
                if m and os.path.exists(os.path.join(CAP, "ps2_%s_gp0.bin" % m.group(1))):
                    frames.add(int(m.group(1)))
            body = json.dumps(sorted(frames)).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def log_message(self, fmt, *args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    args = ap.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("Frame debugger: http://127.0.0.1:%d/ (captures from %s; Ctrl+C to stop)" % (args.port, CAP))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
