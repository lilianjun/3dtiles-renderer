# ADR-0010: cmpt composite support via recursive conversion (P9)

Date: 2026-09-29
Status: accepted

## Context

cmpt (Composite) was the last 3D Tiles 1.0 tile format without real
support (b3dm in P5, i3dm in P7, pnts in P8). A cmpt is only a
container: a 16-byte header followed by concatenated inner tiles
(b3dm / i3dm / pnts / glb), each with its own header and byteLength.
cesium-native's `registerAllTileContentTypes()` already registers the
composite converter, but it had never been verified end-to-end.

## Investigation

We read cesium-native v0.64.0
`Cesium3DTilesContent/src/CmptToGltfConverter.cpp` (not memory) and
confirmed:

- cmpt header is **16 bytes**: magic `"cmpt"`, version 1, byteLength,
  tilesLength. (Contrast: i3dm 32 bytes, b3dm/pnts 28 bytes — another
  instance of the "verify header sizes from source" lesson.)
- Each inner tile is sliced by its own 12-byte header
  (magic/version/byteLength) and dispatched **recursively** through
  `GltfConverters::convert`, so any registered inner format works
  (b3dm, i3dm, pnts, plain glb).
- With multiple successful inner results, the models are merged with
  `Model::merge` into **one** `CesiumGltf::Model`; errors merge too.
- Corrupt inputs (short file, bad magic, unsupported version,
  `byteLength` overrun, inner tile overrunning `byteLength`) yield
  converter *warnings* and an empty/partial result — no exceptions,
  no crashes.

The decisive experiment: a self-generated deterministic cmpt
(`tests/data/gen_p9_cmpt_tileset.py`) packing a b3dm (4 m orange box),
a pnts (64 green points), and an i3dm (6 teal instances) rendered
through the **existing** SDK path with **zero code changes**:

- 1 tile rendered (the merged composite is a single tile);
- 4915 orange (b3dm) pixels, 64/64 green (pnts) points at 1 px each,
  8117 teal (i3dm) pixels in 7 distinct blobs (6 instances expanded);
- all three contents visible in the same frame.

## Decision

cmpt support needs **no SDK code change**: the existing generic
converted-model path already covers it —

- P5's `CESIUM_RTC` extraction (works per merged model);
- P7's multi-buffer merge into a single GLB BIN chunk (the i3dm part of
  the merged model emits multiple buffers — this would hit the same
  `glb BIN length mismatch` bug P7 fixed if not for that fix);
- P7's CPU expansion of `EXT_mesh_gpu_instancing` (the i3dm inner tile
  contributes that extension; `upAxisFix` triggers on the extension
  name regardless of which inner tile added it);
- P8's native POINTS rendering (the pnts inner tile).

A fixture-layout lesson: the first pnts cluster at x=10 fell half out
of the demo's fixed orbit-camera frame (only 28/64 points visible);
moving it to x=7 kept all 64 points in frame. Camera framing is a
fixture concern, not an SDK concern.

## Consequences

- New test coverage: `tests/data/gen_p9_cmpt_tileset.py` (deterministic,
  no network), `tests/cmpt_test.py` (pixel assertions for all three
  inner tiles in one frame), `tests/CMakeLists.txt` registration
  (`cmpt_tileset_screenshot`), fault-injection cases J/K/L (truncated
  inner pnts tile, wrong magic, `tilesLength` larger than the actual
  inner tiles — all graceful skips), and `sanitizer_cmpt` in the
  linux-asan gate.
- Honest boundaries:
  - A cmpt can pack any mix of registered formats, including plain glb
    inner tiles — only the b3dm+pnts+i3dm mix is pixel-verified.
  - A cmpt whose inner tiles fail conversion converts to an empty
    result and is skipped like any other bad tile (no retry).
  - Android/Windows/iOS still have no on-device verification.

## Known third-party bug (cesium-native v0.64.0, not worked around)

`CmptToGltfConverter` reports *all* of its structural failures with
`emplaceWarning` (never `emplaceError`), leaving `result.model` empty.
But `TilesetJsonLoader::loadTileContent`'s continuation only checks
`if (result.errors)` — and `ErrorList::operator bool()` is
`hasErrors()`, false for warnings-only — then unconditionally
dereferences `std::move(*result.model)`. Dereferencing an empty
`std::optional<Model>` is UB; in practice it segfaults the process.

Verified crash inputs (all exit 139 / SIGSEGV on Linux):
  - cmpt shorter than the 16-byte header;
  - cmpt with `version != 1`;
  - cmpt whose `byteLength` exceeds the available bytes;
  - cmpt with `tilesLength > 0` but no convertible inner tiles
    (e.g. `byteLength == 16`).

The b3dm/i3dm/pnts converters use `emplaceError` for structural
failures, so they take the `createFailedResult` path and never hit
this. A wrong-magic `.cmpt` file is also safe: no converter is
registered for the `.cmpt` extension, so it falls through to the
JSON-parse path and fails gracefully.

The crash site is inside cesium-native's loader continuation, before
the SDK's `IPrepareRendererResources::prepareInLoadThread` ever runs,
so the SDK has no clean interception point. Guarding it would require
either patching cesium-native (forbidden) or wrapping the
`IAssetAccessor` to pre-parse/rewrite cmpt bytes (fragile duplication
of converter logic) — neither is acceptable. Recorded here instead,
following the ADR-0007 precedent for third-party issues.
