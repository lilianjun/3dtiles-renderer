# ADR-0028: Draco (KHR_draco_mesh_compression) — already in the tree, now verified

## Status

Accepted (2026-09-30). Closes G7 as "verified working"; no code changes.

## Context

G7 (ADR-0023) stated: "no Draco decoder in the dependency tree;
Draco-compressed glTF content will fail to decode" and estimated "draco
library on Linux/Windows/Android/iOS/WASM toolchains + gltfio decoder
hookup". P29 re-examined that premise from primary sources and found it
wrong on every count.

## Findings (all verified first-hand)

1. **draco is already a dependency.** cesium-native v0.64.0 lists
   `"draco"` in its `vcpkg.json` manifest (line 17) and does an
   unconditional `find_package(draco CONFIG REQUIRED)` in its top-level
   `CMakeLists.txt` (:383). It is provisioned by cesium-native's own
   ezvcpkg flow on **every** native platform — no per-toolchain work was
   ever needed. CI went green on Linux/Windows/Android/iOS with this in
   the tree, so draco already builds on all four.
2. **The decode is already wired.** `CesiumGltfReader` compiles
   `src/decodeDraco.cpp` (29 draco symbols in our built
   `libCesiumGltfReader.a`) and `GltfReader::readGltf` calls it whenever
   `GltfReaderOptions::decodeDraco` is true — the default
   (`GltfReader.h:112`). The tile pipeline
   (`TilesetJsonLoader.cpp:1291`) builds its options via
   `TilesetContentOptions::toGltfReaderOptions()`, which default-constructs
   them, so Draco decoding is on for every tile load.
3. **No gltfio hook is needed** (the estimated "gltfio decoder hookup"
   does not exist as a requirement). cesium-native decodes
   `KHR_draco_mesh_compression` into plain accessors/bufferViews
   *before* the model reaches gltfio; gltfio never sees compressed data.
   (The `libdracodec.a` shipped in Filament's prebuilt package is
   irrelevant to this path.)
4. **WASM is out of scope as before** (stub build, ADR-0006/G9).

## Verification (P29)

New deterministic fixture `tests/data/p29_draco_tileset/` (generator:
`tests/data/gen_p29_draco_tileset.py`; encoder is the `draco_encoder`
built by cesium-native's own ezvcpkg, `-qp 14 -qn 10`): a 10 m box whose
mesh is Draco-compressed (`root.glb`), plus a byte-twin with the mesh
uncompressed (`plain.glb`) — same geometry, material, scene; the only
variable is Draco.

- Draco tile renders: 1 tile, 11,351 gray-box pixels, exit 0.
- Draco vs uncompressed twin: **0/480,000 pixels differ**
  (max abs diff 0.0) — pixel-verified end to end.
- Corrupt Draco bitstream: demo logs `Draco decoding failed: Failed to
  decode geometry data.`, exits 0, tile carries no geometry — graceful,
  no crash. This is upstream's warnings-only behavior; we do not fork it.
- New CTest `draco` passes; `sanitizer_draco` (ASan/LSan/UBSan) passes
  with zero findings.

## Consequences

- G7 is closed: Draco-compressed tilesets work on all native platforms
  with zero SDK code. The "Takes" estimate in ADR-0023 is retracted.
- Lesson for future gap reviews: check `vcpkg.json` / the actual built
  archives before declaring a dependency missing — G7 was written from
  the (incorrect) assumption that "we didn't add it, so it isn't there".
- If a future tileset needs Draco *point clouds* (`-point_cloud`), that
  path is untested and remains out of scope.
