# ADR-0027: SDK-side guard for the cmpt empty-model SIGSEGV (upstream #1457)

## Status

Accepted (2026-09-30). Implements the G2 fix; upstream
`CesiumGS/cesium-native#1457` remains open.

## Context

G2 (ADR-0023) was the only P0 engineering item: a structurally corrupt
cmpt tile crashed the host process. The P9 ADR (0010) recorded the
mechanism but concluded "the SDK has no clean interception point".
P28 re-examined that conclusion and found one.

Root cause, re-verified on master (cesium-native v0.64.0):

- `CmptToGltfConverter::convert` reports **all** structural failures
  via `emplaceWarning` (never `emplaceError`), leaving
  `result.model` (a `std::optional<Model>`) empty.
- The continuation in
  `Cesium3DTilesSelection/src/TilesetJsonLoader.cpp:1306` checks only
  `if (result.errors)` — `ErrorList::operator bool()` is `hasErrors()`,
  false for warnings-only — then does `std::move(*result.model)` at
  `:1312`. Dereferencing an empty optional is UB; in practice it
  SIGSEGVs (locally reproduced: exit 139 on `version != 1`; exit 136 on
  short header / oversized byteLength / tilesLength-with-no-inner-tiles).
- The crash site is reached through the public
  `GltfConverters::getConverterByMagic` dispatch, i.e. after the SDK's
  `IAssetAccessor` has returned the bytes and before the SDK's
  `IPrepareRendererResources::prepareInLoadThread` ever runs.

Verified crash inputs (all `file://` URIs; relative-URI tilesets 404
before reaching the converter and do NOT reproduce):

- cmpt with `byteLength == 16`, `tilesLength == 1`
  (no convertible inner tiles);
- cmpt with `version != 1`;
- cmpt with `byteLength` exceeding the available bytes;
- cmpt shorter than the 16-byte header.

## Decision

Wrap the five magic-dispatched converters (`glTF`, `b3dm`, `cmpt`,
`i3dm`, `pnts`) with a hardening shim, registered via the public
`GltfConverters::registerMagic` — which **overwrites** any previous
registration — immediately after the SDK's one-time
`registerAllTileContentTypes()` call (`src/tileset.cpp`,
`std::call_once`). The shim (new files `src/converter_guard.h` /
`src/converter_guard.cpp`, internal, no public API change) calls the
original converter, then promotes "empty model without a hard error"
to a real error via `emplaceError`. The tile then takes the existing
graceful `TileLoadResult::createFailedResult` path (failed-tile
counter, process alive).

Why this is clean (and why P9's "no interception point" was wrong):

- No cesium-native patch, no loader fork: the override uses only
  public cesium-native API (`registerMagic`, the converter classes'
  public `convert` functions).
- No `IAssetAccessor` byte rewriting: the shim does not duplicate any
  converter parsing logic; it only inspects the already-produced
  result.
- Provably behavior-preserving for valid tiles: a usable conversion
  always produces a model, so the guard's condition
  (`!result.model && !result.errors`) can only fire on paths that are
  UB downstream today. Wrapping all five magics (not just `cmpt`)
  fixes the bug *class* — i3dm/pnts have dozens of warning paths that
  were never individually audited.

## Consequences

- New CTest cases P/Q/R/S in `tests/fault_test.py` (the four verified
  crash inputs): all exit 0, bad tile skipped, no crash. ASan/UBSan
  builds run the same inputs with zero reports.
- Valid cmpt still renders: `cmpt_test.py` (P9 three-color composite)
  passes unchanged.
- `BinaryToGltfConverter` (glTF/glb) was audited and needs no guard:
  it propagates `GltfReader` errors, so an empty model already implies
  a hard error. It is still wrapped for uniformity; the guard never
  fires for it.
- Honest boundaries:
  - The guard converts a crash into a *failed tile*, not into a
    rendered tile — corrupt content is still corrupt.
  - It does not fix upstream; if cesium-native later changes
    `registerMagic` to reject overwrites, the `std::call_once`
    registration must be revisited (it would fail loudly at worst,
    not silently).
  - The guard is platform-independent C++ with no platform branches:
    it compiles and activates on every build with
    `TILES_WITH_CESIUM_NATIVE` (Linux/Windows/Android/iOS). The
    no-op branch in `converter_guard.cpp` exists only for
    hypothetical cesium-off configurations (today: the WASM stub,
    which loads no tiles anyway).
  - Pixel verification is Linux/Mesa only, like every other phase.
- Upstream #1457 stays open; the guard is removed if/when upstream
  makes empty-model results carry errors.
