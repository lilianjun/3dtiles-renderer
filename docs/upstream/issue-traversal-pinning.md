# Upstream issue — TreeTraversalState pins every touched tile, silently defeating `maximumCachedBytes`

> **Status: FILED as [CesiumGS/cesium-native#1458](https://github.com/CesiumGS/cesium-native/issues/1458)
> on 2026-09-29.** The text below is the filed body (minus this header).
> Do not include private repo links: the downstream SDK that observed this
> is in a private repository; the issue text is self-contained.
> (Internal detail: our ADR-0017.)

## Proposed title

`TreeTraversalState` holds an intrusive ref to every tile `beginNode`'d —
including frustum-culled tiles — so `maximumCachedBytes` is silently
ignored on shallow-wide trees

## Environment

- cesium-native **v0.64.0** (also present on current `main` by code
  inspection — see paths below)
- Observed through a downstream C++ SDK that drives
  `Tileset::updateViewGroup` / `loadTiles` per frame and reads
  `TilesetOptions::maximumCachedBytes` as the tile **content** cache
  budget (CPU side).

## Reproduction (no private fixtures needed)

1. Take any tileset with a **shallow, wide** tree: root + ~2 levels,
   ~20–30 tiles, a few MB of total content, `refine: "ADD"`, arranged so
   that every frame's traversal *visits* every tile (bounding volumes
   overlapping the view; culling may still reject individual tiles —
   that is the point).
2. Set `TilesetOptions::maximumCachedBytes` to a small value, e.g.
   **800,000** bytes — well under the ~2.7 MB total content size.
3. Call `updateViewGroup` once per frame for ~150 frames.
4. Observe loaded-tile count / loaded content bytes
   (e.g. via `Tileset::getNumberOfTilesLoaded()` or per-tile byte
   accounting).

**Observed:** loaded tiles stay at **23**, loaded bytes stay at
**2,760,912** — constant for 146 consecutive frames. The unload queue is
always empty; **no eviction ever happens**, and the 800 KB budget is
silently exceeded ~3.4x.

**Expected:** once cached content exceeds `maximumCachedBytes`, tiles
whose content is not referenced by the current frame should become
eligible for `unloadCachedBytes` LRU eviction, keeping the cache near
the budget.

**Control experiment:** with a *deep* tree (implicit QUADTREE, 341
tiles, ~15.6 MB) where culled subtrees are genuinely not touched each
frame, the same 8 MB budget works correctly — 179 tiles /
7,977,744 bytes resident, ~380 evict-and-reload cycles over 3 camera
laps. So the unload path itself is fine; the trigger condition
("is this tile's content referenced?") is what never fires on
shallow-wide trees.

## Mechanism (read from v0.64.0 sources)

1. `Cesium3DTilesSelection/src/TilesetSelection.cpp:1047` —
   `visitTileIfNeeded()` calls `traversalState.beginNode(&tile)` **first**,
   before computing `CullResult` (culling starts ~L1057). Children are
   visited unconditionally by `visitVisibleChildrenNearToFar`
   (`TilesetSelection.cpp:798`), so every child of a visited tile is
   `beginNode`'d even when it is about to be frustum-culled.
2. `CesiumUtility/include/CesiumUtility/TreeTraversalState.h:95` —
   `beginNode` stores the `Tile::Pointer` (intrusive) in
   `_currentTraversal`. `finishNode` (`:197`) only pops the parent-index
   stack; the entry — and its reference — stays. `beginTraversal`
   (`:77`) swaps current→previous, so a tile touched in frame N remains
   referenced through frame N+1 and is released only at frame N+2.
3. `Cesium3DTilesSelection/src/Tile.cpp:364` — `isContentReferenced()`
   subtracts the known internal references and treats *any* remaining
   reference as "content is referenced".
4. `Cesium3DTilesSelection/src/Tile.cpp:448` — only tiles that are
   *not* content-referenced are passed to
   `markTileEligibleForContentUnloading`.

Net effect: on a shallow-wide tree, every frame's traversal touches
every tile → every tile carries a traversal-state reference every frame
→ `isContentReferenced` is always true → nothing is ever eligible for
unloading → `maximumCachedBytes` is defeated. The **effective cache
floor is the per-frame touched working set**, not the configured
budget — and nothing warns you.

## Possible directions (suggestions, not prescriptions)

- Call `beginNode` *after* the cull decision, or release the traversal
  reference for tiles that end up culled / not selected.
- Alternatively, make the unload-eligibility check aware of
  traversal-only references (e.g. don't count references held solely by
  `TreeTraversalState` as "content referenced").

## Downstream impact

A downstream SDK cannot honestly promise "memory bounded by
`setMaxCachedBytes`" for arbitrary tilesets today; its docs must carry
the caveat "budget must exceed the per-frame touched working set".
Fixing this upstream would remove that caveat.

## Checklist before filing

- [ ] Re-read against current `main` (line numbers above are v0.64.0;
      confirm they haven't shifted).
- [ ] Decide whether to attach the minimal shallow-wide tileset
      description as a JSON sketch (no binaries needed).
- [ ] File as the repo owner (public action under their identity).
