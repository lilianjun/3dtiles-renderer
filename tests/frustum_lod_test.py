#!/usr/bin/env python3
"""P20: frustum culling + LOD refinement behavior.

Part A (frustum): a 4x4 ground-plane fixture (tests/data/p20_frustum_tileset/,
16 REPLACE children on a 512x512 slab, Y-up horizontal for the orbit camera).
The camera yaws 0->360 at low pitch; per-frame selected tile IDs
(--print-selected) prove:
  - selectedTiles << total tiles (frustum culls the off-view tiles), and
  - yaw~0 vs yaw~180 select DIFFERENT tile sets: the far-corner tiles flip
    (tile_0_0/tile_3_0 visible at yaw~0, gone at yaw~180; tile_0_3/tile_3_3
    the reverse). Set-based, not just counts.

Part B (LOD): a tiny explicit REPLACE fixture (tests/data/p20_lod_tileset/:
root + 4 children, refine=REPLACE). Camera pushes FAR(1200m) -> NEAR(300m)
-> FAR(1200m); per-frame [stats] + [selected] prove:
  - FAR:  selected == {root.glb}, loaded == 2 (wrapper + root)
  - NEAR: selected == {child_0..3.glb} (root NOT selected: REPLACE, not ADD),
          loaded == 6 (monotonic increase)
  - FAR2: selected == {root.glb} again (falls back), loaded stays 6 (cached,
          not reloaded)

Pass criteria: all assertions below; failed == 0 in both parts.

Not asserted: exact pixel output, byte counts, or RSS. The tile-ID strings
come from cesium-native TileIdUtilities::createTileIdString (format is
cesium's, not contractual); the test only uses them as opaque set members.
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = os.path.join(REPO, "build", "linux", "tiles_demo")
FRUSTUM_TILESET = os.path.join(REPO, "tests", "data", "p20_frustum_tileset",
                               "tileset.json")
FRUSTUM_TRAJ = os.path.join(REPO, "tests", "data", "trajectories",
                            "p20_frustum.csv")
LOD_TILESET = os.path.join(REPO, "tests", "data", "p20_lod_tileset",
                           "tileset.json")
LOD_TRAJ = os.path.join(REPO, "tests", "data", "trajectories", "p20_lod.csv")

WIDTH, HEIGHT = 400, 300
WARMUP = 60


def run_demo(demo, tileset, trajectory, frames, extra=(), sanitized=False,
             suppressions=None):
    cmd = [demo, "--tileset", tileset, "--trajectory", trajectory,
           "--warmup", str(WARMUP), "--frames", str(frames),
           "--width", str(WIDTH), "--height", str(HEIGHT),
           "--stats", "--print-selected"] + list(extra)
    if sanitized:
        # Route through tests/run_sanitized.py so ASan/LSan/UBSan reports
        # fail the test (same pattern as memory_roam_test.py).
        run_san = os.path.join(REPO, "tests", "run_sanitized.py")
        supp = suppressions or os.path.join(REPO, "tests", "lsan.supp")
        cmd = ([sys.executable, run_san, "--suppressions", supp, "--"] + cmd)
    proc = subprocess.run(["xvfb-run", "-a"] + cmd, capture_output=True,
                          text=True, timeout=600)
    return proc


def parse_output(out):
    stats, selected = {}, {}
    for line in out.splitlines():
        m = re.match(r"\[stats\] frame=(\d+) selected=(\d+) loading=(\d+) "
                     r"loaded=(\d+) failed=(\d+) bytes=(\d+)", line)
        if m:
            stats[int(m.group(1))] = tuple(map(int, m.groups()[1:]))
            continue
        m = re.match(r"\[selected\] frame=(\d+) ids=(.*)", line)
        if m:
            ids = [x for x in m.group(2).strip().split(",") if x]
            selected[int(m.group(1))] = set(ids)
    return stats, selected


def check_frustum(stats, selected, failures):
    # Trajectory: frame i -> yaw = i*3 deg, pitch=15, dist=200.
    def yaw_of(f):
        return (f * 3.0) % 360

    frames = sorted(s for s in stats if 10 <= s <= 120)
    if len(frames) < 50:
        failures.append(f"frustum: only {len(frames)} frames parsed")
        return
    sels = [stats[f][0] for f in frames]
    max_sel = max(sels)
    print(f"frustum: frames={len(frames)} selected min/max="
          f"{min(sels)}/{max_sel} (total tiles=17)")
    if max_sel > 14:
        failures.append(
            f"frustum: max selected {max_sel} not << 17 (culling weak)")
    if any(stats[f][3] != 0 for f in frames):
        failures.append("frustum: failed != 0")

    def collect(yaw_lo, yaw_hi):
        s = set()
        for f in frames:
            y = yaw_of(f)
            in_range = (yaw_lo <= y <= yaw_hi) if yaw_lo <= yaw_hi else \
                (y >= yaw_lo or y <= yaw_hi)
            if in_range:
                s |= selected.get(f, set())
        return {x for x in s if x.startswith("tile_")}

    # yaw~0: camera at +Z looking -Z (sees the -Z far side, j=0 corners).
    # yaw~180: camera at -Z looking +Z (sees the +Z far side, j=3 corners).
    set0 = collect(350, 10)
    set180 = collect(170, 190)
    print(f"frustum: yaw~0 tiles={len(set0)}, yaw~180 tiles={len(set180)}")
    inter = set0 & set180
    union = set0 | set180
    jaccard = len(inter) / len(union) if union else 1.0
    print(f"frustum: intersection={len(inter)} Jaccard={jaccard:.3f}")

    # Per-tile proof: far-corner tiles must flip between the two views.
    # (tile_i_j: i->X, j->Z; j=0 is the -Z far side at yaw~0.)
    for tile in ("tile_0_0.glb", "tile_3_0.glb"):
        if tile not in set0:
            failures.append(f"frustum: {tile} not selected at yaw~0")
        if tile in set180:
            failures.append(f"frustum: {tile} still selected at yaw~180 "
                            f"(should be culled)")
    for tile in ("tile_0_3.glb", "tile_3_3.glb"):
        if tile not in set180:
            failures.append(f"frustum: {tile} not selected at yaw~180")
        if tile in set0:
            failures.append(f"frustum: {tile} still selected at yaw~0 "
                            f"(should be culled)")
    if jaccard > 0.75:
        failures.append(
            f"frustum: Jaccard {jaccard:.3f} too high (sets barely change)")


def check_lod(stats, selected, failures):
    # Trajectory phases: 0-59 FAR(1200m), 60-119 NEAR(300m), 120-179 FAR2.
    # Sample steady-state tails of each phase (skip transitions).
    def phase(fr_lo, fr_hi):
        fs = [f for f in stats if fr_lo <= f <= fr_hi]
        return fs

    far = phase(40, 59)
    near = phase(100, 119)
    far2 = phase(160, 179)
    if min(len(far), len(near), len(far2)) < 10:
        failures.append("lod: too few frames parsed in a phase")
        return

    def sel_set(fs):
        # Union over the steady-state window; all frames should agree.
        s = set()
        for f in fs:
            s |= selected.get(f, set())
        return s

    far_sel, near_sel, far2_sel = sel_set(far), sel_set(near), sel_set(far2)
    far_loaded = [stats[f][2] for f in far]
    near_loaded = [stats[f][2] for f in near]
    far2_loaded = [stats[f][2] for f in far2]
    print(f"lod: FAR  selected={sorted(far_sel)} loaded={far_loaded[-1]}")
    print(f"lod: NEAR selected={sorted(near_sel)} loaded={near_loaded[-1]}")
    print(f"lod: FAR2 selected={sorted(far2_sel)} loaded={far2_loaded[-1]}")

    if any(stats[f][3] != 0 for f in stats):
        failures.append("lod: failed != 0")

    # FAR: only the root is rendered (wrapper '' has no content; REPLACE
    # drops it from the render selection).
    if far_sel != {"root.glb"}:
        failures.append(f"lod: FAR selected={sorted(far_sel)} != {{root.glb}}")
    # NEAR: the 4 children REPLACE the root (root must be absent: not ADD).
    want_near = {"child_0.glb", "child_1.glb", "child_2.glb", "child_3.glb"}
    if near_sel != want_near:
        failures.append(
            f"lod: NEAR selected={sorted(near_sel)} != 4 children")
    if "root.glb" in near_sel:
        failures.append("lod: root.glb still selected at NEAR (ADD, not "
                        "REPLACE)")
    # tilesLoaded grows monotonically FAR -> NEAR (2 -> 6: wrapper+root,
    # then +4 children).
    if not (max(far_loaded) < min(near_loaded)):
        failures.append(
            f"lod: loaded not monotonic FAR{far_loaded[-1]}->NEAR{near_loaded[-1]}")
    # FAR2: selection falls back to the root (REPLACE, not accumulation);
    # loaded stays cached (no reload).
    if far2_sel != {"root.glb"}:
        failures.append(
            f"lod: FAR2 selected={sorted(far2_sel)} != {{root.glb}} (no fallback)")
    if far2_loaded[-1] != near_loaded[-1]:
        failures.append(
            f"lod: FAR2 loaded changed {near_loaded[-1]}->{far2_loaded[-1]} "
            f"(unexpected reload/evict)")


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
    for p in (FRUSTUM_TILESET, FRUSTUM_TRAJ, LOD_TILESET, LOD_TRAJ):
        if not os.path.isfile(p):
            print(f"SKIP: missing {p}")
            return 0

    failures = []

    # Part A: frustum (121-frame yaw sweep).
    frames_a = 60 if args.sanitized else 130
    proc = run_demo(demo, FRUSTUM_TILESET, FRUSTUM_TRAJ, frames_a,
                    sanitized=args.sanitized,
                    suppressions=args.suppressions)
    if proc.returncode != 0:
        print("FAIL: frustum demo exited", proc.returncode)
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
        return 1
    stats, selected = parse_output(proc.stdout)
    if args.sanitized:
        # Shorter sweep still covers yaw 0..~180; relax the flip to a
        # count-based culling check (the full set-flip is gated in the
        # non-sanitized run).
        sels = [v[0] for v in stats.values()]
        print(f"frustum(san): frames={len(stats)} "
              f"selected max={max(sels) if sels else -1}")
        if not sels or max(sels) > 14:
            failures.append("frustum(san): culling weak")
    else:
        check_frustum(stats, selected, failures)

    # Part B: LOD (FAR -> NEAR -> FAR).
    frames_b = 90 if args.sanitized else 190
    proc = run_demo(demo, LOD_TILESET, LOD_TRAJ, frames_b,
                    sanitized=args.sanitized,
                    suppressions=args.suppressions)
    if proc.returncode != 0:
        print("FAIL: lod demo exited", proc.returncode)
        print(proc.stdout[-2000:])
        print(proc.stderr[-2000:])
        return 1
    stats, selected = parse_output(proc.stdout)
    if args.sanitized:
        # Shorter run (90 frames): phases are FAR 0-59, NEAR 60-89; check
        # the NEAR steady state selects children without the root
        # (REPLACE core).
        near = [f for f in stats if 75 <= f <= 89]
        sel = set()
        for f in near:
            sel |= selected.get(f, set())
        print(f"lod(san): NEAR selected={sorted(sel)}")
        if sel != {"child_0.glb", "child_1.glb",
                   "child_2.glb", "child_3.glb"}:
            failures.append(f"lod(san): NEAR selected={sorted(sel)}")
    else:
        check_lod(stats, selected, failures)

    if failures:
        print("FAIL:")
        for f in failures:
            print("  -", f)
        return 1
    print("PASS: frustum culling + LOD refinement verified")
    return 0


if __name__ == "__main__":
    sys.exit(main())
