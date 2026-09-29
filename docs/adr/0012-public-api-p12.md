# ADR-0012: SDK public API additions (P12) — resize, lastError, integration guide

## Status

Accepted (2026-09-29).

## Context

P0–P11 built a real rendering pipeline, but nobody had reviewed the SDK
from a host-app developer's perspective. Audit of
`include/tiles_renderer/renderer.h` against "what does a host need to
ship":

| API | Verdict |
|---|---|
| `initialize` / `renderFrame` / `readPixels` / `shutdown` / `version` | sufficient |
| `loadTileset` | sufficient, but failure was `bool`-only with the reason on stderr — a host cannot show it to the user |
| `setOrbitCamera` | sufficient for the current model |
| `renderedTileCount` | sufficient |
| surface resize | **missing** — the only recourse was `shutdown()` + full re-`initialize()` (destroying the Filament engine and forcing a tileset reload) |
| error reporting | **missing** — see above |
| camera target / free lookAt | deliberately **not added** (see below) |
| `unloadTileset` | deliberately **not added** — `loadTileset` already replaces; documented |

## Decision

Two additive, backwards-compatible API additions (no signature changes):

1. **`static bool resize(width, height)`** — recreates the Filament swap
   chain for the *same* native window handle, updates the viewport and
   camera aspect. The host resizes its OS surface first, then calls this
   between frames on the render thread. Same-size calls are a no-op.
2. **`static std::string lastError()`** — human-readable reason for the
   most recent `initialize` / `loadTileset` / `resize` failure; empty on
   success. Valid until the next SDK call. `TilesetRenderer` (internal)
   gained a matching `lastError()` so the tileset-load reason (e.g. the
   offending path) propagates instead of dying on stderr.

Plus `docs/integration.md`: lifecycle, threading model (all `Renderer`
methods except `version()` are single-render-thread; worker threads are
internal to tile I/O), per-platform snippets (Windows/Android/iOS/Web),
resize semantics, and a host checklist. The Linux snippet is
compile- *and* behavior-checked by `tests/host_integration_check.cpp`
(ctest `host_integration`, also under sanitizers).

### Why no camera target

`setOrbitCamera` orbits the tileset's local origin with no target
parameter. Exposing a target would be misleading today: tile *selection*
runs in world space while tile *rendering* runs in the P5-rebased
float32 space, so a host-supplied world-space target would not match
what is drawn for large-coordinate tilesets. The internal `OrbitCamera`
already carries `targetX/Y/Z` (used when no rebase origin exists); a
public free camera is future work once the coordinate story is nailed
down. Documented in `docs/integration.md` rather than shipped
half-correct.

### Why no unloadTileset

`loadTileset` drops the previous tileset before loading the new one, so
an explicit unload adds no capability. Returning to the "no tileset"
state is only possible via `shutdown()` today; if a host needs it, it is
a 5-line addition later.

## Consequences

- New tests: `host_integration` (15/15 ctest) and
  `sanitizer_host_integration` — covering resize (incl. invalid sizes
  and post-shutdown), `lastError` content on failure, error clearing on
  success, and a full load→render→resize→shutdown cycle against the
  `p8_pnts_cloud` fixture.
- Web snippet is marked stub: the wasm build has Filament disabled
  (ADR-0006); the doc shows the intended API shape, not working code.
- `lastError()` returns by value (`std::string`); no lifetime hazards.
