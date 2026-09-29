#!/usr/bin/env python3
"""P18: controllable slow HTTP server fixture (weak-network testing).

Serves a directory over HTTP on 127.0.0.1 with a fixed per-response delay,
so tests can observe tile loading *in flight* and exercise cancellation,
mid-load teardown, and outage recovery — without any external network,
root privileges, or traffic-shaping tools (see docs/adr/0016).

Threaded: cesium-native issues tile requests in parallel, and a
single-threaded server would serialize them (and could stall a request
behind another's delay).

Usage:
    slow_http_server.py --directory <dir> [--delay SECS] [--port N]

Prints "PORT=<n>" on stdout once listening, then serves forever.
"""
import argparse
import functools
import http.server
import socketserver
import sys
import time


class SlowHandler(http.server.SimpleHTTPRequestHandler):
    delay = 0.0
    chunk_bytes = 0      # >0: write responses in chunks this size
    chunk_delay = 0.0    # seconds to sleep between chunks (≈bandwidth cap)
    req_log = None       # optional path: append one line per request path

    def _maybe_log(self):
        if SlowHandler.req_log:
            try:
                with open(SlowHandler.req_log, "a") as f:
                    f.write(self.path.split("?", 1)[0] + "\n")
            except OSError:
                pass

    def copyfile(self, source, outputfile):
        # Bandwidth throttle: paced chunk writes instead of one sendfile.
        if SlowHandler.chunk_bytes > 0:
            while True:
                chunk = source.read(SlowHandler.chunk_bytes)
                if not chunk:
                    break
                outputfile.write(chunk)
                if SlowHandler.chunk_delay > 0:
                    time.sleep(SlowHandler.chunk_delay)
        else:
            super().copyfile(source, outputfile)

    def do_GET(self):
        self._maybe_log()
        if SlowHandler.delay > 0:
            time.sleep(SlowHandler.delay)
        super().do_GET()

    def do_HEAD(self):
        if SlowHandler.delay > 0:
            time.sleep(SlowHandler.delay)
        super().do_HEAD()

    def log_message(self, *args):
        pass  # keep test output clean


class ReusableThreadingServer(socketserver.ThreadingMixIn,
                              http.server.HTTPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--directory", required=True)
    ap.add_argument("--delay", type=float, default=0.3,
                    help="seconds to sleep before each response")
    ap.add_argument("--port", type=int, default=0,
                    help="0 = ephemeral (chosen port printed on stdout)")
    ap.add_argument("--chunk-bytes", type=int, default=0,
                    help=">0: pace response bodies in chunks (bandwidth cap)")
    ap.add_argument("--chunk-delay", type=float, default=0.0,
                    help="seconds between chunks; ~chunk-bytes/chunk-delay B/s")
    ap.add_argument("--log-requests", default=None, metavar="FILE",
                    help="append one line per request path (test ground truth)")
    args = ap.parse_args()

    SlowHandler.delay = args.delay
    SlowHandler.chunk_bytes = args.chunk_bytes
    SlowHandler.chunk_delay = args.chunk_delay
    SlowHandler.req_log = args.log_requests
    handler = functools.partial(SlowHandler, directory=args.directory)
    server = ReusableThreadingServer(("127.0.0.1", args.port), handler)
    print("PORT=%d" % server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
