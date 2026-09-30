#!/usr/bin/env python3
"""P34: tileset cache / statistics / method alignment.

Part A: totalMemoryUsageInBytes returns content bytes (> 0 after load),
        matches TileStats::bytesLoaded.
Part B: trimLoadedTiles() evicts tiles not in use; memoryBytes drops.
Part C: hasExtension() reflects tileset.json "extensionsUsed".
"""
import argparse
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEMO = None
DATA = os.path.join(REPO, "tests", "data")
RUN_SAN = os.path.join(REPO, "tests", "run_sanitized.py")
LSAN_SUPP = os.path.join(REPO, "tests", "lsan.supp")
P3 = os.path.join(DATA, "p3_box_tileset", "tileset.json")
P34 = os.path.join(DATA, "p34_extensions_tileset", "tileset.json")
SANITIZED = False


def run_demo(*args):
    cmd = [DEMO] + list(args)
    if SANITIZED:
        cmd = [sys.executable, RUN_SAN, "--suppressions", LSAN_SUPP,
               "--"] + cmd
    elif not os.environ.get("DISPLAY"):
        cmd = ["xvfb-run", "-a", "-s", "-screen 0 1024x768x24"] + cmd
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    assert r.returncode == 0, f"demo failed: {r.stderr[-2000:]}"
    return r.stdout


def tileset_info(out):
    m = re.search(r"\[tileset-info\] (.*)", out)
    assert m, "no [tileset-info] line"
    return m.group(1)


def get_field(info, name):
    m = re.search(rf"{name}=([0-9]+)", info)
    assert m, f"no {name}= in: {info}"
    return int(m.group(1))


def test_part_a_memory_bytes():
    out = run_demo("--tileset", P3, "--until-loaded", "240",
                   "--print-tileset-info", "--stats")
    info = tileset_info(out)
    mem = get_field(info, "memoryBytes")
    assert mem > 0, f"memoryBytes should be > 0 after load, got {mem}"
    # Same value as TileStats::bytesLoaded (content bytes, not GPU estimate).
    # Take the last --stats line (steady state after until-loaded).
    matches = re.findall(r"bytes=([0-9]+)", out)
    assert matches, "no bytes= in --stats output"
    last_bytes = int(matches[-1])
    assert last_bytes == mem, (
        f"totalMemoryUsageInBytes ({mem}) != TileStats.bytesLoaded ({last_bytes})"
    )
    print(f"Part A PASS: memoryBytes={mem} == bytesLoaded")


def test_part_b_trim():
    out = run_demo(
        "--tileset", P3,
        "--frames", "90",
        "--set-sse-at-frame", "30:100000",  # huge SSE: only root selected
        "--trim-at-frame", "60",
        "--print-tileset-info",
    )
    m = re.search(r"\[p34\] trimLoadedTiles\(\) at frame=60 memoryBefore=([0-9]+)", out)
    assert m, "no [p34] trim line"
    before = int(m.group(1))
    after = get_field(tileset_info(out), "memoryBytes")
    assert before > 0, "nothing loaded before trim"
    assert after < before, (
        f"trimLoadedTiles did not free memory: before={before} after={after}"
    )
    print(f"Part B PASS: trim {before} -> {after} bytes")


def test_part_c_has_extension():
    # p34 fixture declares 3DTILES_content_gltf + KHR_test_extension.
    out = run_demo("--tileset", P34, "--until-loaded", "240",
                   "--has-extension", "3DTILES_content_gltf")
    assert 'hasExtension("3DTILES_content_gltf")=1' in out
    out = run_demo("--tileset", P34, "--until-loaded", "240",
                   "--has-extension", "KHR_test_extension")
    assert 'hasExtension("KHR_test_extension")=1' in out
    out = run_demo("--tileset", P34, "--until-loaded", "240",
                   "--has-extension", "KHR_nonexistent")
    assert 'hasExtension("KHR_nonexistent")=0' in out
    # p3 declares no extensionsUsed: everything is false.
    out = run_demo("--tileset", P3, "--until-loaded", "240",
                   "--has-extension", "3DTILES_content_gltf")
    assert 'hasExtension("3DTILES_content_gltf")=0' in out
    print("Part C PASS: hasExtension correct for present/absent/undeclared")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--tileset", required=True)
    ap.add_argument("--sanitized", action="store_true",
                    help="run demo under run_sanitized.py (no xvfb)")
    args = ap.parse_args()
    DEMO = args.demo
    P3 = args.tileset
    SANITIZED = args.sanitized
    test_part_a_memory_bytes()
    test_part_b_trim()
    test_part_c_has_extension()
    print("ALL P34 CACHE TESTS PASS")
