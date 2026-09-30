#!/usr/bin/env python3
"""P32: cesium.js-style tileset event callbacks (7 events).

Part A (steady load): --event-log on the p3 fixture. Asserts tileLoad fires
once per content tile (root + 2 children; the traversal root has an empty
ID string, same as selectedTileIds), tileVisible fires every frame, the
loadProgress counts converge to pending=0 processing=0, allTilesLoaded
fires while the view is fully loaded, and initialTilesLoaded fires exactly
once.

Part B (failure): a fixture with a corrupt child_a.glb (random bytes).
Asserts onTileFailed fires with id=child_a.glb and a non-empty message,
while the load itself still succeeds (root + child_b render).

Part C (unload): load p3, then --switch-tileset to the bad-child fixture
mid-run. Asserts onTileUnload fires for the old tileset's loaded tiles
(root/child_a/child_b) synchronously at the switch.

Part D (clear): --clear-events-at-frame N stops all events from frame N on.

Usage:
  python3 tests/event_test.py --demo <tiles_demo> --tileset <p3 tileset.json>
      --bad-tileset <p32_bad_child_tileset/tileset.json>
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
LSAN_SUPP = os.path.join(REPO, "tests", "lsan.supp")

SANITIZED = False


def run_demo(demo, tileset, extra_args):
    cmd = [demo, "--tileset", tileset] + extra_args
    if SANITIZED:
        # run_sanitized.py wraps xvfb itself (maybe_wrap_xvfb).
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


def event_counts(stdout):
    counts = {}
    ids = {}
    for m in re.finditer(r"\[event\] (\w+)(.*)", stdout):
        name, rest = m.group(1), m.group(2)
        counts[name] = counts.get(name, 0) + 1
        im = re.search(r"id=([^\s]*)", rest)
        if im:
            ids.setdefault(name, []).append(im.group(1))
    return counts, ids


def part_a(demo, tileset):
    print("--- Part A: steady load events ---")
    out = run_demo(demo, tileset,
                   ["--until-loaded", "240", "--event-log"])
    counts, ids = event_counts(out)
    loaded = sorted(i for i in ids.get("tileLoad", []) if i)
    assert loaded == ["child_a.glb", "child_b.glb", "root.glb"], \
        f"tileLoad ids: {loaded}"
    assert counts.get("tileVisible", 0) > 10, \
        f"tileVisible should fire every frame, got {counts}"
    assert counts.get("initialTilesLoaded", 0) == 1, \
        f"initialTilesLoaded must fire exactly once, got {counts}"
    assert counts.get("allTilesLoaded", 0) >= 1, "allTilesLoaded never fired"
    assert counts.get("loadProgress", 0) >= 1, "loadProgress never fired"
    # The progress counts must converge to (0, 0).
    last = None
    for m in re.finditer(r"\[event\] loadProgress pending=(\d+) "
                         r"processing=(\d+)", out):
        last = (int(m.group(1)), int(m.group(2)))
    assert last == (0, 0), f"loadProgress never converged: {last}"
    assert counts.get("tileFailed", 0) == 0, \
        f"unexpected tileFailed on a good tileset: {counts}"
    print("Part A OK")


def part_b(demo, bad_tileset):
    print("--- Part B: corrupt child content -> tileFailed ---")
    out = run_demo(demo, bad_tileset,
                   ["--until-loaded", "240", "--event-log"])
    counts, ids = event_counts(out)
    failed = ids.get("tileFailed", [])
    assert "child_a.glb" in failed, \
        f"tileFailed should name child_a.glb, got {failed}"
    msgs = re.findall(r"\[event\] tileFailed id=child_a\.glb message=(.*)",
                      out)
    assert msgs and all(m.strip() for m in msgs), \
        "tileFailed message must be non-empty"
    # The load itself survives: the good tiles still fire tileLoad.
    loaded = {i for i in ids.get("tileLoad", []) if i}
    assert {"root.glb", "child_b.glb"} <= loaded, \
        f"good tiles should still load: {loaded}"
    print("Part B OK")


def part_c(demo, tileset, bad_tileset):
    print("--- Part C: switch fires tileUnload for the old tileset ---")
    out = run_demo(
        demo, tileset,
        ["--frames", "120", "--event-log",
         "--switch-tileset", bad_tileset, "--switch-at-frame", "60"])
    counts, ids = event_counts(out)
    unloaded = sorted(ids.get("tileUnload", []))
    # The traversal root (empty ID string, same as in tileLoad) unloads
    # too; the three content tiles unload exactly once each.
    assert unloaded == ["", "child_a.glb", "child_b.glb", "root.glb"], \
        f"tileUnload ids at switch: {unloaded}"
    print("Part C OK")


def part_d(demo, tileset):
    print("--- Part D: clearEventCallbacks stops events ---")
    out = run_demo(demo, tileset,
                   ["--frames", "100", "--event-log",
                    "--clear-events-at-frame", "20"])
    # No [event] line may come from a frame >= 20. Events don't carry frame
    # numbers, so split the output at the [clear-events] marker.
    marker = "[clear-events] frame=20"
    assert marker in out, "missing [clear-events] marker"
    before, after = out.split(marker, 1)
    assert "[event]" in before, "no events before the clear (test is vacuous)"
    assert "[event]" not in after, \
        "events kept firing after clearEventCallbacks()"
    print("Part D OK")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--tileset", required=True)
    ap.add_argument("--bad-tileset", required=True)
    ap.add_argument("--sanitized", action="store_true",
                    help="run each demo invocation under ASan+LSan+UBSan "
                         "(needs a TILES_SANITIZE=ON build)")
    args = ap.parse_args()
    global SANITIZED
    SANITIZED = args.sanitized
    part_a(args.demo, args.tileset)
    part_b(args.demo, args.bad_tileset)
    part_c(args.demo, args.tileset, args.bad_tileset)
    part_d(args.demo, args.tileset)
    print("PASS: tileset_events")


if __name__ == "__main__":
    main()
