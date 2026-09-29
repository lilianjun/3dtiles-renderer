#!/usr/bin/env python3
"""Generate the P7 i3dm test tilesets (tests/data/p7_i3dm_*/).

i3dm (Instanced 3D Models, 3D Tiles 1.0) exercises the cesium-native
I3dmToGltfConverter -> CesiumGltf::Model (EXT_mesh_gpu_instancing) ->
GltfWriter::writeGlb -> gltfio path. Unlike b3dm, the converted model has
TWO buffers (mesh data + appended instance TRANSLATION/ROTATION/SCALE
data), which is what caught the modelToGlb single-buffer bug (P7).

Field semantics were verified against
Cesium3DTilesContent/src/I3dmToGltfConverter.cpp (v0.64.0):
  - 32-byte header: magic "i3dm", version 1, byteLength,
    featureTableJSONByteLength, featureTableBinaryByteLength,
    batchTableJSONByteLength, batchTableBinaryByteLength, glTFFormat.
    (4 magic bytes + 7 uint32 fields; NOT 28 bytes — b3dm's 28-byte
    header lacks the glTFFormat field.)
  - Feature table JSON requires INSTANCES_LENGTH and at least one of
    POSITION / POSITION_QUANTIZED; POSITION/SCALE/NORMAL_UP/NORMAL_RIGHT
    are {"byteOffset": <offset into feature-table binary>}.
    RTC_CENTER is a plain [x, y, z] array (double).
  - cesium-native ALWAYS recenters instance positions around their mean
    (repositionInstances) and bakes (mean + RTC_CENTER) into the glTF
    root-node translation via applyRtcToNodes. So the in-glb node
    translation carries the big offset when RTC_CENTER is huge, while the
    tile transform in tileset.json is handled by the SDK's local-origin
    rebase (P5) in double precision.
  - glTFFormat=1 embeds a binary glb after the batch-table section.

Tilesets generated (all deterministic — fixed layouts, no RNG):

1. p7_i3dm_tileset/ — main rendering test:
     tileset.json   - root -> instances.i3dm, child -> accents.i3dm
     instances.i3dm - 12 instances of an orange 2 m box (4x3 grid,
                      varied uniform SCALE) — exercises POSITION+SCALE.
     accents.i3dm   - 8 instances of a teal 2 m box (2x4 grid, per-instance
                      Y rotation via NORMAL_UP/NORMAL_RIGHT) — exercises
                      the rotation path.
   All instance X coordinates are positive (the Mesa software-GL path was
   observed in P3 to drop negative-X geometry).

2. p7_i3dm_rebase/near|far/ — P5-style rebase for instancing:
   byte-identical instances.i3dm; `far` adds a 123456789.0 m root-tile
   translation. The SDK rebase must reproduce the near image.

3. p7_i3dm_rtc/ — RTC_CENTER at ECEF magnitude
   ([1210000.0, -4736290.5, 4081600.0], |.| ~ 6.37e6 m) with small local
   POSITIONs. cesium-native bakes the center into the glTF node
   translation, so this documents the float32 precision boundary of
   in-asset offsets (the SDK rebase only covers tile transforms).

4. p7_i3dm_many/ — 256 instances in one i3dm (16x16 grid): instance-count
   stress, correctness only.

5. p7_i3dm_rtc_ref/ — no-RTC reference for fixture 3: same 6 instances
   with positions pre-shifted by -(5,0,0), bounding volume at the origin.
   The rtc test requires its screenshot to be bit-identical to fixture 3's.

Deterministic: re-running produces byte-identical output.
"""
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_p3_tileset as p3  # noqa: E402

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
MAIN_DIR = os.path.join(DATA_DIR, "p7_i3dm_tileset")
REBASE_DIR = os.path.join(DATA_DIR, "p7_i3dm_rebase")
RTC_DIR = os.path.join(DATA_DIR, "p7_i3dm_rtc")
MANY_DIR = os.path.join(DATA_DIR, "p7_i3dm_many")

ORANGE = (1.0, 0.45, 0.10)
TEAL = (0.10, 0.75, 0.75)

# ECEF-magnitude center for the RTC test (~6.37e6 m from origin).
RTC_CENTER = [1210000.0, -4736290.5, 4081600.0]
# P5's float32-hostile offset, reused for the tile-transform rebase test.
REBASE_OFFSET = 123456789.0


def glb_bytes_for(center, half, color):
    """Build the same box GLB as gen_p3_tileset.make_glb, in memory."""
    tmp = os.path.join(DATA_DIR, ".tmp_p7.glb")
    p3.make_glb(tmp, center, half, color)
    with open(tmp, "rb") as f:
        data = f.read()
    os.remove(tmp)
    return data


def make_i3dm(path, glb_bytes, positions, scales=None, rotations=None,
              rtc_center=None):
    """Write a minimal i3dm (version 1, glTFFormat=1 = embedded glb).

    positions: [(x, y, z)] float triples, instance count N = len(positions).
    scales:    [s] uniform float scales, or None (defaults to 1.0).
    rotations: [((ux,uy,uz), (rx,ry,rz))] up/right vector pairs, or None.
    rtc_center: [x, y, z] or None.
    """
    n = len(positions)
    assert n > 0
    if scales is None:
        scales = [1.0] * n
    assert len(scales) == n
    assert rotations is None or len(rotations) == n

    ft_bin = struct.pack("<%df" % (3 * n),
                         *[c for p in positions for c in p])
    ft_json = {"INSTANCES_LENGTH": n, "POSITION": {"byteOffset": 0}}
    offset = 3 * n * 4
    ft_bin += struct.pack("<%df" % n, *scales)
    ft_json["SCALE"] = {"byteOffset": offset}
    offset += n * 4
    if rotations is not None:
        ft_bin += struct.pack("<%df" % (3 * n),
                              *[c for up, right in rotations for c in up])
        ft_json["NORMAL_UP"] = {"byteOffset": offset}
        offset += 3 * n * 4
        ft_bin += struct.pack("<%df" % (3 * n),
                              *[c for up, right in rotations for c in right])
        ft_json["NORMAL_RIGHT"] = {"byteOffset": offset}
        offset += 3 * n * 4
    if rtc_center is not None:
        ft_json["RTC_CENTER"] = list(rtc_center)

    ft_json_bytes = json.dumps(ft_json, separators=(",", ":")).encode("utf-8")
    ft_json_bytes += b" " * ((4 - len(ft_json_bytes) % 4) % 4)

    header_len = 32  # magic(4) + 7 x uint32: i3dm has one more field than b3dm
    total = header_len + len(ft_json_bytes) + len(ft_bin) + len(glb_bytes)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sIIIIIII", b"i3dm", 1, total,
                            len(ft_json_bytes), len(ft_bin), 0, 0, 1))
        f.write(ft_json_bytes)
        f.write(ft_bin)
        f.write(glb_bytes)
    print("wrote %s (%d bytes, %d instances)" % (path, total, n))


def y_rotation_up_right(degrees):
    """Up/right vector pair encoding a rotation about +Y by `degrees`."""
    t = math.radians(degrees)
    return ((0.0, 1.0, 0.0), (math.cos(t), 0.0, -math.sin(t)))


def write_tileset(path, root):
    with open(path, "w") as f:
        json.dump({"asset": {"version": "1.0"},
                   "geometricError": 200.0, "root": root}, f, indent=2)
        f.write("\n")
    print("wrote %s" % path)


def box_volume(cx, cy, cz, hx, hy, hz):
    return {"box": [cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz]}


def gen_main():
    """12 orange + 8 teal instances across two tiles."""
    os.makedirs(MAIN_DIR, exist_ok=True)
    box_glb = glb_bytes_for((0, 0, 0), 1.0, ORANGE)

    positions, scales = [], []
    for row in range(3):
        for col in range(4):
            i = row * 4 + col
            positions.append((2.5 + 5.0 * col, 0.0, -5.0 + 5.0 * row))
            scales.append(0.8 + 0.1 * ((i * 7) % 8))  # 0.8..1.5 deterministic
    make_i3dm(os.path.join(MAIN_DIR, "instances.i3dm"), box_glb,
              positions, scales=scales)

    teal_glb = glb_bytes_for((0, 0, 0), 1.0, TEAL)
    t_positions, t_rotations = [], []
    for row in range(4):
        for col in range(2):
            i = row * 2 + col
            t_positions.append((14.0 + 4.5 * col, 0.0, -4.5 + 3.0 * row))
            t_rotations.append(y_rotation_up_right((i * 35) % 360))
    make_i3dm(os.path.join(MAIN_DIR, "accents.i3dm"), teal_glb,
              t_positions, rotations=t_rotations)

    # Root covers x in [-1.75, 20.25]; child covers x in [12.75, 20].
    # The teal tile sits just right of the orange grid so both tiles are
    # in the demo's fixed orbit-camera frame for the pixel test.
    root = {
        "boundingVolume": box_volume(9.25, 0, 0, 11.0, 3, 8),
        "geometricError": 32.0,
        "refine": "ADD",
        "content": {"uri": "instances.i3dm"},
        "children": [{
            "boundingVolume": box_volume(16.25, 0, 0, 3.75, 3, 7),
            "geometricError": 0.0,
            "content": {"uri": "accents.i3dm"},
        }],
    }
    write_tileset(os.path.join(MAIN_DIR, "tileset.json"), root)


def gen_rebase():
    """Same i3dm, near at origin / far translated by REBASE_OFFSET."""
    box_glb = glb_bytes_for((0, 0, 0), 1.0, ORANGE)
    positions = [(2.5 + 5.0 * (i % 3), 0.0, -3.0 + 3.0 * (i // 3))
                 for i in range(6)]
    for name, offset in (("near", 0.0), ("far", REBASE_OFFSET)):
        d = os.path.join(REBASE_DIR, name)
        os.makedirs(d, exist_ok=True)
        make_i3dm(os.path.join(d, "instances.i3dm"), box_glb, positions)
        # NOTE: boundingVolume is always in tile-LOCAL coordinates (the tile
        # transform maps it to world). So `far` keeps the same local volume
        # as `near`; only the root transform carries the offset. Adding the
        # offset to the volume too would double-translate it.
        root = {
            "boundingVolume": box_volume(5.0, 0, 0, 7.5, 3, 6),
            "geometricError": 0.0,
            "content": {"uri": "instances.i3dm"},
        }
        if offset != 0.0:
            root["transform"] = [1, 0, 0, 0,
                                 0, 1, 0, 0,
                                 0, 0, 1, 0,
                                 offset, 0, 0, 1]
        write_tileset(os.path.join(d, "tileset.json"), root)


def gen_rtc():
    """RTC_CENTER at ECEF magnitude; positions local to it."""
    os.makedirs(RTC_DIR, exist_ok=True)
    box_glb = glb_bytes_for((0, 0, 0), 1.0, ORANGE)
    positions = [(2.5 + 5.0 * (i % 3), 0.0, -3.0 + 3.0 * (i // 3))
                 for i in range(6)]
    make_i3dm(os.path.join(RTC_DIR, "instances.i3dm"), box_glb, positions,
              rtc_center=RTC_CENTER)
    # Bounding volume in world coordinates (center + RTC_CENTER).
    root = {
        "boundingVolume": box_volume(5.0 + RTC_CENTER[0], RTC_CENTER[1],
                                     RTC_CENTER[2], 7.5, 3, 6),
        "geometricError": 0.0,
        "content": {"uri": "instances.i3dm"},
    }
    write_tileset(os.path.join(RTC_DIR, "tileset.json"), root)


def gen_rtc_ref():
    """No-RTC reference for the RTC_CENTER test: identical rebased geometry.

    Same 6 instances with positions pre-shifted by -(5, 0, 0) and no
    RTC_CENTER, bounding volume centered at the origin. Renders at
    (rtc + pos) - (rtc + (5,0,0)) == pos - (5,0,0), i.e. the exact rebased
    coordinates the RTC tileset must produce — the rtc test requires the
    two screenshots to be bit-identical.
    """
    d = os.path.join(DATA_DIR, "p7_i3dm_rtc_ref")
    os.makedirs(d, exist_ok=True)
    box_glb = glb_bytes_for((0, 0, 0), 1.0, ORANGE)
    positions = [(2.5 + 5.0 * (i % 3) - 5.0, 0.0, -3.0 + 3.0 * (i // 3))
                 for i in range(6)]
    make_i3dm(os.path.join(d, "instances.i3dm"), box_glb, positions)
    root = {
        "boundingVolume": box_volume(0.0, 0.0, 0.0, 7.5, 3, 6),
        "geometricError": 0.0,
        "content": {"uri": "instances.i3dm"},
    }
    write_tileset(os.path.join(d, "tileset.json"), root)


def gen_many():
    """256 instances: instance-count stress (correctness only)."""
    os.makedirs(MANY_DIR, exist_ok=True)
    box_glb = glb_bytes_for((0, 0, 0), 1.0, ORANGE)
    positions = [(2.0 + 4.0 * (i % 16), 0.0, 2.0 + 4.0 * (i // 16))
                 for i in range(256)]
    make_i3dm(os.path.join(MANY_DIR, "instances.i3dm"), box_glb, positions)
    root = {
        "boundingVolume": box_volume(32.0, 0, 32.0, 32.0, 3, 32.0),
        "geometricError": 0.0,
        "content": {"uri": "instances.i3dm"},
    }
    write_tileset(os.path.join(MANY_DIR, "tileset.json"), root)


def main():
    gen_main()
    gen_rebase()
    gen_rtc()
    gen_rtc_ref()
    gen_many()


if __name__ == "__main__":
    sys.exit(main())
