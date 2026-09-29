#!/usr/bin/env python3
"""Generate the tiny explicit REPLACE-refine tileset for P20 LOD tests
(tests/data/p20_lod_tileset/).

Layout (local meters, Z-up, identity tile transforms):
  root.glb      - 100x100x10 m gray box at origin, geometricError=20
  child_0.glb   - 50x50x10 m red    box at (-25,+25,0), geometricError=0
  child_1.glb   - 50x50x10 m green  box at (+25,+25,0), geometricError=0
  child_2.glb   - 50x50x10 m blue   box at (-25,-25,0), geometricError=0
  child_3.glb   - 50x50x10 m yellow box at (+25,-25,0), geometricError=0

refine=REPLACE on the root: when the camera is close enough that the root's
screen-space error exceeds cesium-native's default maximumScreenSpaceError
(16), the traversal selects the 4 children INSTEAD of the root (not in
addition — that is the REPLACE semantic under test).

SSE math (screenHeight=600, vfov=45deg, as in the demo):
  SSE = geometricError * 600 / (2 * dist * tan(22.5deg))
  root (ge=20): dist=1200 -> SSE=12.1 < 16 (root only)
                dist=300  -> SSE=48.2 > 16 (children replace root)
  children (ge=0): never refine further.

Deterministic: re-running produces byte-identical output.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_p3_tileset import box_geometry

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__),
                                       ), "p20_lod_tileset")


def box_volume(cx, cy, cz, hx, hy, hz):
    # 3D Tiles box: [cx,cy,cz, hx,0,0, 0,hy,0, 0,0,hz]
    return [cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz]


def make_glb_box(path, center, half_xyz, color):
    """Axis-aligned box GLB. half_xyz=(hx,hy,hz); per-face normals stay
    axis-aligned under axis-aligned scaling, so box_geometry's normals are
    reused as-is."""
    positions, normals, indices = box_geometry((0, 0, 0), 1.0)
    hx, hy, hz = half_xyz
    cx, cy, cz = center
    scaled = []
    for i, v in enumerate(positions):
        if i % 3 == 0:
            scaled.append(cx + v * hx)
        elif i % 3 == 1:
            scaled.append(cy + v * hy)
        else:
            scaled.append(cz + v * hz)
    positions = scaled
    pos_bin = struct.pack("<%df" % len(positions), *positions)
    nrm_bin = struct.pack("<%df" % len(normals), *normals)
    idx_bin = struct.pack("<%dH" % len(indices), *indices)
    blob = pos_bin + nrm_bin + idx_bin
    xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_p20_lod_tileset.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "box"}],
        "meshes": [{"primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1},
            "indices": 2, "material": 0}]}],
        "materials": [{
            "name": "box", "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": [color[0], color[1], color[2], 1.0],
                "metallicFactor": 0.0, "roughnessFactor": 0.9}}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 24,
             "type": "VEC3",
             "min": [min(xs), min(ys), min(zs)],
             "max": [max(xs), max(ys), max(zs)]},
            {"bufferView": 1, "componentType": 5126, "count": 24,
             "type": "VEC3"},
            {"bufferView": 2, "componentType": 5123, "count": 36,
             "type": "SCALAR"}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bin),
             "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bin),
             "byteLength": len(nrm_bin), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos_bin) + len(nrm_bin),
             "byteLength": len(idx_bin), "target": 34963}],
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
    print("wrote %s (%d bytes)" % (path, total))


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
            "refine": "REPLACE",
            "content": {"uri": "root.glb"},
            "children": children,
        },
    }
    with open(os.path.join(OUT_DIR, "tileset.json"), "w") as f:
        json.dump(tileset, f, indent=2)
    print("wrote tileset.json")


if __name__ == "__main__":
    main()
