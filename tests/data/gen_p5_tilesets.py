#!/usr/bin/env python3
"""Generate the P5 test tilesets (tests/data/p5_b3dm_tileset/,
tests/data/p5_rebase_tileset/{near,far}/).

Reuses the deterministic box GLB builder from gen_p3_tileset.py.

1. p5_b3dm_tileset/ — b3dm content support (P5 gap 2):
     tileset.json      - 3D Tiles 1.0, local coordinates, meters
     root.glb          - 10 m light-gray box at the origin (plain glb)
     child_a.b3dm      - 4 m orange box at (8, 0, 2), wrapped as b3dm
                         (BATCH_LENGTH=1, no batch table)
     child_b.glb       - 4 m teal box at (8, 0, -2) (plain glb)
   Layout mirrors p3_box_tileset so tests/tileset_test.py can be reused
   unchanged (it asserts orange + teal pixels are both visible).

2. p5_rebase_tileset/near|far/ — ECEF->ENU style rebase (P5 gap 3):
   Two tilesets with byte-identical local geometry; `far` adds a huge
   root-tile translation (123456789.0 m, float32-hostile: the nearest
   float32 is 123456792.0, i.e. a ~3 m error and the tile would render
   ~1.2e8 m off-screen without rebasing).
     tileset.json      - root: 10 m gray box; child: 4 m orange box at (8,0,2)
     root.glb / child.glb

Deterministic: re-running produces byte-identical output.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_p3_tileset as p3  # noqa: E402

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
B3DM_DIR = os.path.join(DATA_DIR, "p5_b3dm_tileset")
REBASE_DIR = os.path.join(DATA_DIR, "p5_rebase_tileset")

GRAY = (0.75, 0.75, 0.78)
ORANGE = (1.0, 0.45, 0.10)
TEAL = (0.10, 0.75, 0.75)


def make_b3dm(path, glb_bytes, batch_length=1):
    """Wrap glb bytes in a minimal b3dm container (version 1).

    Header: magic, version, byteLength, featureTableJSONByteLength,
    featureTableBinaryByteLength, batchTableJSONByteLength,
    batchTableBinaryByteLength. Feature table JSON carries only
    BATCH_LENGTH; no feature/binary or batch tables.
    """
    ft_json = ('{"BATCH_LENGTH":%d}' % batch_length).encode("utf-8")
    ft_json += b" " * ((4 - len(ft_json) % 4) % 4)
    header_len = 28
    total = header_len + len(ft_json) + len(glb_bytes)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIIIIII", b"b3dm", 1, total,
                            len(ft_json), 0, 0, 0))
        f.write(ft_json)
        f.write(glb_bytes)
    print("wrote %s (%d bytes)" % (path, total))


def glb_bytes_for(center, half, color):
    """Build the same box GLB as gen_p3_tileset.make_glb, in memory."""
    import io
    # Reuse make_glb by writing to a temp path, then read back. (Keeps one
    # canonical GLB builder.)
    tmp = os.path.join(DATA_DIR, ".tmp_p5.glb")
    p3.make_glb(tmp, center, half, color)
    with open(tmp, "rb") as f:
        data = f.read()
    os.remove(tmp)
    return data


def write_tileset(path, root):
    with open(path, "w") as f:
        json.dump(root, f, indent=2)
        f.write("\n")
    print("wrote %s" % path)


def gen_b3dm_tileset():
    os.makedirs(B3DM_DIR, exist_ok=True)
    p3.make_glb(os.path.join(B3DM_DIR, "root.glb"), (0, 0, 0), 5.0, GRAY)
    make_b3dm(os.path.join(B3DM_DIR, "child_a.b3dm"),
              glb_bytes_for((8, 0, 2), 2.0, ORANGE))
    p3.make_glb(os.path.join(B3DM_DIR, "child_b.glb"), (8, 0, -2), 2.0, TEAL)

    tileset = {
        "asset": {"version": "1.0"},
        "geometricError": 100.0,
        "root": {
            "boundingVolume": {"box": [0, 0, 0, 5, 0, 0, 0, 5, 0, 0, 0, 5]},
            "geometricError": 16.0,
            "refine": "ADD",
            "content": {"uri": "root.glb"},
            "children": [
                {
                    "boundingVolume": {"box": [8, 0, 2, 2, 0, 0, 0, 2, 0, 0, 0, 2]},
                    "geometricError": 0.0,
                    "content": {"uri": "child_a.b3dm"},
                },
                {
                    "boundingVolume": {"box": [8, 0, -2, 2, 0, 0, 0, 2, 0, 0, 0, 2]},
                    "geometricError": 0.0,
                    "content": {"uri": "child_b.glb"},
                },
            ],
        },
    }
    write_tileset(os.path.join(B3DM_DIR, "tileset.json"), tileset)


def gen_rebase_tilesets():
    # 1.2e8 m offset: float32(123456789.0) == 123456792.0, so without the
    # P5 rebase the tile would be rendered ~1.2e8 m off-screen (invisible).
    OFFSET = 123456789.0
    for name, offset in (("near", 0.0), ("far", OFFSET)):
        d = os.path.join(REBASE_DIR, name)
        os.makedirs(d, exist_ok=True)
        p3.make_glb(os.path.join(d, "root.glb"), (0, 0, 0), 5.0, GRAY)
        p3.make_glb(os.path.join(d, "child.glb"), (8, 0, 2), 2.0, ORANGE)
        root = {
            "boundingVolume": {"box": [0, 0, 0, 5, 0, 0, 0, 5, 0, 0, 0, 5]},
            "geometricError": 16.0,
            "refine": "ADD",
            "content": {"uri": "root.glb"},
            "children": [
                {
                    "boundingVolume": {"box": [8, 0, 2, 2, 0, 0, 0, 2, 0, 0, 0, 2]},
                    "geometricError": 0.0,
                    "content": {"uri": "child.glb"},
                },
            ],
        }
        if offset != 0.0:
            # Column-major 4x4 with translation (offset, 0, 0).
            root["transform"] = [1, 0, 0, 0,
                                 0, 1, 0, 0,
                                 0, 0, 1, 0,
                                 offset, 0, 0, 1]
        tileset = {"asset": {"version": "1.0"},
                   "geometricError": 100.0,
                   "root": root}
        write_tileset(os.path.join(d, "tileset.json"), tileset)


def main():
    gen_b3dm_tileset()
    gen_rebase_tilesets()


if __name__ == "__main__":
    sys.exit(main())
