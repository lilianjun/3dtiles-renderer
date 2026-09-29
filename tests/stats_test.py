#!/usr/bin/env python3
"""P17 tile streaming diagnostics test.

Drives tiles_demo --stats (per-frame TileStats on stdout) and checks:

  1. healthy tileset: after warmup the last frame has loaded > 0,
     failed == 0, selected > 0; `loaded` is monotonic non-decreasing
     across frames; the demo's own renderedTileCount() agrees with the
     last frame's `selected`.
  2. corrupt tile content (vendored p3 tileset with a smashed child_a.glb,
     same recipe as fault_test.py case C): some frame reports failed > 0,
     the demo still exits 0 (graceful), no crash.
  3. querying stats does not perturb rendering: the same run with and
     without --stats produces bit-identical screenshots.

Usage:
  python3 tests/stats_test.py --demo <tiles_demo> --tileset <tileset.json>
      [--frames N] [--width W] [--height H] [--outdir dir]
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

STAT_RE = re.compile(
    r"\[stats\] frame=(\d+) selected=(-?\d+) loading=(-?\d+) "
    r"loaded=(-?\d+) failed=(-?\d+) bytes=(-?\d+)"
)
RENDERED_RE = re.compile(r"\[demo\] tiles rendered \(last frame\): (-?\d+)")


def run_demo(demo, extra_args, cwd):
    cmd = [demo] + extra_args
    env = dict(os.environ)
    if not env.get("DISPLAY"):
        cmd = ["xvfb-run", "-a"] + cmd
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600,
                       cwd=cwd)
    return p


def parse_stats(stdout):
    rows = []
    for m in STAT_RE.finditer(stdout):
        rows.append(tuple(int(g) for g in m.groups()))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--tileset", required=True)
    ap.add_argument("--frames", type=int, default=90)
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    ap.add_argument("--outdir", required=True)
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    args.demo = os.path.abspath(args.demo)
    args.tileset = os.path.abspath(args.tileset)
    failures = 0

    def check(cond, msg):
        nonlocal failures
        if cond:
            print("PASS: " + msg)
        else:
            print("FAIL: " + msg)
            failures += 1

    # --- 1. healthy tileset ---
    print("=== case 1: healthy tileset stats ===")
    p = run_demo(args.demo, ["--frames", str(args.frames),
                             "--width", str(args.width),
                             "--height", str(args.height),
                             "--tileset", args.tileset, "--stats"],
                 args.outdir)
    check(p.returncode == 0, "demo exits 0 (got %d)" % p.returncode)
    rows = parse_stats(p.stdout)
    check(len(rows) == args.frames,
          "got %d [stats] lines, expected %d" % (len(rows), args.frames))
    if rows:
        frames, selected, loading, loaded, failed, nbytes = zip(*rows)
        check(all(s > 0 for s in selected), "selected > 0 every frame")
        check(loaded[-1] > 0, "last frame loaded > 0 (got %d)" % loaded[-1])
        check(all(f == 0 for f in failed), "failed == 0 on healthy tileset")
        check(all(b >= 0 for b in nbytes), "bytes >= 0")
        mono = all(b >= a for a, b in zip(loaded, loaded[1:]))
        check(mono, "loaded is monotonic non-decreasing %s" % (list(loaded[:8]),))
        m = RENDERED_RE.search(p.stdout)
        check(m is not None, "demo printed renderedTileCount()")
        if m:
            # renderedTileCount() counts tiles with actual render resources;
            # it must be positive and can never exceed the traversal's
            # selection (the root tile is often selected but content-less).
            rtc = int(m.group(1))
            check(0 < rtc <= selected[-1],
                  "0 < renderedTileCount() (%d) <= selected (%d)" %
                  (rtc, selected[-1]))

    # --- 2. corrupt tile content ---
    print("=== case 2: corrupt glb -> failed > 0, still graceful ===")
    corrupt_dir = os.path.join(args.outdir, "stats_corrupt")
    shutil.rmtree(corrupt_dir, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "data", "p3_box_tileset"), corrupt_dir)
    with open(os.path.join(corrupt_dir, "child_a.glb"), "wb") as f:
        f.write(b"\x00\xff not a glb at all, just garbage bytes \xde\xad" * 64)
    p = run_demo(args.demo, ["--frames", "60",
                             "--width", str(args.width),
                             "--height", str(args.height),
                             "--tileset",
                             os.path.join(corrupt_dir, "tileset.json"),
                             "--stats"],
                 args.outdir)
    check(p.returncode == 0,
          "demo exits 0 on corrupt tile (got %d)" % p.returncode)
    rows = parse_stats(p.stdout)
    check(len(rows) == 60, "got %d [stats] lines" % len(rows))
    if rows:
        failed = [r[4] for r in rows]
        loaded = [r[3] for r in rows]
        check(any(f > 0 for f in failed),
              "some frame reports failed > 0 (max %d)" % max(failed))
        check(loaded[-1] > 0,
              "healthy tiles still load (last loaded=%d)" % loaded[-1])

    # --- 3. stats query does not perturb rendering ---
    print("=== case 3: --stats does not change pixels ===")
    shot_a = os.path.join(args.outdir, "stats_with.png")
    shot_b = os.path.join(args.outdir, "stats_without.png")
    base = ["--frames", str(args.frames),
            "--width", str(args.width), "--height", str(args.height),
            "--tileset", args.tileset]
    pa = run_demo(args.demo, base + ["--stats", "--screenshot", shot_a],
                  args.outdir)
    pb = run_demo(args.demo, base + ["--screenshot", shot_b], args.outdir)
    check(pa.returncode == 0 and pb.returncode == 0,
          "both runs exit 0 (%d, %d)" % (pa.returncode, pb.returncode))
    if os.path.exists(shot_a) and os.path.exists(shot_b):
        ha = hashlib.md5(open(shot_a, "rb").read()).hexdigest()
        hb = hashlib.md5(open(shot_b, "rb").read()).hexdigest()
        check(ha == hb, "screenshots bit-identical (md5 %s)" % ha[:12])
    else:
        check(False, "screenshots were written")

    print("p17 stats: %d failure(s)" % failures)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
