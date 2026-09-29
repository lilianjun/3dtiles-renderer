#!/usr/bin/env python3
"""P3 tileset rendering test.

Runs the demo with the vendored tiny tileset (tests/data/p3_box_tileset/),
renders N frames headlessly, and checks the screenshot:

  1. the demo exits 0 and reports at least one tile rendered;
  2. the screenshot is not all black;
  3. the dark-blue clear color is still present (background);
  4. pixels of BOTH child-tile colors are present — orange (child_a) and
     teal (child_b) — which proves real tile selection/LOD + glTF content
     rendering, clearly different from the P2 clear+triangle image.

Usage:
  python3 tests/tileset_test.py --demo <tiles_demo> --tileset <tileset.json>
      [--frames N] [--width W] [--height H] [--out <png>]
"""
import argparse
import os
import subprocess
import sys

try:
    from PIL import Image
except ImportError:
    print("FAIL: Pillow is required", file=sys.stderr)
    sys.exit(2)


def is_orange(r, g, b):
    # child_a base color (1.0, 0.45, 0.10), shaded by the sun; lenient band.
    return r > 140 and r > g + 30 and b < 130 and g > 40


def is_teal(r, g, b):
    # child_b base color (0.10, 0.75, 0.75), shaded by the sun; lenient band.
    return g > 110 and b > 110 and r < 120 and g > r + 20 and b > r + 20


def is_clear(r, g, b):
    # dark-blue clear (0.1, 0.2, 0.45) -> ~(26, 51, 115), allow some drift.
    return abs(r - 26) < 40 and abs(g - 51) < 40 and abs(b - 115) < 50


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--tileset", required=True)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    cmd = [args.demo, "--frames", str(args.frames),
           "--width", str(args.width), "--height", str(args.height),
           "--tileset", args.tileset, "--screenshot", args.out]
    if not os.environ.get("DISPLAY"):
        # No X server (plain `ctest` on a headless box): wrap ourselves,
        # same as the P2 screenshot test.
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        print(f"FAIL: demo exited {proc.returncode}")
        return 1

    # The demo prints the last frame's renderable tile count.
    rendered = -1
    for line in proc.stdout.splitlines():
        if "tiles rendered (last frame):" in line:
            try:
                rendered = int(line.rsplit(":", 1)[1].strip())
            except ValueError:
                pass
    print(f"tiles rendered (last frame): {rendered}")
    if rendered < 1:
        print("FAIL: no tile was selected for rendering")
        return 1

    if not os.path.exists(args.out):
        print(f"FAIL: screenshot not written: {args.out}")
        return 1
    img = Image.open(args.out).convert("RGB")
    w, h = img.size
    if (w, h) != (args.width, args.height):
        print(f"FAIL: screenshot size {w}x{h}, expected "
              f"{args.width}x{args.height}")
        return 1
    px = img.load()
    n_orange = n_teal = n_clear = n_black = 0
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if r < 12 and g < 12 and b < 12:
                n_black += 1
            if is_orange(r, g, b):
                n_orange += 1
            elif is_teal(r, g, b):
                n_teal += 1
            elif is_clear(r, g, b):
                n_clear += 1
    total = w * h
    print(f"pixels: total={total} clear={n_clear} orange={n_orange} "
          f"teal={n_teal} black={n_black}")
    if n_black == total:
        print("FAIL: screenshot is all black")
        return 1
    if n_clear < total * 0.10:
        print("FAIL: clear-color background nearly absent "
              "(camera framing wrong?)")
        return 1
    # Both LOD children must contribute visible pixels: this is what makes
    # the P3 image provably different from the P2 clear+red-triangle image.
    if n_orange < 200:
        print(f"FAIL: too few orange (child_a) pixels: {n_orange}")
        return 1
    if n_teal < 200:
        print(f"FAIL: too few teal (child_b) pixels: {n_teal}")
        return 1
    print("PASS: tileset rendered (orange + teal tiles visible)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
