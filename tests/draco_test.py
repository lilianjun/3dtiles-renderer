#!/usr/bin/env python3
"""P29: Draco (KHR_draco_mesh_compression) mesh compression support.

Proves, with a real deterministic fixture (not a stub):
  1. A Draco-compressed tile renders: tile loads, screenshot is taken, the
     box is visible (decode produced real geometry, not a black tile).
  2. The Draco render matches its uncompressed twin (same geometry, same
     material) pixel-for-pixel: at -qp 14 the quantization is far below
     rasterization visibility for this 10 m box (verified: 0/480000 pixels
     differ), so the tolerance only guards against rasterizer variation.
     A missing/broken decode would diff by thousands of pixels.
  3. A corrupt Draco bitstream fails gracefully: the demo logs
     "Draco decoding failed", exits 0 — no crash, no hang. (The tile still
     counts as rendered but carries no geometry; this is upstream
     cesium-native warnings-only behavior, which we do not fork.)

Fixture: tests/data/gen_p29_draco_tileset.py (deterministic given the same
draco_encoder binary; draco comes from cesium-native's own vcpkg manifest,
so no new dependency was added for any platform).
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
LSAN_SUPP = os.path.join(REPO, "tests", "lsan.supp")
DRACO_TILESET = os.path.join(REPO, "tests", "data", "p29_draco_tileset",
                             "tileset.json")
PLAIN_TILESET = os.path.join(REPO, "tests", "data", "p29_draco_tileset",
                             "tileset_plain.json")

WIDTH, HEIGHT = 800, 600
# P30: settle budget (max frames until the tileset converges). The old
# fixed 60-frame render raced async loading the same way the golden
# p7_i3dm flake did; the settled frame is pixel-identical for the twin
# comparison (static camera, deterministic pipeline).
SETTLE_BUDGET = 240

# Tight: -qp 14 quantization is invisible here; this only absorbs
# rasterizer wobble. Measured on 2026-09-30: 0 pixels differ.
MAX_DIFF = 16
MEAN_DIFF = 2.0


def run_demo(demo, tileset, screenshot, sanitized=False):
    cmd = [demo, "--until-loaded", str(SETTLE_BUDGET), "--width", str(WIDTH),
           "--height", str(HEIGHT), "--tileset", tileset,
           "--screenshot", screenshot]
    if sanitized:
        cmd = [sys.executable, RUN_SAN, "--suppressions", LSAN_SUPP,
               "--"] + cmd
    proc = subprocess.run(["xvfb-run", "-a"] + cmd, capture_output=True,
                          text=True, timeout=600)
    return proc


def tiles_rendered(out):
    m = re.search(r"tiles rendered \(last frame\): (\d+)", out)
    return int(m.group(1)) if m else -1


def corrupt_draco_glb(src_glb, dst_glb):
    """Copy src_glb to dst_glb with the embedded Draco bitstream scrambled."""
    with open(src_glb, "rb") as f:
        data = bytearray(f.read())
    clen, _ = struct.unpack("<II", data[12:20])
    gj = json.loads(data[20:20 + clen])
    bin_off = 20 + clen
    bv = gj["bufferViews"][0]  # the draco bufferView
    doff = bin_off + 8 + bv["byteOffset"]
    assert data[doff:doff + 4] == b"DRAC", \
        "expected Draco magic ('DRAC') at draco bufferView"
    for i in range(20, min(60, bv["byteLength"])):
        data[doff + i] ^= 0xFF
    with open(dst_glb, "wb") as f:
        f.write(data)


def is_gray_box(r, g, b):
    # light gray 0.75/0.75/0.78 lit by the P3 sun + P26 IBL -> ~(191,191,199)
    return abs(r - 191) < 60 and abs(g - 191) < 60 and abs(b - 199) < 60


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--out", required=True,
                    help="directory for screenshots")
    ap.add_argument("--sanitized", action="store_true",
                    help="route the demo through tests/run_sanitized.py "
                         "(ASan/LSan/UBSan); sanitizer findings fail the test")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    draco_png = os.path.join(args.out, "p29_draco.png")
    plain_png = os.path.join(args.out, "p29_plain.png")

    # 1. Render the Draco tile: must load and show the box.
    p = run_demo(args.demo, DRACO_TILESET, draco_png,
                 sanitized=args.sanitized)
    assert p.returncode == 0, \
        f"draco demo exit={p.returncode}\n{p.stderr[-2000:]}"
    assert tiles_rendered(p.stdout) >= 1, \
        f"draco tile did not render:\n{p.stdout[-1500:]}"
    assert os.path.exists(draco_png), "draco screenshot missing"

    from PIL import Image
    im = Image.open(draco_png).convert("RGB")
    px = im.load()
    n_gray = sum(1 for y in range(HEIGHT) for x in range(WIDTH)
                 if is_gray_box(*px[x, y]))
    print(f"[draco] gray-box pixels in draco render: {n_gray}")
    assert n_gray > 1000, \
        f"draco tile rendered but box not visible ({n_gray} gray pixels)"

    # 2. The uncompressed twin must match pixel-for-pixel (tight tolerance).
    p = run_demo(args.demo, PLAIN_TILESET, plain_png,
                 sanitized=args.sanitized)
    assert p.returncode == 0, \
        f"plain demo exit={p.returncode}\n{p.stderr[-2000:]}"
    assert tiles_rendered(p.stdout) >= 1, "plain tile did not render"
    assert os.path.exists(plain_png), "plain screenshot missing"

    a = Image.open(draco_png).convert("RGB")
    b = Image.open(plain_png).convert("RGB")
    assert a.size == b.size, f"size mismatch {a.size} vs {b.size}"
    try:
        import numpy as np
        na = np.array(a, dtype=np.int16)
        nb = np.array(b, dtype=np.int16)
        d = np.abs(na - nb)
        maxd, meand = float(d.max()), float(d.mean())
    except ImportError:
        pa, pb = a.load(), b.load()
        maxd, total, n = 0, 0, WIDTH * HEIGHT
        for y in range(HEIGHT):
            for x in range(WIDTH):
                ra, ga, ba = pa[x, y]
                rb, gb, bb = pb[x, y]
                md = max(abs(ra - rb), abs(ga - gb), abs(ba - bb))
                maxd = max(maxd, md)
                total += abs(ra - rb) + abs(ga - gb) + abs(ba - bb)
        meand = total / (3 * n)
    print(f"[draco] max abs diff vs plain: {maxd}, mean: {meand:.4f}")
    assert maxd <= MAX_DIFF, \
        f"draco vs plain max diff {maxd} > {MAX_DIFF}"
    assert meand < MEAN_DIFF, \
        f"draco vs plain mean diff {meand:.4f} >= {MEAN_DIFF}"

    # 3. Corrupt Draco bitstream must fail gracefully (exit 0, no crash).
    tmp = tempfile.mkdtemp(prefix="p29_corrupt_")
    try:
        src_dir = os.path.dirname(DRACO_TILESET)
        for name in os.listdir(src_dir):
            if not name.startswith("."):
                shutil.copy(os.path.join(src_dir, name), tmp)
        corrupt_draco_glb(os.path.join(tmp, "root.glb"),
                          os.path.join(tmp, "root.glb"))
        bad_png = os.path.join(args.out, "p29_corrupt.png")
        p = run_demo(args.demo, os.path.join(tmp, "tileset.json"), bad_png,
                     sanitized=args.sanitized)
        assert p.returncode == 0, \
            f"corrupt draco demo crashed: exit={p.returncode}\n" \
            f"{p.stderr[-2000:]}"
        assert "Draco decoding failed" in p.stdout + p.stderr, \
            "expected a Draco decode warning in the log"
        im = Image.open(bad_png).convert("RGB")
        px = im.load()
        n_gray = sum(1 for y in range(HEIGHT) for x in range(WIDTH)
                     if is_gray_box(*px[x, y]))
        assert n_gray == 0, \
            f"corrupt draco unexpectedly rendered geometry ({n_gray} px)"
        print("[draco] corrupt input failed gracefully "
              "(exit 0, decode warning, no geometry)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("[draco] PASS")


if __name__ == "__main__":
    main()
