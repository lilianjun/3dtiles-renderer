# ADR-0009: pnts real support via native Filament POINTS rendering (P8)

Date: 2026-09-29
Status: accepted

## Context

P5 (b3dm) and P7 (i3dm) closed the textured-mesh tile formats. pnts
(Point Cloud) was the remaining 3D Tiles 1.0 legacy format. The concern
before doing the work was whether Filament v1.77's gltfio actually
renders `POINTS` primitives at a usable size/color, or whether we would
need a CPU-expansion fallback (like P7's instancing expansion) or a
custom point-sprite shader.

## Investigation

We read cesium-native v0.64.0
`Cesium3DTilesContent/src/PntsToGltfConverter.cpp` and confirmed:

- pnts header is **28 bytes**: magic, version, byteLength,
  featureTableJSONByteLength, featureTableBinaryByteLength,
  batchTableJSONByteLength, batchTableBinaryByteLength.
  (Note: unlike i3dm there is no glTF-format field — i3dm's header is
  32 bytes; the P7 lesson about verifying header sizes early applied
  again.)
- The converter emits exactly **one node / one mesh / one primitive**
  with `mode = POINTS`; `POSITION` (VEC3 float) with min/max;
  `RGB`/`RGBA` converted sRGB→linear into `COLOR_0` (VEC3/VEC4 float);
  `KHR_materials_unlit` when there are no normals (RGBA → BLEND
  material); `RTC_CENTER` becomes a `CESIUM_RTC` extension; the node
  matrix is pre-set to `Z_UP_TO_Y_UP`.

The decisive experiment: a self-generated 420-point pnts
(`tests/data/gen_p8_pnts_tileset.py`) rendered through the **existing**
SDK path with **zero code changes**. Screenshot pixel analysis showed
Filament's gltfio + ubershader render POINTS natively:

- all 420 points present, each exactly **1 pixel**, at its **exact sRGB
  color** (200×`(255,0,0)`, 120×`(0,255,0)`, 100×`(0,0,255)`; the image
  contains exactly 4 distinct colors).
- `COLOR_0` is honored; the unlit material reproduces colors exactly.
- The converter's `Z_UP_TO_Y_UP` node matrix is applied natively by
  gltfio — no SDK-side conjugation needed (unlike i3dm, there are no
  per-instance transforms for the converter to conjugate, so no P7-style
  fixup is required).

## Decision

pnts support needs **no SDK code change**: the existing generic
converted-model path already covers it —

- P5's `CESIUM_RTC` extraction (double-precision `rtcCenter`, stripped
  from the GLB before gltfio sees it);
- P7's multi-buffer merge into a single GLB BIN chunk (the pnts
  converter emits separate POSITION / COLOR_0 / index buffers, which
  would otherwise hit the same `glb BIN length mismatch` bug P7 fixed);
- `upAxisFix` only triggers on `EXT_mesh_gpu_instancing`, so pnts keeps
  the identity fix.

New test coverage (`tests/data/gen_p8_pnts_tileset.py`,
`tests/pnts_test.py`, `pnts_tileset_screenshot`, `sanitizer_pnts`):

- main cloud: 200 red (Fibonacci sphere) + 120 green (ring) +
  100 blue (10×10 grid) — per-color pixel counts (~1 px/point);
- 60000-point cloud: loads and renders without crashing
  (correctness only, no perf claim on Mesa);
- tile-transform rebase (123456789 m): near/far screenshots
  **bit-identical**;
- `RTC_CENTER` at ECEF magnitude
  (`[1210000.0, -4736290.5, 4081600.0]`): screenshot **bit-identical**
  to the no-RTC reference. The reference keeps the byte-identical cloud
  without `RTC_CENTER` and the bounding volume at the origin; the RTC
  variant's huge translation is exactly representable in float32, so it
  cancels exactly in the double-precision rebase;
- fault injection (G/H/I): truncated pnts header, wrong magic,
  `POINTS_LENGTH` past the POSITION/RGB data — all skipped gracefully.

## Boundaries (honest)

- Point size is **1 px**, fixed by the gltfio ubershader — no SDK knob
  for larger points; dense clouds subsample naturally by rasterization.
- Mesa software rendering proves correctness only; no frame-rate or
  throughput claim for large clouds on real GPUs.
- pnts `RGB565` / `CONSTANT_RGBA` / `NORMAL` / quantized / batch-table
  variants are supported by the converter but not pixel-tested (only
  `RGB` + `RTC_CENTER` paths are asserted); unknown variants inherit the
  converter's behavior, not SDK bugs.
- As with all formats: Linux/OpenGL only pixel-verified; Android /
  Windows / iOS have no on-device verification.
