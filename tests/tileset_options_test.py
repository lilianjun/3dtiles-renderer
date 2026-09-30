#!/usr/bin/env python3
"""P31: tileset load options + live maximumScreenSpaceError.

Part A (construction): --max-sse 100000 on the p3 fixture selects only the
root tile (no refinement), vs 3 tiles (root + 2 children) at the default 16.
Proves the construction-time option reaches cesium-native traversal.

Part B (live setter): --set-sse-at-frame 30:100000 flips selection from 3
tiles to 1 tile mid-run. Proves setMaximumScreenSpaceError() takes effect
on the next frame without reloading.

Part C (passthrough): --print-tileset-options dumps the effective options
(currentTilesetOptions()); asserts every field — the two overridden ones
plus all defaults. Proves all 11 fields survive the SDK -> cesium-native
mapping (the traversal behavior in A/B proves the mapping itself works).

Usage:
  python3 tests/tileset_options_test.py --demo <tiles_demo>
      --tileset <p3 tileset.json>
"""
import argparse
import os
import re
import subprocess
import sys

P3_DEFAULT_SELECTED = 3  # root + 2 children at SSE=16, default camera
# NOTE: selectedTileIds() also contains one tile whose ID string is ""
# (cesium-native createTileIdString for the traversal root); assertions use
# the non-empty IDs, which are the content tiles.


def named_ids(ids):
    return {i for i in ids if i}


def run_demo(demo, tileset, extra_args):
    cmd = [demo, "--tileset", tileset] + extra_args
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        print(f"FAIL: demo exited {proc.returncode}")
        sys.exit(1)
    return proc.stdout


def parse_selected(stdout):
    # -> list of (frame, [ids]); note the root tile's ID string can be ""
    # (cesium-native createTileIdString), so count list entries, not set
    # members.
    out = []
    for m in re.finditer(r"\[selected\] frame=(\d+) ids=([^\s]*)", stdout):
        ids = m.group(2)
        out.append((int(m.group(1)), ids.split(",") if ids else []))
    return out


def parse_options(stdout):
    m = re.search(r"\[tileset-options\]([^\n]*)", stdout)
    if not m:
        return None
    kv = {}
    for token in m.group(1).strip().split():
        k, _, v = token.partition("=")
        kv[k] = v
    return kv


def part_a(demo, tileset):
    print("--- Part A: construction-time SSE ---")
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--print-selected"])
    sel = parse_selected(out)
    assert sel, "no [selected] lines parsed"
    last = named_ids(sel[-1][1])
    assert last == {"root.glb", "child_a.glb", "child_b.glb"}, \
        f"default SSE: expected root+2 children, got {last}"

    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--print-selected",
                    "--max-sse", "100000"])
    sel = parse_selected(out)
    assert sel, "no [selected] lines parsed (--max-sse)"
    last = named_ids(sel[-1][1])
    assert last == {"root.glb"}, \
        f"--max-sse 100000: expected root only (no refinement), got {last}"
    print("Part A OK")


def part_b(demo, tileset):
    print("--- Part B: live setMaximumScreenSpaceError ---")
    out = run_demo(demo, tileset,
                   ["--frames", "90", "--print-selected",
                    "--set-sse-at-frame", "30:100000"])
    m = re.search(r"\[set-sse\] frame=(\d+) value=", out)
    assert m and int(m.group(1)) == 30, "missing [set-sse] marker at frame 30"
    sel = dict(parse_selected(out))
    assert sel, "no [selected] lines parsed"
    # 10 frames right before the switch: fully loaded, root+2 children.
    pre = [named_ids(sel[f]) for f in range(20, 30) if f in sel]
    assert len(pre) == 10, f"missing pre-switch frames: {sorted(sel)[:12]}"
    assert all(s == {"root.glb", "child_a.glb", "child_b.glb"} for s in pre), \
        f"pre-switch: expected root+2 children, got {pre}"
    # 20 frames right after: refinement disabled, root only.
    post = [named_ids(sel[f]) for f in range(31, 51) if f in sel]
    assert len(post) == 20, "missing post-switch frames"
    assert all(s == {"root.glb"} for s in post), \
        f"post-switch: expected root only, got " \
        f"{sorted(set().union(*post))}"
    print("Part B OK")


def part_c(demo, tileset):
    print("--- Part C: options passthrough ---")
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--print-tileset-options",
                    "--max-sse", "32", "--no-frustum-culling"])
    kv = parse_options(out)
    assert kv is not None, "missing [tileset-options] line"
    expected = {
        "maximumScreenSpaceError": 32.0,
        "forbidHoles": 0.0,
        "preloadAncestors": 1.0,
        "preloadSiblings": 1.0,
        "enableFrustumCulling": 0.0,
        "enableFogCulling": 1.0,
        "maximumSimultaneousTileLoads": 20.0,
        "loadingDescendantLimit": 20.0,
        "enableLodTransitionPeriod": 0.0,
        "lodTransitionLength": 1.0,
    }
    for k, want in expected.items():
        assert k in kv, f"option {k} missing from dump"
        got = float(kv[k])
        assert abs(got - want) < 1e-9, f"{k}: expected {want}, got {got}"
    radii = [float(x) for x in kv["ellipsoidRadii"].split(",")]
    assert len(radii) == 3
    want_radii = (6378137.0, 6378137.0, 6356752.3142451793)
    # Printed at default stream precision; relative tolerance is plenty to
    # prove the values survived the SDK -> cesium-native mapping.
    for got, want in zip(radii, want_radii):
        assert abs(got - want) / want < 1e-4, f"ellipsoid radius {got}"
    print("Part C OK")


def part_d(demo, tileset):
    print("--- Part D: option validation ---")
    # D1: negative SSE normalizes to the default 16 (same selection as the
    # default run in Part A) instead of confusing cesium-native.
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--print-selected",
                    "--max-sse", "-5"])
    sel = parse_selected(out)
    assert sel, "no [selected] lines parsed (--max-sse -5)"
    last = named_ids(sel[-1][1])
    assert last == {"root.glb", "child_a.glb", "child_b.glb"}, \
        f"--max-sse -5: expected default behavior, got {last}"
    # D2: bogus ellipsoid radii fail the load fast (not silently replaced).
    cmd = [demo, "--tileset", tileset, "--until-loaded", "240",
           "--ellipsoid-radii", "0,6378137,6356752"]
    if not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    combined = proc.stdout + proc.stderr
    assert "ellipsoidRadii" in combined, \
        f"expected ellipsoidRadii error, got: {combined[-500:]}"
    assert proc.returncode != 0 or "failed to load tileset" in combined, \
        "bogus ellipsoid radii must fail the load"
    print("Part D OK")


def part_e(demo, tileset):
    print("--- Part E: pre-load stash path ---")
    # E1: setMaximumScreenSpaceError() before loadTileset() applies to the
    # load (stash-then-forward, mirrors setMaxCachedBytes).
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--print-selected",
                    "--preset-sse", "100000"])
    assert "[preset-sse] value=100000" in out, "missing [preset-sse] marker"
    sel = parse_selected(out)
    assert sel, "no [selected] lines parsed (--preset-sse)"
    last = named_ids(sel[-1][1])
    assert last == {"root.glb"}, \
        f"--preset-sse 100000: expected root only, got {last}"
    # E2: a negative pre-load stash means "restore default 16" and wins
    # over the construction value 32 (documented contract).
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--print-selected",
                    "--print-tileset-options",
                    "--preset-sse", "-5", "--max-sse", "32"])
    sel = parse_selected(out)
    assert sel, "no [selected] lines parsed (--preset-sse -5)"
    last = named_ids(sel[-1][1])
    assert last == {"root.glb", "child_a.glb", "child_b.glb"}, \
        f"--preset-sse -5 over --max-sse 32: expected default, got {last}"
    kv = parse_options(out)
    assert kv is not None and abs(float(kv["maximumScreenSpaceError"]) -
                                  16.0) < 1e-9, \
        f"currentTilesetOptions should report 16, got {kv}"
    print("Part E OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--tileset", required=True)
    args = ap.parse_args()
    part_a(args.demo, args.tileset)
    part_b(args.demo, args.tileset)
    part_c(args.demo, args.tileset)
    part_d(args.demo, args.tileset)
    part_e(args.demo, args.tileset)
    print("PASS: tileset_options")


if __name__ == "__main__":
    main()
