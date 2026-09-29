#!/usr/bin/env python3
"""P5: serve the p3 test tileset over local HTTP and render it via tileset_test.py.

Starts a throwaway `python3 -m http.server` on 127.0.0.1 with an ephemeral
port rooted at tests/data/p3_box_tileset, then reuses tileset_test.py
unchanged with an http:// URL. No external network is involved.

Usage (mirrors tileset_test.py):
    http_tileset_test.py --demo <tiles_demo> --out <png> [--frames 60 ...]
"""
import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_listening(port, deadline=15.0):
    start = time.time()
    while time.time() - start < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()

    serve_dir = os.path.join(HERE, "data", "p3_box_tileset")
    port = free_port()
    server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(port),
         "--bind", "127.0.0.1", "--directory", serve_dir],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_listening(port):
            print("FAIL: local HTTP server did not start", flush=True)
            return 1
        url = "http://127.0.0.1:%d/tileset.json" % port
        cmd = [sys.executable, os.path.join(HERE, "tileset_test.py"),
               "--demo", args.demo,
               "--tileset", url,
               "--out", args.out,
               "--frames", str(args.frames),
               "--width", str(args.width),
               "--height", str(args.height)]
        print("rendering over HTTP: %s" % url, flush=True)
        r = subprocess.run(cmd)
        return r.returncode
    finally:
        server.terminate()
        server.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
