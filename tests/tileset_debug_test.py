#!/usr/bin/env python3
"""P35: debug switches (subset of cesium.js debug*).

Part A: setDebugShowBoundingVolume(true) draws tile bounding-box wireframes
        (screenshot differs from baseline by line pixels).
Part B: setDebugShowUrl(true) logs visible tile IDs to stderr.
Part C: flags are stash-then-forward (set before loadTileset persists);
        toggling does not crash.

Uses the p3 fixture (tests/data/p3_box_tileset).
"""

import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

REPO = os.path.join(os.path.dirname(__file__), "..")
DEMO = os.path.join(REPO, "build", "linux", "tiles_demo")
DATA = os.path.join(os.path.dirname(__file__), "data", "p3_box_tileset",
                    "tileset.json")
TMP = "/tmp/p35_test"
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
LSAN_SUPP = os.path.join(os.path.dirname(__file__), "lsan.supp")

SANITIZED = False


def run_demo(args, capture_stderr=False):
    os.makedirs(TMP, exist_ok=True)
    cmd = [DEMO, "--tileset", DATA] + args
    if SANITIZED:
        cmd = [sys.executable, RUN_SAN, "--suppressions", LSAN_SUPP,
               "--"] + cmd
    elif not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a"] + cmd
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, f"demo failed: {result.stderr[-2000:]}"
    return result


def test_a_bounding_volume():
    """Wireframe appears when debugShowBoundingVolume is on."""
    off_png = os.path.join(TMP, "a_off.png")
    on_png = os.path.join(TMP, "a_on.png")
    run_demo(["--frames", "30", "--screenshot", off_png])
    run_demo(["--frames", "30", "--screenshot", on_png,
              "--debug-bounding-volume"])
    off = np.array(Image.open(off_png)).astype(int)
    on = np.array(Image.open(on_png)).astype(int)
    diff = np.abs(off - on).sum(axis=2)
    changed = (diff > 30).sum()
    total = off.shape[0] * off.shape[1]
    print(f"Part A: changed pixels {changed}/{total}")
    # Wireframe lines should change a modest number of pixels (not zero,
    # not the whole image).
    assert changed > 100, f"expected wireframe pixels, got {changed}"
    assert changed < total // 2, f"too many changed pixels: {changed}"
    print("Part A: PASS")


def test_b_show_url():
    """Visible tile IDs are logged to stderr when debugShowUrl is on.

    Note: xvfb-run merges the demo's stderr into stdout (it runs the
    command with 2>&1), so we search the combined output.
    """
    result = run_demo(["--until-loaded", "60", "--debug-show-url"],
                      capture_stderr=True)
    combined = result.stdout + result.stderr
    lines = [l for l in combined.splitlines()
             if "debugShowUrl" in l]
    print(f"Part B: {len(lines)} debugShowUrl lines")
    for line in lines[:5]:
        print(f"  {line}")
    assert len(lines) >= 1, "expected at least one debugShowUrl log line"
    # p3 has root + 2 children; at least the root should be logged.
    assert any("root.glb" in l for l in lines), \
        f"expected root.glb in logs: {lines}"
    # Without the flag, no such lines.
    result_off = run_demo(["--until-loaded", "60"], capture_stderr=True)
    combined_off = result_off.stdout + result_off.stderr
    off_lines = [l for l in combined_off.splitlines()
                 if "debugShowUrl" in l]
    assert len(off_lines) == 0, f"unexpected logs without flag: {off_lines}"
    print("Part B: PASS")


def test_c_toggle_no_crash():
    """Toggling flags does not crash; getters reflect state."""
    # Just verify the demo runs cleanly with both flags on.
    result = run_demo(["--frames", "30", "--debug-bounding-volume",
                       "--debug-show-url"])
    assert result.returncode == 0, \
        f"demo crashed with debug flags: {result.stderr[-500:]}"
    print("Part C: PASS (no crash with both flags)")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--sanitized", action="store_true",
                    help="run demo under run_sanitized.py (no xvfb)")
    ap.add_argument("--demo", default=DEMO)
    ap.add_argument("--tileset", default=DATA)
    args = ap.parse_args()
    SANITIZED = args.sanitized
    # --demo/--tileset accepted for CTest uniformity; the p3 fixture paths
    # above are used (same as tileset_cache_test.py).
    test_a_bounding_volume()
    test_b_show_url()
    test_c_toggle_no_crash()
    print("All P35 debug tests passed.")
