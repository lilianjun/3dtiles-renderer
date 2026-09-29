#!/usr/bin/env python3
"""P21: ADD refinement semantics + region bounding-volume coverage.

Part A (ADD): tests/data/p21_add_tileset/ — same geometry as P20's REPLACE
fixture (root ge=20 + 4 children ge=0) but refine=ADD, driven by the same
p20_lod.csv trajectory (FAR 1200m -> NEAR 300m -> FAR 1200m). Per-frame
[selected] sets prove the ADD/REPLACE contrast:
  - FAR:  selected == {root.glb}
  - NEAR: selected == {root.glb, child_0..3.glb} (root STAYS: ADD, not
          REPLACE — P20's REPLACE fixture selects only the 4 children here)
  - FAR2: selected == {root.glb} again (falls back, not accumulation),
          loaded stays 6 (cached)
The explicit-tileset wrapper tile (empty-string ID, no content) is filtered
out of the sets; see ADR-0018 for why it appears in ADD selections.

Part B (region): tests/data/p21_region_tileset/ — 3D Tiles 1.0 `region`
bounding volumes (radians, WGS84) at lat=0/lon=0, root transform carries the
local boxes to ECEF magnitude, the SDK's existing rebase path
(computeLocalOrigin: BoundingRegion -> OBB center) brings them back near
the origin. p21_region.csv (FAR -> NEAR). Proves:
  - all tiles load (tilesLoaded == 4: wrapper + root + 2 children),
    failed == 0,
  - the render is non-empty: screenshot differs from the --no-tileset
    reference (same trajectory) by >1000 pixels (dev-time: 5382, 4.5%;
    3 runs bit-identical).

Reuses parse_output/run_demo from frustum_lod_test.py (imported, not
copied). Pass criteria: all assertions below; failed == 0 in both parts.

Not asserted: exact pixel output, byte counts, RSS. Tile-ID strings are
cesium-native's (non-contractual); used as opaque set members only.
"""
import argparse
import os
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tests"))
from frustum_lod_test import parse_output, run_demo  # noqa: E402

DEMO = os.path.join(REPO, "build", "linux", "tiles_demo")
ADD_TILESET = os.path.join(REPO, "tests", "data", "p21_add_tileset",
                           "tileset.json")
ADD_TRAJ = os.path.join(REPO, "tests", "data", "trajectories", "p20_lod.csv")
REGION_TILESET = os.path.join(REPO, "tests", "data", "p21_region_tileset",
                              "tileset.json")
REGION_TRAJ = os.path.join(REPO, "tests", "data", "trajectories",
                           "p21_region.csv")

WIDTH, HEIGHT = 400, 300
WARMUP = 60
# Region render must differ from the empty reference by this many pixels.
REGION_MIN_DIFF_PX = 1000


def no_wrapper(ids):
    # Drop the explicit-tileset wrapper tile (empty ID, no content).
    return {x for x in ids if x}


def check_add(stats, selected, failures):
    # Phases: FAR 0-59, NEAR 60-119, FAR2 120-179. Steady-state tails.
    def window(lo, hi):
        return [f for f in stats if lo <= f <= hi]

    far, near, far2 = window(40, 59), window(100, 119), window(160, 179)
    if min(len(far), len(near), len(far2)) < 10:
        failures.append("add: too few frames parsed in a phase")
        return

    def sel_set(fs):
        s = set()
        for f in fs:
            s |= no_wrapper(selected.get(f, set()))
        return s

    far_sel, near_sel, far2_sel = sel_set(far), sel_set(near), sel_set(far2)
    print(f"add: FAR  selected={sorted(far_sel)} "
          f"loaded={stats[far[-1]][2]}")
    print(f"add: NEAR selected={sorted(near_sel)} "
          f"loaded={stats[near[-1]][2]}")
    print(f"add: FAR2 selected={sorted(far2_sel)} "
          f"loaded={stats[far2[-1]][2]}")

    if any(v[3] != 0 for v in stats.values()):
        failures.append("add: failed != 0")

    if far_sel != {"root.glb"}:
        failures.append(f"add: FAR selected={sorted(far_sel)} != {{root.glb}}")
    # ADD core: at NEAR the root STAYS selected alongside the children.
    # (P20's REPLACE fixture selects only the 4 children here — the
    # ADD/REPLACE contrast is the regression gate.)
    want_near = {"root.glb", "child_0.glb", "child_1.glb",
                 "child_2.glb", "child_3.glb"}
    if near_sel != want_near:
        failures.append(
            f"add: NEAR selected={sorted(near_sel)} != root+4 children")
    if "root.glb" not in near_sel:
        failures.append("add: root.glb NOT selected at NEAR (REPLACE, not "
                        "ADD)")
    if not (max(stats[f][2] for f in far) < min(stats[f][2] for f in near)):
        failures.append("add: loaded not monotonic FAR->NEAR")
    if far2_sel != {"root.glb"}:
        failures.append(
            f"add: FAR2 selected={sorted(far2_sel)} != {{root.glb}}")
    if stats[far2[-1]][2] != stats[near[-1]][2]:
        failures.append("add: FAR2 loaded changed (unexpected reload/evict)")


def screenshot_diff_px(path_a, path_b):
    from PIL import Image, ImageChops
    import numpy as np
    a = Image.open(path_a).convert("RGB")
    b = Image.open(path_b).convert("RGB")
    if a.size != b.size:
        raise ValueError(f"screenshot size mismatch {a.size} vs {b.size}")
    n = np.asarray(ImageChops.difference(a, b)).sum(axis=2)
    return int((n > 0).sum())


def check_region(demo, sanitized, suppressions, failures):
    frames = 90 if sanitized else 130
    with tempfile.TemporaryDirectory() as tmp:
        shot = os.path.join(tmp, "region.png")
        proc = run_demo(demo, REGION_TILESET, REGION_TRAJ, frames,
                        extra=("--screenshot", shot),
                        sanitized=sanitized, suppressions=suppressions)
        if proc.returncode != 0:
            failures.append(f"region: demo exited {proc.returncode}")
            print(proc.stdout[-2000:])
            print(proc.stderr[-2000:])
            return
        stats, selected = parse_output(proc.stdout)
        # NEAR steady window: sanitized run is 90 frames (NEAR starts at 60).
        if sanitized:
            near = [f for f in stats if 75 <= f <= 89]
        else:
            near = [f for f in stats if 100 <= f <= 129]
        if len(near) < 10:
            failures.append("region: too few NEAR frames parsed")
            return
        loaded = stats[near[-1]][2]
        failed = max(v[3] for v in stats.values())
        sel = set()
        for f in near:
            sel |= no_wrapper(selected.get(f, set()))
        print(f"region: NEAR selected={sorted(sel)} loaded={loaded} "
              f"failed={failed}")
        if loaded != 4:
            failures.append(
                f"region: tilesLoaded={loaded} != 4 (wrapper+root+2 children)")
        if failed != 0:
            failures.append("region: failed != 0")
        if sel != {"root.glb", "child_0.glb", "child_1.glb"}:
            failures.append(
                f"region: NEAR selected={sorted(sel)} != root+2 children")

        if sanitized:
            return
        # Non-empty render: same trajectory without a tileset must look
        # different. run_demo always passes --tileset, so build the
        # --no-tileset command directly.
        import subprocess
        empty = os.path.join(tmp, "empty.png")
        cmd = [demo, "--no-tileset", "--trajectory", REGION_TRAJ,
               "--warmup", str(WARMUP), "--frames", str(frames),
               "--width", str(WIDTH), "--height", str(HEIGHT),
               "--screenshot", empty]
        proc = subprocess.run(["xvfb-run", "-a"] + cmd, capture_output=True,
                              text=True, timeout=600)
        if proc.returncode != 0:
            failures.append("region: empty-ref demo exited "
                            f"{proc.returncode}")
            return
        ndiff = screenshot_diff_px(shot, empty)
        total = WIDTH * HEIGHT
        print(f"region: screenshot vs empty: {ndiff} differing pixels "
              f"({ndiff / total:.4f})")
        if ndiff < REGION_MIN_DIFF_PX:
            failures.append(
                f"region: render looks empty ({ndiff} px < "
                f"{REGION_MIN_DIFF_PX})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", default=DEMO)
    ap.add_argument("--sanitized", action="store_true",
                    help="shorter run for the ASan/LSan/UBSan gate")
    ap.add_argument("--suppressions", default=None)
    args = ap.parse_args()
    demo = args.demo
    if not os.path.isfile(demo):
        print("SKIP: tiles_demo not built")
        return 0
    for p in (ADD_TILESET, ADD_TRAJ, REGION_TILESET, REGION_TRAJ):
        if not os.path.isfile(p):
            print(f"SKIP: missing {p}")
            return 0

    failures = []

    # Part A: ADD refinement (FAR -> NEAR -> FAR).
    frames_a = 90 if args.sanitized else 190
    proc = run_demo(demo, ADD_TILESET, ADD_TRAJ, frames_a,
                    sanitized=args.sanitized,
                    suppressions=args.suppressions)
    if proc.returncode != 0:
        print("FAIL: add demo exited", proc.returncode)
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
        return 1
    stats, selected = parse_output(proc.stdout)
    if args.sanitized:
        near = [f for f in stats if 75 <= f <= 89]
        sel = set()
        for f in near:
            sel |= no_wrapper(selected.get(f, set()))
        print(f"add(san): NEAR selected={sorted(sel)}")
        want = {"root.glb", "child_0.glb", "child_1.glb",
                "child_2.glb", "child_3.glb"}
        if sel != want:
            failures.append(f"add(san): NEAR selected={sorted(sel)}")
    else:
        check_add(stats, selected, failures)

    # Part B: region bounding volumes (screenshot reference handled inside).
    check_region(demo, args.sanitized, args.suppressions, failures)

    if failures:
        print("FAIL:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS: ADD refinement + region bounding volumes verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
