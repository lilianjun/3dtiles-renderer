#!/usr/bin/env python3
"""Generate the frustum-culling fixture for P20 (tests/data/p20_frustum_tileset/).

A horizontal ground plane (in the camera's Y-up world): 512x512 m, 10 m thick,
tiled as an explicit 4x4 grid of 128x128 m children under a REPLACE root.

  root.glb       - 512x512x10 m gray slab, geometricError=200, refine=REPLACE
  tile_{i}_{j}.glb - 128x128x10 m colored slab, geometricError=0,
                     i,j in 0..3 (i -> +X, j -> +Z in tile-local coords)

Why a ground plane: the SDK orbit camera is Y-up and always looks at the
origin. A Z-up "wall" fixture (like p19_deep_tileset) shows the same tiles
from yaw=0 and yaw=180 (front face vs back face). A horizontal XZ ground
plane instead gives directional views: at low pitch the camera sees the far
half of the plane, and yaw=180 sees the opposite half, so the selected tile
sets are (nearly) disjoint — which is what proves frustum culling per-tile.

SSE math (screenHeight=600, vfov=45deg): root ge=200 at dist=200 gives
SSE=200*600/(2*200*tan(22.5deg))=145 > 16, so the traversal refines to the
16 children; children (ge=0) never refine further.

Deterministic: re-running produces byte-identical output.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_p20_lod_tileset import make_glb_box, box_volume

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__),
                                       ), "p20_frustum_tileset")

GRID = 4
SIZE = 512.0
TILE = SIZE / GRID  # 128


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    make_glb_box(os.path.join(OUT_DIR, "root.glb"), (0, 0, 0),
                 (SIZE / 2, 5, SIZE / 2), (0.5, 0.5, 0.52))

    children = []
    for i in range(GRID):
        for j in range(GRID):
            # i -> X, j -> Z (tile-local Z-up box coords; the camera's Y-up
            # world maps tile X->X, tile Y->Y(up), tile Z->Z(depth)).
            cx = -SIZE / 2 + TILE / 2 + i * TILE
            cz = -SIZE / 2 + TILE / 2 + j * TILE
            # Color-code by quadrant for visual debugging.
            r = 0.2 + 0.6 * (i / (GRID - 1))
            g = 0.2 + 0.6 * (j / (GRID - 1))
            b = 0.5
            name = "tile_%d_%d.glb" % (i, j)
            make_glb_box(os.path.join(OUT_DIR, name), (cx, 0, cz),
                         (TILE / 2, 5, TILE / 2), (r, g, b))
            children.append({
                "boundingVolume": {"box": box_volume(cx, 0, cz,
                                                     TILE / 2, 5, TILE / 2)},
                "geometricError": 0,
                "content": {"uri": name},
            })

    tileset = {
        "asset": {"version": "1.0"},
        "geometricError": 400.0,
        "root": {
            # Box axes: X=512 (width), Y=10 (thin, up in camera Y-up world),
            # Z=512 (depth). A horizontal ground plane for a Y-up camera.
            "boundingVolume": {"box": box_volume(0, 0, 0,
                                                 SIZE / 2, 5, SIZE / 2)},
            "geometricError": 200,
            "refine": "REPLACE",
            "content": {"uri": "root.glb"},
            "children": children,
        },
    }
    with open(os.path.join(OUT_DIR, "tileset.json"), "w") as f:
        json.dump(tileset, f, indent=2)
    print("wrote tileset.json (%d children)" % len(children))


if __name__ == "__main__":
    main()
