# ADR-0022: fail-fast on corrupt tileset.json (P23)

## Status

Accepted (2026-09-29).

## Context

P22 (ADR-0020) made `loadTileset` build-then-commit: the replacement
tileset is constructed in local state and committed only after its root
tile arrives, so a failed load leaves the live tileset untouched. The
wait for the root tile is bounded (30s), but ADR-0020 honestly noted the
residual gap: a **corrupt-but-present** tileset.json always burned the
full 30s before failing.

Root cause: the bytes arrive fine, cesium-native's `TilesetJsonLoader`
fails the parse in a worker thread, and the failure is swallowed — the
`Tileset` simply never gets a root tile. The wait loop only watches
`getRootTile() != nullptr`, which cannot distinguish "still loading"
from "load failed", so it spins to the deadline doing nothing.

Reproduced before the fix: local garbage-bytes tileset.json →
`loadTileset` returns false after ~30.0s with "no root tile within 30s".

## Decision

Pre-flight validation of the root document **before** the cesium
`Tileset` is constructed (`preflightTilesetRoot`, src/tileset.cpp):

- Local file: read + rapidjson parse (synchronous, ~ms).
- `http(s)://`: one blocking fetch through the same
  `RoutingAssetAccessor` the `Tileset` will use (same curl timeouts,
  never throws — network failure surfaces as synthetic 599, P18), then
  the same parse.

The check mirrors what the loader needs: a parseable JSON object with a
`"root"` object member. Anything weaker — garbage bytes, truncated JSON,
empty file, valid JSON that isn't a tileset (missing `"root"`) — fails
here in milliseconds with a specific `lastError()` ("not valid JSON
(parse error at byte N)" / "not a 3D Tiles tileset (missing \"root\")" /
"failed to fetch tileset.json: HTTP 404"...).

Design notes:

- The 30s bounded root-wait is kept as a fallback for failures the
  pre-flight cannot see (valid JSON + `"root"` that cesium still can't
  turn into a root tile — deep semantic errors). Fail-fast covers the
  common corruption cases; the bound covers the rest.
- Slow networks are not mis-killed: the HTTP pre-flight uses the same
  generous curl timeouts (connect 10s, total 60s, low-speed 15s) as tile
  loading, so a slow-but-valid root still passes. The P18
  `slow_http_server` scenario (0.3s/response) is exercised with a
  corrupt document in the new test to prove it.
- The pre-flight adds one extra fetch of the root tileset.json over
  HTTP (small file, negligible) and one extra local read for files.
- No new public API. No cesium-native changes. rapidjson was already on
  the SDK's include path (cesium-native PUBLIC dependency); it is
  header-only — the zero-SDL invariant is unaffected.

## Consequences

- `loadTileset` on a corrupt tileset.json now fails in milliseconds;
  the live tileset keeps rendering (P22 build-then-commit semantics,
  unchanged). `docs/integration.md` updated: the "~30s block" note now
  says corruption fails fast, with 30s remaining only as the bound for
  deep semantic failures / unresponsive servers.
- New test `tileset_failfast` (+ `sanitizer_tileset_failfast`):
  garbage-bytes / valid-JSON-not-a-tileset / truncated-JSON /
  corrupt-over-slow-HTTP — each asserts fail-fast (<5s wall from the
  switch frame to the `[switch]` log), `lastError()` fragment, the live
  p3 tileset undisturbed (selected IDs, healthy stats), and an end
  screenshot bit-identical to a no-switch reference run.
- README test list updated.

## Honest boundaries

- The pre-flight only validates the **root** document's shape, not its
  semantics: a parseable JSON with a `"root"` object that cesium rejects
  deeper down still burns up to 30s. Deliberate — matching cesium's full
  validation would duplicate the loader.
- For HTTP, "network dead but connectable" (black-hole server) still
  takes up to the low-speed timeout (~15s) to surface as 599; only
  refused/DNS failures are instant. Genuine network failure, not
  corruption — acceptable.
- Only Linux/Mesa verified (as with all pixel gates).
