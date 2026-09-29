#!/usr/bin/env python3
"""P11: golden screenshot regression gate.

Re-renders one representative fixture per phase with *fixed* parameters
(fixed frame count, fixed 800x600 window, fixed orbit camera inside
tiles_demo) and compares the result pixel-for-pixel against the frozen
golden in tests/golden/.

Why this exists: P2-P10 proved each format renders correctly, but every
check was "render now, assert now". A golden baseline turns any future
render-pipeline regression into a loud, automatic failure.

Determinism: on this stack (Mesa software GL under xvfb, stb_image_write
PNG) re-rendering is bit-identical run to run — verified for all eight
entries when the goldens were frozen (see tests/golden/README.md). So the
local gate requires STRICT equality (0 differing pixels).

CI escape hatch: env GOLDEN_MAX_DIFF_FRAC (default 0) allows a tiny
fraction of differing pixels for environments where the GL stack is not
bit-identical (mesa version / driver differences). It must stay <= 0.001
(0.1%). ANY nonzero diff is printed explicitly, so tolerance can never
silently hide a regression.

Usage:
  golden_test.py --demo <tiles_demo> [--workdir <dir>]

The golden set is NOT run under sanitizer builds (see CMakeLists and
tests/golden/README.md): ASan-instrumented binaries are for memory
correctness, not pixel baselines.
"""
import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN_DIR = os.path.join(HERE, "golden")
DATA = os.path.join(HERE, "data")

from PIL import Image  # noqa: E402
import numpy as np  # noqa: E402

# name, tileset dir under tests/data (None => --no-tileset), frames
MANIFEST = [
    ("p2_demo", None, 30),
    ("p3_tileset", "p3_box_tileset", 60),
    ("p5_b3dm", "p5_b3dm_tileset", 60),
    ("p7_i3dm", "p7_i3dm_tileset", 60),
    ("p8_pnts", "p8_pnts_cloud", 60),
    ("p9_cmpt", "p9_cmpt_tileset", 60),
    ("p10_11_glb", "p10_11_glb", 90),
    ("p10_11_implicit", "p10_11_implicit", 90),
]

WIDTH, HEIGHT = 800, 600
MAX_ALLOWED_FRAC = 0.001  # hard ceiling for the escape hatch


def render_demo(demo, tileset_dir, out, frames):
    cmd = [demo, "--frames", str(frames),
           "--width", str(WIDTH), "--height", str(HEIGHT)]
    if tileset_dir is None:
        cmd.append("--no-tileset")
    else:
        cmd += ["--tileset",
                os.path.join(DATA, tileset_dir, "tileset.json")]
    cmd += ["--screenshot", out]
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if proc.returncode != 0:
        return "demo exited %d\n%s\n%s" % (
            proc.returncode, proc.stdout[-2000:], proc.stderr[-2000:])
    if not os.path.exists(out):
        return "demo exited 0 but wrote no screenshot"
    return None


def diff_fraction(a_path, b_path):
    a = np.asarray(Image.open(a_path).convert("RGB"))
    b = np.asarray(Image.open(b_path).convert("RGB"))
    if a.shape != b.shape:
        return None, "shape mismatch %s vs %s" % (a.shape, b.shape)
    diff = int((a != b).any(axis=2).sum())
    return diff / (a.shape[0] * a.shape[1]), "%d differing pixels" % diff


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()

    tol = float(os.environ.get("GOLDEN_MAX_DIFF_FRAC", "0"))
    if not (0 <= tol <= MAX_ALLOWED_FRAC):
        print("GOLDEN_MAX_DIFF_FRAC=%s out of range [0, %s]" % (tol, MAX_ALLOWED_FRAC))
        return 2
    if tol > 0:
        print("NOTE: GOLDEN_MAX_DIFF_FRAC=%s active (escape hatch, see README)"
              % tol, flush=True)

    failures = []
    if args.workdir:
        os.makedirs(args.workdir, exist_ok=True)
    with tempfile.TemporaryDirectory(
            dir=args.workdir, prefix="golden_") as work:
        for name, tileset_dir, frames in MANIFEST:
            golden = os.path.join(GOLDEN_DIR, name + ".png")
            if not os.path.exists(golden):
                failures.append("%s: golden file missing: %s" % (name, golden))
                continue
            out = os.path.join(work, name + ".png")
            err = render_demo(args.demo, tileset_dir, out, frames)
            if err is not None:
                failures.append("%s: render failed: %s" % (name, err))
                continue
            frac, detail = diff_fraction(out, golden)
            if frac is None:
                failures.append("%s: %s" % (name, detail))
                continue
            # Always report nonzero diffs explicitly, even when tolerated.
            status = "IDENTICAL" if frac == 0 else detail
            print("[golden] %-16s %s" % (name, status), flush=True)
            if frac > tol:
                failures.append(
                    "%s: %s (%.4f%% of pixels) exceeds allowed %.4f%%"
                    % (name, detail, frac * 100, tol * 100))

    if failures:
        print("\nGOLDEN FAILURES:", flush=True)
        for f in failures:
            print("  -", f, flush=True)
        return 1
    print("\nAll %d goldens match." % len(MANIFEST), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
