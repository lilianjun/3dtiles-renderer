#!/usr/bin/env python3
"""P10: 3D Tiles 1.1 real-support test.

Exercises the full path for the two 1.1 features through tiles_demo,
with pixel assertions on real screenshots:

  A. 3DTILES_content_gltf: asset.version "1.1", tile content is a bare
     .glb (magic "glTF" -> BinaryToGltfConverter -> same SDK render
     bridge as the 1.0 formats). Fixture: data/p10_11_glb/ (orange root
     box + teal child box).
  B. 3DTILES_implicit_tiling: QUADTREE, subtreeLevels 2 /
     availableLevels 2, subtree as hand-written plain JSON with constant
     availability, content/template URLs resolved per tile.
     Fixture: data/p10_11_implicit/ (5 tiles: red L0 + green/blue/
     yellow/magenta L1 quadrant boxes).

Assertions:
  - A: 2 tiles rendered; orange and teal pixels well represented.
  - B: 5 tiles rendered; each of the 5 box colors present (top faces
    render in the exact sRGB base color).

Usage:
  tiles11_test.py --demo <tiles_demo> --outdir <dir> [--frames 60 ...]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
sys.path.insert(0, HERE)
import cmpt_test  # noqa: E402  (reuses render_demo, is_orange, is_teal, count_exact)


def color_mask(img, kind):
    """Tolerant color masks. Box top faces render near (not exactly) the
    base color (e.g. blue tops read (4,4,194)), so use ranges."""
    r, g, b = img[:, :, 0], img[:, :, 1], img[:, :, 2]
    if kind == "red":
        return (r > 150) & (g < 80) & (b < 80)
    if kind == "green":
        return (r < 80) & (g > 150) & (b < 80)
    if kind == "blue":
        return (r < 80) & (g < 80) & (b > 140)
    if kind == "yellow":
        return (r > 150) & (g > 150) & (b < 80)
    if kind == "magenta":
        return (r > 150) & (g < 80) & (b > 140)
    raise ValueError(kind)


def check_color_present(img, kind, label, failures, minimum=50):
    n = int(color_mask(img, kind).sum())
    print("%s px: %d" % (label, n), flush=True)
    if n < minimum:
        failures.append("1.1 implicit: too few %s pixels: %d" % (label, n))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--frames", type=int, default=90)
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)
    failures = []

    # Scenario A: 1.1 bare-glb content.
    print("=== 1.1: bare glb content (3DTILES_content_gltf) ===", flush=True)
    out_a = os.path.join(args.outdir, "p10_11_glb.png")
    res, err = cmpt_test.render_demo(
        args.demo, os.path.join(DATA, "p10_11_glb", "tileset.json"),
        out_a, args.frames, args.width, args.height)
    if err is not None:
        failures.append("1.1 glb: %s" % err)
    else:
        rendered, img = res
        print("tiles rendered (last frame): %d" % rendered, flush=True)
        if rendered != 2:
            failures.append("1.1 glb: expected 2 tiles rendered, got %d"
                            % rendered)
        n_orange = int(cmpt_test.is_orange(img).sum())
        print("orange px: %d" % n_orange, flush=True)
        if n_orange < 1000:
            failures.append("1.1 glb: too few orange pixels: %d" % n_orange)
        n_teal = int(cmpt_test.is_teal(img).sum())
        print("teal px: %d" % n_teal, flush=True)
        if n_teal < 200:
            failures.append("1.1 glb: too few teal pixels: %d" % n_teal)

    # Scenario B: 1.1 implicit quadtree.
    print("=== 1.1: implicit quadtree (3DTILES_implicit_tiling) ===",
          flush=True)
    out_b = os.path.join(args.outdir, "p10_11_implicit.png")
    res, err = cmpt_test.render_demo(
        args.demo, os.path.join(DATA, "p10_11_implicit", "tileset.json"),
        out_b, args.frames, args.width, args.height)
    if err is not None:
        failures.append("1.1 implicit: %s" % err)
    else:
        rendered, img = res
        print("tiles rendered (last frame): %d" % rendered, flush=True)
        if rendered != 5:
            failures.append("1.1 implicit: expected 5 tiles rendered, "
                            "got %d" % rendered)
        check_color_present(img, "red", "red(L0)", failures)
        check_color_present(img, "green", "green(L1)", failures)
        check_color_present(img, "blue", "blue(L1)", failures)
        check_color_present(img, "yellow", "yellow(L1)", failures)
        check_color_present(img, "magenta", "magenta(L1)", failures)

    if failures:
        print("FAIL: tiles11_test", flush=True)
        for f in failures:
            print("  - %s" % f, flush=True)
        return 1
    print("PASS: tiles11_test", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
