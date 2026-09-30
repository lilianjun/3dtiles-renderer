#!/usr/bin/env python3
"""P33: cesium.js-style show / modelMatrix / preloadWhenHidden + read-only
tileset state (tilesLoaded, boundingSphere, timeSinceLoadMs, rootTileId).

Part A (show=false): --hide on the p3 fixture. Asserts the screenshot is
the uniform clear color (26,51,115) — no tile content reaches the scene.

Part B (preloadWhenHidden): --hide --preload-hidden. Asserts tilesLoaded=1
(the traversal keeps loading in the background) while the screenshot stays
the uniform clear color.

Part C (modelMatrix): --model-matrix-tx 20. Asserts the screenshot differs
from the untransformed baseline, and boundingSphere reports center=(20,0,0)
(radius unchanged).

Part D (read-only state): --print-tileset-info on a settled load. Asserts
tilesLoaded=1, boundingSphere center=(0,0,0) radius=sqrt(75) (the p3 root
box is center (0,0,0) half-axes 5,5,5), timeSinceLoadMs > 0, and
rootTileId="root.glb" (cesium-native's empty-ID wrapper tile is unwrapped).

Part E (live modelMatrix): --model-matrix-tx-at-frame applies the same
tx=20 translation mid-stream. Asserts the screenshot is bit-identical to
the load-time application from Part C (live recomposition of already-
loaded tiles matches the load-time path exactly).

Usage:
  python3 tests/tileset_properties_test.py --demo <tiles_demo> \
      --tileset <p3 tileset.json>
"""
import argparse
import math
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
LSAN_SUPP = os.path.join(REPO, "tests", "lsan.supp")

SANITIZED = False
CLEAR = (26, 51, 115)  # SDK clear color (matches screenshot_test.py)


def run_demo(demo, tileset, extra_args):
    cmd = [demo, "--tileset", tileset] + extra_args
    if SANITIZED:
        cmd = [sys.executable, RUN_SAN, "--suppressions", LSAN_SUPP,
               "--"] + cmd
    elif not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        print(f"FAIL: demo exited {proc.returncode}")
        sys.exit(1)
    return proc.stdout


def parse_tileset_info(stdout):
    m = re.search(
        r"\[tileset-info\] tilesLoaded=(\d+) "
        r"boundingSphere=\(([-\d.e]+),([-\d.e]+),([-\d.e]+)\) "
        r"radius=([-\d.e]+) timeSinceLoadMs=(\d+) "
        r"rootTileId=\"([^\"]*)\" show=(\d+)", stdout)
    assert m, "missing [tileset-info] line"
    return {
        "tilesLoaded": int(m.group(1)),
        "center": (float(m.group(2)), float(m.group(3)), float(m.group(4))),
        "radius": float(m.group(5)),
        "timeMs": int(m.group(6)),
        "rootId": m.group(7),
        "show": int(m.group(8)),
    }


def load_pixels(path):
    from PIL import Image
    return list(Image.open(path).convert("RGB").getdata())


def part_a(demo, tileset, tmp):
    print("--- Part A: show=false renders clear color ---")
    out_png = os.path.join(tmp, "hide.png")
    run_demo(demo, tileset, ["--frames", "60", "--hide",
                             "--screenshot", out_png])
    px = load_pixels(out_png)
    assert len(set(px)) == 1, \
        f"hide screenshot not uniform: {len(set(px))} colors"
    assert px[0] == CLEAR, f"hide screenshot not clear color: {px[0]}"
    print("Part A OK")


def part_b(demo, tileset, tmp):
    print("--- Part B: preloadWhenHidden loads but does not render ---")
    out_png = os.path.join(tmp, "preload.png")
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--hide", "--preload-hidden",
                    "--print-tileset-info", "--screenshot", out_png])
    info = parse_tileset_info(out)
    assert info["tilesLoaded"] == 1, \
        f"tiles should load while hidden+preload, got {info}"
    assert info["show"] == 0
    px = load_pixels(out_png)
    assert len(set(px)) == 1 and px[0] == CLEAR, \
        "preload-hidden screenshot must stay clear color"
    print("Part B OK")


def part_c(demo, tileset, tmp):
    print("--- Part C: modelMatrix translation moves the render ---")
    base_png = os.path.join(tmp, "base.png")
    tx_png = os.path.join(tmp, "tx.png")
    run_demo(demo, tileset, ["--frames", "60", "--screenshot", base_png])
    out = run_demo(demo, tileset,
                   ["--frames", "60", "--model-matrix-tx", "20",
                    "--print-tileset-info", "--screenshot", tx_png])
    base = load_pixels(base_png)
    tx = load_pixels(tx_png)
    diff = sum(1 for a, b in zip(base, tx) if a != b)
    assert diff > 1000, \
        f"modelMatrix tx=20 should move pixels, diff={diff}"
    info = parse_tileset_info(out)
    cx, cy, cz = info["center"]
    assert abs(cx - 20.0) < 1e-6 and abs(cy) < 1e-6 and abs(cz) < 1e-6, \
        f"boundingSphere center should be (20,0,0), got {info['center']}"
    assert abs(info["radius"] - math.sqrt(75.0)) < 1e-3, \
        f"radius should be unchanged, got {info['radius']}"
    print(f"Part C OK (pixels moved: {diff})")


def part_d(demo, tileset):
    print("--- Part D: read-only tileset state ---")
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--print-tileset-info"])
    info = parse_tileset_info(out)
    assert info["tilesLoaded"] == 1, f"tilesLoaded: {info}"
    cx, cy, cz = info["center"]
    assert abs(cx) < 1e-9 and abs(cy) < 1e-9 and abs(cz) < 1e-9, \
        f"center: {info['center']}"
    assert abs(info["radius"] - math.sqrt(75.0)) < 1e-3, \
        f"radius: {info['radius']}"
    assert info["timeMs"] > 0, f"timeSinceLoadMs: {info['timeMs']}"
    assert info["rootId"] == "root.glb", f"rootTileId: {info['rootId']!r}"
    assert info["show"] == 1
    print("Part D OK")


def part_e(demo, tileset, tmp):
    print("--- Part E: live setModelMatrix re-applies to loaded tiles ---")
    # Apply tx=20 at frame 30 (after tiles are loaded); the result must be
    # bit-identical to applying it at load time (part C's tx.png).
    live_png = os.path.join(tmp, "live.png")
    load_png = os.path.join(tmp, "load.png")
    run_demo(demo, tileset,
             ["--frames", "60", "--model-matrix-tx", "20",
              "--screenshot", load_png])
    out = run_demo(demo, tileset,
                   ["--frames", "60", "--model-matrix-tx", "20",
                    "--model-matrix-tx-at-frame", "30",
                    "--print-tileset-info", "--screenshot", live_png])
    assert "[p33] live modelMatrix tx=20 at frame=30" in out, \
        "live matrix application marker missing"
    live = load_pixels(live_png)
    load = load_pixels(load_png)
    diff = sum(1 for a, b in zip(live, load) if a != b)
    assert diff == 0, \
        f"live re-apply must match load-time application, diff={diff}"
    info = parse_tileset_info(out)
    assert abs(info["center"][0] - 20.0) < 1e-6, \
        f"boundingSphere after live set: {info['center']}"
    print("Part E OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--tileset", required=True)
    ap.add_argument("--sanitized", action="store_true",
                    help="run each demo invocation under ASan+LSan+UBSan "
                         "(needs a TILES_SANITIZE=ON build)")
    args = ap.parse_args()
    global SANITIZED
    SANITIZED = args.sanitized
    with tempfile.TemporaryDirectory(prefix="p33_") as tmp:
        part_a(args.demo, args.tileset, tmp)
        part_b(args.demo, args.tileset, tmp)
        part_c(args.demo, args.tileset, tmp)
        part_d(args.demo, args.tileset)
        part_e(args.demo, args.tileset, tmp)
    print("PASS: tileset_properties")


if __name__ == "__main__":
    main()
