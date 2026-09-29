#!/usr/bin/env python3
"""P26: image-based lighting (IBL) verification.

Renders the deterministic p15_metal fixture twice — once with the default
procedural IBL (on) and once with --no-ibl (off, the pre-P26 scene) — and
asserts, without hard-coding Mesa pixel values:

  1. The metal box is dramatically brighter with IBL on than off: the
     environment reflections replace the black void (this is P26's goal).
  2. The IBL-on metal shows environment character: its mean color shifts
     toward the procedural sky (blue-dominant) instead of staying
     near-black.
  3. The dielectric box stays sane with IBL on: brighter than without,
     not blown out, top face still clearly lit.
  4. Both renders show both boxes with identical pixel counts (IBL changes
     lighting, not geometry).

This test also serves as the ASan/LSan/UBSan gate for the IBL path:
--sanitized routes the IBL-on render through tests/run_sanitized.py, so a
leak or heap error in the cubemap/SH/IndirectLight/mipmap code fails the
sanitizer lane.

Usage:
  ibl_test.py --demo <tiles_demo> --outdir <dir> [--sanitized]
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
LSAN_SUPP = os.path.join(REPO, "tests", "lsan.supp")
METAL_TILESET = os.path.join(REPO, "tests", "data", "p15_metal", "tileset.json")

WIDTH, HEIGHT = 800, 600
FRAMES = 60
BG = (26, 51, 115)  # demo clear color
LX, RX = 340, 460  # left/right region split (dead zone in the middle)


def run_demo(demo, tileset, screenshot, no_ibl=False, sanitized=False):
    cmd = [demo, "--frames", str(FRAMES), "--width", str(WIDTH),
           "--height", str(HEIGHT), "--tileset", tileset,
           "--screenshot", screenshot]
    if no_ibl:
        cmd.append("--no-ibl")
    if sanitized:
        cmd = [sys.executable, RUN_SAN, "--suppressions", LSAN_SUPP,
               "--"] + cmd
    proc = subprocess.run(["xvfb-run", "-a"] + cmd, capture_output=True,
                          text=True, timeout=600)
    return proc


def tiles_rendered(out):
    m = re.search(r"tiles rendered \(last frame\): (\d+)", out)
    return int(m.group(1)) if m else -1


def region_stats(path, x0, x1):
    from PIL import Image
    import numpy as np
    img = np.asarray(Image.open(path).convert("RGB")).astype(np.int32)
    bg = np.array(BG)
    mask = np.sqrt(((img - bg) ** 2).sum(axis=2)) > 30
    px = img[:, x0:x1][mask[:, x0:x1]]
    lum = px.mean(axis=1)
    return {"n": len(px), "mean": float(lum.mean()),
            "max": float(lum.max()), "color": px.mean(axis=0)}


def color_dist(a, b):
    return float(((a - b) ** 2).sum() ** 0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--sanitized", action="store_true",
                    help="route the IBL-on render through "
                         "tests/run_sanitized.py (ASan/LSan/UBSan)")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    failures = []

    on_png = os.path.join(args.outdir, "p26_metal_ibl_on.png")
    off_png = os.path.join(args.outdir, "p26_metal_ibl_off.png")

    p = run_demo(args.demo, METAL_TILESET, on_png, no_ibl=False,
                 sanitized=args.sanitized)
    if p.returncode != 0:
        failures.append("ibl-on render exited %d\n%s" %
                        (p.returncode, p.stderr[-2000:]))
    elif tiles_rendered(p.stdout) != 2:
        failures.append("ibl-on: expected 2 tiles, got %d" %
                        tiles_rendered(p.stdout))
    if "IBL enabled" not in p.stdout:
        failures.append("ibl-on: IBL was not enabled in the demo log")

    p = run_demo(args.demo, METAL_TILESET, off_png, no_ibl=True,
                 sanitized=False)
    if p.returncode != 0:
        failures.append("ibl-off render exited %d\n%s" %
                        (p.returncode, p.stderr[-2000:]))
    elif tiles_rendered(p.stdout) != 2:
        failures.append("ibl-off: expected 2 tiles, got %d" %
                        tiles_rendered(p.stdout))
    if "IBL disabled" not in p.stdout:
        failures.append("ibl-off: IBL was not disabled in the demo log")

    if failures:
        print("FAILURES:")
        for f in failures:
            print(" -", f)
        sys.exit(1)

    diel_on = region_stats(on_png, 0, LX)
    metal_on = region_stats(on_png, RX, 800)
    diel_off = region_stats(off_png, 0, LX)
    metal_off = region_stats(off_png, RX, 800)
    print("dielectric: off mean=%.1f max=%.0f | on mean=%.1f max=%.0f"
          % (diel_off["mean"], diel_off["max"],
             diel_on["mean"], diel_on["max"]), flush=True)
    print("metal:      off mean=%.1f max=%.0f | on mean=%.1f max=%.0f "
          "on_color=%s"
          % (metal_off["mean"], metal_off["max"],
             metal_on["mean"], metal_on["max"],
             metal_on["color"].round(1)), flush=True)

    # Geometry must be identical; only lighting changes.
    for name, a, b in (("dielectric", diel_on, diel_off),
                       ("metal", metal_on, metal_off)):
        if a["n"] < 5000:
            failures.append("ibl: %s box missing (n=%d)" % (name, a["n"]))
        if a["n"] != b["n"]:
            failures.append("ibl: %s pixel count changed with IBL "
                            "(%d vs %d)" % (name, a["n"], b["n"]))

    # 1. Metal dramatically brighter with IBL (P26's goal). The pre-P26
    # baseline is ~5.6 mean; IBL brings it to ~55. A 3x bar is far below the
    # measured ~10x, so this guards the feature, not Mesa's exact numbers.
    if not metal_on["mean"] > 3.0 * metal_off["mean"]:
        failures.append("ibl: metal not brighter with IBL on "
                        "(on=%.1f, off=%.1f)"
                        % (metal_on["mean"], metal_off["mean"]))
    # The no-IBL render must reproduce the pre-P26 dark metal.
    if not metal_off["mean"] < 15.0:
        failures.append("ibl: no-IBL metal not dark (mean=%.1f)" %
                        metal_off["mean"])

    # 2. Environment character: the IBL-on metal reflects the procedural
    # sky, so its mean color must shift strongly and toward blue.
    if color_dist(metal_on["color"], metal_off["color"]) < 30.0:
        failures.append("ibl: metal color did not shift with IBL (dist=%.1f)"
                        % color_dist(metal_on["color"], metal_off["color"]))
    if not metal_on["color"][2] > metal_on["color"][0] + 10.0:
        failures.append("ibl: metal does not show sky reflection "
                        "(color=%s)" % metal_on["color"].round(1))

    # 3. Dielectric stays sane: brighter, not blown out, top face lit.
    if not diel_on["mean"] > diel_off["mean"]:
        failures.append("ibl: dielectric not brighter with IBL on "
                        "(on=%.1f, off=%.1f)"
                        % (diel_on["mean"], diel_off["mean"]))
    if not diel_on["max"] < 220.0:
        failures.append("ibl: dielectric blown out (max=%.0f)" %
                        diel_on["max"])
    if not diel_on["max"] > 80.0:
        failures.append("ibl: dielectric top face not lit (max=%.0f)" %
                        diel_on["max"])
    # Metal must not blow out either.
    if not metal_on["max"] < 220.0:
        failures.append("ibl: metal blown out (max=%.0f)" % metal_on["max"])

    if failures:
        print("FAILURES:")
        for f in failures:
            print(" -", f)
        sys.exit(1)
    print("ibl: OK (metal %.1f -> %.1f with IBL)"
          % (metal_off["mean"], metal_on["mean"]))


if __name__ == "__main__":
    main()
