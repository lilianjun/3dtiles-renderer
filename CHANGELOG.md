# Changelog

All notable changes to this project are documented here, in
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) style.
Each entry corresponds to a real merged phase (P0–P13); nothing is
aspirational. Platform abbreviations: L = Linux, W = Windows,
A = Android, i = iOS, wasm = WebAssembly.

## [Unreleased]

### Added
- P26: image-based lighting (IBL), on by default (ADR-0025). A Filament
  `IndirectLight` driven by a small procedural environment generated
  deterministically at initialize(): 64x64x6 RGBA8 cubemap (analytic
  sky-gradient + sun, GPU mipmaps) for specular reflections, plus 3-band
  spherical harmonics of the same function for diffuse irradiance.
  Fixes the P15 gap where metals were black except for the sun's specular
  lobe (measured on `p15_metal`: metal mean 5.6 → 55.2, roughness-0.08
  metal 0.0 → 57.7, nothing blown out). New minimal public API
  `Renderer::setIblEnabled(bool)` (false = pre-P26 sun-only look) and demo
  flag `--no-ibl`. New CTest `ibl` (on/off A/B: metal brightens >3x,
  shows sky-reflection character, dielectric stays sane, geometry
  unchanged) and `sanitizer_ibl` gate. Goldens re-frozen for the
  intentional lighting change (9/11 entries; unlit `p2_demo`/`p8_pnts`
  unchanged). `pbr_test.py` metal/rough checks now pin `--no-ibl` since
  their assertions are no-IBL statements about the PBR model. Honest
  tradeoff: box-filtered GPU mips (not cmgen GGX prefiltering), LDR
  environment, no intensity knob yet; other platforms compile the same
  core-API path but have no pixel verification.
- P25: KTX2 (`KHR_texture_basisu` / `image/ktx2`) texture support, truly
  verified (ADR-0024). Deterministic UASTC fixture
  (`tests/data/gen_p25_ktx2.py`, fixed `toktx 4.4.2 --uastc` payload as
  base64 — the UASTC decode is lossless for this flat-color checkerboard,
  0/4096 texels differ from the PNG twin). New CTest `ktx2`:
  KTX2-textured box renders and matches its PNG twin (measured max/mean
  diff 0.0/0.0000, tolerance max ≤ 16 / mean < 2.0); corrupt KTX2 fails
  gracefully (exit 0, 0 tiles). New `sanitizer_ktx2` gate. Root cause of
  the initial crash found and fixed: cesium-native's vcpkg `libktx.a`
  vendors an incompatible `basist::` basisu copy that collided with
  Filament's `libbasis_transcoder.a` (heap corruption; proven by
  link-order experiment). On cesium builds KTX2 is now decoded by a new
  internal provider (`src/ktx2_libktx_provider.cpp`) using the same
  `libktx` cesium links (transcode to RGBA32 — GPU block compression is
  future work); `ktxreader`/`basis_transcoder` are not linked there.
  Non-cesium platforms use Filament's `createKtx2Provider` (no conflict
  possible, transcoder proven correct standalone). Also documented:
  uncompressed KTX2 can never pass cesium-native v0.64.0's ImageDecoder
  (`KTX2 loading failed with error: Operation succeeded.`).
- P24: upstream issue draft for the P19 `TreeTraversalState` pinning
  finding (`docs/upstream/issue-traversal-pinning.md`, not yet filed):
  re-verified on master that an 800 KB `maximumCachedBytes` budget is
  silently exceeded ~3.4x on a shallow-wide tree (23 tiles /
  2,760,912 bytes resident, zero eviction over 146 frames), with the
  mechanism cited file:line from cesium-native v0.64.0 sources.
- P24: remaining-gaps roadmap (ADR-0023) — honest P0/P1/P2 inventory:
  no real-device render verification (P0), corrupt-cmpt SIGSEGV
  upstream #1457 still open (P0), KTX2/BasisU black textures,
  traversal pinning, no IBL, i3dm CPU expansion (P1), Draco, 1px pnts,
  WASM stub, translation-only ECEF rebase (P2 / won't-fix-for-now).

### Fixed
- CI install step (P14 gate, first actually exercised): the Linux job's
  "Install SDK to temp prefix" step failed with
  `file INSTALL cannot set permissions on "/usr/local/include"`.
  cesium-native v0.64.0's per-library header install rules bake
  `${CMAKE_INSTALL_PREFIX}` into absolute destinations at configure
  time, so the install-time `--prefix` override never applied to them.
  Fixed by setting `-DCMAKE_INSTALL_PREFIX="${RUNNER_TEMP}/tiles_sdk"`
  at configure time (the install step keeps the same `--prefix`).
  Verified locally end-to-end: full `cmake --install` + external
  `install_smoke` consumer (`sdk version: 0.1.0`). Like the P22 glm
  issue, this gate had never gone green before — every earlier run was
  cancelled or failed before reaching it.
- Windows/Android/iOS/WASM CI compile (P22 follow-up hotfix): the
  `#include <glm/...>` / `#include <curl/curl.h>` in `src/tileset.cpp`
  were unconditional, but those CI configs build the SDK *without*
  cesium-native (glm/curl only arrive via cesium-native's vcpkg tree),
  so all four platforms failed at `tileset.cpp:57` with C1083 /
  "'glm/gtc/matrix_transform.hpp' file not found". The includes are now
  inside `#ifdef TILES_WITH_CESIUM_NATIVE` (their only TU usage already
  was). Also fixed `src/renderer.cpp`: `g_pendingMaxCachedBytes` was
  defined inside `#ifdef TILES_WITH_FILAMENT` but used unconditionally
  by `setMaxCachedBytes()` — the WASM stub build (Filament OFF) failed
  with "use of undeclared identifier". Root-cause note: the P19 hotfix
  for the same glm symptom placed its include-dir glob inside
  `if(TILES_WITH_CESIUM_NATIVE)`, which is OFF on those four platforms,
  so it never actually fixed them — and its CI run was cancelled by the
  P20 push before anyone noticed. Verified locally by compiling the SDK
  in both CI-equivalent configs (cesium OFF + Filament ON;
  cesium OFF + Filament OFF).
- `loadTileset` fail-fast on corrupt tileset.json (P23, ADR-0022):
  the root document is now validated *before* the cesium `Tileset` is
  constructed (local: read + rapidjson parse; http(s): one blocking fetch
  through the same `RoutingAssetAccessor`, then the same parse). A
  corrupt-but-present tileset.json (garbage bytes, truncated JSON, valid
  JSON without `"root"`) used to burn the full 30s root-wait before
  failing; it now fails in milliseconds with a specific `lastError()`
  ("not valid JSON (parse error at byte N)" /
  "not a 3D Tiles tileset (missing \"root\")" /
  "failed to fetch tileset.json: HTTP 404"...). Slow networks are not
  mis-killed — the pre-flight fetch uses the same generous curl timeouts
  as tile loading. The 30s bounded wait remains as a fallback for deep
  semantic failures the pre-flight can't see. P22's build-then-commit
  guarantee is unchanged: a failed load changes nothing.
- `loadTileset` re-entry safety (P22, ADR-0020): a second `loadTileset()`
  is now build-then-commit — the replacement tileset is fully loaded
  (root tile arrived) before it replaces the live one. Previously
  `Renderer::loadTileset` did `s.tileset.reset()` *before* the new load,
  so ANY failed second load (even a missing file) destroyed the working
  tileset and left the SDK rendering the clear color. Now a failed load
  changes nothing: the current tileset keeps rendering, `lastError()`
  explains the failure. Documented in `docs/integration.md` as a
  guarantee. Demo gained `--switch-tileset PATH --switch-at-frame N`
  (repeatable, test/dev only).

### Added
- Corrupt-tileset fail-fast test (P23): new `tileset_failfast` ctest
  (`tests/tileset_failfast_test.py`, 4 scenarios x 3 reps) plus
  `sanitizer_tileset_failfast` (ASan/LSan/UBSan, no reports). Scenarios:
  garbage-bytes tileset.json, valid-JSON-not-a-tileset, truncated JSON,
  corrupt document over slow HTTP (0.3s/response — proves the P18 slow
  path isn't mis-killed). Each asserts fail-fast (<5s wall from switch
  frame to `[switch]` log), a specific `lastError()` fragment, the live
  tileset undisturbed (selected IDs + healthy stats), and an end
  screenshot bit-identical to a no-switch reference run.
- Tileset switch safety test (P22): new `tileset_switch` ctest
  (`tests/tileset_switch_test.py`, 4 scenarios x 3 reps, ~20s) plus
  `sanitizer_tileset_switch` (ASan/LSan/UBSan, no reports). Scenarios:
  clean p3->p8 switch (end screenshot bit-identical to direct-p8 load,
  selected IDs flip with no residue), failed-switch-then-retry (bad path
  fails at the probe, p3 keeps rendering, retry with p8 succeeds),
  reload-same-path (pixel-identical), switch-away mid slow-HTTP load
  (3 tiles in flight at switch time, no crash).
- ADD refinement + region bounding-volume verification (P21, ADR-0019):
  new `add_region` ctest (`tests/add_region_test.py`, ~3s, PASS over 3 runs)
  plus `sanitizer_add_region` (ASan/LSan/UBSan, no reports). ADD fixture
  `tests/data/p21_add_tileset/` (same geometry as P20's REPLACE fixture,
  `refine: "ADD"`): FAR->NEAR->FAR proves the ADD/REPLACE contrast as a
  regression gate — NEAR selects `{root.glb, child_0..3.glb}` (root stays,
  vs P20's REPLACE selecting only the 4 children), FAR2 falls back to
  `{root.glb}`, loaded 2->6 cached. Region fixture
  `tests/data/p21_region_tileset/` (`gen_p21_region_tileset.py`): 3D Tiles
  1.0 `region` BVs (radians, WGS84) at lat=0/lon=0 with a root-tile
  `transform` translating local boxes to ECEF(6378137,0,0); the SDK's
  existing rebase path (`computeLocalOrigin`: BoundingRegion -> OBB center)
  brings them back near the origin with zero SDK changes — NEAR selects
  `{root.glb, child_0.glb, child_1.glb}`, loaded=4, failed=0, and the
  screenshot differs from the `--no-tileset` reference by 5382 px (4.5%,
  bit-identical over 3 runs).
- Frustum culling + LOD refinement verification (P20, ADR-0018):
  `Renderer::selectedTileIds()` (render thread; ID strings of the last
  traversal's `tilesToRenderThisFrame` via cesium-native
  `TileIdUtilities::createTileIdString`; diagnostic, format not contractual),
  demo `--print-selected`, ground-plane frustum fixture
  `tests/data/p20_frustum_tileset/` (16 REPLACE children on a 512x512 slab,
  Y-up horizontal — a Z-up "wall" fixture can't show set flips with the
  orbit camera, which always looks at its target), tiny REPLACE LOD fixture
  `tests/data/p20_lod_tileset/` (root ge=20 + 4 children ge=0), and new
  `frustum_lod` ctest (`tests/frustum_lod_test.py`, <2s, bit-identical over
  3 runs): yaw~0 vs yaw~180 select different tile sets (corner tiles flip,
  Jaccard=0.50, max selected 13 << 17); FAR->NEAR->FAR proves REPLACE
  (NEAR selects exactly the 4 children, root absent; loaded 2->6; FAR2 falls
  back to root with loaded cached at 6); plus `sanitizer_frustum_lod`
  (ASan/LSan/UBSan via `tests/run_sanitized.py`, no reports). Upstream
  notes: cesium-native creates an empty-ID wrapper root tile above the JSON
  root (its refine inherits the JSON root's; REPLACE drops it from the
  render selection, ADD keeps it) — `tilesLoaded` counts it, which is why
  the LOD fixture shows loaded=2 at FAR, not 1.
- Camera-roam memory boundedness (P19, ADR-0017):
  `Renderer::setMaxCachedBytes()` (render thread; applies to next
  `loadTileset()` and live-mutates a loaded tileset; `<=0` restores the
  512MB Cesium default; content-cache bytes, not GPU VRAM), demo
  `--cache-budget` / `--print-rss`, deep implicit fixture
  `tests/data/p19_deep_tileset/` (341 tiles / ~15.6MB, levels 0-4) plus
  3-lap trajectory `tests/data/trajectories/p19_deep.csv`, and new
  `memory_roam` ctest (`tests/memory_roam_test.py`): 8MB budget holds
  `tilesLoaded` at 179 and `bytesLoaded` at 7,977,744 (bit-identical over
  3 runs) while the HTTP request log proves 253 distinct tiles with
  ~630 hits (eviction churn); lap-end values stable within 6%; plus
  `sanitizer_memory_roam` (150 frames under ASan/LSan/UBSan, no reports).
  Key upstream finding documented: cesium-native v0.64.0's
  `TreeTraversalState` pins every `beginNode`'d tile (intrusive ref) for
  ~2 frames, so in shallow/wide trees the traversal working set — not
  `maximumCachedBytes` — is the effective cache floor.
- Weak-network resilience gate (P18, ADR-0016): `tests/slow_http_server.py`
  (threaded localhost HTTP with per-response delay, chunk-paced bandwidth
  cap, and per-request logging) plus `tests/weaknet_test.py` (new
  `weak_network` ctest, ~8s): slow-net completion (`loaded==4, failed==0`),
  request prevention on deselection (far camera → children never requested,
  server log is ground truth), mid-load camera change (graceful steady
  state), mid-load teardown (`--exit-on-loading`, also gated under
  ASan/UBSan as `sanitizer_weaknet_abort`), and server outage (kill +
  same-port restart → exit 0, never crashes). Demo-only hooks:
  `--until-loaded N`, `--exit-on-loading`, `--zoom-out-on-loading`.
- Tile streaming diagnostics (P17, ADR-0015): new `Renderer::tileStats()`
  returning `Renderer::TileStats` — `selectedTiles` (last traversal's render
  selection), `tilesLoading` (worker + main load queue lengths),
  `tilesLoaded` (tiles in `TileLoadState::Done`, counted exactly by walking
  the instantiated tile tree — `forEachLoadedTile` would also count
  still-loading tiles and lie during streaming), `tilesFailed`
  (permanent + transient failures), `bytesLoaded`
  (`Tileset::getTotalDataBytes`, content bytes — not a GPU memory estimate).
  All `-1` when no tileset is loaded; render-thread-only like every other
  API except `version()`. New `tile_stats` ctest (via `tiles_demo --stats`):
  healthy tileset -> loaded > 0, failed == 0, loaded monotonic,
  `renderedTileCount()` consistent with `selected`; corrupt glb ->
  failed > 0 with graceful exit 0; screenshots with/without `--stats` are
  bit-identical (querying stats never perturbs rendering). FPS is
  deliberately not reported (Mesa software numbers are meaningless) and no
  CPU/GPU memory estimate is fabricated (cesium-native exposes only content
  bytes; Filament v1.77 has no public per-scene GPU accounting).
- Deterministic camera trajectory replay (P16, ADR-0014): the demo drives
  `setOrbitCamera` per logical frame from a CSV keyframe trajectory
  (`tests/data/trajectories/p16_orbit_push.csv`, smoothstep interpolation,
  no wall clock). The player lives in the demo layer (`samples/demo/`,
  header-only, no SDL/SDK dependency) — camera control is the host's job,
  not the SDK's (ADR-0003/0012). New `trajectory_determinism` ctest runs the
  12-frame orbit + push-in trajectory twice and requires every frame PNG
  bit-identical; golden gains `p16_traj_f00/f05/f11` (first/middle/last
  frames). `tiles_demo --trajectory <csv> [--frame-dir <dir>] [--warmup N]`
  also serves as an on-device smoke recorder later.
- PBR material pipeline verification (P15, ADR-0013): deterministic fixtures
  (`tests/data/gen_p15_pbr_tileset.py`, no network) covering metallic 0 vs 1,
  roughness 0.08 vs 0.9, hand-made normal map vs flat normals,
  `alphaMode=BLEND` vs `OPAQUE`, single vs double-sided culling, and a
  procedural checkerboard `baseColorTexture` — asserted by the new
  `pbr_materials_screenshot` ctest with comparison-based (not hard-coded)
  pixel assertions.

### Fixed
- **Real crash on network failure (P18):** cesium-native's
  `CurlAssetAccessor` throws `std::runtime_error` when the network itself
  fails (refused/DNS/timeout). On Linux our process mixes libstdc++ (GCC)
  with libc++ (Filament prebuilts); the exception's destructor interposed
  to libc++abi's version → heap corruption / SIGSEGV (found by the new
  outage test, confirmed under ASan as `alloc-dealloc-mismatch`). The SDK
  now uses its own `NonThrowingCurlAccessor` (`src/tileset.cpp`): blocking
  libcurl transfers in worker threads that **never throw** — failures
  surface as synthetic HTTP 599 responses, which cesium-native loaders
  already handle gracefully (tile `Failed`, no exception). As a side effect,
  the `tests/lsan.supp` Entry 1 leak (CurlAssetAccessor handle cache) no
  longer applies — the leaker is not instantiated anymore.
- `tileStats().tilesLoading` (P18): was queue-lengths only and reported 0
  while bytes were still in flight; now also counts tiles in
  `ContentLoading`/`ContentLoaded` via the tree walk.
- glTF textures (PNG/JPEG) rendered black: gltfio's `ResourceLoader` had no
  `TextureProvider` registered ("Missing texture provider for image/png").
  The render bridge now wires Filament's prebuilt stb decoder
  (`createStbProvider`, `libstb.a`) wherever the platform package ships it
  (`TILES_WITH_STB_PROVIDER`; configure-time warning otherwise). No
  third-party source changed.

## [0.1.0] — 2026-09-29 (P13: SDK install & packaging)

### Added
- `cmake --install` support: installs the `tiles_renderer` static library,
  public headers (including the generated `version.h`), and a
  `tiles_rendererConfig.cmake` / `tiles_rendererConfigVersion.cmake` pair, so
  external projects can `find_package(tiles_renderer CONFIG REQUIRED)` and
  link `tiles_renderer::tiles_renderer`. Verified end-to-end with a minimal
  external consumer that configures, links, and prints
  `Renderer::version()` → `0.1.0`.
- Cesium Native's own CMake config is co-installed (`share/cesium-native`),
  plus the vcpkg package configs its `find_dependency()` chain needs, so the
  consumer's `find_package` resolves the full static-link closure from the
  same prefix. Filament's prebuilt archives and headers are installed next to
  the SDK; the config references them by absolute install path
  (`--start-group`/`--end-group` preserved on GNU toolchains).
- Install-time compatibility fixups (`cmake/tiles_debug_compat.cmake`):
  `<prefix>/debug` symlinks to the prefix and vcpkg debug-suffixed archive
  names (`libfmtd.a`, …) link to their release counterparts, because this SDK
  ships release third-party libs only. Debug-info fidelity is not claimed.
- `TILES_VCPKG_INSTALLED_DIR` cache variable to override vcpkg-tree
  auto-detection for exotic setups.

### Packaging boundaries (honest)
- **Source-integrated distribution.** There is no prebuilt binary
  distribution: consumers build the SDK from source (FetchContent pulls
  Cesium Native v0.64.0 and Filament v1.77.0) and install from that build
  tree. The installed config must not be relocated across platforms.
- `version()` remains generated from the single CMake `project(VERSION)`.

## [0.1.0] — 2026-09-29 (P12: public API audit + host integration guide)

### Added
- `Renderer::resize(w, h)`: rebuilds the swapchain on the same native window
  handle (previously only shutdown + re-init could change size).
- `Renderer::lastError()`: human-readable reason for the last `loadTileset`
  failure (previously bool-only).
- `docs/integration.md`: host integration guide for all four platforms
  (native handle ownership, render-thread affinity, lifecycle).
- `tests/host_integration_check.cpp`: 28 checks covering the documented
  API contract.

### Decisions (ADR-0012)
- Thread model documented: every public API except `version()` must be
  called on the same render thread.
- Deliberately NOT added: a free `lookAt` camera API and `unloadTileset`.

## [0.1.0] — 2026-09-29 (P11: golden screenshot regression)

### Added
- `tests/golden/`: 8 frozen reference screenshots covering P2/P3/P5/P7/P8/
  P9/P10 render paths.
- `golden_regression` test: strict pixel equality against the frozen
  baseline; `GOLDEN_MAX_DIFF_FRAC` escape hatch defaults to 0.
- Reverse-verified: flipping 1 pixel fails the gate; restoring passes.

## [0.1.0] — 2026-09-29 (P10: 3D Tiles 1.1)

### Added
- Bare `.glb` tile content (no 1.0 wrapper) via `BinaryToGltfConverter`.
- Implicit `QUADTREE` subdivision with hand-written JSON subtree tiles
  (constant availability); 5 tiles verified on screen.

### Notes
- `OCTREE` not exercised (same family), subtree bitstream availability not
  covered, S2 and other extensions untested.

## [0.1.0] — 2026-09-29 (P9: cmpt composite)

### Added
- `cmpt` composite tiles via cesium-native `CmptToGltfConverter` (recursive
  split + `Model::merge`); zero SDK code changes — P5/P7/P8 paths cover it.
  Deterministic self-generated cmpt (b3dm + pnts + i3dm in one tile)
  verified on screen; corrupt cmpt inputs fail gracefully.

### Fixed (third-party)
- Found and reproduced a cesium-native crash: `CmptToGltfConverter`
  structural failure leaves an empty model with only a warning, then
  `TilesetJsonLoader` unconditionally dereferences `*result.model`
  (SIGSEGV). Filed upstream as `CesiumGS/cesium-native#1457`; the SDK
  treats converter failure as tile-skip, never a crash.

## [0.1.0] — 2026-09-29 (P8: pnts point cloud)

### Added
- `pnts` point clouds rendered as native Filament `POINTS` primitives
  (gltfio + ubershader path, no SDK code changes). Pixel-verified: 420
  tri-color points land on exactly the right pixels with exact colors.
  Deterministic self-generated data (incl. 60k-point stress, rebase
  near/far, RTC reference); corrupt pnts skipped gracefully.

## [0.1.0] — 2026-09-29 (P7: i3dm instancing)

### Added
- `i3dm` instanced meshes via `I3dmToGltfConverter`
  (`EXT_mesh_gpu_instancing`). Filament v1.77 gltfio parses but does not
  execute GPU instancing, so the render bridge CPU-expands instances into
  ordinary glTF nodes (correctness first, N draw calls — ADR-0008).

### Fixed
- Three real bugs found during bring-up: i3dm header is 32 bytes (not 28),
  multi-buffer merging, and converter up-axis conjugate compensation.
- Pixel assertions: rebase near/far bit-identical, RTC vs no-RTC
  bit-identical; corrupt i3dm skipped gracefully.

## [0.1.0] — 2026-09-29 (P6: sanitizer gate + robustness)

### Added
- `linux-asan` preset (`-fsanitize=address,undefined`, own targets only;
  `-fno-sanitize=vptr` — prebuilt third-party libs ship without RTTI, so
  vptr reports are false positives).
- 6 sanitizer scenarios + fault injection (missing path, corrupt
  tileset.json, corrupt glb all fail gracefully) + API lifecycle tests.
- `registerAllTileContentTypes` made `std::call_once`; `renderer.cpp` now
  really calls `engine->destroy(renderer)` with a corrected teardown order.
- CI runs the sanitizer gate as its own job (not local-only).

### Notes
- One documented suppression: cesium-native v0.64.0 `CurlAssetAccessor`
  leaks a cache handle (third-party, upstream).

## [0.1.0] — 2026-09-29 (P5: HTTP, b3dm, rebase)

### Added
- `http(s)://` tileset loading via CesiumCurl (tested against a local
  `http.server`; no external network dependency).
- `b3dm` batched meshes via `Model` → `writeGlb` → gltfio conversion path.
- ECEF local-origin rebase: translating by ~123456789 m renders
  bit-identical pixels (kills float32 jitter for far-from-origin data).

## [0.1.0] — 2026-09-29 (P4: four-platform wiring)

### Added
- Swapchain wiring for Android/Vulkan (`ANativeWindow`), Windows/Vulkan
  (`HWND`), iOS/Metal (`UIView`); Filament v1.77.0 prebuilt packages per
  platform. Squash-merged via PR #1 with CI 5/5 green.
- Fixed a CI trigger misconfiguration (`main` → `master`).

### Notes (ADR-0006)
- WASM is a stub: Filament ships only filament.js, no C++ library.
- Honest boundary: only Linux headless rendering is pixel-verified;
  Android/Windows/iOS have no real-device verification.

## [0.1.0] — 2026-09-29 (P3: real tileset on screen)

### Added
- Real tileset scheduling (`Tileset::updateViewGroup` + `loadTiles`) and
  the full `IPrepareRendererResources` → gltfio `AssetLoader`/ubershader →
  Filament scene pipeline. Two root causes in the scheduling path fixed.
- Deterministic self-generated tileset (`tests/data/gen_p3_tileset.py`,
  no network); 3 colored boxes on screen.

### Notes (ADR-0005)
- Local paths only, bare GLB only, no ECEF→ENU rebase (P5 closed these).

## [0.1.0] — 2026-09-29 (P2: real Filament rendering on Linux)

### Changed
- `initialize`/`renderFrame` went from stub to real implementation:
  Filament Engine (OpenGL) + X11 swapchain + Scene/View/Camera with
  ordered shutdown. Headless verification (xvfb + Mesa software rendering):
  deep-blue clear + solid-red unlit triangle, pixel-verified.

### Notes (ADR-0004)
- Software rendering validates correctness only, never performance.

## [0.1.0] — 2026-09-29 (P1: SDK/demo split)

### Changed
- Split into `tiles_renderer` (static SDK, **zero SDL dependency** —
  ADR-0003) and `tiles_demo` (SDL3 host). The SDK takes a native window
  handle (`HWND` / `ANativeWindow` / `UIView` / canvas); the host app owns
  the window and the lifecycle.
- SDK links Cesium Native v0.64.0 + Filament v1.77.0; the demo links
  SDL3 3.4.16. Verified with `nm`: zero SDL symbols in the SDK archive.

## [0.1.0] — 2026-09-29 (P0: scaffold)

### Added
- Repository scaffold: CMake presets for linux/windows/android/ios/wasm,
  GitHub Actions CI matrix, `src` skeleton, smoke test, ADR-001
  (Cesium Native + Filament stack decision).
