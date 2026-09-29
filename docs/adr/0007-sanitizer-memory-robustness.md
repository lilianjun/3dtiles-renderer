# ADR-0007: Sanitizer builds and memory/resource robustness (P6)

Date: 2026-09-29
Status: accepted

## Context

The SDK now does real work every frame: Filament engine/swapchain/scene
lifetime, cesium-native tile scheduling across loader threads, gltfio asset
creation/destruction, libcurl workers, and host-driven lifecycle
(initialize/shutdown, possibly repeated). Manual review does not scale for
this — we want machines checking every run.

## Decision

1. **Sanitizer builds for our targets only** (`-DTILES_SANITIZE=ON`, or the
   `linux-asan` CMake preset):
   `-fsanitize=address,undefined` + `-fno-omit-frame-pointer` +
   `-fno-sanitize-recover=all`, applied via `tiles_apply_sanitizers()` to
   `tiles_renderer`, `tiles_demo`, `tiles_renderer_smoke`,
   `tiles_renderer_lifecycle` — and nothing else.
2. **Third-party code stays uninstrumented** (cesium-native, SDL3, Filament
   prebuilts). Rationale: prebuilt or hours to rebuild; the ASan/LSan
   runtimes still intercept *all* allocations at run time, so leaks inside
   third-party code are still detected — they are triaged, not hidden.
3. **Leak triage policy** (`tests/lsan.supp`):
   - Leaks in our code (`src/`, `samples/`) are bugs — fix, never suppress.
   - Third-party one-time-init / driver leaks (Mesa, X11, SDL, Filament
     prebuilts, cesium-native) get a suppression entry **with a comment
     naming the source and the reporting scenario**.
   - Broad patterns (e.g. `leak:libGL*`) are banned: Filament/GL resource
     lifetime is our responsibility (see shutdown ordering below).
4. **Scenario gate** (ctest, only when `TILES_SANITIZE=ON`): the P2 scene,
   P3 local tileset, P5 b3dm, P5 HTTP load, fault injection, and the API
   lifecycle test each run under `ASAN_OPTIONS=detect_leaks=1`,
   `UBSAN_OPTIONS=halt_on_error=1`, `LSAN_OPTIONS=suppressions=...`. Any
   sanitizer marker in the output, or any unexpected exit code, fails.
5. **Fault-injection tests** (always on, sanitizer or not): nonexistent
   tileset path → graceful exit 1; corrupt tileset.json → graceful exit 1
   (bounded 30 s root-tile wait); corrupt .glb → tile skipped, exit 0.
   Crashes (signal exits) fail the test.
6. **API lifecycle test** (`tests/lifecycle.cpp`): pre-init calls, invalid
   configs, double initialize, post-shutdown calls, and re-initialize after
   shutdown all have defined, asserted behavior.

## Shutdown ordering (verified by the leak gate)

`Renderer::shutdown()` must destroy, in order:
1. the tileset (`TilesetRenderer` reset) — tile `free()` needs a live engine;
2. user Filament resources (lights, renderables, materials, buffers, camera);
3. `View`, `Scene`, **`Renderer`**, **`SwapChain`** — reverse of creation
   order per the Filament docs (P6 fix: the old code destroyed the
   SwapChain before the Renderer, and never called
   `engine->destroy(renderer)` at all — the pointer was only nulled);
4. `Engine::destroy` last.

## UBSan `vptr` vs `-fno-rtti` prebuilts (false positive, 2026-09-29)

The first tileset run under UBSan reported
`member call on address ... which does not point to an object of type
'MaterialProvider'` with `object has invalid vptr` in
`~FilamentPrepareResources` (`_materialProvider->destroyMaterials()`).
Root-caused as a **false positive**:
- the vptr is bit-identical at construction and destruction (instrumented
  debug print) and ASan reports no heap error on the object;
- `typeid(*_materialProvider)` shows the vtable's typeinfo slot is null;
- the prebuilt Filament **and** cesium-native static libs contain **zero**
  `__typeinfo` symbols — both are compiled with `-fno-rtti`, so UBSan's
  vptr validator can never resolve their dynamic types.
Fix: `-fno-sanitize=vptr` in `tiles_apply_sanitizers()` (all other UBSan
checks stay on), with the rationale in a code comment. Do not use `typeid`
on Filament/cesium-native objects.

## Consequences

- `cmake --preset linux-asan` gives a Debug + sanitizer build in
  `build/linux-asan` (fresh build dir; third-party rebuilds there are
  expected to take a while on small machines).
- On the devserver, `-DTILES_SANITIZE=ON` can also be flipped on the
  existing `build/linux` dir: only our targets rebuild (~minutes).
- `registerAllTileContentTypes()` now uses `std::call_once` (P3 TODO):
  the old `static bool` flag raced on concurrent first use.
- iOS/Android/Windows/WASM keep their existing validation story (P4);
  sanitizers are a Linux-host gate.

## Results

(P6 verification, 2026-09-29, devserver Ubuntu 24.04, Mesa/llvmpipe + xvfb.)

Sanitizer gate (`-DTILES_SANITIZE=ON`, ASan+LSan+UBSan, `-fno-sanitize=vptr`
per above, `-fno-sanitize-recover=all`): **6/6 pass**
- `sanitizer_demo` (P2 scene), `sanitizer_tileset` (P3 local tileset),
  `sanitizer_b3dm` (P5 b3dm), `sanitizer_http` (P5 HTTP),
  `sanitizer_faults` (fault injection), `sanitizer_lifecycle` (API
  lifecycle) — zero ASan/UBSan errors, zero unsuppressed leaks.

Normal suite (`TILES_SANITIZE=OFF`): **9/9 pass** (7 pre-existing +
`lifecycle` + `fault_inputs`). SDK `nm` check: zero SDL symbols.

Triage findings:
- **1 real third-party leak fixed by suppression**: cesium-native v0.64.0
  `CurlAssetAccessor::CurlCache` never calls `curl_easy_cleanup()` on its
  cached `CURL*` handles (~9.4 MiB per HTTP tileset load). Suppressed in
  `tests/lsan.supp` entry 1 with source/scenario comment; proper fix is
  upstream (destructor for `CurlCache`). Our code never calls libcurl
  directly, so the `leak:Curl_*` patterns cannot hide our own leaks.
- **1 test bug fixed**: `tests/lifecycle.cpp` never pumped SDL events, so
  the X11 connection was never flushed, the window never mapped, and
  Filament's `beginFrame()` kept reporting "swap chain not ready". Fixed
  with a `pumpEvents()` + `renderFrames()` helper (mirrors the demo's main
  loop); the SDK contract (return false → caller retries) was already
  correct.
- **0 leaks / UB in our code** (`src/`, `samples/`, `tests/`).

Known limits: sanitizer gate is Linux-only; iOS/Android/Windows/WASM keep
their P4 validation story (no on-device runs here).
