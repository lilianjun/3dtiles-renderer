# ADR-0030: Tileset API alignment with cesium.js, phase 1 (P31)

## Status

Accepted (2026-09-30).

## Context

li asked to align the SDK's tileset API with cesium.js's `Cesium3DTileset`
as the reference, done in phases (P31–P35, see
`docs/tileset-api-roadmap.md`). A read-only survey of the cesium.js main
branch (report in `~/workspace/cesiumjs-tileset-api-report.md`) confirmed
the constructor options that have real cesium-native v0.64.0 semantics
versus the ones that are cesium.js-private traversal policy.

## Decision

Expose the mappable subset as `Renderer::TilesetOptions` (11 fields),
applied at `loadTileset()` construction onto
`Cesium3DTilesSelection::TilesetOptions`:

- `maximumScreenSpaceError` (16), `forbidHoles` (false),
  `preloadAncestors`/`preloadSiblings` (true), `enableFrustumCulling`/
  `enableFogCulling` (true), `maximumSimultaneousTileLoads`/
  `loadingDescendantLimit` (20), `enableLodTransitionPeriod` (false),
  `lodTransitionLength` (1.0), `ellipsoidRadii` (WGS84).
- Live-mutable: `setMaximumScreenSpaceError()` /
  `maximumScreenSpaceError()` / `currentTilesetOptions()`, following the
  existing stash-then-forward pattern of `setMaxCachedBytes` (P19): a
  value set before any load applies to the next `loadTileset()`; a value
  set later writes straight into `Tileset::getOptions()` (read every
  `updateViewGroup`), so it takes effect on the next frame with no reload.
- Deliberately NOT replicated: `skipLevelOfDetail`, `dynamicScreenSpaceError`,
  `foveated*`, `progressiveResolutionHeightFraction` — cesium.js-private
  traversal policy with no cesium-native mapping. Style/customShader,
  clipping planes, ion, picking/collision, multi-tileset composition are
  separate later phases, not silently dropped.

Validation at `loadTileset()` (fail-fast, live tileset untouched on
failure — same contract as P22/P23):

- negative/NaN SSE → default 16 (same as the live setter);
- `maximumSimultaneousTileLoads`/`loadingDescendantLimit` = 0 → default 20
  (0 simultaneous loads makes cesium-native's load pump exit early
  forever — a silent deadlock, not a legal setting);
- non-positive/NaN `lodTransitionLength` → 1.0 (it feeds a division);
- non-finite or ≤ 0 `ellipsoidRadii` → the load FAILS with `lastError`
  (silently substituting WGS84 for user-supplied radii would be
  dishonest).

`shutdown()` does not clear the pending SSE, matching the existing
`g_pendingMaxCachedBytes` semantics.

## Consequences

- Public API grows: `TilesetOptions`, the two-arg `loadTileset()`
  overload, `setMaximumScreenSpaceError()`, `maximumScreenSpaceError()`,
  `currentTilesetOptions()`. Single-arg `loadTileset()` is unchanged
  (default options).
- New test `tileset_options` (28th ctest): behavioral proof that SSE
  changes the render selection both at construction (3 tiles → root only
  at SSE=100000) and live mid-run (`--set-sse-at-frame 30:100000`
  flips frames 20–29 → 31–50), plus full 11-field passthrough and the
  validation paths (negative SSE normalizes; bogus radii fail the load).
- Demo gains `--max-sse`, `--set-sse-at-frame N:V`, `--preset-sse N`
  (pre-load stash path), `--no-frustum-culling`,
  `--print-tileset-options`, `--ellipsoid-radii` (test/dev hooks only).
- Zero SDL in `libtiles_renderer.a` re-verified (`nm | grep -c SDL_` = 0).
