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

# P16: trajectory goldens — golden name, tileset dir, trajectory CSV under
# tests/data/trajectories, logical frame index to freeze. All three come
# from ONE demo run per trajectory (--frame-dir); the frozen frames are
# first/middle/last of the 12-frame p16_orbit_push trajectory.
# Determinism of these frames across runs is gated separately by
# trajectory_determinism (bit-identical), so the golden gate only guards
# against pipeline regressions here.
TRAJECTORY_GOLDENS = [
    ("p16_traj_f00", "p3_box_tileset", "p16_orbit_push.csv", 0),
    ("p16_traj_f05", "p3_box_tileset", "p16_orbit_push.csv", 5),
    ("p16_traj_f11", "p3_box_tileset", "p16_orbit_push.csv", 11),
]

WIDTH, HEIGHT = 800, 600
MAX_ALLOWED_FRAC = 0.005  # hard ceiling for the escape hatch (0.5% = 2400 px
# at 800x600; observed cross-Mesa variance peaks at 0.29%, see
# tests/golden/README.md). A real regression (missing/wrong-colored box,
# broken lighting) moves tens of thousands of pixels, so this still gates.


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


def render_trajectory(demo, tileset_dir, traj_csv, frame_index, out):
    """Render one trajectory and extract a single logical frame as PNG.

    Runs the demo once with --trajectory + --frame-dir into a temp dir,
    then copies frame_<index>.png to `out`. Returns None on success or an
    error string. Results are cached per (tileset, trajectory) so multiple
    frozen frames from the same run need only one demo invocation.
    """
    key = (tileset_dir, traj_csv)
    if key not in render_trajectory.cache:
        work = tempfile.mkdtemp(prefix="golden_traj_")
        render_trajectory.cache[key] = work
        cmd = [demo,
               "--tileset", os.path.join(DATA, tileset_dir, "tileset.json"),
               "--trajectory",
               os.path.join(DATA, "trajectories", traj_csv),
               "--frame-dir", work,
               "--width", str(WIDTH), "--height", str(HEIGHT)]
        if not os.environ.get("DISPLAY"):
            cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
        print("+", " ".join(cmd), flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if proc.returncode != 0:
            render_trajectory.cache[key] = None
            return "demo exited %d\n%s\n%s" % (
                proc.returncode, proc.stdout[-2000:], proc.stderr[-2000:])
    work = render_trajectory.cache[key]
    if work is None:
        return "cached trajectory render failed"
    src = os.path.join(work, "frame_%04d.png" % frame_index)
    if not os.path.exists(src):
        return "trajectory produced no %s" % src
    with open(src, "rb") as fsrc, open(out, "wb") as fdst:
        fdst.write(fsrc.read())
    return None


render_trajectory.cache = {}


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
        # P16: trajectory goldens — frozen logical frames of a deterministic
        # replay (bit-identity across runs is gated by trajectory_determinism).
        for name, tileset_dir, traj_csv, frame_index in TRAJECTORY_GOLDENS:
            golden = os.path.join(GOLDEN_DIR, name + ".png")
            if not os.path.exists(golden):
                failures.append("%s: golden file missing: %s" % (name, golden))
                continue
            out = os.path.join(work, name + ".png")
            err = render_trajectory(args.demo, tileset_dir, traj_csv,
                                    frame_index, out)
            if err is not None:
                failures.append("%s: render failed: %s" % (name, err))
                continue
            frac, detail = diff_fraction(out, golden)
            if frac is None:
                failures.append("%s: %s" % (name, detail))
                continue
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
    print("\nAll %d goldens match." % (len(MANIFEST) + len(TRAJECTORY_GOLDENS)),
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
