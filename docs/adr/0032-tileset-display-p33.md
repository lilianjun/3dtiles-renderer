# ADR-0032: Tileset API alignment with cesium.js, phase 3 (P33 — show / modelMatrix / read-only state)

## Status

Accepted (2026-09-30).

## Context

P31 mapped cesium.js's `Cesium3DTileset` constructor options; P32 added the
7 tileset events. Phase 3 (P33, see `docs/tileset-api-roadmap.md`) replicates
cesium.js's display/transform surface and its read-only tileset state:

- `show`, `modelMatrix`, `preloadWhenHidden`
- `tilesLoaded`, `boundingSphere`, `timeSinceLoad` (`timeSinceLoadMs`),
  `rootTile` (`rootTileId`)

The hard part is `modelMatrix`: our P5 rebase subtracts a world-space
`localOrigin` from every tile transform before the float32 conversion, and
the traversal (tile selection/LOD) uses cesium-native's authored tile
transforms. A naive "just set the Filament transform" would desync
selection from rendering.

## Decision

- `Renderer::setShow(bool)` / `isShow()`,
  `Renderer::setPreloadWhenHidden(bool)` / `isPreloadWhenHidden()`,
  `Renderer::setModelMatrix(const double[16])` / `modelMatrix(double[16])`.
  All three persist across `loadTileset()` (stash-then-forward, same pattern
  as P19/P31/P32); the new `TilesetRenderer` gets the stash before loading.
- `show=false` removes all tile entities from the Filament scene. With
  `preloadWhenHidden=true` the traversal keeps running (tiles load, stats
  and events still fire) but nothing is ever added to the scene; with it
  false the traversal is skipped entirely and the tileset freezes.
- `modelMatrix` is stored (double, column-major) on the `Impl` and on the
  `FilamentPrepareResources`. The per-tile render transform is composed in
  one place, `FilamentPrepareResources::composeRenderTransform`:

      render = modelMatrix * tileTransform * translate(rtcCenter)
               * upAxisFix - localOrigin

  `localOrigin` (P5 rebase) is deliberately a **fixed** float32-precision
  device — it does NOT move with `modelMatrix`, so the tileset visibly moves
  relative to the orbit camera target (the cesium.js behavior). An earlier
  draft subtracted the *transformed* origin (`modelMatrix * localOrigin`),
  which exactly cancels the user's translation (verified: 0 pixels changed
  for tx=20) — that draft was rejected.
- `setModelMatrix` re-applies immediately to already-loaded tiles by walking
  the tree and recomposing each tile's Filament transform from its stored
  double-precision pieces (`rtcCenter`, `upAxisFix` in `TileRenderData` —
  they were previously recomputed-and-dropped in `prepareInMainThread`).
  Tiles prepared later pick it up from the prepare resources. The
  `prepareResources` of a *new* load gets the stashed matrix explicitly,
  because the stash is forwarded to the fresh `TilesetRenderer` before its
  prepare resources exist (a real bug caught during P33 testing: tx=20 had
  no effect until this sync was added).
- Read-only state: `tilesLoaded()` (same condition as P32's
  `allTilesLoaded`, queryable without callbacks), `boundingSphere()`
  (root bounding volume -> sphere, `modelMatrix` applied to the center,
  radius scaled by the matrix's maximum axis scale), `timeSinceLoadMs()`
  (steady clock from the first `update()` after load), `rootTileId()`
  (unwraps cesium-native's internal empty-ID wrapper tile — the real
  tileset.json root is its single child — and returns that child's ID;
  `""` when no tileset is loaded).
- P32 hardening done alongside: `tileStates` is now keyed by
  `const Tile*` instead of the ID string (the traversal root and implicit
  tiles can share an empty ID, which used to collide), and the event tree
  walk runs every frame (it also feeds P33's `tilesLoaded()`); only the
  transition bookkeeping allocates.
- P31 hardening: `setMaximumScreenSpaceError` now rejects `+inf` as well
  (`std::isfinite`), at both the stash and the load-normalization sites.

## Honest differences from cesium.js

- Tile selection/LOD still uses the tileset's **authored**
  (untransformed) tile transforms — cesium-native has no runtime root
  transform API, and camera-space SSE is meaningless under an arbitrary
  matrix (see the design note in the planning doc). For large placements,
  set the matrix before (or right after) `loadTileset`.
- `timeSinceLoad` is integer milliseconds (`timeSinceLoadMs`), not a
  `JulianDate` duration.
- `rootTileId` returns the tileset.json root tile's ID string; cesium.js's
  `rootTile` returns the tile object. (Our tiles are not public objects.)
- `tilesLoaded` is true when nothing is in flight; failed tiles do not
  count as in-flight (same as cesium.js's `allTilesLoaded`).

## Verification

- `tests/tileset_properties_test.py` (ctest `tileset_properties`, #30):
  A show=false -> uniform clear color; B hidden+preload -> `tilesLoaded=1`
  but still clear; C tx=20 -> 275,670 pixels move and bounding sphere
  center=(20,0,0); D read-only state matches the p3 fixture (center
  (0,0,0), radius sqrt(75)=8.66025, `rootTileId="root.glb"`,
  `timeSinceLoadMs>0`); E live setModelMatrix recomposition is
  bit-identical to load-time application.
- `sanitizer_properties` (ASan+LSan+UBSan) clean.
- Full `ctest` 29/30 (`weak_network` fails on a clean tree too —
  pre-existing environment issue: the 3 GLB requests reach the slow
  server but never complete client-side; HTTP stack untouched by P33);
  MinGW SDK pre-screen passes; SDK has 0 SDL symbols.

## Consequences

- `TileRenderData` now carries `rtcCenter`/`upAxisFix` (a few dozen bytes
  per loaded tile) to support live matrix recomposition.
- The event tree walk is no longer gated on registered callbacks; it runs
  every frame (pointer chasing, no per-tile allocation when no callbacks).
- P34/P35 remain per `docs/tileset-api-roadmap.md`.
