#!/usr/bin/env python3
"""P19: camera-roam memory boundedness.

Verifies that during a multi-lap camera roam over a deep implicit tileset,
the Cesium tile content cache evicts cold tiles (LRU) so that
tileStats().tilesLoaded / bytesLoaded stay bounded far below the total
tileset size, while the set of *requested* tiles proves the camera actually
visited a wide area (i.e. boundedness comes from eviction, not from a tiny
working set).

Fixture: tests/data/p19_deep_tileset/ (implicit QUADTREE, levels 0-4,
341 tiles, ~15.6MB total). Trajectory: tests/data/trajectories/p19_deep.csv
(3 yaw laps, 432 frames).

Method: serves the fixture over a local HTTP server that logs every
request. Distinct .glb URLs prove coverage; total .glb hits exceeding
distinct proves eviction+reload churn.

Pass criteria:
  - failed == 0
  - distinct .glb requested >= 200 (wide coverage over 3 laps)
  - total .glb hits > distinct (tiles were evicted and re-requested)
  - max tilesLoaded <= 200 and max bytesLoaded <= budget * 1.1
    (bounded; budget is 8MB, total tileset is ~15.6MB)
  - per-lap-end tilesLoaded stable within 15% (no upward drift)
  - RSS end < 400MB (generous; documents actual in ADR-0017)

Not asserted: exact RSS stability. The base Filament/Mesa render loop shows
slow RSS growth even with no tileset (pre-existing, out of P19 scope);
ADR-0017 records the numbers honestly.
"""
import argparse
import http.server
import os
import re
import socketserver
import subprocess
import sys
import threading
import urllib.parse

DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = os.path.join(REPO, "build", "linux", "tiles_demo")
TILESET_DIR = os.path.join(DATA_DIR, "p19_deep_tileset")
TRAJECTORY = os.path.join(DATA_DIR, "trajectories", "p19_deep.csv")

CACHE_BUDGET = 8_000_000  # 8MB; total fixture is ~15.6MB
WARMUP = 60
FRAMES = 500  # 432-frame trajectory + margin
WIDTH, HEIGHT = 400, 300


class Handler(http.server.SimpleHTTPRequestHandler):
    log_path = None

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = urllib.parse.unquote(self.path.split("?")[0])
        with open(self.log_path, "a") as f:
            f.write(path + "\n")
        full = os.path.join(TILESET_DIR, path.lstrip("/"))
        if os.path.isfile(full):
            self.send_response(200)
            self.send_header("Content-Length", str(os.path.getsize(full)))
            self.end_headers()
            with open(full, "rb") as fh:
                self.wfile.write(fh.read())
        else:
            self.send_response(404)
            self.end_headers()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", default=DEMO)
    ap.add_argument("--sanitized", action="store_true",
                    help="run under ASan/LSan/UBSan via tests/run_sanitized.py")
    ap.add_argument("--suppressions", default=None)
    args = ap.parse_args()
    demo = args.demo
    if not os.path.isfile(demo):
        print("SKIP: tiles_demo not built")
        return 0
    log_path = "/tmp/p19_roam_requests.log"
    Handler.log_path = log_path
    open(log_path, "w").write("")
    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.TCPServer(("127.0.0.1", 0), Handler)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        demo_cmd = [
            demo,
            "--tileset", f"http://127.0.0.1:{port}/tileset.json",
            "--trajectory", TRAJECTORY,
            "--warmup", str(WARMUP),
            "--frames", str(FRAMES),
            "--cache-budget", str(CACHE_BUDGET),
            "--width", str(WIDTH), "--height", str(HEIGHT),
            "--stats", "--print-rss",
        ]
        if args.sanitized:
            # Shorter run under sanitizers (ASan is much slower); the
            # boundedness assertions still apply. Requires tests/lsan.supp
            # for the known third-party CurlAssetAccessor suppression.
            fi = demo_cmd.index("--frames")
            demo_cmd[fi + 1] = "150"
            run_san = os.path.join(REPO, "tests", "run_sanitized.py")
            supp = args.suppressions or os.path.join(REPO, "tests",
                                                     "lsan.supp")
            cmd = (["xvfb-run", "-a", sys.executable, run_san,
                    "--suppressions", supp, "--"] + demo_cmd)
        else:
            cmd = ["xvfb-run", "-a"] + demo_cmd
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=900)
    finally:
        httpd.shutdown()

    if proc.returncode != 0:
        print("FAIL: demo exited", proc.returncode)
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
        return 1

    # Parse per-frame stats.
    frames = []
    for line in proc.stdout.splitlines():
        m = re.search(
            r"\[stats\] frame=(\d+) selected=(\d+) loading=(\d+) "
            r"loaded=(\d+) failed=(\d+) bytes=(\d+)", line)
        if m:
            frames.append(tuple(map(int, m.groups())))
    if not frames:
        print("FAIL: no [stats] lines parsed")
        return 1

    max_loaded = max(f[3] for f in frames)
    max_bytes = max(f[5] for f in frames)
    max_failed = max(f[4] for f in frames)
    # Per-lap ends: trajectory is 432 frames, laps end at 143/287/431.
    lap_ends = {}
    for f in frames:
        frame_no = f[0]
        for lap_end in (143, 287, 431):
            if abs(frame_no - lap_end) <= 2:
                lap_ends[lap_end] = f[3]

    # Request log analysis.
    with open(log_path) as f:
        reqs = [l.strip() for l in f if l.strip().endswith(".glb")]
    distinct = len(set(reqs))
    total = len(reqs)

    rss_start = rss_end = None
    for line in proc.stdout.splitlines():
        m = re.search(r"\[rss\] start=(\d+)KB", line)
        if m:
            rss_start = int(m.group(1))
        m = re.search(r"\[rss\] end=(\d+)KB", line)
        if m:
            rss_end = int(m.group(1))

    print(f"frames parsed: {len(frames)}")
    print(f"max loaded: {max_loaded}, max bytes: {max_bytes}, "
          f"max failed: {max_failed}")
    print(f"lap-end loaded: {lap_ends}")
    print(f"distinct glb: {distinct}, total glb hits: {total}")
    print(f"rss: {rss_start}KB -> {rss_end}KB")

    failures = []
    # Sanitized runs are shorter (150 frames ~ 1 lap); relax coverage.
    min_distinct = 80 if args.sanitized else 200
    if max_failed != 0:
        failures.append(f"failed={max_failed} != 0")
    if distinct < min_distinct:
        failures.append(f"distinct glb {distinct} < {min_distinct}")
    if total <= distinct:
        failures.append(
            f"total hits {total} <= distinct {distinct} (no eviction churn)")
    if max_loaded > 200:
        failures.append(f"max loaded {max_loaded} > 200 (not bounded)")
    if max_bytes > CACHE_BUDGET * 1.1:
        failures.append(
            f"max bytes {max_bytes} > budget*1.1 (not bounded)")
    if not args.sanitized and len(lap_ends) >= 2:
        vals = list(lap_ends.values())
        if max(vals) > min(vals) * 1.15:
            failures.append(f"lap-end loaded not stable: {lap_ends}")
    if not args.sanitized and (rss_end is None or rss_end > 400 * 1024):
        # Sanitized builds (ASan shadow/quarantine) are not representative
        # for RSS; there we only gate on sanitizer reports + boundedness.
        failures.append(f"rss end {rss_end}KB > 400MB")

    if failures:
        print("FAIL:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS: roam memory bounded (eviction verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
