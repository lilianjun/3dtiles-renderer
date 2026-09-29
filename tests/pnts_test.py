#!/usr/bin/env python3
"""P8: pnts (Point Cloud) real-support test.

Exercises the full path
  pnts -> cesium-native PntsToGltfConverter (POINTS primitive) ->
  SDK modelToGlb (multi-buffer merge + RTC extraction, P5/P7) ->
  gltfio -> Filament
through tiles_demo, with pixel assertions on real screenshots.

Key P8 finding (no SDK code change needed): Filament v1.77's gltfio +
ubershader render POINTS primitives natively — every point lands as
exactly one pixel in its exact sRGB color (420/420 points verified).
The SDK's existing generic converted-model path (P5 RTC extraction,
P7 multi-buffer merge) already covers pnts; the pnts converter's
Z_UP_TO_Y_UP node matrix is applied natively by gltfio.

Cases (all fixtures generated deterministically by
data/gen_p8_pnts_tileset.py, no network):
  1. main:  p8_pnts_cloud  - 420 points: 200 red (Fibonacci sphere),
     120 green (ring), 100 blue (10x10 grid). Requires all three colors
     present at ~1 px per point.
  2. many:  p8_pnts_many   - 60000 points in a box. Correctness only
     (no perf claim on Mesa): must load, render, and not crash.
  3. rebase: p8_pnts_rebase/near|far - byte-identical pnts, `far` adds a
     123456789.0 m root-tile transform. Requires bit-identical
     screenshots (P5-style rebase proof, now for point clouds).
  4. rtc:   p8_pnts_rtc vs p8_pnts_rtc_ref - ECEF-magnitude RTC_CENTER
     ([1210000.0, -4736290.5, 4081600.0]) extracted by the SDK in double
     precision. Requires bit-identical screenshots against the no-RTC
     reference.

Usage:
  pnts_test.py --demo <tiles_demo> --out <png> [--frames 60 ...]
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


def count_teal(img):
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    return int(((r < 110) & (g > 140) & (b > 140)).sum())


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


def check_identical(a_path, b_path, what):
    a = np.asarray(Image.open(a_path).convert("RGB")).astype(np.int32)
    b = np.asarray(Image.open(b_path).convert("RGB")).astype(np.int32)
    if a.shape != b.shape:
        return "shape mismatch %s vs %s" % (a.shape, b.shape)
    diff = np.abs(a - b)
    n = int((diff.sum(axis=2) > 0).sum())
    print("%s: max channel diff %d, differing pixels %d"
          % (what, int(diff.max()), n), flush=True)
    if n != 0:
        return "%s: screenshots differ in %d pixels" % (what, n)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--out", required=True,
                    help="main cloud screenshot")
    ap.add_argument("--many-out", required=True)
    ap.add_argument("--rebase-near-out", required=True)
    ap.add_argument("--rebase-far-out", required=True)
    ap.add_argument("--rtc-out", required=True)
    ap.add_argument("--rtc-ref-out", required=True)
    ap.add_argument("--frames", type=int, default=60)
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()
    failures = []

    def tile(name):
        return os.path.join(DATA, name, "tileset.json")

    # Case 1: main cloud — 200 red + 120 green + 100 blue points.
    # Filament renders each point as exactly one pixel in its exact sRGB
    # color (verified: 420/420 on Mesa); allow 5% slack for rasterization
    # differences on other drivers.
    print("=== case 1: main pnts cloud (420 points) ===", flush=True)
    res, err = render_demo(args.demo, tile("p8_pnts_cloud"), args.out,
                           args.frames, args.width, args.height)
    if err is not None:
        failures.append("main: %s" % err)
    else:
        rendered, img = res
        print("tiles rendered (last frame): %d" % rendered, flush=True)
        if rendered != 1:
            failures.append("main: expected 1 tile rendered, got %d"
                            % rendered)
        n_r = count_exact(img, (255, 0, 0))
        n_g = count_exact(img, (0, 255, 0))
        n_b = count_exact(img, (0, 0, 255))
        print("red=%d/200 green=%d/120 blue=%d/100" % (n_r, n_g, n_b),
              flush=True)
        if n_r < 190:
            failures.append("main: too few red points: %d/200" % n_r)
        if n_g < 114:
            failures.append("main: too few green points: %d/120" % n_g)
        if n_b < 95:
            failures.append("main: too few blue points: %d/100" % n_b)

    # Case 2: 60000 points — correctness only, no perf claim.
    print("=== case 2: 60000 points ===", flush=True)
    res, err = render_demo(args.demo, tile("p8_pnts_many"), args.many_out,
                           args.frames, args.width, args.height)
    if err is not None:
        failures.append("many: %s" % err)
    else:
        rendered, img = res
        print("tiles rendered (last frame): %d" % rendered, flush=True)
        if rendered < 1:
            failures.append("many: no tile rendered")
        n_t = count_teal(img)
        print("many: teal pixels=%d (60000 points)" % n_t, flush=True)
        if n_t < 30000:
            failures.append("many: too few teal pixels: %d" % n_t)

    # Case 3: tile-transform rebase — bit-identical near/far.
    print("=== case 3: rebase near/far ===", flush=True)
    for name, out in (("near", args.rebase_near_out),
                      ("far", args.rebase_far_out)):
        res, err = render_demo(
            args.demo, tile(os.path.join("p8_pnts_rebase", name)), out,
            args.frames, args.width, args.height)
        if err is not None:
            failures.append("rebase %s: %s" % (name, err))
    err = check_identical(args.rebase_near_out, args.rebase_far_out,
                          "rebase near vs far")
    if err is not None:
        failures.append(err)

    # Case 4: RTC_CENTER — bit-identical vs the no-RTC reference.
    print("=== case 4: RTC_CENTER vs reference ===", flush=True)
    for name, sub, out in (("rtc", "p8_pnts_rtc", args.rtc_out),
                           ("rtc_ref", "p8_pnts_rtc_ref", args.rtc_ref_out)):
        res, err = render_demo(args.demo, tile(sub), out,
                               args.frames, args.width, args.height)
        if err is not None:
            failures.append("rtc %s: %s" % (name, err))
    err = check_identical(args.rtc_out, args.rtc_ref_out,
                          "rtc vs no-rtc reference")
    if err is not None:
        failures.append(err)

    if failures:
        print("FAIL: pnts_test (%d failure(s)):" % len(failures), flush=True)
        for f in failures:
            print("  - %s" % f, flush=True)
        return 1
    print("PASS: pnts_test (420 pts + 60000 + rebase + RTC_CENTER)",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
