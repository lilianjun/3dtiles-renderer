#!/usr/bin/env python3
"""Generate the P8 pnts (Point Cloud) test tilesets (tests/data/p8_pnts_*/).

pnts (3D Tiles 1.0) exercises the cesium-native PntsToGltfConverter ->
CesiumGltf::Model (POINTS primitive) -> GltfWriter::writeGlb -> gltfio path.

Field semantics were verified against
Cesium3DTilesContent/src/PntsToGltfConverter.cpp (v0.64.0):
  - 28-byte header: magic "pnts", version 1, byteLength,
    featureTableJSONByteLength, featureTableBinaryByteLength,
    batchTableJSONByteLength, batchTableBinaryByteLength.
  - Feature table JSON requires POINTS_LENGTH and POSITION {byteOffset};
    optional RGB/RGBA/RGB565/CONSTANT_RGBA, NORMAL/NORMAL_OCT16P,
    BATCH_ID, QUANTIZED variants, RTC_CENTER (plain [x, y, z] doubles).
  - The converter emits ONE node / ONE mesh / ONE primitive with
    mode = POINTS, POSITION (VEC3 float) + COLOR_0 (VEC3/VEC4 float,
    sRGB->linear converted) + optional NORMAL / _BATCHID, a PBR material
    (metallic 0, roughness 0.9), and KHR_materials_unlit when the cloud has
    no normals. Node matrix is Z_UP_TO_Y_UP (applied natively by gltfio;
    no SDK-side conjugation needed — unlike i3dm there is no per-instance
    transform for the converter to conjugate).
  - RTC_CENTER becomes a CESIUM_RTC extension, which the SDK's modelToGlb
    already extracts into a double-precision rtcCenter and strips (P5).
  - Multiple glTF buffers (position / color / ...) are merged into the
    single GLB BIN chunk by the P7 multi-buffer fix — pnts hits the same
    code path as i3dm here.
  - No batch table is generated (BATCH_ID / metadata not needed for the
    rendering test); batch table JSON/binary lengths are 0.

Tilesets generated (all deterministic — fixed layouts, no RNG):

1. p8_pnts_cloud/ — main rendering test:
     tileset.json - root -> cloud.pnts
     cloud.pnts   - 420 points: 200 red on a Fibonacci sphere (r=3),
                    120 green on a ring (r=4, XZ plane, y=2),
                    100 blue on a 10x10 XZ grid (y=-2.5).
     Bounding box centered at the origin, half extents (5,5,5).

2. p8_pnts_many/ — 60000 points in a 10x10x10 box (fixed-seed RNG),
     single teal color. Load/render correctness only, no perf claim.

3. p8_pnts_rebase/near|far/ — P5-style tile-transform rebase: byte-identical
     cloud.pnts; `far` adds a 123456789.0 m root-tile transform. The SDK
     rebase must reproduce the near image bit-identically.

4. p8_pnts_rtc/ — RTC_CENTER at ECEF magnitude
     ([1210000.0, -4736290.5, 4081600.0], |.| ~ 6.37e6 m) with the cloud
     centered at the origin. Bounding volume centered at RTC_CENTER.
     The SDK extracts RTC_CENTER in double precision and rebases around
     it, so rendering must equal the no-RTC reference bit-identically.

5. p8_pnts_rtc_ref/ — no-RTC reference for fixture 4: byte-identical
     cloud.pnts without RTC_CENTER, bounding volume at the origin.
     Both render M*p (M = Z_UP_TO_Y_UP node matrix applied by gltfio),
     so the screenshots must be bit-identical.

Deterministic: re-running produces byte-identical output.
"""
import json
import math
import os
import random
import struct

DATA_DIR = os.path.dirname(os.path.abspath(__file__))

CLOUD_DIR = os.path.join(DATA_DIR, "p8_pnts_cloud")
MANY_DIR = os.path.join(DATA_DIR, "p8_pnts_many")
REBASE_DIR = os.path.join(DATA_DIR, "p8_pnts_rebase")
RTC_DIR = os.path.join(DATA_DIR, "p8_pnts_rtc")
RTC_REF_DIR = os.path.join(DATA_DIR, "p8_pnts_rtc_ref")

# ECEF-magnitude center for the RTC test (~6.37e6 m from origin).
RTC_CENTER = [1210000.0, -4736290.5, 4081600.0]
# P5's float32-hostile offset, reused for the tile-transform rebase test.
REBASE_OFFSET = 123456789.0

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
TEAL = (0, 200, 200)


def make_pnts(path, positions, colors, rtc_center=None):
    """Write a minimal deterministic pnts file.

    positions: list of (x, y, z) float triples.
    colors: list of (r, g, b) uint8 triples, same length.
    rtc_center: [x, y, z] or None.
    """
    assert len(positions) == len(colors) and len(positions) > 0
    n = len(positions)
    ft_json = {
        "POINTS_LENGTH": n,
        "POSITION": {"byteOffset": 0},
        "RGB": {"byteOffset": n * 12},
    }
    if rtc_center is not None:
        ft_json["RTC_CENTER"] = list(rtc_center)
    ft_json_bytes = json.dumps(ft_json, separators=(",", ":")).encode("ascii")
    while len(ft_json_bytes) % 4 != 0:
        ft_json_bytes += b" "
    binary = b"".join(struct.pack("<3f", *p) for p in positions)
    binary += b"".join(struct.pack("<3B", *c) for c in colors)
    byte_length = 28 + len(ft_json_bytes) + len(binary)
    header = struct.pack("<4s6I", b"pnts", 1, byte_length,
                         len(ft_json_bytes), len(binary), 0, 0)
    with open(path, "wb") as f:
        f.write(header + ft_json_bytes + binary)


def box_volume(cx, cy, cz, hx, hy, hz):
    return {"box": [cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz]}


def write_tileset(path, root):
    tileset = {
        "asset": {"version": "1.0"},
        "geometricError": 200.0,
        "root": root,
    }
    with open(path, "w") as f:
        json.dump(tileset, f, indent=2)
        f.write("\n")


def fibonacci_sphere(n, radius):
    """Deterministic near-uniform points on a sphere (no RNG)."""
    pts = []
    golden = math.pi * (3.0 - math.sqrt(5.0))
    for i in range(n):
        y = 1.0 - (2.0 * i + 1.0) / n
        r = math.sqrt(max(0.0, 1.0 - y * y))
        theta = golden * i
        pts.append((radius * r * math.cos(theta),
                    radius * y,
                    radius * r * math.sin(theta)))
    return pts


def gen_cloud():
    """Main fixture: red sphere + green ring + blue grid, ~420 points."""
    os.makedirs(CLOUD_DIR, exist_ok=True)
    positions, colors = [], []
    for p in fibonacci_sphere(200, 3.0):
        positions.append(p)
        colors.append(RED)
    for i in range(120):
        a = 2.0 * math.pi * i / 120
        positions.append((4.0 * math.cos(a), 2.0, 4.0 * math.sin(a)))
        colors.append(GREEN)
    for ix in range(10):
        for iz in range(10):
            positions.append((-3.6 + 0.8 * ix, -2.5, -3.6 + 0.8 * iz))
            colors.append(BLUE)
    assert len(positions) == 420
    make_pnts(os.path.join(CLOUD_DIR, "cloud.pnts"), positions, colors)
    root = {
        "boundingVolume": box_volume(0, 0, 0, 5, 5, 5),
        "geometricError": 32.0,
        "refine": "ADD",
        "content": {"uri": "cloud.pnts"},
    }
    write_tileset(os.path.join(CLOUD_DIR, "tileset.json"), root)


def gen_many():
    """60000 points in a 10^3 box: load/render correctness only."""
    os.makedirs(MANY_DIR, exist_ok=True)
    rng = random.Random(1234)
    positions = [(rng.uniform(-5, 5), rng.uniform(-5, 5), rng.uniform(-5, 5))
                 for _ in range(60000)]
    colors = [TEAL] * 60000
    make_pnts(os.path.join(MANY_DIR, "cloud.pnts"), positions, colors)
    root = {
        "boundingVolume": box_volume(0, 0, 0, 5.5, 5.5, 5.5),
        "geometricError": 0.0,
        "content": {"uri": "cloud.pnts"},
    }
    write_tileset(os.path.join(MANY_DIR, "tileset.json"), root)


def gen_rebase():
    """Same pnts, near at origin / far translated by REBASE_OFFSET."""
    positions, colors = [], []
    for p in fibonacci_sphere(200, 3.0):
        positions.append(p)
        colors.append(RED)
    for name, offset in (("near", 0.0), ("far", REBASE_OFFSET)):
        d = os.path.join(REBASE_DIR, name)
        os.makedirs(d, exist_ok=True)
        make_pnts(os.path.join(d, "cloud.pnts"), positions, colors)
        # NOTE: boundingVolume is always in tile-LOCAL coordinates (the tile
        # transform maps it to world). So `far` keeps the same local volume
        # as `near`; only the root transform carries the offset. Adding the
        # offset to the volume too would double-translate it.
        root = {
            "boundingVolume": box_volume(0, 0, 0, 5, 5, 5),
            "geometricError": 0.0,
            "content": {"uri": "cloud.pnts"},
        }
        if offset != 0.0:
            root["transform"] = [1, 0, 0, 0,
                                 0, 1, 0, 0,
                                 0, 0, 1, 0,
                                 offset, 0, 0, 1]
        write_tileset(os.path.join(d, "tileset.json"), root)


def gen_rtc():
    """RTC_CENTER at ECEF magnitude; cloud centered at the origin."""
    os.makedirs(RTC_DIR, exist_ok=True)
    positions = fibonacci_sphere(200, 3.0)
    colors = [RED] * 200
    make_pnts(os.path.join(RTC_DIR, "cloud.pnts"), positions, colors,
              rtc_center=RTC_CENTER)
    # Bounding volume in world coordinates (center + RTC_CENTER).
    root = {
        "boundingVolume": box_volume(RTC_CENTER[0], RTC_CENTER[1],
                                     RTC_CENTER[2], 5, 5, 5),
        "geometricError": 0.0,
        "content": {"uri": "cloud.pnts"},
    }
    write_tileset(os.path.join(RTC_DIR, "tileset.json"), root)


def gen_rtc_ref():
    """No-RTC reference for the RTC_CENTER test.

    Byte-identical cloud.pnts minus RTC_CENTER, bounding volume at the
    origin. Both variants render M*p (M = the Z_UP_TO_Y_UP node matrix
    gltfio applies), so the screenshots must be bit-identical: the RTC
    variant computes fl32(T(rtc))*M*p - fl32(rtc), and every intermediate
    value (rtc components, rtc +/- small offsets) is exactly representable
    in float32, so the huge translation cancels exactly.
    """
    os.makedirs(RTC_REF_DIR, exist_ok=True)
    positions = fibonacci_sphere(200, 3.0)
    colors = [RED] * 200
    make_pnts(os.path.join(RTC_REF_DIR, "cloud.pnts"), positions, colors)
    root = {
        "boundingVolume": box_volume(0, 0, 0, 5, 5, 5),
        "geometricError": 0.0,
        "content": {"uri": "cloud.pnts"},
    }
    write_tileset(os.path.join(RTC_REF_DIR, "tileset.json"), root)


if __name__ == "__main__":
    gen_cloud()
    gen_many()
    gen_rebase()
    gen_rtc()
    gen_rtc_ref()
    print("generated p8 pnts fixtures under", DATA_DIR)
