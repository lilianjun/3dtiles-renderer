# ADR-0020: loadTileset re-entry and switching safety (P22)

## Status

Accepted (2026-09-29).

## Context

`Renderer::loadTileset()` documented "loading a new tileset replaces the
old one; there is no separate unload" (ADR-0012), but a second call was
never exercised. Hosts will hit this in the obvious ways: switching scenes,
retrying after a failure, reloading the same URL.

## Findings (empirical, before the fix)

Reading `Renderer::loadTileset` (src/renderer.cpp) showed the real defect
was one layer above where P22 started looking:

```cpp
s.tileset.reset(); // drop any previously loaded tileset first   <-- BUG
auto tileset = std::make_unique<TilesetRenderer>(s.engine, s.scene);
...
if (!tileset->load(tilesetUrl)) {
    setLastError("loadTileset: " + tileset->lastError());
    return false;   // old tileset already destroyed; s.tileset stays null
}
s.tileset = std::move(tileset);
```

Reproduced: load p3, then `loadTileset("/nonexistent/x.json")` → returns
false (correct) but the p3 tileset is gone — the demo renders the clear
color and `tileStats()` reports all -1. A missing file destroyed the
working scene. (The inner `TilesetRenderer::Impl::loadTileset` had the
same shape — members replaced before the new root arrived — but
`Renderer::loadTileset` always constructs a fresh `TilesetRenderer`, so
the outer layer was the live defect.)

## Decision

Build-then-commit at both layers:

- `Renderer::loadTileset` (src/renderer.cpp): the new `TilesetRenderer`
  is fully loaded first; `s.tileset.reset()` + assignment happen only on
  success. A failed load returns false and the current tileset keeps
  rendering untouched.
- `TilesetRenderer::Impl::loadTileset` (src/tileset.cpp): the replacement
  cesium `Tileset` (plus its accessor / prepare-resources / AsyncSystem)
  is built in locals; members are committed only after the root tile
  arrives. Defense in depth — today the impl is single-shot per instance,
  but the invariant "failed load changes nothing" now holds at both
  layers.

Destruction ordering on the success path is safe: the old cesium
`Tileset` is destroyed by the `tileset = std::move(...)` assignment while
its externals still hold the old `FilamentPrepareResources` alive, so
`unloadAll()` → `free()` removes the old Filament scene nodes before the
old prepare resources' destructor runs `destroyMaterials()`.

## Consequences

- Documented guarantee (docs/integration.md): `loadTileset` may be called
  any number of times; each successful call atomically replaces the
  current tileset; a failed call changes nothing (the previous tileset,
  if any, keeps rendering; `lastError()` explains the failure).
- New test `tileset_switch` (+ `sanitizer_tileset_switch`): four
  scenarios — clean A→B switch (end screenshot bit-identical to a
  direct-B load, selected IDs flip with no residue), failed-switch-then-
  retry (bad path fails at the probe, p3 keeps rendering, retry with p8
  succeeds), reload-same-path (pixel-identical), switch-away mid slow-
  HTTP load (3 tiles in flight at switch time, no crash, new tileset
  renders). Demo gained `--switch-tileset PATH --switch-at-frame N`
  (repeatable; failures logged as `[switch] ok=0`, never fatal).
- No new public API. No cesium-native changes.

## Honest boundaries

- The 30s root-wait inside `loadTileset` is unchanged: switching to a
  corrupt-but-present tileset.json blocks the render thread up to 30s
  before failing (observed in testing). Fail-fast on corrupt JSON is
  future work.
- Only Linux/Mesa verified. The mid-load scenario abandons in-flight
  curl requests by destroying the old `RoutingAssetAccessor`; safe under
  ASan/LSan/UBSan here, but a host switching rapidly on a real network
  will churn connections — acceptable, not optimized.
