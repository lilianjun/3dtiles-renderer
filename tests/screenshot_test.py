#!/usr/bin/env python3
"""P2 screenshot test: render N frames headless, assert the PNG is a real render.

Runs tiles_demo (under xvfb-run when no X display is available), then checks
with Pillow that the screenshot:
  - exists and has the expected size,
  - is not (nearly) all black,
  - contains the dark-blue clear color,
  - contains the red triangle.

Exit 0 on success, 1 on failure (with a reason on stdout).
"""
import argparse
import os
import subprocess
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True, help="path to tiles_demo binary")
    ap.add_argument("--out", required=True, help="screenshot PNG path to produce")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)

    cmd = [
        args.demo,
        "--frames", str(args.frames),
        "--width", str(args.width),
        "--height", str(args.height),
        "--screenshot", args.out,
    ]
    if not os.environ.get("DISPLAY"):
        # No X server (plain `ctest` on a headless box): wrap ourselves.
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd

    print("[screenshot-test] running:", " ".join(cmd), flush=True)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        print("[screenshot-test] FAIL: demo timed out")
        return 1
    print(r.stdout, flush=True)
    print(r.stderr, file=sys.stderr, flush=True)
    if r.returncode != 0:
        print(f"[screenshot-test] FAIL: demo exited {r.returncode}")
        return 1
    if not os.path.isfile(args.out):
        print(f"[screenshot-test] FAIL: {args.out} was not created")
        return 1

    try:
        from PIL import Image
    except ImportError:
        print("[screenshot-test] FAIL: Pillow is not installed (pip install pillow)")
        return 1

    im = Image.open(args.out).convert("RGB")
    if im.size != (args.width, args.height):
        print(f"[screenshot-test] FAIL: size {im.size}, "
              f"expected {(args.width, args.height)}")
        return 1

    px = list(im.getdata())
    n = len(px)

    bright = sum(1 for p in px if sum(p) > 60)
    if bright < n * 0.05:
        print("[screenshot-test] FAIL: image is (nearly) all black")
        return 1

    # Clear color is dark blue (0.1, 0.2, 0.45) -> ~(26, 51, 115) in 8-bit.
    def is_clear(p):
        return p[0] < 100 and p[1] < 150 and p[2] > 80 and p[2] > p[0]

    # Triangle is pure red (1, 0, 0) -> ~(255, 0, 0).
    def is_triangle(p):
        return p[0] > 150 and p[1] < 100 and p[2] < 100

    n_clear = sum(1 for p in px if is_clear(p))
    n_tri = sum(1 for p in px if is_triangle(p))
    print(f"[screenshot-test] clear-ish pixels: {n_clear}, "
          f"triangle-ish pixels: {n_tri}", flush=True)
    if n_clear < n * 0.20:
        print("[screenshot-test] FAIL: clear color not found in screenshot")
        return 1
    if n_tri < 100:
        print("[screenshot-test] FAIL: red triangle not found in screenshot")
        return 1

    print(f"[screenshot-test] OK: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
