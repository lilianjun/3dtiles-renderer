#!/usr/bin/env python3
"""Generate the P25 KTX2 texture fixtures (tests/data/p25_ktx2_tileset/).

P25: KTX2 / BasisU texture support. The render bridge registers a KTX2
texture provider for "image/ktx2" (src/tileset.cpp); this fixture proves
a KTX2-textured box renders the same colors as its PNG twin.

Contents:
  p25_ktx2_tileset/ - box with a red/blue checkerboard baseColor texture
                      stored as UASTC KTX2 (KHR_texture_basisu extension)
                      check_ktx2.glb + tileset.json
  p25_png_tileset/   - same box, same checker pixels as PNG (reference for
                      the pixel comparison in tests/ktx2_test.py)
                      check_png.glb + tileset.json

The KTX2 payload is a fixed UASTC file (base64 below), produced once by
Khronos toktx 4.4.2 from the same checker pixels the PNG writer emits
(`toktx --uastc --uastc_quality 2`, 64x64, 1 mip level, sRGB). Embedding
the bytes keeps the generator dependency-free and byte-deterministic:
re-running this script must produce byte-identical files. Uncompressed
RGBA8 KTX2 is intentionally NOT used: cesium-native v0.64.0's ImageDecoder
rejects KTX2 that needs no transcoding ("KTX2 loading failed with error:
Operation succeeded."), so it can never reach the renderer.
"""
import json
import os
import struct
import zlib

OUT_KTX2 = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "p25_ktx2_tileset")
OUT_PNG = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "p25_png_tileset")

SIZE, CHECKS = 64, 8


# --------------------------------------------------------------------------
# Pixel data: red/blue checkerboard (vertically asymmetric on purpose, so a
# vertical flip in the decode path shows up as a large pixel diff).
# --------------------------------------------------------------------------
def checker_pixels(size=SIZE, checks=CHECKS):
    cell = size // checks
    px = bytearray()
    for y in range(size):
        for x in range(size):
            if ((x // cell) + (y // cell)) % 2 == 0:
                px += bytes((255, 0, 0, 255))      # red
            else:
                px += bytes((0, 0, 255, 255))      # blue
    return bytes(px)


# --------------------------------------------------------------------------
# Minimal PNG writer (RGBA8, no interlace) — same approach as
# gen_p15_pbr_tileset.py.
# --------------------------------------------------------------------------
def chunk(typ, data):
    c = typ + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))


def png_rgba(pixels, size):
    raw = b"".join(b"\x00" + pixels[y * size * 4:(y + 1) * size * 4]
                   for y in range(size))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    return png


# --------------------------------------------------------------------------
# Deterministic UASTC KTX2 payload (base64). See module docstring for
# provenance. Decodes to the same 64x64 red/blue checkerboard that
# checker_pixels() emits (modulo UASTC lossy compression).
# --------------------------------------------------------------------------
_UASTC_KTX2_B64 = (
"q0tUWCAyMLsNChoKAAAAAAEAAABAAAAAQAAAAAAAAAAAAAAAAQAAAAEAAAAAAAAAaAAAACwAAACUAAAAdAAAAAAAAAAAAAAAAAAAAAAAAAAQAQAAAAAAAAAQAAAAAAAAABAAAAAAAAAsAAAAAAAAAAIAKACmAQIAAwMAABAAAAAAAAAAAAB/AAAAAAAAAAAA/////xIAAABLVFhvcmllbnRhdGlvbgByZAAAACcAAABLVFh3cml0ZXIAdG9rdHggdjQuNC4yIC8gbGlia3R4IHY0LjQuMgAALAAAAEtUWHdyaXRlclNjUGFyYW1zAC0tdWFzdGMgLS11YXN0Y19xdWFsaXR5IDIAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAABcA4P8fAuABAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA9x8A4B96AAAAAAAAAAAAABcA4P8fAuABAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAA9x8A4B96AAAAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAAXAOD/HwLgAQAAAAAAAAAAFwDg/x8C4AEAAAAAAAAAAPcfAOAfegAAAAAAAAAAAAD3HwDgH3oAAAAAAAAAAAAA"
)


def ktx2_uastc():
    import base64
    return base64.b64decode(_UASTC_KTX2_B64)


# --------------------------------------------------------------------------
# Box geometry with UVs (24 verts, indexed).
# --------------------------------------------------------------------------
def box_geometry(size=4.0):
    h = size / 2.0
    faces = [
        # +X, -X, +Y, -Y, +Z, -Z: (normal, 4 corners CCW from outside)
        ((1, 0, 0), [(h, -h, -h), (h, -h, h), (h, h, h), (h, h, -h)]),
        ((-1, 0, 0), [(-h, -h, h), (-h, -h, -h), (-h, h, -h), (-h, h, h)]),
        ((0, 1, 0), [(-h, h, -h), (h, h, -h), (h, h, h), (-h, h, h)]),
        ((0, -1, 0), [(-h, -h, h), (h, -h, h), (h, -h, -h), (-h, -h, -h)]),
        ((0, 0, 1), [(-h, -h, h), (-h, h, h), (h, h, h), (h, -h, h)]),
        ((0, 0, -1), [(h, -h, -h), (h, h, -h), (-h, h, -h), (-h, -h, -h)]),
    ]
    uvs = [(0, 0), (1, 0), (1, 1), (0, 1)]
    pos, nrm, uv, idx = [], [], [], []
    for normal, corners in faces:
        base = len(pos) // 3
        for p, t in zip(corners, uvs):
            pos.extend(p)
            nrm.extend(normal)
            uv.extend(t)
        idx.extend([base, base + 1, base + 2, base, base + 2, base + 3])
    return pos, nrm, uv, idx


def make_glb(path, image_bytes, mime_type, basisu_ext):
    pos, nrm, uv, idx = box_geometry()
    parts = [
        struct.pack("<%df" % len(pos), *pos),
        struct.pack("<%df" % len(nrm), *nrm),
        struct.pack("<%df" % len(uv), *uv),
        struct.pack("<%dH" % len(idx), *idx),
        image_bytes,
    ]
    buffer_views = []
    offset = 0
    for i, part in enumerate(parts):
        bv = {"buffer": 0, "byteOffset": offset, "byteLength": len(part)}
        if i < 3:
            bv["target"] = 34962
        elif i == 3:
            bv["target"] = 34963
        buffer_views.append(bv)
        offset += len(part)
    blob = b"".join(parts)
    blob += b"\x00" * ((4 - len(blob) % 4) % 4)

    texture = {"source": 0}
    gltf = {
        "asset": {"version": "2.0", "generator": "gen_p25_ktx2.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "m"}],
        "meshes": [{
            "primitives": [{
                "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                "indices": 3,
                "material": 0,
            }]
        }],
        "materials": [{
            "name": "check",
            "pbrMetallicRoughness": {
                "baseColorFactor": [1, 1, 1, 1],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
                "baseColorTexture": {"index": 0},
            },
        }],
        "textures": [texture],
        "images": [{"bufferView": 4, "mimeType": mime_type}],
        "samplers": [{"magFilter": 9729, "minFilter": 9729,
                      "wrapS": 10497, "wrapT": 10497}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126,
             "count": len(pos) // 3, "type": "VEC3",
             "min": [-2.0, -2.0, -2.0], "max": [2.0, 2.0, 2.0]},
            {"bufferView": 1, "componentType": 5126,
             "count": len(nrm) // 3, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126,
             "count": len(uv) // 2, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123,
             "count": len(idx), "type": "SCALAR"},
        ],
        "bufferViews": buffer_views,
        "buffers": [{"byteLength": len(blob)}],
    }
    if basisu_ext:
        # KHR_texture_basisu: the spec-correct way to reference a BasisU
        # image; gltfio routes by image mimeType, the extension is for
        # spec conformance.
        texture["extensions"] = {"KHR_texture_basisu": {"source": 0}}
        gltf["extensionsUsed"] = ["KHR_texture_basisu"]

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


def write_tileset(out_dir, glb_name):
    os.makedirs(out_dir, exist_ok=True)
    tileset = {
        "asset": {"version": "1.0"},
        "geometricError": 100.0,
        "root": {
            "boundingVolume": {"box": [0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2]},
            "geometricError": 0.0,
            "refine": "REPLACE",
            "content": {"uri": glb_name},
        },
    }
    with open(os.path.join(out_dir, "tileset.json"), "w") as f:
        json.dump(tileset, f, indent=1)
        f.write("\n")


def main():
    px = checker_pixels()
    ktx2 = ktx2_uastc()
    png = png_rgba(px, SIZE)

    os.makedirs(OUT_KTX2, exist_ok=True)
    make_glb(os.path.join(OUT_KTX2, "check_ktx2.glb"), ktx2, "image/ktx2",
             basisu_ext=True)
    write_tileset(OUT_KTX2, "check_ktx2.glb")

    os.makedirs(OUT_PNG, exist_ok=True)
    make_glb(os.path.join(OUT_PNG, "check_png.glb"), png, "image/png",
             basisu_ext=False)
    write_tileset(OUT_PNG, "check_png.glb")
    print("done")


if __name__ == "__main__":
    main()
