# ADR-0016: Weak-Network Resilience Testing

**Status:** Accepted (2026-09-29)
**Phase:** P18

## Context

The SDK streams tile content over HTTP (tileset.json, .glb, .b3dm, etc.).
Real deployments hit slow networks, cancelled loads, and outages. We needed
automated, deterministic tests for these — without external network
dependencies, root privileges, or traffic-shaping tools (CI must run them).

Additionally, P17's `tileStats()` exposed that `tilesLoading` (previously
queue-lengths only) lied during slow downloads: the worker/main queues drain
as soon as requests are dispatched, while curl transfers continue in the
background.

## Decision

### 1. Local controllable HTTP fixture

`tests/slow_http_server.py`: a threaded `http.server` on 127.0.0.1 with
per-response `--delay`, optional bandwidth throttling (`--chunk-bytes` /
`--chunk-delay` for paced chunk writes), and `--log-requests` (append one
line per request path — ground truth for "was this tile ever requested?").

Why not `tc` / proxies: they need root, affect the whole machine, and are
not reproducible in CI. A userspace server is deterministic and hermetic.

### 2. Test suite (`tests/weaknet_test.py`, CTest `weak_network`)

Five cases, all driven by steady-state assertions (not fixed frame counts),
whole suite < 2 min:

- **slow**: 0.3s/response; must settle (`--until-loaded`) with
  `loaded==4, failed==0`.
- **cancel_prevent**: far camera from frame 0 (trajectory); children must
  NEVER be requested (server log proves it — only `tileset.json`+`root.glb`).
- **cancel_midload**: zoom out while loads in flight; must exit 0, `failed==0`,
  selection follows the camera once in-flight loads resolve.
- **abort**: `--exit-on-loading`; teardown mid-load must exit 0 (also run
  under ASan/UBSan as `sanitizer_weaknet_abort`).
- **outage**: kill the server mid-load, restart on the same port; must exit 0,
  never crash.

### 3. `tileStats().tilesLoading` semantics fix (P18)

`tilesLoading` now = worker queue + main queue + tiles in `ContentLoading` /
`ContentLoaded` state (tree walk). Rationale: queue-only counting reports 0
while bytes are still in flight. The tree walk is O(tiles) per frame, same
as the existing `tilesLoaded` walk.

Note: at frame boundaries a tile can be counted twice (still queued AND
already `ContentLoading`); this transient over-count is documented and does
not affect settle detection (both drain to 0).

### 4. Cancellation semantics (observed, cesium-native v0.64.0)

- There is **no mid-flight HTTP cancellation API** in cesium-native: tiles
  with in-flight loads stay selected until their loads resolve (no popping),
  THEN deselection applies. The `cancel_midload` test asserts this steady
  state (`selected 4->2`, `failed==0`), not a hard abort.
- Deselected tiles whose content already downloaded stay in
  `ContentLoaded` (finalization deferred until re-selected). This is why
  `cancel_midload` ends at `loading==2` rather than 0 — asserted as a stable
  steady state, not a bug.

### 5. Real bug found & fixed: mixed-runtime exception crash

The outage test exposed a **heap-corruption crash** (SIGSEGV / ASan
`alloc-dealloc-mismatch`): cesium-native's `CurlAssetAccessor` throws
`std::runtime_error` on network failure (refused, DNS, timeout). Our Linux
process mixes libstdc++ (our code, GCC) with libc++ (Filament prebuilts);
`libc++.so` precedes `libstdc++.so` in DT_NEEDED, so the exception's
destructor interposes to libc++abi's version, which `free()`s
libstdc++-allocated memory.

**Fix:** the SDK no longer uses `CurlAssetAccessor`. New
`NonThrowingCurlAccessor` (`src/tileset.cpp`) does blocking libcurl GETs in
worker threads and **never throws**: network failures become synthetic
HTTP 599 responses with empty bodies, which cesium-native loaders already
handle gracefully (same path as HTTP 500 → tile `Failed`, no exception).

Consequences:
- `tests/lsan.supp` Entry 1 (CurlAssetAccessor handle-cache leak) is now
  obsolete — the leaker is no longer instantiated. Entry kept (harmless).
- `Renderer::loadTileset` docs updated: http(s) failures are graceful.

## Consequences

- `weak_network` (19th CTest) and `sanitizer_weaknet_abort` (30th sanitizer
  test) gate P18.
- Demo gains test hooks (`--until-loaded`, `--exit-on-loading`,
  `--zoom-out-on-loading`); these are demo-only, not SDK API.
- SDK remains zero-SDL; `git diff --check` clean.

## Boundaries (honest)

- No mid-flight request abortion exists to test; "cancel" = deselection
  applies after in-flight loads resolve.
- Outage recovery = graceful failure (tiles `Failed`), not automatic retry;
  cesium-native does not retry `Failed` tiles within a session.
- Bandwidth throttling is chunk-pacing, not a true token bucket; adequate
  for correctness tests, not for performance characterization.
