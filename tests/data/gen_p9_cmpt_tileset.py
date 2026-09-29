#!/usr/bin/env python3
"""Generate the P9 cmpt (Composite) test tileset (tests/data/p9_cmpt_tileset/).

cmpt (3D Tiles 1.0) exercises the cesium-native CmptToGltfConverter ->
merged CesiumGltf::Model -> SDK modelToGlb -> gltfio path.

Header semantics were verified against
Cesium3DTilesContent/src/CmptToGltfConverter.cpp (v0.64.0):
  - 16-byte header: magic "cmpt", version 1, byteLength, tilesLength.
  - Each inner tile is concatenated verbatim; every inner tile carries
    its own 12-byte header (magic, version, byteLength).
  - Each inner tile is dispatched recursively through GltfConverters, so
    b3dm / i3dm / pnts / glb payloads all work; the resulting Models are
    merged with Model::merge into ONE CesiumGltf::Model.
  - A corrupt cmpt (bad magic, truncated, inner tile overruns byteLength)
    yields converter warnings and an empty/failed result — the SDK must
    skip the tile gracefully (fault_test.py cases J/K/L).

Fixture generated (deterministic — fixed layouts, no RNG):

  p9_cmpt_tileset/
    tileset.json    - root -> composite.cmpt, bounding box covers all parts
    composite.cmpt  - 3 inner tiles:
      1. b3dm: 4 m orange box at the origin (P5-style payload)
      2. pnts: 64 green points in a 2.1 m box at (7, 0.5, 0)
               (P8-style payload; each point renders as exactly 1 px)
      3. i3dm: 6 teal instances (2 rows x 3) around (-10, 0, 0),
               scale 1.3 (P7-style payload; cesium-native recenters
               positions around their mean into the node translation,
               so absolute positions work)

Deterministic: re-running produces byte-identical output.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_p3_tileset as p3  # noqa: E402
import gen_p5_tilesets as p5  # noqa: E402
import gen_p7_i3dm_tileset as p7  # noqa: E402
import gen_p8_pnts_tileset as p8  # noqa: E402

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
CMPT_DIR = os.path.join(DATA_DIR, "p9_cmpt_tileset")

ORANGE = (1.0, 0.45, 0.10)
TEAL = (0.10, 0.75, 0.75)


def make_cmpt(path, inner_tiles):
    """Concatenate inner tile binaries under a 16-byte cmpt header.

    Header (verified against CmptToGltfConverter.cpp): magic "cmpt",
    version 1, byteLength (whole file), tilesLength (inner tile count).
    """
    assert len(inner_tiles) > 0
    body = b"".join(inner_tiles)
    total = 16 + len(body)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIII", b"cmpt", 1, total, len(inner_tiles)))
        f.write(body)
    print("wrote %s (%d bytes, %d inner tiles)"
          % (path, total, len(inner_tiles)))


def box_volume(cx, cy, cz, hx, hy, hz):
    return {"box": [cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz]}


def mem_b3dm(glb_bytes, batch_length=1):
    """Same wrapping as gen_p5_tilesets.make_b3dm, but in memory."""
    tmp = os.path.join(DATA_DIR, ".tmp_p9.b3dm")
    p5.make_b3dm(tmp, glb_bytes, batch_length=batch_length)
    with open(tmp, "rb") as f:
        data = f.read()
    os.remove(tmp)
    return data


def mem_pnts(positions, colors):
    tmp = os.path.join(DATA_DIR, ".tmp_p9.pnts")
    p8.make_pnts(tmp, positions, colors)
    with open(tmp, "rb") as f:
        data = f.read()
    os.remove(tmp)
    return data


def mem_i3dm(glb_bytes, positions, scales=None):
    tmp = os.path.join(DATA_DIR, ".tmp_p9.i3dm")
    p7.make_i3dm(tmp, glb_bytes, positions, scales=scales)
    with open(tmp, "rb") as f:
        data = f.read()
    os.remove(tmp)
    return data


def gen_main():
    os.makedirs(CMPT_DIR, exist_ok=True)

    # Inner tile 1: b3dm — 4 m orange box at the origin.
    orange_glb = p5.glb_bytes_for((0, 0, 0), 2.0, ORANGE)
    b3dm = mem_b3dm(orange_glb, batch_length=1)

    # Inner tile 2: pnts — 64 green points in a 4x4x4 grid (2.4 m box)
    # centered at (7, 0.5, 0). Must sit inside the demo's fixed
    # orbit-camera frame (yaw 30, pitch 18, distance 20): x=10 put the
    # cluster half out of frame, x=7 keeps all 64 points visible.
    positions, colors = [], []
    for ix in range(4):
        for iy in range(4):
            for iz in range(4):
                positions.append((7.0 - 1.05 + 0.7 * ix,
                                  0.5 - 1.05 + 0.7 * iy,
                                  -1.05 + 0.7 * iz))
                colors.append((0, 255, 0))
    pnts = mem_pnts(positions, colors)

    # Inner tile 3: i3dm — 6 teal instances around (-10, 0, 0).
    teal_glb = p5.glb_bytes_for((0, 0, 0), 1.0, TEAL)
    i_positions = [(-12.0 + 4.0 * col, 0.0, -2.0 + 4.0 * row)
                   for row in range(2) for col in range(3)]
    i3dm = mem_i3dm(teal_glb, i_positions, scales=[1.3] * 6)

    make_cmpt(os.path.join(CMPT_DIR, "composite.cmpt"), [b3dm, pnts, i3dm])

    # Root bounding volume covers x in [-14, 14] (i3dm mean at -10, pnts
    # at 10, box at 0), y/z generous for the orbit camera.
    root = {
        "asset": {"version": "1.0"},
        "geometricError": 0.0,
        "root": {
            "boundingVolume": box_volume(0, 0, 0, 14, 5, 5),
            "geometricError": 0.0,
            "content": {"uri": "composite.cmpt"},
        },
    }
    path = os.path.join(CMPT_DIR, "tileset.json")
    with open(path, "w") as f:
        json.dump(root, f, indent=2)
        f.write("\n")
    print("wrote %s" % path)


def main():
    gen_main()


if __name__ == "__main__":
    sys.exit(main())
