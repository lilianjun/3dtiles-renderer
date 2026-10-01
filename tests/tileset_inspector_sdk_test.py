#!/usr/bin/env python3
"""P36-A: Inspector SDK gap APIs (no ImGui yet — demo flag hooks only).

Part A: --debug-content-volume draws the content wireframe
        (FilamentAsset::getWireframe, the old P35 debugShowBoundingVolume
        behavior, now split out).
Part B: --debug-bounding-volume draws tile bounding volumes from
        tileset.json as yellow line boxes (new P36 semantic).
Part C: --debug-request-volume draws viewer request volumes as cyan line
        boxes (p36 fixture declares them); on p3 (no request volumes)
        the flag is a no-op that must not crash.
Part D: --debug-freeze-frame from frame 1 skips tile selection entirely:
        the frame stays blank (clear color) while a normal run renders tiles.
Part E: --stats prints the extended TileStats (visited/pending/processing);
        invariant tilesLoading == pendingRequests + tilesProcessing holds
        on every frame, and visited counts the walked tree.
Part F: live setModelMatrix recomposes BV line-box transforms.

Uses p3 (tests/data/p3_box_tileset) and the p36 request-volume fixture.
"""

import argparse
import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image

REPO = os.path.join(os.path.dirname(__file__), "..")
DEMO = None  # set from --demo in main()
DATA = os.path.join(os.path.dirname(__file__), "data", "p3_box_tileset",
                    "tileset.json")
DATA_RV = os.path.join(os.path.dirname(__file__), "data",
                       "p36_request_volume_tileset", "tileset.json")
TMP = "/tmp/p36a_test"
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
LSAN_SUPP = os.path.join(os.path.dirname(__file__), "lsan.supp")

SANITIZED = False

STAT_RE = re.compile(
    r"\[stats\] frame=(\d+) selected=(-?\d+) loading=(-?\d+) "
    r"loaded=(-?\d+) failed=(-?\d+) bytes=(-?\d+) "
    r"visited=(-?\d+) pending=(-?\d+) processing=(-?\d+)"
)


def run_demo(args, tileset=DATA, capture_stderr=False):
    os.makedirs(TMP, exist_ok=True)
    cmd = [DEMO, "--tileset", tileset] + args
    if SANITIZED:
        cmd = [sys.executable, RUN_SAN, "--suppressions", LSAN_SUPP,
               "--"] + cmd
    elif not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a"] + cmd
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, f"demo failed: {result.stderr[-2000:]}"
    return result


def changed_pixels(a_path, b_path):
    a = np.array(Image.open(a_path)).astype(int)
    b = np.array(Image.open(b_path)).astype(int)
    diff = np.abs(a - b).sum(axis=2)
    return (diff > 30).sum(), a.shape[0] * a.shape[1]


def test_a_content_volume():
    """Content wireframe appears with --debug-content-volume."""
    off_png = os.path.join(TMP, "a_off.png")
    on_png = os.path.join(TMP, "a_on.png")
    run_demo(["--frames", "30", "--screenshot", off_png])
    run_demo(["--frames", "30", "--screenshot", on_png,
              "--debug-content-volume"])
    changed, total = changed_pixels(off_png, on_png)
    print(f"Part A: changed pixels {changed}/{total}")
    assert changed > 100, f"expected wireframe pixels, got {changed}"
    assert changed < total // 2, f"too many changed pixels: {changed}"
    print("Part A: PASS")


def test_b_tile_bounding_volume():
    """Tile BVs from tileset.json draw as line boxes."""
    off_png = os.path.join(TMP, "b_off.png")
    on_png = os.path.join(TMP, "b_on.png")
    run_demo(["--frames", "30", "--screenshot", off_png])
    run_demo(["--frames", "30", "--screenshot", on_png,
              "--debug-bounding-volume"])
    changed, total = changed_pixels(off_png, on_png)
    print(f"Part B: changed pixels {changed}/{total}")
    assert changed > 100, f"expected BV box pixels, got {changed}"
    assert changed < total // 2, f"too many changed pixels: {changed}"
    # Both flags together must not crash (for the p3 fixture the tile BV
    # and the content wireframe coincide, so no extra pixels are expected).
    both_png = os.path.join(TMP, "b_both.png")
    run_demo(["--frames", "30", "--screenshot", both_png,
              "--debug-bounding-volume", "--debug-content-volume"])
    print("Part B: PASS")


def test_c_request_volume():
    """Request volumes draw on the p36 fixture; no-op (no crash) on p3."""
    off_png = os.path.join(TMP, "c_off.png")
    on_png = os.path.join(TMP, "c_on.png")
    run_demo(["--frames", "30", "--screenshot", off_png], tileset=DATA_RV)
    run_demo(["--frames", "30", "--screenshot", on_png,
              "--debug-request-volume"], tileset=DATA_RV)
    changed, total = changed_pixels(off_png, on_png)
    print(f"Part C: changed pixels {changed}/{total}")
    assert changed > 100, f"expected request-volume pixels, got {changed}"
    assert changed < total // 2, f"too many changed pixels: {changed}"
    # p3 declares no viewerRequestVolume: flag must not crash and must
    # not change the picture.
    p3_off = os.path.join(TMP, "c_p3_off.png")
    p3_on = os.path.join(TMP, "c_p3_on.png")
    run_demo(["--frames", "30", "--screenshot", p3_off])
    run_demo(["--frames", "30", "--screenshot", p3_on,
              "--debug-request-volume"])
    changed_p3, _ = changed_pixels(p3_off, p3_on)
    print(f"Part C: p3 (no RV) changed pixels {changed_p3}")
    assert changed_p3 == 0, f"request-volume flag changed p3: {changed_p3}"
    print("Part C: PASS")


def test_d_freeze_frame():
    """Freeze from frame 1: no tile selection runs; the frame stays in the
    initial empty-scene state (identical to frame 1 of a normal run)."""
    normal_png = os.path.join(TMP, "d_normal.png")
    frozen_png = os.path.join(TMP, "d_frozen.png")
    run_demo(["--frames", "30", "--screenshot", normal_png])
    run_demo(["--frames", "30", "--screenshot", frozen_png,
              "--debug-freeze-frame"])
    changed, total = changed_pixels(normal_png, frozen_png)
    print(f"Part D: normal vs frozen changed pixels {changed}/{total}")
    assert changed > total // 4, \
        f"frozen frame should lack tiles, only {changed} changed"
    # The frozen frame never progressed past the initial state: it must
    # match frame 1 of a normal run (tiles never selected/loaded).
    # (--frames 1 exits before tileset cleanup on the sanitizer build, a
    # pre-existing leak unrelated to P36, so this comparison is skipped
    # under --sanitized.)
    if not SANITIZED:
        first_png = os.path.join(TMP, "d_first.png")
        run_demo(["--frames", "1", "--screenshot", first_png])
        changed_first, _ = changed_pixels(frozen_png, first_png)
        print(f"Part D: frozen vs first-frame changed pixels {changed_first}")
        assert changed_first == 0, \
            f"frozen frame diverged from initial state: {changed_first}"
    print("Part D: PASS")


def test_e_extended_stats():
    """Extended TileStats fields and the loading invariant."""
    result = run_demo(["--frames", "40", "--stats"])
    rows = [tuple(int(g) for g in m.groups())
            for m in STAT_RE.finditer(result.stdout)]
    assert len(rows) == 40, f"got {len(rows)} [stats] lines, expected 40"
    for (frame, selected, loading, loaded, failed, b, visited, pending,
         processing) in rows:
        assert loading == pending + processing, \
            f"frame {frame}: loading {loading} != pending {pending} + " \
            f"processing {processing}"
        assert visited >= 0, f"frame {frame}: visited not reported"
        assert pending >= 0 and processing >= 0
    # After settling, the whole tree was walked (p3: root + 2 children +
    # one synthetic empty-ID tile, see --print-selected) and nothing is
    # in flight.
    last = rows[-1]
    print(f"Part E: last frame visited={last[6]} loaded={last[3]} "
          f"loading={last[2]}")
    assert last[6] == 4, f"expected visited=4, got {last[6]}"
    assert last[2] == 0 and last[7] == 0 and last[8] == 0
    assert last[3] == 4, f"expected loaded=4, got {last[3]}"
    print("Part E: PASS")


def test_f_live_model_matrix_moves_bv():
    """Live setModelMatrix recomposes BV/RQ line-box transforms (P36-A fix):
    applying tx=20 at load vs live at frame 15 must give identical frames."""
    at_load = os.path.join(TMP, "f_at_load.png")
    live = os.path.join(TMP, "f_live.png")
    run_demo(["--frames", "30", "--screenshot", at_load,
              "--debug-bounding-volume", "--model-matrix-tx", "20"])
    run_demo(["--frames", "30", "--screenshot", live,
              "--debug-bounding-volume", "--model-matrix-tx", "20",
              "--model-matrix-tx-at-frame", "15"])
    changed, total = changed_pixels(at_load, live)
    print(f"Part F: at-load vs live-tx changed pixels {changed}/{total}")
    assert changed == 0, \
        f"BV boxes did not follow live setModelMatrix: {changed}"
    print("Part F: PASS")


def main():
    global DEMO, SANITIZED
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--sanitized", action="store_true")
    ap.add_argument("--tileset", default=DATA)  # accepted for CTest uniformity
    args = ap.parse_args()
    DEMO = args.demo
    SANITIZED = args.sanitized
    test_a_content_volume()
    test_b_tile_bounding_volume()
    test_c_request_volume()
    test_d_freeze_frame()
    test_e_extended_stats()
    test_f_live_model_matrix_moves_bv()
    print("ALL PASS")


if __name__ == "__main__":
    main()
