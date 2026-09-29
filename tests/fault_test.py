#!/usr/bin/env python3
"""P6 fault-injection test: bad inputs must fail gracefully, never crash.

Driven through tiles_demo; the exit-code contract is:
  A. nonexistent tileset path -> exit 1 (clean error message, no crash)
  B. corrupt tileset.json      -> exit 1 (bounded load wait, no crash)
  C. corrupt .glb tile content -> exit 0 (bad tile skipped, rest renders)
  D. truncated i3dm            -> exit 0 (bad tile skipped, no crash)
  E. i3dm with wrong magic    -> exit 0 (bad tile skipped, no crash)
  F. i3dm INSTANCES_LENGTH past the POSITION data
                              -> exit 0 (bad tile skipped, no crash)
  G. truncated pnts            -> exit 0 (bad tile skipped, no crash)
  H. pnts with wrong magic    -> exit 0 (bad tile skipped, no crash)
  I. pnts POINTS_LENGTH past the POSITION/RGB data
                              -> exit 0 (bad tile skipped, no crash)
  J. cmpt with a truncated inner pnts tile
                              -> exit 0 (bad tile skipped, no crash)
  K. cmpt with wrong magic     -> exit 0 (bad tile skipped, no crash)
  L. cmpt tilesLength larger than the actual inner tiles
                              -> exit 0 (bad tile skipped, no crash)
  M. 1.1 tileset with corrupt bare-glb content
                              -> exit 0 (bad tile skipped, no crash)
  N. 1.1 implicit tileset with a missing subtree file
                              -> exit 0 (subtree load fails, no crash)
  O. 1.1 implicit tileset with an illegal subdivisionScheme
                              -> exit 0 (no implicit loader, no crash)

A crash (segfault/abort, e.g. exit -11/-6) fails the test. When the demo
binary was built with -DTILES_SANITIZE=ON, wrap this script in
run_sanitized.py to also gate on ASan/LSan/UBSan reports.

Usage:
  fault_test.py --demo <tiles_demo> [--workdir <tmpdir>]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sanitizer_common import find_reports, maybe_wrap_xvfb

HERE = os.path.dirname(os.path.abspath(__file__))


def run_demo(demo, args, timeout=600):
    cmd = maybe_wrap_xvfb(
        [demo, "--width", "800", "--height", "600"] + args)
    print("+", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout)
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    reports = find_reports(proc.stdout + proc.stderr)
    if reports:
        print("FAIL: sanitizer reports: %s" % ", ".join(reports))
        return None
    return proc.returncode


def crashed(returncode):
    # Unix: negative = killed by signal (e.g. -11 SIGSEGV, -6 SIGABRT).
    # Windows: 0xC0000005 etc. Either way, anything outside {0, 1} here is
    # a crash, not a graceful failure.
    return returncode not in (0, 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--demo", required=True)
    ap.add_argument("--workdir", default=None)
    args = ap.parse_args()

    workdir = args.workdir or tempfile.mkdtemp(prefix="fault_test_")
    os.makedirs(workdir, exist_ok=True)
    failures = 0

    # Case A: nonexistent tileset path.
    print("=== case A: nonexistent tileset path ===")
    rc = run_demo(args.demo, ["--frames", "5",
                              "--tileset", "/nonexistent/tileset.json"])
    if rc is None:
        failures += 1
    elif crashed(rc):
        print("FAIL: case A crashed (exit %d)" % rc)
        failures += 1
    elif rc != 1:
        print("FAIL: case A: expected graceful exit 1, got %d" % rc)
        failures += 1
    else:
        print("PASS: case A (graceful exit 1)")

    # Case B: corrupt tileset.json (invalid JSON). The SDK's load() waits a
    # bounded 30 s for the root tile, then fails gracefully.
    print("=== case B: corrupt tileset.json ===")
    bad_dir = os.path.join(workdir, "bad_json")
    os.makedirs(bad_dir, exist_ok=True)
    with open(os.path.join(bad_dir, "tileset.json"), "w") as f:
        f.write("{ this is not valid json !!!")
    rc = run_demo(args.demo, ["--frames", "5",
                              "--tileset",
                              os.path.join(bad_dir, "tileset.json")],
                  timeout=300)
    if rc is None:
        failures += 1
    elif crashed(rc):
        print("FAIL: case B crashed (exit %d)" % rc)
        failures += 1
    elif rc != 1:
        print("FAIL: case B: expected graceful exit 1, got %d" % rc)
        failures += 1
    else:
        print("PASS: case B (graceful exit 1)")

    # Case C: corrupt .glb tile content. Copy the vendored tileset, smash
    # one child glb with garbage bytes; the bad tile must be skipped while
    # the rest still renders (exit 0).
    print("=== case C: corrupt .glb tile content ===")
    corrupt_dir = os.path.join(workdir, "corrupt_glb")
    shutil.rmtree(corrupt_dir, ignore_errors=True)
    shutil.copytree(os.path.join(HERE, "data", "p3_box_tileset"), corrupt_dir)
    with open(os.path.join(corrupt_dir, "child_a.glb"), "wb") as f:
        f.write(b"\x00\xff not a glb at all, just garbage bytes \xde\xad" * 64)
    shot = os.path.join(workdir, "corrupt_glb.png")
    rc = run_demo(args.demo, ["--frames", "30",
                              "--tileset",
                              os.path.join(corrupt_dir, "tileset.json"),
                              "--screenshot", shot])
    if rc is None:
        failures += 1
    elif crashed(rc):
        print("FAIL: case C crashed (exit %d)" % rc)
        failures += 1
    elif rc != 0:
        print("FAIL: case C: expected exit 0 (bad tile skipped), got %d" % rc)
        failures += 1
    elif not os.path.exists(shot):
        print("FAIL: case C: screenshot not written")
        failures += 1
    else:
        print("PASS: case C (corrupt tile skipped, rest rendered)")

    # P7 cases D/E/F: corrupt i3dm tile content. Same contract as case C:
    # the bad tile is skipped, the demo exits 0, no crash.
    print("=== cases D/E/F: corrupt i3dm tile content ===")
    i3dm_dir = os.path.join(workdir, "corrupt_i3dm")
    shutil.rmtree(i3dm_dir, ignore_errors=True)
    os.makedirs(i3dm_dir)
    with open(os.path.join(HERE, "data", "p7_i3dm_tileset",
                           "instances.i3dm"), "rb") as f:
        good_i3dm = f.read()
    import json as _json
    import struct as _struct
    # D: truncated mid-header.
    with open(os.path.join(i3dm_dir, "tile.i3dm"), "wb") as f:
        f.write(good_i3dm[:20])
    # E: wrong magic.
    with open(os.path.join(i3dm_dir, "tile_e.i3dm"), "wb") as f:
        f.write(b"xxxx" + good_i3dm[4:])
    # F: INSTANCES_LENGTH larger than the POSITION data (reads past end).
    ft_len = _struct.unpack("<I", good_i3dm[12:16])[0]
    ft = _json.loads(good_i3dm[32:32 + ft_len].decode("utf-8"))
    ft["INSTANCES_LENGTH"] = ft["INSTANCES_LENGTH"] * 2
    new_ft = _json.dumps(ft, separators=(",", ":")).encode("utf-8")
    new_ft += b" " * ((4 - len(new_ft) % 4) % 4)
    assert len(new_ft) == ft_len, "feature-table padding changed"
    with open(os.path.join(i3dm_dir, "tile_f.i3dm"), "wb") as f:
        f.write(good_i3dm[:32] + new_ft + good_i3dm[32 + ft_len:])
    for case, uri in (("D", "tile.i3dm"), ("E", "tile_e.i3dm"),
                      ("F", "tile_f.i3dm")):
        ts_path = os.path.join(i3dm_dir, "tileset_%s.json" % case)
        with open(ts_path, "w") as f:
            _json.dump({
                "asset": {"version": "1.0"},
                "root": {
                    "boundingVolume": {
                        "box": [9.25, 0, 0, 10.25, 0, 0, 0, 3, 0, 0, 0, 8]},
                    "geometricError": 0.0,
                    "content": {"uri": uri},
                },
            }, f)
        shot = os.path.join(workdir, "corrupt_i3dm_%s.png" % case)
        rc = run_demo(args.demo, ["--frames", "30", "--tileset", ts_path,
                                  "--screenshot", shot])
        if rc is None:
            failures += 1
        elif crashed(rc):
            print("FAIL: case %s crashed (exit %d)" % (case, rc))
            failures += 1
        elif rc != 0:
            print("FAIL: case %s: expected exit 0 (bad tile skipped), "
                  "got %d" % (case, rc))
            failures += 1
        else:
            print("PASS: case %s (corrupt i3dm skipped, no crash)" % case)

    # P8 cases G/H/I: corrupt pnts tile content. Same contract as case C:
    # the bad tile is skipped, the demo exits 0, no crash.
    print("=== cases G/H/I: corrupt pnts tile content ===")
    pnts_dir = os.path.join(workdir, "corrupt_pnts")
    shutil.rmtree(pnts_dir, ignore_errors=True)
    os.makedirs(pnts_dir)
    with open(os.path.join(HERE, "data", "p8_pnts_cloud",
                           "cloud.pnts"), "rb") as f:
        good_pnts = f.read()
    # G: truncated mid-header (pnts header is 28 bytes).
    with open(os.path.join(pnts_dir, "tile.pnts"), "wb") as f:
        f.write(good_pnts[:20])
    # H: wrong magic.
    with open(os.path.join(pnts_dir, "tile_h.pnts"), "wb") as f:
        f.write(b"xxxx" + good_pnts[4:])
    # I: POINTS_LENGTH larger than the POSITION/RGB data (reads past end).
    # NOTE: no padding assertion needed here: the P8 fixture's RGB data
    # ends exactly at the feature-table binary end (byteOffset == POINTS*12
    # already 4-aligned for 420 points), so doubling POINTS_LENGTH keeps the
    # JSON at the same length.
    ft_len = _struct.unpack("<I", good_pnts[12:16])[0]
    ft = _json.loads(good_pnts[28:28 + ft_len].decode("utf-8"))
    ft["POINTS_LENGTH"] = ft["POINTS_LENGTH"] * 2
    new_ft = _json.dumps(ft, separators=(",", ":")).encode("utf-8")
    new_ft += b" " * ((4 - len(new_ft) % 4) % 4)
    assert len(new_ft) == ft_len, "feature-table padding changed"
    with open(os.path.join(pnts_dir, "tile_i.pnts"), "wb") as f:
        f.write(good_pnts[:28] + new_ft + good_pnts[28 + ft_len:])
    for case, uri in (("G", "tile.pnts"), ("H", "tile_h.pnts"),
                      ("I", "tile_i.pnts")):
        ts_path = os.path.join(pnts_dir, "tileset_%s.json" % case)
        with open(ts_path, "w") as f:
            _json.dump({
                "asset": {"version": "1.0"},
                "root": {
                    "boundingVolume": {
                        "box": [0, 0, 0, 5, 0, 0, 0, 5, 0, 0, 0, 5]},
                    "geometricError": 0.0,
                    "content": {"uri": uri},
                },
            }, f)
        shot = os.path.join(workdir, "corrupt_pnts_%s.png" % case)
        rc = run_demo(args.demo, ["--frames", "30", "--tileset", ts_path,
                                  "--screenshot", shot])
        if rc is None:
            failures += 1
        elif crashed(rc):
            print("FAIL: case %s crashed (exit %d)" % (case, rc))
            failures += 1
        elif rc != 0:
            print("FAIL: case %s: expected exit 0 (bad tile skipped), "
                  "got %d" % (case, rc))
            failures += 1
        else:
            print("PASS: case %s (corrupt pnts skipped, no crash)" % case)

    # P9 cases J/K/L: corrupt cmpt tile content. Same contract as case C:
    # the bad tile is skipped, the demo exits 0, no crash.
    print("=== cases J/K/L: corrupt cmpt tile content ===")
    cmpt_dir = os.path.join(workdir, "corrupt_cmpt")
    shutil.rmtree(cmpt_dir, ignore_errors=True)
    os.makedirs(cmpt_dir)
    with open(os.path.join(HERE, "data", "p9_cmpt_tileset",
                           "composite.cmpt"), "rb") as f:
        good_cmpt = f.read()
    # J: inner pnts tile truncated (cmpt envelope stays valid: the pnts
    # inner tile's byteLength is cut by 100 bytes, the trailing i3dm is
    # dropped, and the cmpt byteLength is set to the actual new size).
    # The inner pnts converter reports an *error*, so the merged result
    # fails gracefully.
    pnts_off = 16 + _struct.unpack("<I", good_cmpt[16 + 8:16 + 12])[0]
    pnts_len = _struct.unpack("<I", good_cmpt[pnts_off + 8:pnts_off + 12])[0]
    cut = 100
    assert pnts_len > cut + 12
    new_pnts_len = pnts_len - cut
    trunc = (good_cmpt[:pnts_off] +
             good_cmpt[pnts_off:pnts_off + 8] +
             _struct.pack("<I", new_pnts_len) +
             good_cmpt[pnts_off + 12:pnts_off + new_pnts_len])
    trunc = trunc[:8] + _struct.pack("<I", len(trunc)) + trunc[12:]
    with open(os.path.join(cmpt_dir, "tile_j.cmpt"), "wb") as f:
        f.write(trunc)
    # K: wrong magic.
    with open(os.path.join(cmpt_dir, "tile_k.cmpt"), "wb") as f:
        f.write(b"xxxx" + good_cmpt[4:])
    # L: tilesLength larger than the actual inner tiles (converter stops
    # at byteLength with a warning; the tile must not crash the demo).
    # cmpt header: magic(4) version(4) byteLength(4) tilesLength(4).
    tiles_length = _struct.unpack("<I", good_cmpt[12:16])[0]
    with open(os.path.join(cmpt_dir, "tile_l.cmpt"), "wb") as f:
        f.write(good_cmpt[:12] + _struct.pack("<I", tiles_length + 5)
                + good_cmpt[16:])
    for case, uri in (("J", "tile_j.cmpt"), ("K", "tile_k.cmpt"),
                      ("L", "tile_l.cmpt")):
        ts_path = os.path.join(cmpt_dir, "tileset_%s.json" % case)
        with open(ts_path, "w") as f:
            _json.dump({
                "asset": {"version": "1.0"},
                "root": {
                    "boundingVolume": {
                        "box": [0, 0, 0, 14, 0, 0, 0, 5, 0, 0, 0, 5]},
                    "geometricError": 0.0,
                    "content": {"uri": uri},
                },
            }, f)
        shot = os.path.join(workdir, "corrupt_cmpt_%s.png" % case)
        rc = run_demo(args.demo, ["--frames", "30", "--tileset", ts_path,
                                  "--screenshot", shot])
        if rc is None:
            failures += 1
        elif crashed(rc):
            print("FAIL: case %s crashed (exit %d)" % (case, rc))
            failures += 1
        elif rc != 0:
            print("FAIL: case %s: expected exit 0 (bad tile skipped), "
                  "got %d" % (case, rc))
            failures += 1
        else:
            print("PASS: case %s (corrupt cmpt skipped, no crash)" % case)

    # M/N/O: 3D Tiles 1.1 bad inputs (P10).
    eleven_dir = os.path.join(workdir, "bad_11")
    shutil.rmtree(eleven_dir, ignore_errors=True)
    os.makedirs(eleven_dir)
    p10data = os.path.join(HERE, "data")
    # M: corrupt bare-glb content in a 1.1 tileset (bad magic).
    m_dir = os.path.join(eleven_dir, "m")
    os.makedirs(m_dir)
    with open(os.path.join(p10data, "p10_11_glb", "box.glb"), "rb") as f:
        good_glb = f.read()
    with open(os.path.join(m_dir, "box.glb"), "wb") as f:
        f.write(b"xxxx" + good_glb[4:])
    with open(os.path.join(m_dir, "tileset.json"), "w") as f:
        _json.dump({
            "asset": {"version": "1.1"},
            "root": {
                "boundingVolume": {"box": [0, 0, 0, 8, 0, 0, 0, 5, 0,
                                           0, 0, 5]},
                "geometricError": 0.0,
                "content": {"uri": "box.glb"},
            },
        }, f)
    # N: implicit tileset whose subtree file is missing.
    n_dir = os.path.join(eleven_dir, "n")
    shutil.copytree(os.path.join(p10data, "p10_11_implicit", "tiles"),
                    os.path.join(n_dir, "tiles"))
    shutil.copy(os.path.join(p10data, "p10_11_implicit", "tileset.json"),
                os.path.join(n_dir, "tileset.json"))
    # O: implicit tileset with an illegal subdivisionScheme.
    o_dir = os.path.join(eleven_dir, "o")
    shutil.copytree(os.path.join(p10data, "p10_11_implicit", "tiles"),
                    os.path.join(o_dir, "tiles"))
    shutil.copytree(os.path.join(p10data, "p10_11_implicit", "subtrees"),
                    os.path.join(o_dir, "subtrees"))
    with open(os.path.join(p10data, "p10_11_implicit",
                           "tileset.json")) as f:
        o_ts = _json.load(f)
    o_ts["root"]["extensions"]["3DTILES_implicit_tiling"][
        "subdivisionScheme"] = "HEXAGON"
    with open(os.path.join(o_dir, "tileset.json"), "w") as f:
        _json.dump(o_ts, f)
    for case in ("M", "N", "O"):
        ts_path = os.path.join(eleven_dir, case.lower(), "tileset.json")
        shot = os.path.join(workdir, "bad_11_%s.png" % case)
        rc = run_demo(args.demo, ["--frames", "30", "--tileset", ts_path,
                                  "--screenshot", shot])
        if rc is None:
            failures += 1
        elif crashed(rc):
            print("FAIL: case %s crashed (exit %d)" % (case, rc))
            failures += 1
        elif rc != 0:
            print("FAIL: case %s: expected exit 0 (bad input skipped), "
                  "got %d" % (case, rc))
            failures += 1
        else:
            print("PASS: case %s (1.1 bad input skipped, no crash)" % case)

    if failures:
        print("FAIL: fault_test (%d case(s) failed)" % failures)
        return 1
    print("PASS: fault_test (all cases graceful)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
