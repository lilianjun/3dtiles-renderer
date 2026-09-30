#!/usr/bin/env python3
"""P7: i3dm (Instanced 3D Models) real-support test.

Exercises the full path
  i3dm -> cesium-native I3dmToGltfConverter (EXT_mesh_gpu_instancing) ->
  SDK modelToGlb (multi-buffer merge + instance expansion + up-axis fix) ->
  gltfio -> Filament
through tiles_demo, with pixel assertions on real screenshots.

Cases (all fixtures generated deterministically by
data/gen_p7_i3dm_tileset.py, no network):
  1. main:  p7_i3dm_tileset  - 12 orange instances (POSITION+SCALE) in one
     tile + 8 teal instances (per-instance Y rotation via
     NORMAL_UP/NORMAL_RIGHT) in a second tile. Requires both tiles
     rendered, both colors well represented, and SEVERAL distinct blobs
     per color -- one rendered box per tile would give exactly one blob,
     so this proves instancing really expanded.
  2. many:  p7_i3dm_many    - 256 instances in a single i3dm. Correctness
     only (no perf claim on Mesa): must load, render 60 frames, and show
     several instances; must not crash or hit an instance limit.
  3. rebase: p7_i3dm_rebase/near|far - byte-identical i3dm, `far` adds a
     123456789.0 m root-tile translation. Requires bit-identical
     screenshots (P5-style rebase proof, now for instanced content).
  4. rtc:   p7_i3dm_rtc vs p7_i3dm_rtc_ref - ECEF-magnitude RTC_CENTER
     ([1210000.0, -4736290.5, 4081600.0]) baked by the converter into the
     glTF node translation. Requires bit-identical screenshots against
     the no-RTC reference (proves the converter's up-axis-conjugated RTC
     offset is cancelled exactly by the SDK's double-precision rebase).

Usage:
  i3dm_test.py --demo <tiles_demo> --out <png> [--frames 60 ...]
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")

from PIL import Image  # noqa: E402
import numpy as np  # noqa: E402


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


def big_components(mask, min_px=200):
    """Count components with more than min_px pixels (erode tiny specks)."""
    h, w = mask.shape
    seen = np.zeros((h, w), dtype=bool)
    big = 0
    for y in range(h):
        for x in range(w):
            if mask[y, x] and not seen[y, x]:
                size = 0
                stack = [(y, x)]
                seen[y, x] = True
                while stack:
                    cy, cx = stack.pop()
                    size += 1
                    for ny, nx in ((cy - 1, cx), (cy + 1, cx),
                                   (cy, cx - 1), (cy, cx + 1)):
                        if (0 <= ny < h and 0 <= nx < w and mask[ny, nx]
                                and not seen[ny, nx]):
                            seen[ny, nx] = True
                            stack.append((ny, nx))
                if size >= min_px:
                    big += 1
    return big


def render_demo(demo, tileset, out, frames, width, height):
    # P30: settle-gated capture (kills the fixed-frame screenshot race,
    # the P26-P28 flake class). `frames` is the give-up budget.
    cmd = [demo, "--until-loaded", str(4 * frames),
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
                    help="main tileset screenshot")
    ap.add_argument("--many-out", required=True)
    ap.add_argument("--rebase-near-out", required=True)
    ap.add_argument("--rebase-far-out", required=True)
    ap.add_argument("--rtc-out", required=True)
    ap.add_argument("--rtc-ref-out", required=True)
    ap.add_argument("--frames", type=int, default=60,
                      help="P30: max frames (settle budget)")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()
    failures = []

    def tile(name):
        return os.path.join(DATA, name, "tileset.json")

    # Case 1: main tileset — 12 orange + 8 teal instances, 2 tiles.
    print("=== case 1: main i3dm tileset (instancing) ===", flush=True)
    res, err = render_demo(args.demo, tile("p7_i3dm_tileset"), args.out,
                           args.frames, args.width, args.height)
    if err is not None:
        failures.append("main: %s" % err)
    else:
        rendered, img = res
        print("tiles rendered (last frame): %d" % rendered, flush=True)
        if rendered != 2:
            failures.append("main: expected 2 tiles rendered, got %d"
                            % rendered)
        om, tm = is_orange(img), is_teal(img)
        n_o, n_t = int(om.sum()), int(tm.sum())
        big_o, big_t = big_components(om), big_components(tm)
        print("orange px=%d big_blobs=%d | teal px=%d big_blobs=%d"
              % (n_o, big_o, n_t, big_t), flush=True)
        # One box per tile would yield exactly one blob per color; several
        # blobs prove the instances really expanded and rendered.
        if n_o < 5000:
            failures.append("main: too few orange pixels: %d" % n_o)
        if big_o < 8:
            failures.append("main: too few orange blobs: %d "
                            "(12 instances expected)" % big_o)
        if n_t < 2000:
            failures.append("main: too few teal pixels: %d" % n_t)
        if big_t < 5:
            failures.append("main: too few teal blobs: %d "
                            "(8 rotated instances expected)" % big_t)

    # Case 2: 256 instances — correctness only, no perf claim.
    print("=== case 2: 256 instances ===", flush=True)
    res, err = render_demo(args.demo, tile("p7_i3dm_many"), args.many_out,
                           args.frames, args.width, args.height)
    if err is not None:
        failures.append("many: %s" % err)
    else:
        rendered, img = res
        print("tiles rendered (last frame): %d" % rendered, flush=True)
        if rendered < 1:
            failures.append("many: no tile rendered")
        n_big = big_components(is_orange(img))
        print("many: orange big blobs=%d" % n_big, flush=True)
        if n_big < 5:
            failures.append("many: too few orange blobs: %d" % n_big)

    # Case 3: tile-transform rebase — bit-identical near/far.
    print("=== case 3: rebase near/far ===", flush=True)
    for name, out in (("near", args.rebase_near_out),
                      ("far", args.rebase_far_out)):
        res, err = render_demo(
            args.demo, tile(os.path.join("p7_i3dm_rebase", name)), out,
            args.frames, args.width, args.height)
        if err is not None:
            failures.append("rebase %s: %s" % (name, err))
    err = check_identical(args.rebase_near_out, args.rebase_far_out,
                          "rebase near vs far")
    if err is not None:
        failures.append(err)

    # Case 4: RTC_CENTER — bit-identical vs the no-RTC reference.
    print("=== case 4: RTC_CENTER vs reference ===", flush=True)
    for name, sub, out in (("rtc", "p7_i3dm_rtc", args.rtc_out),
                           ("rtc_ref", "p7_i3dm_rtc_ref", args.rtc_ref_out)):
        res, err = render_demo(args.demo, tile(sub), out,
                               args.frames, args.width, args.height)
        if err is not None:
            failures.append("rtc %s: %s" % (name, err))
    err = check_identical(args.rtc_out, args.rtc_ref_out,
                          "rtc vs no-rtc reference")
    if err is not None:
        failures.append(err)

    if failures:
        print("FAIL: i3dm_test (%d failure(s)):" % len(failures), flush=True)
        for f in failures:
            print("  - %s" % f, flush=True)
        return 1
    print("PASS: i3dm_test (instancing + 256 + rebase + RTC_CENTER)",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
