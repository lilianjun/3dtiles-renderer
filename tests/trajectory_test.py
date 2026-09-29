#!/usr/bin/env python3
"""P16: deterministic camera trajectory replay test (ADR-0014).

Runs tiles_demo twice over the P3 box tileset with the same trajectory
(tests/data/trajectories/p16_orbit_push.csv), dumping every logical frame
as PNG, and asserts:

  1. the demo exits 0 and reports at least one tile rendered, on both runs;
  2. EVERY frame PNG is byte-identical between the two runs
     (bit-identical replay — the whole point of logical-frame driving);
  3. the camera actually moved: frame 0 and frame 11 differ;
  4. both child-tile colors (orange + teal) are visible in the first,
     middle and last frames — the trajectory keeps the tiles in view.

Async tile loading is isolated by the demo's warmup (fixed 60 frames at the
first-keyframe camera before the recorded trajectory starts); any loading
timing leak into the trajectory frames would break assertion 2.

Usage:
  python3 tests/trajectory_test.py --demo <tiles_demo> --workdir <dir>
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

try:
    from PIL import Image
except ImportError:
    print("FAIL: Pillow is required", file=sys.stderr)
    sys.exit(2)

HERE = os.path.dirname(os.path.abspath(__file__))
TRAJECTORY = os.path.join(HERE, "data", "trajectories", "p16_orbit_push.csv")
TILESET = os.path.join(HERE, "data", "p3_box_tileset", "tileset.json")
WIDTH, HEIGHT = 800, 600


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def run_demo(demo, outdir):
    shutil.rmtree(outdir, ignore_errors=True)
    os.makedirs(outdir, exist_ok=True)
    cmd = [demo,
           "--tileset", TILESET,
           "--trajectory", TRAJECTORY,
           "--frame-dir", outdir,
           "--width", str(WIDTH), "--height", str(HEIGHT)]
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        return f"demo exited {proc.returncode}"
    rendered = -1
    for line in proc.stdout.splitlines():
        if "tiles rendered (last frame):" in line:
            try:
                rendered = int(line.rsplit(":", 1)[1].strip())
            except ValueError:
                pass
    if rendered < 1:
        return f"no tile rendered (count={rendered})"
    frames = sorted(f for f in os.listdir(outdir)
                    if f.startswith("frame_") and f.endswith(".png"))
    if len(frames) != 12:
        return f"expected 12 frame PNGs, got {len(frames)}"
    return None


def is_orange(r, g, b):
    return r > 140 and r > g + 30 and b < 130 and g > 40


def is_teal(r, g, b):
    return g > 110 and b > 110 and r < 120 and g > r + 20 and b > r + 20


def check_tiles_visible(path):
    img = Image.open(path).convert("RGB")
    if img.size != (WIDTH, HEIGHT):
        return f"size {img.size}, expected {(WIDTH, HEIGHT)}"
    px = img.load()
    n_orange = n_teal = 0
    for y in range(0, HEIGHT, 2):
        for x in range(0, WIDTH, 2):
            r, g, b = px[x, y]
            if is_orange(r, g, b):
                n_orange += 1
            elif is_teal(r, g, b):
                n_teal += 1
    if n_orange < 50 or n_teal < 50:
        return (f"child tiles not visible in {os.path.basename(path)}: "
                f"orange={n_orange}, teal={n_teal}")
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--workdir", required=True)
    args = ap.parse_args()

    run1 = os.path.join(args.workdir, "run1")
    run2 = os.path.join(args.workdir, "run2")

    err = run_demo(args.demo, run1)
    if err:
        print(f"FAIL (run 1): {err}")
        return 1
    err = run_demo(args.demo, run2)
    if err:
        print(f"FAIL (run 2): {err}")
        return 1

    # Bit-identical replay: every frame PNG must match across runs.
    frames = sorted(f for f in os.listdir(run1)
                    if f.startswith("frame_") and f.endswith(".png"))
    for f in frames:
        h1, h2 = md5(os.path.join(run1, f)), md5(os.path.join(run2, f))
        if h1 != h2:
            print(f"FAIL: {f} differs between runs ({h1} vs {h2}) — "
                  f"trajectory replay is not deterministic")
            return 1
    print(f"OK: all {len(frames)} frames bit-identical across two runs")

    # The camera actually moved along the trajectory.
    if md5(os.path.join(run1, frames[0])) == md5(os.path.join(run1, frames[-1])):
        print("FAIL: first and last frames identical — camera did not move")
        return 1
    print("OK: first/last frames differ (camera moved)")

    # Both child tiles stay in view on first/middle/last frames.
    for f in (frames[0], frames[len(frames) // 2], frames[-1]):
        err = check_tiles_visible(os.path.join(run1, f))
        if err:
            print(f"FAIL: {err}")
            return 1
    print("OK: orange + teal child tiles visible in first/middle/last frames")

    print("[trajectory-test] OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
