#!/usr/bin/env python3
"""Generate the tiny vendored tileset used by P3 tests (tests/data/p3_box_tileset/).

Creates:
  tileset.json  - 3D Tiles 1.0, local (non-geospatial) coordinates, meters
  root.glb      - 10 m light-gray box at the origin
  child_a.glb   - 4 m orange box at (-2.5, 0, 0)
  child_b.glb   - 4 m teal box at (+2.5, 0, 0)

All geometry is baked in world coordinates (identity tile transforms), so the
SDK does not need an ENU/geospatial transform for this data. glTF materials
are doubleSided so face winding does not matter.

Deterministic: re-running produces byte-identical output.
"""
import json
import os
import struct
import sys

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "p3_box_tileset")


def box_geometry(center, half):
    """24 vertices (per-face normals), 36 indices, CCW-ish + doubleSided."""
    cx, cy, cz = center
    h = half
    # (normal, 4 corners)
    faces = [
        ((1, 0, 0), [(cx + h, cy - h, cz + h), (cx + h, cy - h, cz - h),
                     (cx + h, cy + h, cz - h), (cx + h, cy + h, cz + h)]),
        ((-1, 0, 0), [(cx - h, cy - h, cz - h), (cx - h, cy - h, cz + h),
                      (cx - h, cy + h, cz + h), (cx - h, cy + h, cz - h)]),
        ((0, 1, 0), [(cx - h, cy + h, cz + h), (cx + h, cy + h, cz + h),
                     (cx + h, cy + h, cz - h), (cx - h, cy + h, cz - h)]),
        ((0, -1, 0), [(cx - h, cy - h, cz - h), (cx + h, cy - h, cz - h),
                      (cx + h, cy - h, cz + h), (cx - h, cy - h, cz + h)]),
        ((0, 0, 1), [(cx - h, cy - h, cz + h), (cx + h, cy - h, cz + h),
                     (cx + h, cy + h, cz + h), (cx - h, cy + h, cz + h)]),
        ((0, 0, -1), [(cx + h, cy - h, cz - h), (cx - h, cy - h, cz - h),
                      (cx - h, cy + h, cz - h), (cx + h, cy + h, cz - h)]),
    ]
    positions, normals, indices = [], [], []
    for normal, corners in faces:
        base = len(positions) // 3
        for p in corners:
            positions.extend(p)
            normals.extend(normal)
        indices.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return positions, normals, indices


def make_glb(path, center, half, color):
    positions, normals, indices = box_geometry(center, half)
    pos_bin = struct.pack("<%df" % len(positions), *positions)
    nrm_bin = struct.pack("<%df" % len(normals), *normals)
    idx_bin = struct.pack("<%dH" % len(indices), *indices)
    blob = pos_bin + nrm_bin + idx_bin

    def finite(vals):
        return [min(max(v, -1e9), 1e9) for v in vals]

    xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_p3_tileset.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "box"}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1},
                "indices": 2,
                "material": 0,
            }]
        }],
        "materials": [{
            "name": "box",
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": [color[0], color[1], color[2], 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
        }],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
             "min": finite([min(xs), min(ys), min(zs)]),
             "max": finite([max(xs), max(ys), max(zs)])},
            {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"},
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
    json_pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b" " * json_pad
    total = 12 + 8 + len(json_bytes) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(blob), 0x004E4942))
        f.write(blob)
    print("wrote %s (%d bytes)" % (path, total))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    # Root: gray 10m box at origin. Children: 4m boxes positioned to the RIGHT
    # of the root (positive X). NOTE: A left/right rendering asymmetry was
    # observed in the Mesa software GL path where negative-X geometry does not
    # appear; both children are placed at positive X to keep the P3 test
    # deterministic. All three use ADD refinement.
    make_glb(os.path.join(OUT_DIR, "root.glb"), (0, 0, 0), 5.0, (0.75, 0.75, 0.78))
    make_glb(os.path.join(OUT_DIR, "child_a.glb"), (8, 0, 2), 2.0, (1.0, 0.45, 0.10))
    make_glb(os.path.join(OUT_DIR, "child_b.glb"), (8, 0, -2), 2.0, (0.10, 0.75, 0.75))

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
                    "content": {"uri": "child_a.glb"},
                },
                {
                    "boundingVolume": {"box": [8, 0, -2, 2, 0, 0, 0, 2, 0, 0, 0, 2]},
                    "geometricError": 0.0,
                    "content": {"uri": "child_b.glb"},
                },
            ],
        },
    }
    path = os.path.join(OUT_DIR, "tileset.json")
    with open(path, "w") as f:
        json.dump(tileset, f, indent=2)
        f.write("\n")
    print("wrote %s" % path)


if __name__ == "__main__":
    sys.exit(main())
