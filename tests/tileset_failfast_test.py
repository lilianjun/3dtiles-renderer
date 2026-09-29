#!/usr/bin/env python3
"""P23: corrupt-tileset.json fail-fast regression gate.

Background: P22 left a known gap — a corrupt-but-present tileset.json
blocked the render thread for the full 30s root-wait before failing
(cesium-native's TilesetJsonLoader fails the parse in a worker, and
getRootTile() stays nullptr for "still loading" and "load failed" alike,
so the wait loop could not tell them apart).

P23 fix (src/tileset.cpp): pre-flight validation of the root document
BEFORE the cesium Tileset is constructed —
  * local file: read + rapidjson parse (~ms)
  * http(s): one blocking fetch through the same RoutingAssetAccessor
    (same curl timeouts, never throws), then the same parse
Parseable JSON object with a "root" object member is required; anything
weaker fails in milliseconds. Slow networks are NOT affected: the
pre-flight fetch uses the same generous timeouts as tile loading, so a
slow-but-valid root still passes (scenario S3 serves corruption over the
P18 slow_http_server with delay=0.3 to prove it).

Scenario matrix (each 3x in normal mode, 1x under --sanitized):
  S1 corrupt_local: p3 -> garbage-bytes tileset.json at frame 60.
     Must fail FAST (<5s wall from the switch frame to the [switch] log),
     error mentions invalid JSON; p3 keeps rendering (selected root.glb,
     stats loaded>0 failed==0); end screenshot bit-identical to a
     direct-p3 reference run (proves the failed load changed nothing).
  S2 wrong_json: p3 -> valid JSON that is not a tileset (no "root").
     Same assertions; error mentions missing "root".
  S3 corrupt_http: p3 -> corrupt tileset.json served over slow HTTP
     (0.3s/response). Must fail fast with an invalid-JSON error; p3 keeps
     rendering.

The <5s bound is generous on purpose: a local pre-flight is ~ms, so any
regression back toward the 30s wait trips it with huge margin, while
xvfb+Mesa frame-time jitter (~0.1s) can never false-trip it.
"""
import argparse
import hashlib
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = os.path.join(REPO, "build", "linux", "tiles_demo")
P3 = os.path.join(REPO, "tests", "data", "p3_box_tileset", "tileset.json")
TRAJ = os.path.join(REPO, "tests", "data", "trajectories", "p22_switch.csv")
SERVER = os.path.join(REPO, "tests", "slow_http_server.py")
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
SUPP = os.path.join(REPO, "tests", "lsan.supp")

WIDTH, HEIGHT = 400, 300
WARMUP = 60
SWITCH_AT = 60
FRAMES = 150
# Fail-fast budget: the pre-flight is ~ms; 5s leaves huge margin over
# frame jitter while catching any return of the 30s spin.
FAIL_FAST_SECS = 5.0

STAT_RE = re.compile(
    r"\[stats\] frame=(\d+) selected=(-?\d+) loading=(-?\d+) "
    r"loaded=(-?\d+) failed=(-?\d+) bytes=(-?\d+)")
SEL_RE = re.compile(r"\[selected\] frame=(\d+) ids=(.*)")
SWITCH_RE = re.compile(r"\[switch\] frame=(\d+) ok=(\d)(?: error=(.*))?")


class TimestampedDemo:
    """tiles_demo with per-line wall-clock timestamps (fail-fast timing)."""

    def __init__(self, cmd, env=None, san=None):
        self.proc = subprocess.Popen(
            ["xvfb-run", "-a"] + cmd, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self.lines = queue.Queue()  # (timestamp, line)
        self._san = san
        self._thread = threading.Thread(target=self._pump, daemon=True)
        self._thread.start()

    def _pump(self):
        try:
            for line in self.proc.stdout:
                self.lines.put((time.monotonic(), line.rstrip("\n")))
        except Exception:
            pass

    def wait_done(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return None

    def drain(self):
        out = []
        while True:
            try:
                out.append(self.lines.get_nowait())
            except queue.Empty:
                break
        return out

    def sanitizer_reports(self):
        if not self._san:
            return []
        return self._san("\n".join(l for _, l in self.drain()))


def run_demo(args, tileset, switch_to=None, frames=FRAMES, extra=(),
             sanitized=False, timeout=300, shot=None):
    cmd = [args.demo, "--tileset", tileset, "--trajectory", TRAJ,
           "--warmup", str(WARMUP), "--frames", str(frames),
           "--width", str(WIDTH), "--height", str(HEIGHT),
           "--stats", "--print-selected"] + list(extra)
    if switch_to is not None:
        cmd += ["--switch-tileset", switch_to,
                "--switch-at-frame", str(SWITCH_AT)]
    if shot:
        cmd += ["--screenshot", shot]
    env, san = None, None
    if sanitized:
        sys.path.insert(0, os.path.join(REPO, "tests"))
        from sanitizer_common import sanitizer_env, find_reports
        env = sanitizer_env(args.suppressions)
        san = find_reports
    d = TimestampedDemo(cmd, env=env, san=san)
    rc = d.wait_done(timeout=timeout)
    lines = d.drain()
    return rc, lines, d


def parse(lines):
    stats, selected, switches = {}, {}, {}
    for _, line in lines:
        m = STAT_RE.search(line)
        if m:
            stats[int(m.group(1))] = tuple(map(int, m.groups()[1:]))
            continue
        m = SEL_RE.search(line)
        if m:
            selected[int(m.group(1))] = set(
                x for x in m.group(2).strip().split(",") if x)
            continue
        m = SWITCH_RE.search(line)
        if m:
            switches[int(m.group(1))] = (int(m.group(2)),
                                         (m.group(3) or "").strip())
    return stats, selected, switches


def switch_latency(lines):
    """Wall seconds from the switch frame's start to the [switch] log.

    The demo fires the switch before rendering frame SWITCH_AT, so the
    last log line before [switch] marks ~frame start; the [switch] line
    itself is printed synchronously right after loadTileset() returns.
    """
    t_prev, t_switch = None, None
    for ts, line in lines:
        m = SWITCH_RE.search(line)
        if m and int(m.group(1)) == SWITCH_AT:
            t_switch = ts
            break
        t_prev = ts
    if t_switch is None or t_prev is None:
        return None
    return t_switch - t_prev


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def check(cond, msg, failures):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def write_corrupt_variants(tmp):
    garbage = os.path.join(tmp, "garbage.json")
    with open(garbage, "wb") as f:
        f.write(b"\x00\x01\x02not json at all {{{ truncated")
    wrong = os.path.join(tmp, "wrong.json")
    with open(wrong, "w") as f:
        f.write('{"hello": "world", "asset": {"version": "1.0"}}')
    truncated = os.path.join(tmp, "truncated.json")
    with open(truncated, "w") as f:
        f.write('{"asset": {"version": "1.0"}, "root": {"boundingVolume":')
    return garbage, wrong, truncated


def assert_failed_switch(name, args, tmp, failures, bad_tileset,
                         err_fragment, sanitized, shot=None, ref=None):
    """Common assertions for one corrupt-switch scenario."""
    rc, lines, demo = run_demo(args, P3, bad_tileset, sanitized=sanitized,
                               shot=shot)
    check(rc == 0, "%s: demo exit %r" % (name, rc), failures)
    stats, selected, switches = parse(lines)
    ok, err = switches.get(SWITCH_AT, (1, ""))
    check(ok == 0, "%s: switch should fail (%s)" % (name, switches),
          failures)
    check(err_fragment in err,
          "%s: error should mention %r, got %r" % (name, err_fragment, err),
          failures)
    lat = switch_latency(lines)
    check(lat is not None and lat < FAIL_FAST_SECS,
          "%s: fail-fast %.2fs >= %.0fs budget" %
          (name, lat if lat is not None else -1, FAIL_FAST_SECS), failures)
    print("%s: switch failed in %.3fs (error: %s)" %
          (name, lat if lat is not None else -1, err[:80]))
    # The live p3 tileset is untouched: still selected, stats healthy.
    for f in (SWITCH_AT + 1, SWITCH_AT + 5, FRAMES - 1):
        ids = selected.get(f, set())
        check("root.glb" in ids,
              "%s: frame %d should still select root.glb (%s)"
              % (name, f, ids), failures)
        st = stats.get(f)
        check(st is not None and st[2] > 0 and st[3] == 0,
              "%s: frame %d stats unhealthy after failed switch (%s)"
              % (name, f, st), failures)
    # Pixel proof: end screenshot identical to a run that never switched.
    if shot is not None and ref is not None:
        check(md5(shot) == md5(ref),
              "%s: post-failure screenshot != no-switch reference" % name,
              failures)
    if sanitized:
        reports = demo.sanitizer_reports()
        check(not reports, "%s: sanitizer reports %s" % (name, reports),
              failures)


def scenario_corrupt_local(args, tmp, failures, sanitized):
    """S1: garbage bytes as tileset.json — must fail in ms, not 30s."""
    garbage, _, _ = write_corrupt_variants(tmp)
    shot = os.path.join(tmp, "s1_fail.png")
    ref = os.path.join(tmp, "s1_ref.png")
    # Reference: p3 with no switch at all (pixel ground truth for the
    # "failed load changed nothing" proof).
    rc, _, _ = run_demo(args, P3, switch_to=None, sanitized=sanitized,
                        shot=ref)
    check(rc == 0, "S1 ref: demo exit %r" % rc, failures)
    assert_failed_switch("S1 corrupt_local", args, tmp, failures, garbage,
                         "not valid JSON", sanitized, shot=shot, ref=ref)
    print("S1 corrupt_local: ok")


def scenario_wrong_json(args, tmp, failures, sanitized):
    """S2: valid JSON, not a tileset (missing root) — also fail-fast."""
    _, wrong, truncated = write_corrupt_variants(tmp)
    for name, path in (("S2a wrong_json", wrong),
                       ("S2b truncated_json", truncated)):
        frag = "missing \"root\"" if name == "S2a wrong_json" \
            else "not valid JSON"
        assert_failed_switch(name, args, tmp, failures, path, frag,
                             sanitized)
    print("S2 wrong_json: ok")


class SlowServer:
    def __init__(self, directory, delay):
        self.proc = None
        self.port = None
        self.directory = directory
        self.delay = delay

    def start(self):
        self.proc = subprocess.Popen(
            [sys.executable, SERVER, "--directory", self.directory,
             "--delay", str(self.delay), "--port", "0"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        for _ in range(100):
            line = self.proc.stdout.readline()
            m = re.match(r"PORT=(\d+)", line or "")
            if m:
                self.port = int(m.group(1))
                return
        raise RuntimeError("slow_http_server did not print PORT=")

    def stop(self):
        if self.proc:
            self.proc.terminate()
            self.proc.wait(timeout=10)
            self.proc = None


def scenario_corrupt_http(args, tmp, failures, sanitized):
    """S3: corrupt tileset.json over slow HTTP — the pre-flight fetch path.

    delay=0.3 also proves P18's slow-network scenario is not mis-killed:
    the pre-flight waits for the (slow but responsive) server instead of
    racing it.
    """
    srvdir = os.path.join(tmp, "srv")
    os.makedirs(srvdir, exist_ok=True)
    with open(os.path.join(srvdir, "tileset.json"), "wb") as f:
        f.write(b"this is not json {{{")
    server = SlowServer(srvdir, delay=0.3)
    server.start()
    try:
        url = "http://127.0.0.1:%d/tileset.json" % server.port
        assert_failed_switch("S3 corrupt_http", args, tmp, failures, url,
                             "not valid JSON", sanitized)
    finally:
        server.stop()
    print("S3 corrupt_http: ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", default=DEMO)
    ap.add_argument("--sanitized", action="store_true")
    ap.add_argument("--suppressions", default=SUPP)
    args = ap.parse_args()
    reps = 1 if args.sanitized else 3
    failures = []
    for rep in range(reps):
        print("--- rep %d/%d ---" % (rep + 1, reps))
        with tempfile.TemporaryDirectory(prefix="p23_failfast_") as tmp:
            scenario_corrupt_local(args, tmp, failures, args.sanitized)
            scenario_wrong_json(args, tmp, failures, args.sanitized)
            scenario_corrupt_http(args, tmp, failures, args.sanitized)
    if failures:
        print("FAILURES (%d):" % len(failures))
        for f in failures:
            print("  -", f)
        sys.exit(1)
    print("PASS: tileset fail-fast gate (%d reps)" % reps)


if __name__ == "__main__":
    sys.exit(main())
