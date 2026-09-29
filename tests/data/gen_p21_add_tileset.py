#!/usr/bin/env python3
"""Generate the tiny explicit ADD-refine tileset for P21 tests
(tests/data/p21_add_tileset/).

Same geometry/SSE math as gen_p20_lod_tileset.py (P20), but refine=ADD:
  root.glb      - 100x100x10 m gray box at origin, geometricError=20
  child_0..3    - 50x50x10 m colored boxes in the 4 quadrants, ge=0

ADD semantics under test: when the camera is close enough that the root's
screen-space error exceeds cesium-native's maximumScreenSpaceError (16),
the traversal selects the 4 children IN ADDITION TO the root (the root
stays rendered — the exact opposite of P20's REPLACE fixture).

Reuses make_glb_box/box_volume from gen_p20_lod_tileset.py (imported, not
copied). Deterministic: re-running produces byte-identical output.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_p20_lod_tileset import box_volume, make_glb_box

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "p21_add_tileset")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    gray = (0.55, 0.55, 0.55)
    colors = [(0.9, 0.15, 0.12), (0.12, 0.7, 0.2),
              (0.15, 0.3, 0.9), (0.9, 0.8, 0.15)]
    quads = [(-25, 25), (25, 25), (-25, -25), (25, -25)]

    make_glb_box(os.path.join(OUT_DIR, "root.glb"), (0, 0, 0),
                 (50, 50, 5), gray)

    children = []
    for i, ((qx, qy), color) in enumerate(zip(quads, colors)):
        name = "child_%d.glb" % i
        make_glb_box(os.path.join(OUT_DIR, name), (qx, qy, 0),
                     (25, 25, 5), color)
        children.append({
            "boundingVolume": {"box": box_volume(qx, qy, 0, 25, 25, 5)},
            "geometricError": 0,
            "content": {"uri": name},
        })

    tileset = {
        "asset": {"version": "1.0"},
        "geometricError": 100.0,
        "root": {
            "boundingVolume": {"box": box_volume(0, 0, 0, 50, 50, 5)},
            "geometricError": 20,
            "refine": "ADD",
            "content": {"uri": "root.glb"},
            "children": children,
        },
    }
    with open(os.path.join(OUT_DIR, "tileset.json"), "w") as f:
        json.dump(tileset, f, indent=2)
    print("wrote tileset.json")


if __name__ == "__main__":
    main()
