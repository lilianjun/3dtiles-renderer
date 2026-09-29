#!/usr/bin/env python3
"""Generate the P10 3D Tiles 1.1 test tilesets (tests/data/p10_11_*/).

3D Tiles 1.1 support in the SDK chain was verified against
cesium-native v0.64.0 sources before writing any fixture:

  - Bare .glb tile content: magic "glTF" is registered in
    registerAllTileContentTypes() -> BinaryToGltfConverter::convert ->
    GltfConverterResult{model} -> the same TilesetJsonLoader continuation
    (TilesetJsonLoader.cpp ~L1296) as b3dm/i3dm/pnts/cmpt, i.e. the same
    SDK prepareInLoadThread -> render bridge. TilesetJsonLoader does not
    gate on asset.version, so a 1.1 tileset with glb content needs no
    SDK change (P5 already rendered bare glbs inside a 1.0 tileset).
  - Implicit tiling: root tile carrying the 3DTILES_implicit_tiling
    extension triggers createImplicitQuadtreeLoader(); subtree files are
    fetched through the regular IAssetAccessor (file:// here) and parsed
    by SubtreeFileReader. A subtree that does not start with the "subt"
    magic is parsed as plain JSON, so the fixture subtree is hand-written
    JSON with constant availability (no bitstream buffers needed).
  - URL templates use {level}/{x}/{y} placeholders, resolved against the
    tileset.json base URL (ImplicitTilingUtilities::resolveUrl).

Fixtures generated (deterministic — fixed layouts, no RNG):

  p10_11_glb/
    tileset.json - asset.version "1.1", extensionsUsed
                   ["3DTILES_content_gltf"]; root content is a bare
                   orange box.glb, one child with a bare teal box.glb.
    box.glb / child.glb

  p10_11_implicit/
    tileset.json - asset.version "1.1", root with
                   3DTILES_implicit_tiling (QUADTREE, subtreeLevels 2,
                   availableLevels 2), content uri template
                   "tiles/{level}_{x}_{y}.glb", subtree uri template
                   "subtrees/{level}_{x}_{y}.subtree".
    subtrees/0_0_0.subtree - plain JSON: tileAvailability constant 1,
                   contentAvailability [{constant 1}],
                   childSubtreeAvailability {constant 0}.
    tiles/       - 5 glbs: level 0 red box at the origin (half 0.5),
                   level 1 quadrant boxes (half 1.0) at (+/-4, +/-4, 0):
                   (1,0,0) green, (1,1,0) blue, (1,0,1) yellow,
                   (1,1,1) magenta. Quadrant mapping follows the spec
                   (x/y index from the box minimum corner); the pixel
                   test only counts colors, not positions, so a mirrored
                   mapping would still pass.

Deterministic: re-running produces byte-identical output.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_p3_tileset as p3  # noqa: E402

DATA_DIR = os.path.dirname(os.path.abspath(__file__))
GLB_DIR = os.path.join(DATA_DIR, "p10_11_glb")
IMPLICIT_DIR = os.path.join(DATA_DIR, "p10_11_implicit")

ORANGE = (1.0, 0.45, 0.10)
TEAL = (0.10, 0.75, 0.75)


def box_volume(cx, cy, cz, hx, hy, hz):
    return {"box": [cx, cy, cz, hx, 0, 0, 0, hy, 0, 0, 0, hz]}


def write_tileset(path, root):
    with open(path, "w") as f:
        json.dump(root, f, indent=2)
        f.write("\n")
    print("wrote %s" % path)


def gen_glb_tileset():
    """Scenario A: 3D Tiles 1.1, bare .glb tile content (3DTILES_content_gltf)."""
    os.makedirs(GLB_DIR, exist_ok=True)
    p3.make_glb(os.path.join(GLB_DIR, "box.glb"), (0, 0, 0), 2.0, ORANGE)
    p3.make_glb(os.path.join(GLB_DIR, "child.glb"), (6, 0, 0), 1.0, TEAL)
    root = {
        "asset": {"version": "1.1"},
        "extensionsUsed": ["3DTILES_content_gltf"],
        "geometricError": 100.0,
        "root": {
            "boundingVolume": box_volume(0, 0, 0, 8, 5, 5),
            "geometricError": 10.0,
            "refine": "ADD",
            "content": {"uri": "box.glb"},
            "children": [
                {
                    "boundingVolume": box_volume(6, 0, 0, 2, 2, 2),
                    "geometricError": 0.0,
                    "content": {"uri": "child.glb"},
                }
            ],
        },
    }
    write_tileset(os.path.join(GLB_DIR, "tileset.json"), root)


def gen_implicit_tileset():
    """Scenario B: 3D Tiles 1.1 implicit QUADTREE with a hand-written subtree."""
    os.makedirs(os.path.join(IMPLICIT_DIR, "tiles"), exist_ok=True)
    os.makedirs(os.path.join(IMPLICIT_DIR, "subtrees"), exist_ok=True)

    # Level 0: small red box at the origin (half 0.5).
    p3.make_glb(os.path.join(IMPLICIT_DIR, "tiles", "0_0_0.glb"),
                (0, 0, 0), 0.5, (1.0, 0.0, 0.0))
    # Level 1 quadrants (root box is 16x16 in x/y, quadrant centers +/-4).
    quads = [
        ((1, 0, 0), (-4, -4, 0), (0.0, 1.0, 0.0)),   # green
        ((1, 1, 0), (4, -4, 0), (0.0, 0.0, 1.0)),    # blue
        ((1, 0, 1), (-4, 4, 0), (1.0, 1.0, 0.0)),    # yellow
        ((1, 1, 1), (4, 4, 0), (1.0, 0.0, 1.0)),     # magenta
    ]
    for (level, x, y), center, color in quads:
        p3.make_glb(
            os.path.join(IMPLICIT_DIR, "tiles",
                         "%d_%d_%d.glb" % (level, x, y)),
            center, 1.0, color)

    # Subtree as plain JSON (no "subt" magic -> parsed by the JSON path).
    # constant 1 = every tile in this subtree exists and has content;
    # childSubtreeAvailability constant 0 = no deeper subtrees.
    subtree = {
        "tileAvailability": {"constant": 1},
        "contentAvailability": [{"constant": 1}],
        "childSubtreeAvailability": {"constant": 0},
    }
    with open(os.path.join(IMPLICIT_DIR, "subtrees", "0_0_0.subtree"),
              "w") as f:
        json.dump(subtree, f, indent=2)
        f.write("\n")

    root = {
        "asset": {"version": "1.1"},
        "extensionsUsed": ["3DTILES_implicit_tiling"],
        "extensionsRequired": ["3DTILES_implicit_tiling"],
        "geometricError": 100.0,
        "root": {
            "boundingVolume": box_volume(0, 0, 0, 8, 8, 8),
            "geometricError": 50.0,
            "refine": "ADD",
            "content": {"uri": "tiles/{level}_{x}_{y}.glb"},
            "extensions": {
                "3DTILES_implicit_tiling": {
                    "subdivisionScheme": "QUADTREE",
                    "subtreeLevels": 2,
                    "availableLevels": 2,
                    "subtrees": {
                        "uri": "subtrees/{level}_{x}_{y}.subtree"
                    },
                }
            },
        },
    }
    write_tileset(os.path.join(IMPLICIT_DIR, "tileset.json"), root)


def main():
    gen_glb_tileset()
    gen_implicit_tileset()


if __name__ == "__main__":
    main()
