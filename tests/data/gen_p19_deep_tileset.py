#!/usr/bin/env python3
"""Generate the P19-deep camera-roam memory-boundedness fixture
(tests/data/p19_deep_tileset/).

Implicit QUADTREE, levels 0-4 (1 + 4 + 16 + 64 + 256 = 341 tiles), root box
128x128x8 centered at the origin. Every tile carries a content glb: a
subdivided box (~34KB) with a deterministic per-tile color derived from
(level, x, y) — no RNG anywhere, re-running is byte-identical.

Why deep: cesium-native v0.64.0's TreeTraversalState holds intrusive
Tile::Pointer references to every tile passed to beginNode() — which runs
BEFORE frustum culling in visitTileIfNeeded. In a shallow/wide tree (like
p19_roam_tileset, 3 levels) the traversal touches every tile every frame, so
no tile ever becomes eligible for content unloading and maximumCachedBytes
is silently ineffective (documented in docs/adr/0017). A 5-level tree lets
whole subtrees fall out of the traversal when their parent is culled, so
those tiles DO become eligible and the LRU cache can evict them — this is
the fixture that actually exercises eviction.

Subtree is hand-written plain JSON (same recipe as P10/P19-shallow):
tileAvailability/contentAvailability constant 1, childSubtreeAvailability
constant 0, subtreeLevels 5 / availableLevels 5.
"""
import json
import os
import struct
import sys

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(DATA_DIR, "p19_deep_tileset")

GRID = 12  # subdivisions per box-face edge -> ~34KB glb per tile
LEVELS = 5  # 0..4
ROOT_HALF = 64.0  # root is 128x128
ROOT_HZ = 4.0

# Per-level geometric errors: tuned so a camera at distance ~14 refines all
# the way to level 4 (SSE > 16 at each level).
GEOMETRIC_ERRORS = [400.0, 150.0, 50.0, 15.0, 0.0]


def subdivided_box(center, hx, hy, hz, n):
    """Build a subdivided box mesh. Returns (positions, normals, indices)."""
    cx, cy, cz = center
    positions = []
    normals = []
    indices = []

    def grid_face(origin, u_axis, v_axis, normal, nu, nv):
        base = len(positions)
        for i in range(nu + 1):
            for j in range(nv + 1):
                fu = -1.0 + 2.0 * i / nu
                fv = -1.0 + 2.0 * j / nv
                p = [origin[k] + fu * u_axis[k] + fv * v_axis[k]
                     for k in range(3)]
                positions.append(p)
                normals.append(list(normal))
        for i in range(nu):
            for j in range(nv):
                a = base + i * (nv + 1) + j
                b = base + (i + 1) * (nv + 1) + j
                c = base + (i + 1) * (nv + 1) + j + 1
                d = base + i * (nv + 1) + j + 1
                indices.extend([a, b, c, a, c, d])

    # +X / -X faces
    grid_face([cx + hx, cy, cz], [0, hy, 0], [0, 0, hz], [1, 0, 0], n, n)
    grid_face([cx - hx, cy, cz], [0, hy, 0], [0, 0, hz], [-1, 0, 0], n, n)
    # +Y / -Y faces
    grid_face([cx, cy + hy, cz], [hx, 0, 0], [0, 0, hz], [0, 1, 0], n, n)
    grid_face([cx, cy - hy, cz], [hx, 0, 0], [0, 0, hz], [0, -1, 0], n, n)
    # +Z / -Z faces
    grid_face([cx, cy, cz + hz], [hx, 0, 0], [0, hy, 0], [0, 0, 1], n, n)
    grid_face([cx, cy, cz - hz], [hx, 0, 0], [0, hy, 0], [0, 0, -1], n, n)
    return positions, normals, indices


def make_glb(path, center, hx, hy, hz, color):
    positions, normals, indices = subdivided_box(center, hx, hy, hz, GRID)
    n_verts = len(positions)

    # Binary buffer: positions (float32), normals (float32), indices (uint32),
    # then a per-vertex color? No — use a material baseColorFactor instead.
    pos_bin = struct.pack("<%df" % (3 * n_verts),
                          *[c for p in positions for c in p])
    nrm_bin = struct.pack("<%df" % (3 * n_verts),
                          *[c for p in normals for c in p])
    idx_bin = struct.pack("<%dI" % len(indices), *indices)

    # Align each chunk to 4 bytes.
    def pad(b):
        return b + b"\x00" * ((4 - len(b) % 4) % 4)

    pos_bin, nrm_bin, idx_bin = pad(pos_bin), pad(nrm_bin), pad(idx_bin)
    bin_chunk = pos_bin + nrm_bin + idx_bin

    pos_off = 0
    nrm_off = len(pos_bin)
    idx_off = nrm_off + len(nrm_bin)

    gltf = {
        "asset": {"version": "2.0", "generator": "p19-deep-gen"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "indices": 2,
                "material": 0,
            }]
        }],
        "materials": [{
            "pbrMetallicRoughness": {
                "baseColorFactor": [color[0], color[1], color[2], 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            }
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": n_verts,
             "type": "VEC3", "max": [1, 1, 1], "min": [-1, -1, -1]},
            {"bufferView": 1, "componentType": 5126, "count": n_verts,
             "type": "VEC3"},
            {"bufferView": 2, "componentType": 5125,
             "count": len(indices), "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": pos_off,
             "byteLength": len(pos_bin), "target": 34962},
            {"buffer": 0, "byteOffset": nrm_off,
             "byteLength": len(nrm_bin), "target": 34962},
            {"buffer": 0, "byteOffset": idx_off,
             "byteLength": len(idx_bin), "target": 34963},
        ],
        "buffers": [{"byteLength": len(bin_chunk)}],
    }
    json_chunk = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_chunk += b" " * ((4 - len(json_chunk) % 4) % 4)

    total_len = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total_len))
        f.write(struct.pack("<II", len(json_chunk), 0x4E4F534A))
        f.write(json_chunk)
        f.write(struct.pack("<II", len(bin_chunk), 0x004E4942))
        f.write(bin_chunk)
    return os.path.getsize(path)


def tile_color(level, x, y):
    # Deterministic pseudo-color from (level, x, y); keep away from black.
    r = ((level * 67 + x * 131 + 41) % 200 + 55) / 255.0
    g = ((level * 89 + y * 137 + 97) % 200 + 55) / 255.0
    b = ((level * 53 + x * 71 + y * 79 + 149) % 200 + 55) / 255.0
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
    # Level L: 4^L tiles; tile (x, y) covers
    # [-ROOT_HALF + x*size, -ROOT_HALF + (x+1)*size] x same for y,
    # where size = 2*ROOT_HALF / 2^L.
    for level in range(LEVELS):
        n = 2 ** level
        size = 2 * ROOT_HALF / n
        half = size / 2.0
        hz = max(0.5, ROOT_HZ / (2 ** level))
        for x in range(n):
            for y in range(n):
                cx = -ROOT_HALF + (x + 0.5) * size
                cy = -ROOT_HALF + (y + 0.5) * size
                total_bytes += make_glb(
                    os.path.join(tiles_dir, "%d_%d_%d.glb" % (level, x, y)),
                    (cx, cy, 0), half, half, hz, tile_color(level, x, y))
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
        "geometricError": 800.0,
        "root": {
            "boundingVolume": box_volume(0, 0, 0, ROOT_HALF, ROOT_HALF,
                                         ROOT_HZ),
            "geometricError": GEOMETRIC_ERRORS[0],
            "refine": "ADD",
            "content": {"uri": "tiles/{level}_{x}_{y}.glb"},
            "extensions": {
                "3DTILES_implicit_tiling": {
                    "subdivisionScheme": "QUADTREE",
                    "subtreeLevels": LEVELS,
                    "availableLevels": LEVELS,
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

    print("wrote %d tiles, %d bytes total" % (count, total_bytes))


if __name__ == "__main__":
    sys.exit(main())
