# ADR-0033: Tileset API alignment with cesium.js, phase 4 (P34 — cache / statistics)

## Status

Accepted (2026-10-01).

## Context

P31 mapped cesium.js's `Cesium3DTileset` constructor options, P32 added the
7 tileset events, P33 added show/modelMatrix/read-only state. Phase 4 (P34,
see `docs/tileset-api-roadmap.md`) replicates the remaining cache,
statistics and introspection surface:

- `totalMemoryUsageInBytes`
- `trimLoadedTiles`
- `hasExtension`
- (evaluated) `loadTilesetAsync`

## Decision

- `Renderer::totalMemoryUsageInBytes()` returns cesium-native
  `Tileset::getTotalDataBytes()` — tile + raster content bytes currently
  held. It is the same value as `TileStats::bytesLoaded`, queryable at any
  time (0 with no tileset). Documented honestly in the header: content
  bytes, NOT a GPU memory estimate.
- `Renderer::trimLoadedTiles()` is a render-thread one-shot request. The
  next `updateTiles()` briefly zeroes `maximumCachedBytes` around
  `loadTiles()` so cesium-native's internal
  `unloadCachedBytes(0, tileCacheUnloadTimeLimit)` evicts everything not in
  use, then restores the budget. No private/internal cesium-native API is
  touched (`unloadCachedBytes` on `TilesetContentManager` is internal-only;
  `Tileset::loadTiles()` calls it every frame as part of the public update
  flow). Tiles in use are never evicted (cesium-native guarantee);
  evictions flow through the P32 `tileUnload` event path. Honest note: a
  trim when every loaded tile is in the current view legitimately frees
  nothing.
- `Renderer::hasExtension(name)` checks the tileset.json top-level
  `extensionsUsed` (exact string match), cached from
  `Tileset::getMetadata()` at `loadTileset()` time. False with no tileset.
- `loadTilesetAsync` is deliberately NOT provided. The SDK is a
  single-threaded render model (every method except `version()` must run on
  the render thread); a true async tileset constructor would need a full
  thread-safety rework of `Impl` and the Filament resources. Hosts that
  need non-blocking loads call `loadTileset()` on a worker thread and
  marshal `renderFrame()` to the render thread. Documented in the header.
- P32 hardening done in the same phase: per-tile transition events are now
  collected during the frame's state walk and dispatched after the walk
  (the ADR-0031 "collect first, then dispatch" claim is now literally
  true; previously callbacks fired inline mid-walk), and the header
  documents the no-throw contract for callbacks. ADR-0031/0032
  consequences, the CHANGELOG P32/P33 entries, and the
  `tileset_properties_test.py` docstring were corrected to match reality
  (walk always runs since P33; ctest was 29/30 not 30/30 —
  `weak_network` fails on a clean tree too).

## Verification

- `tests/tileset_cache_test.py` (ctest `tileset_cache`, #31, 3 parts):
  A `totalMemoryUsageInBytes()=1944` equals `TileStats.bytesLoaded` after
  a settled p3 load; B `--set-sse-at-frame 30:100000` narrows the view to
  root only, then `--trim-at-frame 60` drops `1944 → 648` bytes; C
  `hasExtension("3DTILES_content_gltf")=1` /
  `hasExtension("KHR_test_extension")=1` on the new
  `tests/data/p34_extensions_tileset` fixture (declares both in
  `extensionsUsed`), `=0` for an undeclared name and for the p3 fixture
  (no `extensionsUsed`).
- `sanitizer_cache` scene (ASan+LSan+UBSan) clean.
- `tileset_events` + `tileset_properties` still pass after the
  collect-then-dispatch refactor (event order preserved).
- Full `ctest` 29/30 (`weak_network` pre-existing, fails on clean tree);
  MinGW SDK pre-screen passes; SDK has 0 SDL symbols.
- Demo gains `--trim-at-frame N` and `--has-extension NAME` test hooks;
  `--print-tileset-info` now also prints `memoryBytes` and the
  `hasExtension("...")` verdict.

## Consequences

- `Impl` gains `trimRequested` (one-shot flag) and `extensionsUsed`
  (cached vector). No new threads, no new locks.
- The trim path temporarily mutates `Tileset::getOptions()` (non-const
  overload, same live-mutation pattern as P19/P31) inside the render
  thread only; the budget is restored in the same frame even if
  `loadTiles()` throws (it cannot — but the restore is unconditional
  code, not exception-dependent).
- `totalMemoryUsageInBytes()` callers must not treat the value as GPU
  VRAM; the header says so explicitly.
