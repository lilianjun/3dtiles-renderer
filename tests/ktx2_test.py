#!/usr/bin/env python3
"""P25: KTX2 (KHR_texture_basisu / image/ktx2) texture support.

Proves, with a real deterministic UASTC fixture (not a stub):
  1. A KTX2-textured box renders: tile loads, screenshot is taken.
  2. The KTX2 render matches its PNG twin (same geometry, same checker
     pixels) within a tight tolerance. The embedded UASTC payload decodes
     losslessly for this flat-color checkerboard (verified: 0/4096 texture
     texels differ), so the tolerance only guards against rasterizer
     variation, not codec loss. A missing/black texture would diff by
     thousands of pixels at high amplitude.
  3. A corrupt KTX2 (zeroed magic) fails gracefully: the demo logs the
     load error and exits 0 — no crash, no hang.

Fixture: tests/data/gen_p25_ktx2.py (deterministic; re-run is byte-identical).
"""
import argparse
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
KTX2_TILESET = os.path.join(REPO, "tests", "data", "p25_ktx2_tileset",
                            "tileset.json")
PNG_TILESET = os.path.join(REPO, "tests", "data", "p25_png_tileset",
                           "tileset.json")

WIDTH, HEIGHT = 800, 600
FRAMES = 90

# Tight: codec is lossless here; this only absorbs rasterizer wobble.
MAX_DIFF = 16
MEAN_DIFF = 2.0


def run_demo(demo, tileset, screenshot, sanitized=False):
    cmd = [demo, "--frames", str(FRAMES), "--width", str(WIDTH),
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


def corrupt_ktx2_glb(src_glb, dst_glb):
    """Copy src_glb to dst_glb with the embedded KTX2 magic zeroed."""
    with open(src_glb, "rb") as f:
        data = bytearray(f.read())
    import json
    clen, _ = struct.unpack("<II", data[12:20])
    gj = json.loads(data[20:20 + clen])
    bin_off = 20 + clen
    bv = gj["bufferViews"][gj["images"][0]["bufferView"]]
    koff = bin_off + 8 + bv["byteOffset"]
    assert data[koff:koff + 12].startswith(b"\xabKTX 20"), \
        "expected KTX2 magic at image bufferView"
    for i in range(64):
        data[koff + i] = 0
    with open(dst_glb, "wb") as f:
        f.write(data)


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
    ktx2_png = os.path.join(args.out, "p25_ktx2.png")
    png_png = os.path.join(args.out, "p25_png.png")

    # 1-2. Render KTX2 and PNG twins.
    p = run_demo(args.demo, KTX2_TILESET, ktx2_png, sanitized=args.sanitized)
    assert p.returncode == 0, f"ktx2 demo exit={p.returncode}\n{p.stderr[-2000:]}"
    assert tiles_rendered(p.stdout) >= 1, \
        f"ktx2 tile did not render:\n{p.stdout[-1500:]}"
    assert os.path.exists(ktx2_png), "ktx2 screenshot missing"

    p = run_demo(args.demo, PNG_TILESET, png_png, sanitized=args.sanitized)
    assert p.returncode == 0, f"png demo exit={p.returncode}\n{p.stderr[-2000:]}"
    assert tiles_rendered(p.stdout) >= 1, "png tile did not render"
    assert os.path.exists(png_png), "png screenshot missing"

    # Pixel comparison (Pillow; numpy optional).
    from PIL import Image
    a = Image.open(ktx2_png).convert("RGB")
    b = Image.open(png_png).convert("RGB")
    assert a.size == b.size, f"size mismatch {a.size} vs {b.size}"
    try:
        import numpy as np
        na = np.array(a, dtype=np.int16)
        nb = np.array(b, dtype=np.int16)
        d = np.abs(na - nb)
        maxd, meand = float(d.max()), float(d.mean())
    except ImportError:
        pa, pb = a.load(), b.load()
        maxd, total, n = 0, 0, a.size[0] * a.size[1]
        for y in range(a.size[1]):
            for x in range(a.size[0]):
                ra, ga, ba = pa[x, y]
                rb, gb, bb = pb[x, y]
                md = max(abs(ra - rb), abs(ga - gb), abs(ba - bb))
                maxd = max(maxd, md)
                total += abs(ra - rb) + abs(ga - gb) + abs(ba - bb)
        meand = total / (3 * n)
    print(f"[ktx2] max abs diff vs png: {maxd}, mean: {meand:.4f}")
    assert maxd <= MAX_DIFF, \
        f"ktx2 vs png max diff {maxd} > {MAX_DIFF}"
    assert meand < MEAN_DIFF, \
        f"ktx2 vs png mean diff {meand:.4f} >= {MEAN_DIFF}"

    # 3. Corrupt KTX2 must fail gracefully (exit 0, no crash/hang).
    tmp = tempfile.mkdtemp(prefix="p25_corrupt_")
    try:
        src_dir = os.path.dirname(KTX2_TILESET)
        for name in os.listdir(src_dir):
            shutil.copy(os.path.join(src_dir, name), tmp)
        corrupt_ktx2_glb(os.path.join(tmp, "check_ktx2.glb"),
                         os.path.join(tmp, "check_ktx2.glb"))
        bad_png = os.path.join(args.out, "p25_corrupt.png")
        p = run_demo(args.demo, os.path.join(tmp, "tileset.json"), bad_png,
                     sanitized=args.sanitized)
        assert p.returncode == 0, \
            f"corrupt ktx2 demo crashed: exit={p.returncode}\n{p.stderr[-2000:]}"
        assert tiles_rendered(p.stdout) == 0, \
            "corrupt ktx2 tile should not render"
        print("[ktx2] corrupt input failed gracefully (exit 0, 0 tiles)")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("[ktx2] PASS")


if __name__ == "__main__":
    main()
