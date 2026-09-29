#!/usr/bin/env python3
"""P9: cmpt (Composite) real-support test.

Exercises the full path
  cmpt -> cesium-native CmptToGltfConverter (recursive inner-tile
  conversion + Model::merge) -> SDK modelToGlb -> gltfio -> Filament
through tiles_demo, with pixel assertions on a real screenshot.

The composite fixture (data/p9_cmpt_tileset/composite.cmpt, generated
deterministically by data/gen_p9_cmpt_tileset.py, no network) packs three
inner tiles:
  1. b3dm: 4 m orange box at the origin
  2. pnts: 64 green points in a 2.4 m box at (10, 0.5, 0)
  3. i3dm: 6 teal instances (2x3) around (-10, 0, 0), scale 1.3

Assertions:
  - all three contents visible in one frame (orange box pixels, green
    point pixels at ~1 px/point, teal instance pixels in several blobs);
  - exactly 1 tile rendered (the merged cmpt is a single tile).

Usage:
  cmpt_test.py --demo <tiles_demo> --out <png> [--frames 60 ...]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")

from PIL import Image  # noqa: E402
import numpy as np  # noqa: E402


def count_exact(img, color):
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    return int(((r == color[0]) & (g == color[1]) & (b == color[2])).sum())


def is_orange(img):
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    return (r > 180) & (g > 90) & (g < 170) & (b < 90)


def is_teal(img):
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    return (r < 110) & (g > 140) & (b > 140)


def count_components(mask):
    """4-connected component count via iterative flood fill (no scipy)."""
    h, w = mask.shape
    seen = np.zeros((h, w), dtype=bool)
    n = 0
    for y in range(h):
        for x in range(w):
            if mask[y, x] and not seen[y, x]:
                n += 1
                stack = [(y, x)]
                seen[y, x] = True
                while stack:
                    cy, cx = stack.pop()
                    for ny, nx in ((cy - 1, cx), (cy + 1, cx),
                                   (cy, cx - 1), (cy, cx + 1)):
                        if (0 <= ny < h and 0 <= nx < w and mask[ny, nx]
                                and not seen[ny, nx]):
                            seen[ny, nx] = True
                            stack.append((ny, nx))
    return n


def render_demo(demo, tileset, out, frames, width, height):
    cmd = [demo, "--frames", str(frames),
           "--width", str(width), "--height", str(height),
           "--tileset", tileset, "--screenshot", out]
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        return None, "demo exited %d" % proc.returncode
    rendered = -1
    for line in proc.stdout.splitlines():
        if "tiles rendered (last frame):" in line:
            try:
                rendered = int(line.rsplit(":", 1)[1].strip())
            except ValueError:
                pass
    if not os.path.exists(out):
        return None, "screenshot not written: %s" % out
    img = np.asarray(Image.open(out).convert("RGB")).astype(np.int32)
    if img.shape[1] != width or img.shape[0] != height:
        return None, "screenshot size %dx%d, expected %dx%d" % (
            img.shape[1], img.shape[0], width, height)
    return (rendered, img), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--out", required=True, help="cmpt screenshot")
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()
    failures = []

    print("=== cmpt: b3dm + pnts + i3dm in one composite tile ===",
          flush=True)
    res, err = render_demo(
        args.demo,
        os.path.join(DATA, "p9_cmpt_tileset", "tileset.json"),
        args.out, args.frames, args.width, args.height)
    if err is not None:
        failures.append("cmpt: %s" % err)
    else:
        rendered, img = res
        print("tiles rendered (last frame): %d" % rendered, flush=True)
        if rendered != 1:
            failures.append("cmpt: expected 1 tile rendered, got %d"
                            % rendered)

        # Inner tile 1 (b3dm): orange box must be well represented.
        n_orange = int(is_orange(img).sum())
        print("orange px: %d" % n_orange, flush=True)
        if n_orange < 1000:
            failures.append("cmpt: too few orange (b3dm) pixels: %d"
                            % n_orange)

        # Inner tile 2 (pnts): 64 green points, ~1 px each (P8 proved
        # Filament renders points at exactly 1 px in the exact sRGB
        # color; 5% slack for other drivers).
        n_green = count_exact(img, (0, 255, 0))
        print("green px: %d/64" % n_green, flush=True)
        if n_green < 60:
            failures.append("cmpt: too few green (pnts) points: %d/64"
                            % n_green)

        # Inner tile 3 (i3dm): 6 teal instances — several distinct blobs
        # prove the instances expanded instead of collapsing to one mesh.
        teal_mask = is_teal(img)
        n_teal = int(teal_mask.sum())
        n_blobs = count_components(teal_mask)
        print("teal px: %d, blobs: %d" % (n_teal, n_blobs), flush=True)
        if n_teal < 500:
            failures.append("cmpt: too few teal (i3dm) pixels: %d" % n_teal)
        if n_blobs < 3:
            failures.append("cmpt: expected >=3 teal blobs (6 instances), "
                            "got %d" % n_blobs)

    if failures:
        print("FAIL:")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("PASS: cmpt composite renders all three inner tiles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
