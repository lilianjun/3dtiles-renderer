#!/usr/bin/env python3
"""P22: loadTileset re-entry and switching safety.

Background: Renderer::loadTileset claimed "loading a new tileset replaces
the old one" (ADR-0012), but a second call was never exercised. Worse, the
implementation did s.tileset.reset() BEFORE the new load, so ANY failed
second load (even a missing file) destroyed the working tileset and left
the SDK rendering the clear color with tileStats() all -1.

P22 fix (src/renderer.cpp + src/tileset.cpp): build-then-commit. The
replacement tileset is fully constructed (root tile arrived) before it
replaces the live one; a failed load leaves the current tileset untouched.

Scenario matrix (each 3x in normal mode):
  S1 switch_clean: p3 -> p8 at frame 60. End screenshot bit-identical to a
     direct-p8 reference run; selected IDs flip from p3's to p8's with no
     residue (root.glb gone after the switch, cloud.pnts present).
  S2 fail_retry: p3 -> /nonexistent (fails fast at the file probe; p3 keeps
     rendering, selected IDs unchanged) -> p8 (succeeds). Proves a failed
     loadTileset() never disturbs the live tileset.
  S3 reload_same: p3 -> p3 (same path). No crash/leak; end screenshot
     bit-identical to direct-p3.
  S4 switch_mid_load: p3 served over slow HTTP (2s/response); switch to
     local p8 at frame 10 while p3 tile content is still in flight.
     Abandons in-flight curl requests; must not crash; p8 renders after.

In --sanitized mode S1/S2/S3 run 1x and S4 runs 1x through
tests/run_sanitized.py (ASan/LSan/UBSan gate on the teardown paths).

Not asserted: byte counts, RSS. Tile-ID strings are cesium-native's
(TileIdUtilities::createTileIdString); used as opaque set members only.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = os.path.join(REPO, "build", "linux", "tiles_demo")
P3 = os.path.join(REPO, "tests", "data", "p3_box_tileset", "tileset.json")
P8 = os.path.join(REPO, "tests", "data", "p8_pnts_cloud", "tileset.json")
P3_DIR = os.path.join(REPO, "tests", "data", "p3_box_tileset")
TRAJ = os.path.join(REPO, "tests", "data", "trajectories", "p22_switch.csv")
SERVER = os.path.join(REPO, "tests", "slow_http_server.py")
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
SUPP = os.path.join(REPO, "tests", "lsan.supp")

WIDTH, HEIGHT = 400, 300
WARMUP = 60
SWITCH_AT = 60
FRAMES = 150

STAT_RE = re.compile(
    r"\[stats\] frame=(\d+) selected=(-?\d+) loading=(-?\d+) "
    r"loaded=(-?\d+) failed=(-?\d+) bytes=(-?\d+)")
SEL_RE = re.compile(r"\[selected\] frame=(\d+) ids=(.*)")
SWITCH_RE = re.compile(r"\[switch\] frame=(\d+) ok=(\d)(?: error=(.*))?")


def run_demo(args, tileset, frames, warmup=WARMUP, extra=(),
             sanitized=False, timeout=300, settle=0):
    cmd = [args.demo, "--tileset", tileset, "--trajectory", TRAJ,
           "--warmup", str(warmup), "--frames", str(frames),
           "--width", str(WIDTH), "--height", str(HEIGHT),
           "--stats", "--print-selected"] + list(extra)
    if settle > 0:
        # P30: keep rendering after the fixed scenario until the tileset
        # settles, then screenshot. The p22 trajectory is camera-static,
        # so the settled frame is comparable with the reference run.
        cmd += ["--settle-before-screenshot", str(settle)]
    if sanitized:
        cmd = ([sys.executable, RUN_SAN, "--suppressions", args.suppressions,
                "--"] + cmd)
    proc = subprocess.run(["xvfb-run", "-a"] + cmd, capture_output=True,
                          text=True, timeout=timeout)
    return proc


def parse(out):
    stats, selected, switches = {}, {}, {}
    for line in out.splitlines():
        m = STAT_RE.match(line)
        if m:
            stats[int(m.group(1))] = tuple(map(int, m.groups()[1:]))
            continue
        m = SEL_RE.match(line)
        if m:
            selected[int(m.group(1))] = set(
                x for x in m.group(2).strip().split(",") if x)
            continue
        m = SWITCH_RE.match(line)
        if m:
            switches[int(m.group(1))] = (int(m.group(2)),
                                         (m.group(3) or "").strip())
    return stats, selected, switches


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def check(cond, msg, failures):
    if not cond:
        failures.append(msg)
        print("FAIL:", msg)


def scenario_switch_clean(args, tmp, failures, sanitized):
    """S1: p3 -> p8; screenshot must equal direct-p8; IDs must flip cleanly."""
    shot = os.path.join(tmp, "s1_switch.png")
    ref = os.path.join(tmp, "s1_direct.png")
    p = run_demo(args, P3, FRAMES, extra=(
        "--switch-tileset", P8, "--switch-at-frame", str(SWITCH_AT),
        "--screenshot", shot), sanitized=sanitized, settle=120)
    check(p.returncode == 0, "S1: demo exit %d" % p.returncode, failures)
    stats, selected, switches = parse(p.stdout + p.stderr)
    check(switches.get(SWITCH_AT, (0,))[0] == 1,
          "S1: no [switch] ok=1 at frame %d (%s)" % (SWITCH_AT, switches),
          failures)
    # Before the switch: p3's tiles; after: p8's, with no p3 residue.
    for f in (SWITCH_AT - 2, SWITCH_AT - 1):
        check("root.glb" in selected.get(f, set()),
              "S1: frame %d should select root.glb (%s)" % (f, selected.get(f)),
              failures)
    for f in (SWITCH_AT + 1, SWITCH_AT + 5, FRAMES - 1):
        ids = selected.get(f, set())
        check("cloud.pnts" in ids,
              "S1: frame %d should select cloud.pnts (%s)" % (f, ids),
              failures)
        check("root.glb" not in ids and "child_a.glb" not in ids
              and "child_b.glb" not in ids,
              "S1: frame %d still selects p3 tiles (%s)" % (f, ids), failures)
    check(all(s[3] == 0 for s in stats.values()),
          "S1: some tile failed to load", failures)
    # Pixel proof: identical to loading p8 directly.
    p2 = run_demo(args, P8, FRAMES,
                  extra=("--screenshot", ref), sanitized=sanitized,
                  settle=120)
    check(p2.returncode == 0, "S1 ref: demo exit %d" % p2.returncode,
          failures)
    check(md5(shot) == md5(ref),
          "S1: switch screenshot != direct-p8 screenshot", failures)
    print("S1 switch_clean: ok (screenshot md5 %s)" % md5(shot))


def scenario_fail_retry(args, tmp, failures, sanitized):
    """S2: failed switch must not disturb the live tileset; retry succeeds."""
    bad = "/nonexistent/p22_tileset.json"
    p = run_demo(args, P3, 160, extra=(
        "--switch-tileset", bad, "--switch-at-frame", str(SWITCH_AT),
        "--switch-tileset", P8, "--switch-at-frame", "100"),
        sanitized=sanitized)
    check(p.returncode == 0, "S2: demo exit %d" % p.returncode, failures)
    stats, selected, switches = parse(p.stdout + p.stderr)
    ok, err = switches.get(SWITCH_AT, (1, ""))
    check(ok == 0, "S2: bad-path switch should fail (%s)" % (switches,),
          failures)
    check("file not found" in err,
          "S2: expected 'file not found' error, got %r" % err, failures)
    # p3 survives the failed switch: still selected, stats healthy.
    for f in (SWITCH_AT + 1, 99):
        ids = selected.get(f, set())
        check("root.glb" in ids,
              "S2: frame %d should still select root.glb after failed "
              "switch (%s)" % (f, ids), failures)
        st = stats.get(f)
        check(st is not None and st[0] > 0 and st[2] > 0,
              "S2: frame %d stats unhealthy after failed switch (%s)"
              % (f, st), failures)
    # Retry with a good tileset succeeds.
    check(switches.get(100, (0,))[0] == 1,
          "S2: retry switch should succeed (%s)" % (switches,), failures)
    ids = selected.get(159, set())
    check("cloud.pnts" in ids and "root.glb" not in ids,
          "S2: final frame should show p8 only (%s)" % (ids,), failures)
    print("S2 fail_retry: ok")


def scenario_reload_same(args, tmp, failures, sanitized):
    """S3: reloading the same tileset is safe and pixel-identical."""
    shot = os.path.join(tmp, "s3_reload.png")
    ref = os.path.join(tmp, "s3_direct.png")
    p = run_demo(args, P3, 120, extra=(
        "--switch-tileset", P3, "--switch-at-frame", str(SWITCH_AT),
        "--screenshot", shot), sanitized=sanitized, settle=120)
    check(p.returncode == 0, "S3: demo exit %d" % p.returncode, failures)
    _, selected, switches = parse(p.stdout + p.stderr)
    check(switches.get(SWITCH_AT, (0,))[0] == 1,
          "S3: no [switch] ok=1 (%s)" % (switches,), failures)
    check("root.glb" in selected.get(119, set()),
          "S3: final frame should select root.glb", failures)
    p2 = run_demo(args, P3, 120, extra=("--screenshot", ref),
                  sanitized=sanitized, settle=120)
    check(p2.returncode == 0, "S3 ref: demo exit %d" % p2.returncode,
          failures)
    check(md5(shot) == md5(ref),
          "S3: reload screenshot != direct-p3 screenshot", failures)
    print("S3 reload_same: ok (screenshot md5 %s)" % md5(shot))


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


def scenario_switch_mid_load(args, tmp, failures, sanitized):
    """S4: switch away from a slow-HTTP tileset while its content is still
    in flight. Must not crash; the new tileset renders afterwards."""
    server = SlowServer(P3_DIR, delay=2)
    server.start()
    try:
        url = "http://127.0.0.1:%d/tileset.json" % server.port
        p = run_demo(args, url, 60, warmup=5, extra=(
            "--switch-tileset", P8, "--switch-at-frame", "10"),
            sanitized=sanitized, timeout=300)
    finally:
        server.stop()
    check(p.returncode == 0, "S4: demo exit %d" % p.returncode, failures)
    stats, selected, switches = parse(p.stdout + p.stderr)
    check(switches.get(10, (0,))[0] == 1,
          "S4: no [switch] ok=1 at frame 10 (%s)" % (switches,), failures)
    # Document (not assert): was p3 actually still loading at switch time?
    pre = [stats[f][1] for f in sorted(stats) if f < 10 and f in stats]
    print("S4: p3 loading counts before switch (frames<10): %s" % pre[:8])
    ids = selected.get(59, set())
    check("cloud.pnts" in ids,
          "S4: final frame should select cloud.pnts (%s)" % (ids,), failures)
    print("S4 switch_mid_load: ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", default=DEMO)
    ap.add_argument("--sanitized", action="store_true")
    ap.add_argument("--suppressions", default=SUPP)
    args = ap.parse_args()
    reps = 1 if args.sanitized else 3
    failures = []
    with tempfile.TemporaryDirectory(prefix="p22_switch_") as tmp:
        for i in range(reps):
            print("--- rep %d/%d ---" % (i + 1, reps))
            scenario_switch_clean(args, tmp, failures,
                                  sanitized=args.sanitized)
            scenario_fail_retry(args, tmp, failures,
                                sanitized=args.sanitized)
            scenario_reload_same(args, tmp, failures,
                                 sanitized=args.sanitized)
            scenario_switch_mid_load(args, tmp, failures,
                                     sanitized=args.sanitized)
    if failures:
        print("FAILURES: %d" % len(failures))
        sys.exit(1)
    print("tileset_switch: all scenarios passed (%dx)" % reps)


if __name__ == "__main__":
    main()
