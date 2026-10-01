#!/usr/bin/env python3
"""P36-B: Inspector separate-window smoke test.

P36-B pivot: the Inspector lives in its own SDL3 window (not a Filament
overlay). This test verifies:
  A. --inspector-smoke N exits 0 and writes a PNG screenshot (3D window).
  B. The Inspector window was created and ImGui rendered without crashing
     (verified via stdout banner + clean exit; the UI is in a separate
     OS window, not composited into the 3D screenshot).
  C. Multi-frame run (N=60) does not crash.

Usage: tileset_inspector_ui_test.py --demo <demo> --tileset <tileset.json>
"""
import argparse
import os
import subprocess
import sys
import tempfile


def run_demo(demo, tileset_path, out_png, inspector_frames):
    cmd = [
        demo,
        "--tileset",
        tileset_path,
        "--screenshot",
        out_png,
    ]
    if inspector_frames > 0:
        cmd += ["--inspector", "--inspector-smoke", str(inspector_frames)]
    else:
        cmd += ["--frames", "10"]
    # Wrap in xvfb-run when headless (like other demo tests).
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a"] + cmd
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--tileset", required=True)
    ns = ap.parse_args()
    demo = ns.demo
    tileset_path = ns.tileset

    with tempfile.TemporaryDirectory() as out_dir:
        # Part A+B: inspector smoke exits 0, writes PNG, shows banner.
        ui_png = os.path.join(out_dir, "p36b_ui.png")
        r_ui = run_demo(demo, tileset_path, ui_png, 10)
        assert r_ui.returncode == 0, f"inspector-smoke failed:\n{r_ui.stderr[-2000:]}"
        assert os.path.exists(ui_png), "screenshot not written"
        assert os.path.getsize(ui_png) > 1000, "screenshot too small"
        stdout = r_ui.stdout
        assert "Inspector mode" in stdout, "no Inspector banner in stdout"
        assert "separate window" in stdout, "not using separate-window mode"
        assert "[demo] OK" in stdout, "no OK marker"
        print("[test] Part A+B PASS: inspector smoke exits 0, window created")

        # Part C: longer run must not crash.
        long_png = os.path.join(out_dir, "p36b_long.png")
        r_long = run_demo(demo, tileset_path, long_png, 60)
        assert r_long.returncode == 0, f"60-frame run failed:\n{r_long.stderr[-2000:]}"
        assert os.path.exists(long_png), "long-run screenshot not written"
        print("[test] Part C PASS: 60-frame inspector run clean")

        print("[test] ALL P36-B UI PARTS PASS")


if __name__ == "__main__":
    main()
