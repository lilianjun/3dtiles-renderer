#!/usr/bin/env python3
"""Generate the Draco fixture used by P29 tests (tests/data/p29_draco_tileset/).

Creates:
  tileset.json        - 3D Tiles 1.0, single root, content = root.glb (Draco)
  tileset_plain.json  - same, but content = plain.glb (uncompressed twin)
  root.glb            - 10 m light-gray box at origin, mesh compressed with
                        google/draco (KHR_draco_mesh_compression)
  plain.glb           - byte-twin of root.glb except the mesh is uncompressed
  box.drc             - the raw draco bitstream (kept for the corrupt-input test)

The two glbs share geometry (tests/data/gen_p3_tileset.py box_geometry),
material (light-gray 0.75/0.75/0.78, metallic 0, roughness 0.9, doubleSided)
and node/scene layout; the ONLY difference is Draco compression of the mesh.
Deterministic given the same draco_encoder binary.

Requires draco_encoder: uses the copy built by cesium-native's ezvcpkg
(~/.ezvcpkg/<hash>/installed/x64-linux/tools/draco/draco_encoder),
or one found on PATH.
"""
import json
import os
import shutil
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_p3_tileset import box_geometry  # noqa: E402

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "p29_draco_tileset")

COLOR = (0.75, 0.75, 0.78)  # same light gray as p3 root


def find_encoder():
    p = shutil.which("draco_encoder")
    if p:
        return p
    ez = os.path.expanduser("~/.ezvcpkg")
    if os.path.isdir(ez):
        for h in os.listdir(ez):
            cand = os.path.join(
                ez, h, "installed", "x64-linux", "tools", "draco",
                "draco_encoder")
            if os.path.isfile(cand):
                return cand
    raise SystemExit("draco_encoder not found (need cesium-native's ezvcpkg "
                     "build or PATH)")


def write_glb(path, gltf, blob):
    json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    json_bytes += b" " * ((4 - len(json_bytes) % 4) % 4)
    blob_pad = (4 - len(blob) % 4) % 4
    blob += b"\x00" * blob_pad
    total = 12 + 8 + len(json_bytes) + 8 + len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(blob), 0x004E4942))
        f.write(blob)
    print("wrote %s (%d bytes)" % (path, total))


def common_gltf():
    return {
        "asset": {"version": "2.0", "generator": "gen_p29_draco_tileset.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "box"}],
        "materials": [{
            "name": "box",
            "doubleSided": True,
            "pbrMetallicRoughness": {
                "baseColorFactor": [COLOR[0], COLOR[1], COLOR[2], 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
        }],
    }


def make_plain_glb(path, positions, normals, indices):
    pos_bin = struct.pack("<%df" % len(positions), *positions)
    nrm_bin = struct.pack("<%df" % len(normals), *normals)
    idx_bin = struct.pack("<%dH" % len(indices), *indices)
    blob = pos_bin + nrm_bin + idx_bin
    xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
    gltf = common_gltf()
    gltf["meshes"] = [{"primitives": [{
        "attributes": {"POSITION": 0, "NORMAL": 1},
        "indices": 2, "material": 0}]}]
    gltf["accessors"] = [
        {"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
         "min": [min(xs), min(ys), min(zs)],
         "max": [max(xs), max(ys), max(zs)]},
        {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5123, "count": 36, "type": "SCALAR"},
    ]
    gltf["bufferViews"] = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(pos_bin),
         "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bin), "byteLength": len(nrm_bin),
         "target": 34962},
        {"buffer": 0, "byteOffset": len(pos_bin) + len(nrm_bin),
         "byteLength": len(idx_bin), "target": 34963},
    ]
    gltf["buffers"] = [{"byteLength": len(blob)}]
    write_glb(path, gltf, blob)


def make_draco_glb(path, drc_bytes):
    # Per KHR_draco_mesh_compression: accessors carry count/type but NO
    # bufferView; the extension maps attribute -> accessor and points at the
    # bufferView holding the draco bitstream.
    gltf = common_gltf()
    gltf["extensionsUsed"] = ["KHR_draco_mesh_compression"]
    gltf["extensionsRequired"] = ["KHR_draco_mesh_compression"]
    gltf["meshes"] = [{"primitives": [{
        "attributes": {"POSITION": 0, "NORMAL": 1},
        "indices": 2, "material": 0,
        "extensions": {"KHR_draco_mesh_compression": {
            "bufferView": 0,
            "attributes": {"POSITION": 0, "NORMAL": 1},
        }},
    }]}]
    gltf["accessors"] = [
        {"componentType": 5126, "count": 24, "type": "VEC3",
         "min": [-5.0, -5.0, -5.0], "max": [5.0, 5.0, 5.0]},
        {"componentType": 5126, "count": 24, "type": "VEC3"},
        {"componentType": 5123, "count": 36, "type": "SCALAR"},
    ]
    gltf["bufferViews"] = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(drc_bytes)}]
    gltf["buffers"] = [{"byteLength": len(drc_bytes)}]
    write_glb(path, gltf, drc_bytes)


TILESET = """{
  "asset": {"version": "1.0"},
  "geometricError": 100.0,
  "root": {
    "boundingVolume": {"box": [0,0,0, 5,0,0, 0,5,0, 0,0,5]},
    "geometricError": 16.0,
    "refine": "ADD",
    "content": {"uri": "%s"}
  }
}
"""


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    work = os.path.join(OUT_DIR, ".work")
    os.makedirs(work, exist_ok=True)
    positions, normals, indices = box_geometry((0, 0, 0), 5.0)

    # .obj with normals for draco_encoder
    obj = os.path.join(work, "box.obj")
    with open(obj, "w") as f:
        for i in range(0, len(positions), 3):
            f.write("v %s %s %s\n"
                    % (positions[i], positions[i + 1], positions[i + 2]))
        for i in range(0, len(normals), 3):
            f.write("vn %s %s %s\n"
                    % (normals[i], normals[i + 1], normals[i + 2]))
        for i in range(0, len(indices), 3):
            a, b, c = indices[i] + 1, indices[i + 1] + 1, indices[i + 2] + 1
            f.write("f %d//%d %d//%d %d//%d\n" % (a, a, b, b, c, c))
    drc = os.path.join(work, "box.drc")
    enc = find_encoder()
    r = subprocess.run(
        [enc, "-i", obj, "-o", drc, "-qp", "14", "-qn", "10", "-cl", "7"],
        capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("draco_encoder failed:\n" + r.stderr)
    with open(drc, "rb") as f:
        drc_bytes = f.read()
    print("draco bitstream: %d bytes (raw: %d)" %
          (len(drc_bytes),
           len(positions) * 4 + len(normals) * 4 + len(indices) * 2))
    shutil.copy(drc, os.path.join(OUT_DIR, "box.drc"))

    make_plain_glb(os.path.join(OUT_DIR, "plain.glb"),
                   positions, normals, indices)
    make_draco_glb(os.path.join(OUT_DIR, "root.glb"), drc_bytes)
    with open(os.path.join(OUT_DIR, "tileset.json"), "w") as f:
        f.write(TILESET % "root.glb")
    with open(os.path.join(OUT_DIR, "tileset_plain.json"), "w") as f:
        f.write(TILESET % "plain.glb")
    print("done:", OUT_DIR)


if __name__ == "__main__":
    main()
