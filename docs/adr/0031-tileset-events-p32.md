# ADR-0031: Tileset API alignment with cesium.js, phase 2 (P32 — events)

## Status

Accepted (2026-09-30).

## Context

P31 mapped cesium.js's `Cesium3DTileset` constructor options. Phase 2
(P32, see `docs/tileset-api-roadmap.md`) replicates the 7 cesium.js tileset
events: `tileLoad`, `tileUnload`, `tileFailed`, `tileVisible`,
`loadProgress`, `allTilesLoaded`, `initialTilesLoaded`.

cesium-native v0.64.0 exposes only one event hook,
`TilesetOptions::loadErrorCallback` (fired from
`TilesetContentManager::propagateTilesetContentLoaderResult` for
tileset.json / layer.json / implicit-subtree failures, contract allows a
worker thread). Per-tile load completion has no callback: the only public
signal is the `TileLoadState` transition observed by walking the tile tree
(`Tileset::getRootTile()`). The tile content URL is generally not
recoverable from a `Tile` (`TileID` covers only the ID types); only
string tile IDs carry a usable `url`.

## Decision

- `Renderer::TilesetEventCallbacks` (7 `std::function` members) +
  `Renderer::setEventCallbacks()` / `clearEventCallbacks()`. Callbacks
  persist across `loadTileset()` (stash-then-forward, same pattern as
  P19/P31) and are stored on the live `TilesetRenderer`, so each new load
  inherits the current set.
- All callbacks fire on the render thread, inside `renderFrame()` after
  traversal, in this deterministic order per frame:
  1. `tileLoad` / `tileUnload` / `tileFailed` (state transitions),
  2. `tileVisible` (only walked when the callback is registered),
  3. `loadProgress` (only when the counts changed since last frame),
  4. `allTilesLoaded` (every frame the view is fully loaded — matches
     cesium.js, which raises it per frame too),
  5. `initialTilesLoaded` (once per load cycle, then the flag resets on
     the next `loadTileset()`).
- State-transition detection: one tree walk per frame compares each
  tile's `TileLoadState` against the previous frame's (`Done` in →
  `tileLoad`; `Done` out → `tileUnload`; `Failed` /
  `FailedTemporarily` in → `tileFailed`). Keyed by the ID string from
  `TileIdUtilities::createTileIdString`; the traversal root yields an
  empty string and is reported as such (consistent with the existing
  `selectedTileIds()` behavior).
- `tileFailed` also drains `loadErrorCallback` (queued under a mutex in a
  shared struct the callback owns — the callback can outlive the Impl
  during teardown, so it never touches `this`).
- `loadTileset()` replacing a loaded tileset fires `tileUnload` for the
  old tileset's loaded tiles synchronously before the old
  `TilesetRenderer` is destroyed. A failed load leaves the old tileset
  untouched (P22), so no unload events fire on failure.
- The traversal root tile has an empty ID string (this is existing
  `createTileIdString` behavior, consistent with `selectedTileIds()`);
  it fires `tileLoad`/`tileUnload` like any other tile. Event consumers
  that only care about content tiles should skip empty IDs.
- Reentrancy rule: callbacks must not call mutating `Renderer` APIs
  (`loadTileset`, `shutdown`, setters); read-only queries are safe. SDK
  code collects the events first, then dispatches, so a misbehaving
  callback cannot corrupt the per-tile state map mid-walk.

Semantics defined relative to cesium.js:

- `pendingRequests` in `loadProgress` = worker + main-thread traversal
  queue lengths; `tilesProcessing` = tiles in `ContentLoading` /
  `ContentLoaded`. cesium.js's counter is internal; this is the closest
  observable equivalent in cesium-native.
- `TileEventInfo.url` is best-effort: populated only when the tile ID is
  a string (e.g. external tilesets); plain content tiles report an empty
  URL because cesium-native does not expose it from `Tile`.
- Per-tile failure messages are generic
  (`"tile content failed to load"`); detailed reasons live in
  `loadErrorCallback` messages for tileset.json-level failures.

## Consequences

- One extra tree walk per frame while any callback is registered (free
  when `TilesetEventCallbacks` is empty); `tileVisible`'s selection walk
  is skipped unless registered.
- Demo gains `--event-log` (streams `[event]` lines) and
  `--clear-events-at-frame N` (test hook for `clearEventCallbacks`).
  Also fixed a latent P22 demo bug found by the P32 test:
  `--switch-at-frame` re-fired when `renderFrame()` returned false
  (`rendered` doesn't advance on failed frames), causing duplicate
  `tileUnload` events — switches are now one-shot.
- New ctest `tileset_events` (4 parts: steady load, corrupt-child
  `tileFailed`, switch `tileUnload`, clear-stops-delivery) with a new
  `tests/data/p32_bad_child_tileset` fixture (random bytes as
  `child_a.glb`).
