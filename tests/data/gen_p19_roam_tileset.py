#!/usr/bin/env python3
"""Generate the P19 camera-roam memory-boundedness fixture
(tests/data/p19_roam_tileset/).

Implicit QUADTREE, levels 0-2 (1 + 4 + 16 = 21 tiles), root box 64x64x8
centered at the origin. Every tile carries a content glb: a subdivided box
(~135KB of vertex/index data) with a deterministic per-tile color derived
from (level, x, y) — no RNG anywhere, re-running is byte-identical.

Why big-ish tiles: P19 must prove tile *eviction* works, not just that a
tiny tileset fits in memory. With ~135KB per tile and the test's
--cache-budget of 400KB, only ~3 tiles can stay resident: a camera sweep
across the 4x4 level-2 grid forces constant evict/reload churn, and
tileStats().tilesLoaded must stay bounded far below 21.

Subtree is hand-written plain JSON (no "subt" magic -> parsed by the JSON
path, same recipe as P10): tileAvailability/contentAvailability constant
1, childSubtreeAvailability constant 0, subtreeLevels 3 / availableLevels 3
so one subtree file covers all three levels.
"""
import json
import os
import struct
import sys

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(DATA_DIR, "p19_roam_tileset")

GRID = 24  # subdivisions per box-face edge -> ~135KB glb per tile


def subdivided_box(center, hx, hy, hz, n):
    """Per-face grids (flat normals, no shared verts), deterministic."""
    cx, cy, cz = center
    positions, normals, indices = [], [], []
    base = 0
    # (origin, u-axis, v-axis, normal) per face
    faces = [
        ((hx, -hy, -hz), (0.0, 2 * hy, 0.0), (0.0, 0.0, 2 * hz), (1, 0, 0)),
        ((-hx, -hy, -hz), (0.0, 0.0, 2 * hz), (0.0, 2 * hy, 0.0), (-1, 0, 0)),
        ((-hx, hy, -hz), (2 * hx, 0.0, 0.0), (0.0, 0.0, 2 * hz), (0, 1, 0)),
        ((-hx, -hy, -hz), (0.0, 0.0, 2 * hz), (2 * hx, 0.0, 0.0), (0, -1, 0)),
        ((-hx, -hy, hz), (2 * hx, 0.0, 0.0), (0.0, 2 * hy, 0.0), (0, 0, 1)),
        ((-hx, -hy, -hz), (0.0, 2 * hy, 0.0), (2 * hx, 0.0, 0.0), (0, 0, -1)),
    ]
    for origin, du, dv, normal in faces:
        ox, oy, oz = origin
        for j in range(n + 1):
            fj = j / n
            for i in range(n + 1):
                fi = i / n
                positions.append(cx + ox + du[0] * fi + dv[0] * fj)
                positions.append(cy + oy + du[1] * fi + dv[1] * fj)
                positions.append(cz + oz + du[2] * fi + dv[2] * fj)
                normals.extend(normal)
        for j in range(n):
            for i in range(n):
                a = base + j * (n + 1) + i
                b = a + 1
                c = a + (n + 1)
                d = c + 1
                indices.extend([a, c, b, b, c, d])
        base += (n + 1) * (n + 1)
    return positions, normals, indices


def make_glb(path, center, hx, hy, hz, color):
    positions, normals, indices = subdivided_box(center, hx, hy, hz, GRID)
    pos_bin = struct.pack("<%df" % len(positions), *positions)
    nrm_bin = struct.pack("<%df" % len(normals), *normals)
    idx_bin = struct.pack("<%dH" % len(indices), *indices)
    blob = pos_bin + nrm_bin + idx_bin

    xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
    nverts = len(positions) // 3
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_p19_roam_tileset.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "tile"}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "indices": 2,
                "material": 0,
            }]
        }],
        "materials": [{
            "name": "tile",
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": [color[0], color[1], color[2], 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": nverts,
             "type": "VEC3",
             "min": [min(xs), min(ys), min(zs)],
             "max": [max(xs), max(ys), max(zs)]},
            {"bufferView": 1, "componentType": 5126, "count": nverts,
             "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123,
             "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bin),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bin),
             "byteLength": len(nrm_bin), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bin) + len(nrm_bin),
             "byteLength": len(idx_bin), "target": 34963},
        ],
        "buffers": [{"byteLength": len(blob)}],
    }
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    total = 12 + 8 + len(json_bytes) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(blob), 0x004E4942))
        f.write(blob)
    return total


def tile_color(level, x, y):
    # Deterministic per-tile color, no RNG.
    r = ((level * 67 + x * 131 + 41) % 200 + 30) / 255.0
    g = ((level * 37 + y * 149 + 97) % 200 + 30) / 255.0
    b = ((x * 53 + y * 79 + 151) % 200 + 30) / 255.0
    return (r, g, b)


def box_volume(cx, cy, cz, hx, hy, hz):
    return {"box": [cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz]}


def main():
    tiles_dir = os.path.join(OUT_DIR, "tiles")
    sub_dir = os.path.join(OUT_DIR, "subtrees")
    os.makedirs(tiles_dir, exist_ok=True)
    os.makedirs(sub_dir, exist_ok=True)

    total_bytes = 0
    count = 0
    # Level 0: whole 64x64 root slab.
    total_bytes += make_glb(os.path.join(tiles_dir, "0_0_0.glb"),
                            (0, 0, 0), 32, 32, 4, tile_color(0, 0, 0))
    count += 1
    # Level 1: 4 quadrants, 32x32, centers (+/-16, +/-16).
    for x in range(2):
        for y in range(2):
            cx = -16.0 + x * 32.0
            cy = -16.0 + y * 32.0
            total_bytes += make_glb(
                os.path.join(tiles_dir, "1_%d_%d.glb" % (x, y)),
                (cx, cy, 0), 16, 16, 3, tile_color(1, x, y))
            count += 1
    # Level 2: 16 tiles, 16x16.
    for x in range(4):
        for y in range(4):
            cx = -24.0 + x * 16.0
            cy = -24.0 + y * 16.0
            total_bytes += make_glb(
                os.path.join(tiles_dir, "2_%d_%d.glb" % (x, y)),
                (cx, cy, 0), 8, 8, 2, tile_color(2, x, y))
            count += 1

    subtree = {
        "tileAvailability": {"constant": 1},
        "contentAvailability": [{"constant": 1}],
        "childSubtreeAvailability": {"constant": 0},
    }
    with open(os.path.join(sub_dir, "0_0_0.subtree"), "w") as f:
        json.dump(subtree, f, indent=2)
        f.write("\n")

    root = {
        "asset": {"version": "1.1"},
        "extensionsUsed": ["3DTILES_implicit_tiling"],
        "extensionsRequired": ["3DTILES_implicit_tiling"],
        "geometricError": 400.0,
        "root": {
            "boundingVolume": box_volume(0, 0, 0, 32, 32, 4),
            "geometricError": 100.0,
            "refine": "ADD",
            "content": {"uri": "tiles/{level}_{x}_{y}.glb"},
            "extensions": {
                "3DTILES_implicit_tiling": {
                    "subdivisionScheme": "QUADTREE",
                    "subtreeLevels": 3,
                    "availableLevels": 3,
                    "subtrees": {
                        "uri": "subtrees/{level}_{x}_{y}.subtree"
                    },
                }
            },
        },
    }
    with open(os.path.join(OUT_DIR, "tileset.json"), "w") as f:
        json.dump(root, f, indent=2)
        f.write("\n")
    print("wrote %d tiles, %d content bytes total" % (count, total_bytes))


if __name__ == "__main__":
    main()
