#!/usr/bin/env python3
"""Generate deterministic PBR material fixtures for P15 (tests/data/p15_*/).

All geometry is baked in world coordinates (identity tile transforms), so the
SDK does not need an ENU/geospatial transform. Tilesets reference bare .glb
files (like P10), so glTF materials pass through cesium-native untouched
into gltfio's ubershader path.

Scenarios (each a tileset dir):
  p15_metal/   - dielectric (metallic=0) vs metal (metallic=1), same base
                 color and roughness; boxes at x=3 and x=11.
  p15_rough/   - roughness=0.08 vs roughness=0.9 (both dielectric).
  p15_normal/  - hand-made normal map vs flat normals.
  p15_alpha_opaque/ / p15_alpha_blend/ - same red box, OPAQUE vs BLEND
                 (alpha 0.5 in baseColor).
  p15_sided_single/ / p15_sided_double/ - plane whose normal faces AWAY
                 from the camera; single-sided must be culled, double-sided
                 visible.
  p15_tex/     - box with a procedural checkerboard baseColor texture.

Deterministic: re-running produces byte-identical output (fixed data, no
randomness, no network).
"""
import json
import os
import struct
import sys
import zlib

DATA_DIR = os.path.dirname(os.path.abspath(__file__))


# --------------------------------------------------------------------------
# Minimal PNG writer (stdlib only): 8-bit RGB, no filtering.
# --------------------------------------------------------------------------
def png_rgb(width, height, rows):
    """rows: list of bytes objects, each exactly 3*width bytes."""
    assert len(rows) == height and all(len(r) == 3 * width for r in rows)
    raw = b"".join(b"\x00" + r for r in rows)
    comp = zlib.compress(raw, 6)

    def chunk(typ, data):
        out = struct.pack(">I", len(data)) + typ + data
        out += struct.pack(">I", zlib.crc32(typ + data) & 0xFFFFFFFF)
        return out

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", comp)
    png += chunk(b"IEND", b"")
    return png


def checkerboard_png(size=64, checks=8):
    cell = size // checks
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            v = 255 if ((x // cell) + (y // cell)) % 2 == 0 else 0
            row += bytes((v, v, v))
        rows.append(bytes(row))
    return png_rgb(size, size, rows)


def normal_map_png(size=64):
    """Smooth sinusoidal bumps encoded as tangent-space normals."""
    import math
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            # Height field h(x, y); normal = normalize(-dh/dx, -dh/dh, 1).
            # Use analytic derivatives of h = sin(ax)*sin(by).
            a = 2.0 * math.pi * 5.0 / size
            b = 2.0 * math.pi * 4.0 / size
            dhdx = a * math.cos(a * x) * math.sin(b * y) * 3.0
            dhdy = b * math.sin(a * x) * math.cos(b * y) * 3.0
            inv = 1.0 / math.sqrt(dhdx * dhdx + dhdy * dhdy + 1.0)
            nx, ny, nz = -dhdx * inv, -dhdy * inv, inv
            row += bytes((
                max(0, min(255, int(round((nx * 0.5 + 0.5) * 255)))),
                max(0, min(255, int(round((ny * 0.5 + 0.5) * 255)))),
                max(0, min(255, int(round((nz * 0.5 + 0.5) * 255)))),
            ))
        rows.append(bytes(row))
    return png_rgb(size, size, rows)


# --------------------------------------------------------------------------
# Geometry: box with POSITION/NORMAL/TEXCOORD_0/TANGENT, plane facing -Z.
# --------------------------------------------------------------------------
def box_geometry_uv(center, half):
    cx, cy, cz = center
    h = half
    # (normal, tangent, 4 corners, 4 uvs)
    faces = [
        ((1, 0, 0), (0, 0, -1),
         [(cx + h, cy - h, cz + h), (cx + h, cy - h, cz - h),
          (cx + h, cy + h, cz - h), (cx + h, cy + h, cz + h)]),
        ((-1, 0, 0), (0, 0, 1),
         [(cx - h, cy - h, cz - h), (cx - h, cy - h, cz + h),
          (cx - h, cy + h, cz + h), (cx - h, cy + h, cz - h)]),
        ((0, 1, 0), (1, 0, 0),
         [(cx - h, cy + h, cz + h), (cx + h, cy + h, cz + h),
          (cx + h, cy + h, cz - h), (cx - h, cy + h, cz - h)]),
        ((0, -1, 0), (1, 0, 0),
         [(cx - h, cy - h, cz - h), (cx + h, cy - h, cz - h),
          (cx + h, cy - h, cz + h), (cx - h, cy - h, cz + h)]),
        ((0, 0, 1), (1, 0, 0),
         [(cx - h, cy - h, cz + h), (cx + h, cy - h, cz + h),
          (cx + h, cy + h, cz + h), (cx - h, cy + h, cz + h)]),
        ((0, 0, -1), (-1, 0, 0),
         [(cx + h, cy - h, cz - h), (cx - h, cy - h, cz - h),
          (cx - h, cy - h, cz - h), (cx - h, cy + h, cz - h)]),
    ]
    uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
    pos, nrm, uv, tan, idx = [], [], [], [], []
    for normal, tangent, corners in faces:
        base = len(pos) // 3
        for p, t in zip(corners, uvs):
            pos.extend(p)
            nrm.extend(normal)
            uv.extend(t)
            tan.extend([tangent[0], tangent[1], tangent[2], 1.0])
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return pos, nrm, uv, tan, idx


def plane_geometry_facing_minus_z(center, size):
    """Single quad whose FRONT face points -Z (away from the demo camera at
    +Z): single-sided must be back-face culled, double-sided visible."""
    cx, cy, cz = center
    h = size / 2.0
    corners = [(cx - h, cy - h, cz), (cx + h, cy - h, cz),
               (cx + h, cy + h, cz), (cx - h, cy + h, cz)]
    uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
    pos, nrm, uv, tan, idx = [], [], [], [], []
    for p, t in zip(corners, uvs):
        pos.extend(p)
        nrm.extend([0.0, 0.0, -1.0])
        uv.extend(t)
        tan.extend([1.0, 0.0, 0.0, 1.0])
    # Winding reversed so the geometric front face is -Z (normal attribute
    # agrees); from the +Z camera this quad is back-facing.
    idx.extend([0, 2, 1, 0, 3, 2])
    return pos, nrm, uv, tan, idx


# --------------------------------------------------------------------------
# GLB writer with full PBR material control.
# --------------------------------------------------------------------------
def make_glb(path, pos, nrm, uv, tan, idx, material, images=()):
    pos_bin = struct.pack("<%df" % len(pos), *pos)
    nrm_bin = struct.pack("<%df" % len(nrm), *nrm)
    uv_bin = struct.pack("<%df" % len(uv), *uv)
    tan_bin = struct.pack("<%df" % len(tan), *tan)
    idx_bin = struct.pack("<%dH" % len(idx), *idx)
    parts = [pos_bin, nrm_bin, uv_bin, tan_bin, idx_bin] + list(images)

    buffer_views = []
    offset = 0
    for i, part in enumerate(parts):
        target = 34962 if i < 4 else (34963 if i == 4 else None)
        bv = {"buffer": 0, "byteOffset": offset, "byteLength": len(part)}
        if target is not None:
            bv["target"] = target
        buffer_views.append(bv)
        offset += len(part)
    blob = b"".join(parts)
    # Pad blob to 4 bytes (images already multiples of 1; pad overall).
    pad = (4 - len(blob) % 4) % 4
    blob += b"\x00" * pad

    accessors = [
        {"bufferView": 0, "componentType": 5126, "count": len(pos) // 3,
         "type": "VEC3",
         "min": [min(pos[0::3]), min(pos[1::3]), min(pos[2::3])],
         "max": [max(pos[0::3]), max(pos[1::3]), max(pos[2::3])]},
        {"bufferView": 1, "componentType": 5126, "count": len(nrm) // 3,
         "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": len(uv) // 2,
         "type": "VEC2"},
        {"bufferView": 3, "componentType": 5126, "count": len(tan) // 4,
         "type": "VEC4"},
        {"bufferView": 4, "componentType": 5123, "count": len(idx),
         "type": "SCALAR"},
    ]
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_p15_pbr_tileset.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "m"}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1,
                               "TEXCOORD_0": 2, "TANGENT": 3},
                "indices": 4,
                "material": 0,
            }]
        }],
        "materials": [material],
        "accessors": accessors,
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(blob)}],
    }
    if images:
        gltf["images"] = [
            {"bufferView": 5 + i, "mimeType": "image/png"}
            for i in range(len(images))
        ]
        gltf["textures"] = [{"source": i} for i in range(len(images))]
        gltf["samplers"] = [{"magFilter": 9729, "minFilter": 9987,
                             "wrapS": 10497, "wrapT": 10497}]

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


def pbr_material(base_color, metallic, roughness, alpha_mode="OPAQUE",
                 double_sided=True, base_tex=None, normal_tex=None):
    m = {
        "name": "pbr",
        "doubleSided": double_sided,
        "pbrMetallicRoughness": {
            "baseColorFactor": list(base_color),
            "metallicFactor": metallic,
            "roughnessFactor": roughness,
        },
    }
    if base_tex is not None:
        m["pbrMetallicRoughness"]["baseColorTexture"] = {"index": base_tex}
    if normal_tex is not None:
        m["normalTexture"] = {"index": normal_tex}
    if alpha_mode != "OPAQUE":
        m["alphaMode"] = alpha_mode
    return m


def write_tileset(path, tiles):
    """tiles: list of (uri, cx, cy, cz, half)."""
    children = []
    for uri, cx, cy, cz, half in tiles:
        children.append({
            "boundingVolume": {"box": [cx, cy, cz, half, 0, 0,
                                       0, half, 0, 0, 0, half]},
            "geometricError": 0.0,
            "content": {"uri": uri},
        })
    xs = [cx for _, cx, _, _, _ in tiles]
    root = {
        "asset": {"version": "1.0"},
        "geometricError": 100.0,
        "root": {
            "boundingVolume": {"box": [8, 0, 0, 9, 0, 0, 0, 4, 0, 0, 0, 4]},
            "geometricError": 10.0,
            "refine": "ADD",
            "children": children,
        },
    }
    with open(path, "w") as f:
        json.dump(root, f, indent=2)
        f.write("\n")
    print("wrote %s" % path)


def box_glb(path, center, half, material, images=()):
    pos, nrm, uv, tan, idx = box_geometry_uv(center, half)
    make_glb(path, pos, nrm, uv, tan, idx, material, images)


def main():
    # --- p15_metal: dielectric vs metal, same base color / roughness ---
    d = os.path.join(DATA_DIR, "p15_metal")
    os.makedirs(d, exist_ok=True)
    base = (0.55, 0.55, 0.60, 1.0)
    box_glb(os.path.join(d, "diel.glb"), (3, 0, 0), 1.5,
            pbr_material(base, 0.0, 0.35))
    box_glb(os.path.join(d, "metal.glb"), (11, 0, 0), 1.5,
            pbr_material(base, 1.0, 0.35))
    write_tileset(os.path.join(d, "tileset.json"),
                  [("diel.glb", 3, 0, 0, 1.5),
                   ("metal.glb", 11, 0, 0, 1.5)])

    # --- p15_rough: roughness 0.08 vs 0.9 (metallic, so roughness drives the
    # specular lobe almost alone: tight bright sun glint vs dim sheen) ---
    d = os.path.join(DATA_DIR, "p15_rough")
    os.makedirs(d, exist_ok=True)
    base = (0.60, 0.60, 0.65, 1.0)
    box_glb(os.path.join(d, "r01.glb"), (3, 0, 0), 1.5,
            pbr_material(base, 1.0, 0.08))
    box_glb(os.path.join(d, "r09.glb"), (11, 0, 0), 1.5,
            pbr_material(base, 1.0, 0.9))
    write_tileset(os.path.join(d, "tileset.json"),
                  [("r01.glb", 3, 0, 0, 1.5),
                   ("r09.glb", 11, 0, 0, 1.5)])

    # --- p15_normal: normal-mapped vs flat ---
    nmap_png = normal_map_png()
    d = os.path.join(DATA_DIR, "p15_normal")
    os.makedirs(d, exist_ok=True)
    base = (0.55, 0.55, 0.60, 1.0)
    box_glb(os.path.join(d, "nmap.glb"), (3, 0, 0), 1.5,
            pbr_material(base, 0.0, 0.5, normal_tex=0), images=[nmap_png])
    box_glb(os.path.join(d, "flat.glb"), (11, 0, 0), 1.5,
            pbr_material(base, 0.0, 0.5))
    write_tileset(os.path.join(d, "tileset.json"),
                  [("nmap.glb", 3, 0, 0, 1.5),
                   ("flat.glb", 11, 0, 0, 1.5)])

    # --- p15_alpha: same red box, OPAQUE vs BLEND(alpha 0.5) ---
    for name, alpha_mode, alpha in (("p15_alpha_opaque", "OPAQUE", 1.0),
                                    ("p15_alpha_blend", "BLEND", 0.5)):
        d = os.path.join(DATA_DIR, name)
        os.makedirs(d, exist_ok=True)
        box_glb(os.path.join(d, "box.glb"), (8, 0, 0), 1.5,
                pbr_material((0.9, 0.1, 0.1, alpha), 0.0, 0.6,
                             alpha_mode=alpha_mode))
        write_tileset(os.path.join(d, "tileset.json"),
                      [("box.glb", 8, 0, 0, 1.5)])

    # --- p15_sided: plane facing AWAY from the camera (-Z) ---
    for name, double_sided in (("p15_sided_single", False),
                               ("p15_sided_double", True)):
        d = os.path.join(DATA_DIR, name)
        os.makedirs(d, exist_ok=True)
        pos, nrm, uv, tan, idx = plane_geometry_facing_minus_z((8, 0, 0), 3.0)
        make_glb(os.path.join(d, "plane.glb"), pos, nrm, uv, tan, idx,
                 pbr_material((0.1, 0.8, 0.1, 1.0), 0.0, 0.6,
                              double_sided=double_sided))
        write_tileset(os.path.join(d, "tileset.json"),
                      [("plane.glb", 8, 0, 0, 1.5)])

    # --- p15_tex: checkerboard baseColor texture ---
    d = os.path.join(DATA_DIR, "p15_tex")
    os.makedirs(d, exist_ok=True)
    box_glb(os.path.join(d, "check.glb"), (8, 0, 0), 1.5,
            pbr_material((1.0, 1.0, 1.0, 1.0), 0.0, 0.6, base_tex=0),
            images=[checkerboard_png()])
    write_tileset(os.path.join(d, "tileset.json"),
                  [("check.glb", 8, 0, 0, 1.5)])


if __name__ == "__main__":
    main()
