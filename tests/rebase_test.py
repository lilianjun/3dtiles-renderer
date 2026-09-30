#!/usr/bin/env python3
"""P5: ECEF-style rebase test.

Renders two tilesets with byte-identical local geometry:
  - near: tileset at the origin (no root transform)
  - far:  same tileset, root tile translated by 123456789.0 m
           (float32-hostile: without the P5 rebase the tiles would render
            ~1.2e8 m off-screen and nothing orange would appear)

Passes iff:
  1. Both renders pass tileset_test.py's pixel assertions (orange present),
     reusing the same demo/frames/camera — i.e. the far tileset renders
     despite its huge translation.
  2. The orange (child tile) pixel centroid in `far` is within 25 px of the
     centroid in `near` — i.e. the rebase reproduces the near-camera image.

Usage:
    rebase_test.py --demo <tiles_demo> --near-out <png> --far-out <png>
                   [--frames 60 ...]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data", "p5_rebase_tileset")

from PIL import Image  # noqa: E402
import numpy as np  # noqa: E402


def is_orange(px):
    r, g, b = px[:, :, 0], px[:, :, 1], px[:, :, 2]
    return (r > 140) & (r > g + 30) & (b < 130) & (g > 40)


def orange_centroid(path):
    img = np.asarray(Image.open(path).convert("RGB")).astype(np.int32)
    ys, xs = np.nonzero(is_orange(img))
    if len(xs) == 0:
        return None
    return (float(xs.mean()), float(ys.mean()), len(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--near-out", required=True)
    ap.add_argument("--far-out", required=True)
    ap.add_argument("--frames", type=int, default=60,
                      help="P30: max frames (settle budget)")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()

    outs = {}
    for name, sub in (("near", "near"), ("far", "far")):
        out = args.near_out if name == "near" else args.far_out
        outs[name] = out
        cmd = [sys.executable, os.path.join(HERE, "tileset_test.py"),
               "--demo", args.demo,
               "--tileset", os.path.join(DATA, sub, "tileset.json"),
               "--out", out,
               "--frames", str(args.frames),
               "--width", str(args.width),
               "--height", str(args.height),
               "--expect", "orange"]
        print("rendering %s tileset ..." % name, flush=True)
        r = subprocess.run(cmd)
        if r.returncode != 0:
            print("FAIL: tileset_test.py failed for '%s'" % name, flush=True)
            return 1

    c_near = orange_centroid(outs["near"])
    c_far = orange_centroid(outs["far"])
    print("near orange centroid: (%.1f, %.1f) px=%d" % c_near, flush=True)
    print("far  orange centroid: (%.1f, %.1f) px=%d" % c_far, flush=True)
    dist = abs(c_near[0] - c_far[0]) + abs(c_near[1] - c_far[1])
    print("centroid L1 distance: %.1f px (tolerance 25 px)" % dist, flush=True)
    if dist > 25.0:
        print("FAIL: rebase did not reproduce the near-camera image", flush=True)
        return 1
    print("PASS: far tileset rebases to the near-camera image", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
