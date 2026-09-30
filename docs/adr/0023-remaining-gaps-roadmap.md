# ADR-0023: Remaining-gaps roadmap (P24)

## Status

Accepted (2026-09-29). Living document: update an item's row when its
state changes; do not rewrite history.

## Context

P0–P23 plus two hotfixes brought the SDK to: all 3D Tiles 1.0 tile
formats pixel-verified (b3dm/i3dm/pnts/cmpt), 3D Tiles 1.1 bare-glb +
implicit QUADTREE, PBR six-feature verification, frustum culling,
REPLACE/ADD refinement, region bounding volumes, weak-network gates,
cache-budget boundedness, tileset switch safety, corrupt-input
fail-fast, golden regression, sanitizer gates, install packaging, and a
first fully-green six-job CI (Linux/sanitizers/Windows/Android/iOS/
WASM — the WASM job was later removed 2026-09-30, see G9). This ADR honestly lists what is still **not** done, what each
item would take, and a priority from a production-SDK viewpoint.
"Won't fix (for now)" is a valid, documented outcome — the rule is no
silent gaps.

Priority scale: **P0** = blocks calling this a production SDK;
**P1** = worth doing, real user value; **P2** = nice-to-have or
explicitly out of scope.

## P0 — must resolve before "production"

### G1. No real-device render verification (Android / iOS / Windows)

- **State:** CI compiles all four platform configs, but *zero* pixels
  have ever been verified off Linux/Mesa. Android (Vulkan/
  ANativeWindow), iOS (Metal/UIView), Windows (Vulkan/HWND) swapchain
  wiring is code-reviewed only.
- **Takes:** physical devices (or a device farm) + a small on-device
  smoke harness (load p3 fixture, screenshot, compare hash). Blocked on
  hardware access, not engineering.
- **Why P0:** the SDK's entire value proposition is cross-platform; an
  unrendered platform is an untested promise.
- **Plan (2026-09-30, user-approved, revised same day):** Android goes
  first. `samples/android-smoke` is a minimal on-device harness: it inits
  the Renderer on the `SurfaceView`'s ANativeWindow, loads the bundled p3
  fixture, renders until the tile pipeline settles (same 20-frame streak
  rule as the desktop demo's `--until-loaded`), then shows PASS/FAIL +
  tile counts on screen and writes `smoke_result.txt`.
  - **Route change (2026-09-30):** the paired-device channel exposes no
    APK install / app launch / screenshot commands, so the original
    "watcher cron installs the APK when the phone comes online" plan is
    **abandoned** (cron `android-smoke-on-device-online` disabled). New
    route: CI's android job builds a debug-signed `app-debug.apk`
    (libsmoke_jni.so comes from the repo's CMake android preset as a
    prebuilt jniLib; Gradle does no NDK compile) and uploads it as the
    `android-smoke-apk` artifact → li downloads, installs, opens the app,
    waits ~30s, screenshots the PASS/FAIL overlay and sends it back.
  - iOS/Windows real-device runs remain open (need hardware).
  - **Result (2026-09-30): Android PASS on first real-device run.**
    Redmi K70 Ultra, CI run 36674302583 (`android-smoke-apk`, commit
    d8e3089): `PASS / tiles: loaded=4 failed=0 rendered=1 /
    frames=22 settle=323ms`. Renderer initialized on the ANativeWindow,
    Cesium Native loaded the bundled p3 fixture (4 tiles, 0 failed),
    Filament rendered actual pixels (box visible in screenshot). Two
    packaging bugs were caught and fixed along the way: the first APK
    shipped a stub SDK (android job missed `-DTILES_WITH_CESIUM_NATIVE=ON`,
    commit b313666) and the first full build failed to link
    (vcpkg-built spdlog/curl need API 28+ libc symbols; ANDROID_PLATFORM
    and app minSdk raised 24 → 28, commit d8e3089). G1 is now
    **partially closed (Android + Windows)** — see Windows bullet below.
  - **Windows (2026-09-30): PASS on first real-device run.** li ran
    the CI-built `windows-smoke` artifact (run 36707534325, commit
    9dd407b) on his Windows PC: NVIDIA GeForce RTX 3060, Filament
    resolved the **Vulkan** backend, created a real 800x600 swapchain,
    `SMOKE PASS / tiles: loaded=4 failed=0 rendered=3 / frames=22`.
    Renderer initialized on the HWND, Cesium Native loaded the bundled
    p3 fixture (4 tiles, 0 failed), Filament rendered actual pixels on
    a real GPU. Two link bugs were caught and fixed getting there:
    `backend.lib`'s WGL/OpenGL objects needed `bluegl` + `opengl32`
    (180 unresolved symbols, commit 9dd407b), and downloading the
    official windows tgz proved it always shipped `stb.lib` — the old
    "no libstb on Windows" claim was wrong; the detector only accepted
    the Unix name `libstb.a`, silently disabling texture decoding on
    Windows (same commit fixed it). The `windows-smoke` artifact was
    package-verified before shipping to li: PE32+ x86-64, only system
    DLL imports, real Cesium/Filament symbols (not stubs), valid
    `tileset.json` + 3 GLBs.
  - G1 is now **partially closed (Android + Windows)**; iOS remains open
    (deferred — li has no iPhone).

### G2. Corrupt cmpt can SIGSEGV the host (upstream #1457, open, 0 comments)

- **State (2026-09-30, P28): FIXED SDK-side.** The P9 ADR-0010 claim
  "no clean interception point" was re-examined and found wrong:
  `GltfConverters::registerMagic` overwrites, so the SDK now
  re-registers hardened wrappers for the five magic-dispatched
  converters after its one-time `registerAllTileContentTypes()` call.
  "Empty model + warnings-only" is promoted to a hard error, and the
  tile fails gracefully (`failed` counter, process alive) instead of
  the unchecked `*result.model` dereference in `TilesetJsonLoader`
  (UB, SIGSEGV). See ADR-0027. Upstream #1457 itself remains open.
- **Takes:** nothing further unless upstream changes `registerMagic`
  overwrite semantics.
- **Why P0:** a malformed tile crashing the host process is a
  security-relevant robustness hole; everything else in P6/P23 was
  built to fail gracefully.

## P1 — worth doing

### G3. KTX2 / BasisU textures — ✅ wired (P25, ADR-0024)

- **State:** closed 2026-09-29. The P24-era premise ("only PNG/JPEG
  providers registered") was fixed by P25: cesium builds (Linux) use an
  internal provider (`src/ktx2_libktx_provider.cpp`) that decodes via
  cesium's own libktx to RGBA32 — exactly one `basist::` copy per
  binary, avoiding the heap-corruption symbol collision between
  cesium's libktx and Filament's basis_transcoder (verified by link
  experiment, see ADR-0024). Non-cesium platforms (Win/Android/iOS) use
  Filament's `createKtx2Provider`. Pixel-verified: UASTC fixture renders
  bit-identical to its PNG twin; corrupt KTX2 fails gracefully.
- **Remaining honest boundary:** transcodes to RGBA32, not GPU block
  compression (BasisU VRAM advantage is future work).

### G4. TreeTraversalState pinning defeats `maximumCachedBytes` on shallow-wide trees

- **State:** verified on master (P24): 800 KB budget, 23 tiles /
  2,760,912 bytes resident for 146 straight frames, zero eviction.
  Root cause read from cesium-native v0.64.0 sources and cited
  (file:line) in `docs/upstream/issue-traversal-pinning.md`. Draft
  issue written, **not yet filed**.
- **Takes:** file the issue; then either upstream fix or an SDK-side
  mitigation (none known that doesn't fight the traversal).
  Documented workaround today: budget must exceed the per-frame
  touched working set (ADR-0017).
- **Why P1:** breaks the memory-boundedness promise for a class of
  tilesets; but deep/realistic trees evict correctly, so impact is
  bounded.

### G5. IBL (image-based lighting) — ✅ done (P26)

- **State:** closed 2026-09-30. `Renderer::initialize()` now builds a
  deterministic procedural 6x64x64 RGBA8 cubemap (analytic sky gradient
  + sun toward the P3 sun direction) plus a 3-band spherical-harmonics
  fit of the same function (Fibonacci 2048 samples) as the
  IndirectLight; GPU generates mipmaps. Zero new dependencies, zero
  binary blobs. Public API added exactly one method,
  `Renderer::setIblEnabled(bool)` (default on); demo flag `--no-ibl`
  for A/B. Measured: metallic=1 box mean 5.6 → 55.2 (~10x). All golden
  baselines re-frozen (default lighting changed); ctest 26/26,
  sanitizer_ibl clean, SDK zero SDL.

### G6. i3dm is CPU-expanded, not GPU instancing

- **State:** `EXT_mesh_gpu_instancing` is expanded to plain glTF nodes
  at load (ADR-0008) — N draw calls. Correct pixels, poor scaling.
  P27 investigated replacing it with GPU instancing and concluded it
  is a **documented boundary** (ADR-0026): Filament v1.77's
  `InstanceBuffer` requires shader cooperation (`instanced=true` +
  `getInstanceIndex()`), which the precompiled gltfio ubershader does
  not have, and gltfio exposes no hook to attach instance buffers to
  its renderables; the engine's automatic instancing (`instanceify`)
  is material-agnostic but opportunistic (sort-order dependent, cannot
  promise one draw per mesh) and a global flag with no measurable
  benefit on Mesa.
- **Takes:** a Filament version whose gltfio executes
  `EXT_mesh_gpu_instancing` natively (or exposes a Builder/post-build
  hook), or a measured perf reason on real hardware to enable
  `setAutomaticInstancingEnabled(true)` (see ADR-0026 for the
  revisit conditions).
- **Why P1:** performance, which we cannot even measure on Mesa
  software rendering — so this stays P1 until G1 gives us a device to
  measure on. Doing it blind risks optimizing the wrong thing.

## P2 — nice-to-have / explicitly out of scope

### G7. Draco mesh compression — ✅ verified working (P29, ADR-0028)

- **State:** the G7 premise was wrong: draco was in the dependency tree
  all along. cesium-native v0.64.0 lists `"draco"` in its `vcpkg.json`
  and `find_package(draco CONFIG REQUIRED)` unconditionally, so ezvcpkg
  provisions it on Linux/Windows/Android/iOS with zero per-platform
  work; `CesiumGltfReader` decodes `KHR_draco_mesh_compression` into
  plain accessors before gltfio ever sees the mesh
  (`GltfReaderOptions::decodeDraco` defaults to true). No gltfio hook
  exists or is needed. Pixel-verified 2026-09-30: Draco box renders
  **bit-identical** (0/480,000 px) to its uncompressed twin; corrupt
  Draco bitstream degrades gracefully (decode warning, exit 0, no
  crash). CTest `draco` + `sanitizer_draco` green.
- **Takes:** nothing — closed.
- **Why P2:** was "valuable compression win in theory, heavy dependency
  in practice"; in practice the dependency was already there.

### G8. pnts point size fixed at 1px

- **State:** Filament v1.77 gltfio renders POINTS at a hardcoded 1px
  (P8 verified: 200 red + 120 green + 100 blue, each exactly 1px).
- **Takes:** custom point-cloud material/render path bypassing gltfio.
- **Why P2:** cosmetic; points are visible and correctly colored.

### G9. WASM target removed (2026-09-30, user decision)

- **State:** the Emscripten/WASM build (compile-only stub — official
  Filament Web distribution ships filament.js with no linkable C++
  library, ADR-0006) has been **removed entirely**: wasm preset, CI job,
  `TILES_PLATFORM_WASM` code branches, and README references deleted.
  A stub that compiles is not WASM support, and keeping it green was
  masking that fact.
- **Future direction (per li):** the web renderer will be a **separate
  JS project** — loading and data parsing written in JS, rendering via
  filament.js. Cesium Native on Emscripten is out of scope (too costly
  for the value). Re-evaluate after the C++ SDK is complete.
- **Why not P2 / won't fix in this repo:** the C++ SDK will never
  target Web; web is a different project, not a gap in this one.

### G10. ECEF rebase is a local-origin translation, not full ENU

- **State:** large ECEF coordinates are translated to a local origin
  for float precision (P5, pixel-verified bit-identical); orientation
  is not converted to east-north-up.
- **Why P2:** sufficient for rendering correctness at the scales we
  target; full ENU only matters for geo-measurement accuracy.

## Already covered (not gaps — listed so nobody re-opens them)

b3dm / i3dm / pnts / cmpt, 3D Tiles 1.1 bare glb (`3DTILES_content_gltf`)
+ implicit QUADTREE, region/box bounding volumes, REPLACE/ADD
refinement, frustum culling, weak-network resilience, cache budget +
eviction, tileset switch safety, corrupt-input fail-fast, golden
regression, ASan/LSan/UBSan gates, install packaging + CI smoke.

## Consequences

- The next upstream-facing action is filing the traversal-pinning
  issue (draft in `docs/upstream/issue-traversal-pinning.md`).
- The next engineering phase should be picked from P0/P1 by weighing
  device access (G1/G6 need it) against dependency risk (G3).
- This file is updated, not rewritten, as items move.
