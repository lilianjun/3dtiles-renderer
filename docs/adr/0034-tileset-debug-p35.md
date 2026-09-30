# ADR-0034: Tileset API alignment with cesium.js, phase 5 (P35 — debug switches)

## Status

Accepted (2026-10-01).

## Context

P31 mapped cesium.js's `Cesium3DTileset` constructor options, P32 added the
7 tileset events, P33 added show/modelMatrix/read-only state, P34 added
cache/statistics. Phase 5 (P35, see `docs/tileset-api-roadmap.md`)
replicates a subset of cesium.js's `debug*` switches:

- `debugShowBoundingVolume`
- `debugWireframe`
- `debugShowUrl`
- (evaluated) `debugColorizeTiles`

## Decision

### Provided

**`setDebugShowBoundingVolume(bool)` / `isDebugShowBoundingVolume()`**

Draws each loaded tile's bounding-box hierarchy as lines, via
`gltfio::FilamentAsset::getWireframe()` (a lazily-created `LINES`
renderable owned by the asset; the caller must not destroy it).

Implementation:

- New `wireframeInScene` flag on `TileRenderData`.
- `Impl::updateTileWireframe()` adds/removes the wireframe entity to match
  the tile's scene membership and the flag.
- Called from `updateTileVisibility()` (per-frame, covers tiles entering
  or leaving the scene) and from `Impl::updateWireframeVisibility()` (walks
  all loaded tiles; called when the flag toggles so the change applies
  immediately to already-loaded tiles).
- `Renderer` layer uses stash-then-forward across `loadTileset()`, same
  pattern as the P33 display flags.

The wireframe follows the tile's render transform because it is part of
the asset's entity hierarchy (the root transform is applied to the
asset's root instance).

**`setDebugShowUrl(bool)` / `isDebugShowUrl()`**

cesium.js renders the tile URL as on-screen text. This SDK has no text
renderer, so the honest equivalent is logging: when a tile becomes
visible and the flag is on, its tile ID (via
`TileIdUtilities::createTileIdString`, e.g. `root.glb`) is written to
stderr with a `[tiles_renderer] debugShowUrl:` prefix. Logged once per
tile per visibility transition, not per frame.

### Deliberately NOT provided

**`debugWireframe`** — Filament v1.77 has no runtime wireframe toggle for
gltfio materials. Rasterization mode (fill vs lines) is baked into the
material at build time (`matc`); `Material`, `RenderableManager`, and
`MaterialEnums` expose no runtime polygon-mode switch. Emulating it would
require rebuilding every tile's materials with a line-mode variant or
hand-rolled wireframe geometry — disproportionate cost for a debug flag.
The header documents this; the roadmap marks it as evaluated/won't-do.

**`debugColorizeTiles`** — evaluated and deferred. Runtime recoloring
would require enumerating and patching every loaded asset's material
instances (`gltfio::FilamentAsset` exposes no public material-instance
enumeration API in v1.77). Possible via a custom `MaterialProvider`, but
that would fork the gltfio material pipeline — out of scope for a debug
flag. Marked as evaluated/deferred in the roadmap.

## Consequences

- Public API: 4 new static methods on `Renderer` (2 setters, 2 getters).
- `TilesetRenderer` gains matching methods; `Impl` gains two flags;
  `TileRenderData` gains `wireframeInScene`.
- Demo: `--debug-bounding-volume`, `--debug-show-url` test hooks.
- Tests: `tests/tileset_debug_test.py` (new CTest `tileset_debug`):
  Part A locks the wireframe pixel delta (2424/480000 on the p3 fixture);
  Part B locks the URL log lines; Part C checks both flags together don't
  crash.
- Behavior when flags are off: zero (wireframe entities are never added;
  no logging).
- Honest boundary: `debugShowUrl` logs to stderr, not on-screen text;
  documented in the header.
