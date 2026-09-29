#!/usr/bin/env python3
"""Generate the tiny region-bounding-volume tileset for P21 tests
(tests/data/p21_region_tileset/).

3D Tiles 1.0 `region` bounding volumes (geographic, radians) have never been
exercised by any fixture — every previous fixture used `box`. This one uses
real WGS84 coordinates:

  Location: lat=0, lon=0 (equator / prime meridian) — ECEF is exactly
            (6378137, 0, 0), easy to verify by hand.
  root      - region [-h,-h,+h,+h] (h = 0.0005 deg in radians, ~111 m wide),
              ge=20, refine=ADD, transform = translate(ECEF(0,0,0))
  child_0/1 - west / east halves of the root region, ge=0

Content placement: the GLB boxes are authored around the local origin; the
root tile's 4x4 `transform` (column-major translation by the ECEF center)
puts them at ECEF magnitude. The SDK's existing rebase path
(computeLocalOrigin: BoundingRegion -> OBB center, P5) subtracts ~the same
ECEF center, so the boxes render near (0,0,0) where the orbit camera looks.
No new SDK code, no invented coordinate math — pure spec constructs.

SSE math (screenHeight=600, vfov=45deg): root ge=20 refines below ~905 m,
so the p21_region trajectory (FAR 1200 m -> NEAR 300 m) exercises both.

Reuses make_glb_box from gen_p20_lod_tileset.py. Deterministic.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_p20_lod_tileset import make_glb_box

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "p21_region_tileset")

# WGS84.
_A = 6378137.0
_E2 = 0.00669437999014


def ecef(lat_deg, lon_deg, h):
    """Geodetic -> ECEF (meters), double precision."""
    lat = math.radians(lat_deg)
    lon = math.radians(lon_deg)
    n = _A / math.sqrt(1.0 - _E2 * math.sin(lat) ** 2)
    x = (n + h) * math.cos(lat) * math.cos(lon)
    y = (n + h) * math.cos(lat) * math.sin(lon)
    z = (n * (1.0 - _E2) + h) * math.sin(lat)
    return (x, y, z)


def translate_matrix(tx, ty, tz):
    # Column-major 4x4 translation, as 3D Tiles tile.transform expects.
    return [1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            tx, ty, tz, 1]


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    half_deg = 0.0005
    h = math.radians(half_deg)  # ~8.727e-6 rad
    cx, cy, cz = ecef(0.0, 0.0, 0.0)
    assert abs(cx - 6378137.0) < 1e-6 and cy == 0.0 and cz == 0.0, \
        (cx, cy, cz)
    print("ecef(0,0,0) = (%.3f, %.3f, %.3f)" % (cx, cy, cz))

    # Local boxes (meters); the root transform carries them to ECEF.
    make_glb_box(os.path.join(OUT_DIR, "root.glb"), (0, 0, 0),
                 (25, 25, 10), (0.55, 0.55, 0.55))
    make_glb_box(os.path.join(OUT_DIR, "child_0.glb"), (-12.5, 0, 0),
                 (12.5, 25, 10), (0.9, 0.15, 0.12))
    make_glb_box(os.path.join(OUT_DIR, "child_1.glb"), (12.5, 0, 0),
                 (12.5, 25, 10), (0.15, 0.3, 0.9))

    children = [
        {
            # West half of the root region.
            "boundingVolume": {"region": [-h, -h, 0.0, h, -10.0, 10.0]},
            "geometricError": 0,
            "content": {"uri": "child_0.glb"},
        },
        {
            # East half of the root region.
            "boundingVolume": {"region": [0.0, -h, h, h, -10.0, 10.0]},
            "geometricError": 0,
            "content": {"uri": "child_1.glb"},
        },
    ]
    tileset = {
        "asset": {"version": "1.0"},
        "geometricError": 100.0,
        "root": {
            "boundingVolume": {"region": [-h, -h, h, h, -10.0, 10.0]},
            "geometricError": 20,
            "refine": "ADD",
            "transform": translate_matrix(cx, cy, cz),
            "content": {"uri": "root.glb"},
            "children": children,
        },
    }
    with open(os.path.join(OUT_DIR, "tileset.json"), "w") as f:
        json.dump(tileset, f, indent=2)
    print("wrote tileset.json")


if __name__ == "__main__":
    main()
