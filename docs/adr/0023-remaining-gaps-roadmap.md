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
WASM). This ADR honestly lists what is still **not** done, what each
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

### G2. Corrupt cmpt can SIGSEGV the host (upstream #1457, open, 0 comments)

- **State:** `CmptToGltfConverter` leaves an empty model with only a
  warning on structural failure; `TilesetJsonLoader` then dereferences
  it → SIGSEGV (reproduced locally, exit 139). Filed as
  `CesiumGS/cesium-native#1457` on 2026-09-29; still open, no upstream
  response.
- **Takes:** either an upstream fix, or a defensive SDK-side guard
  (validate converter result before handing to the loader — needs care
  not to fork loader behavior).
- **Why P0:** a malformed tile crashing the host process is a
  security-relevant robustness hole; everything else in P6/P23 was
  built to fail gracefully.

## P1 — worth doing

### G3. KTX2 / BasisU textures not wired

- **State:** the render bridge registers texture providers only for
  `image/png` and `image/jpeg` (`src/tileset.cpp`, P15). A glTF using
  `KHR_texture_basisu` / `image/ktx2` gets gltfio's "Missing texture
  provider" warning and renders **black** — same silent-black failure
  mode P15 fixed for PNG/JPEG.
- **Takes:** a KTX2/BasisU transcoder (e.g. basisu native library)
  integrated per platform + `addTextureProvider("image/ktx2", …)` +
  pixel-verified fixture. Non-trivial dependency work on 4 toolchains.
- **Why P1:** KTX2 is the standard compressed texture format for glTF;
  real-world tilesets use it. Not P0 because uncompressed PNG/JPEG
  tilesets (the common case for our fixtures) work.

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

### G5. No IBL (image-based lighting)

- **State:** PBR verified against a single directional light (P15);
  metals show only the sun's specular lobe and otherwise go dark.
  Honest look, documented — but not product-grade visuals.
- **Takes:** an environment map / IBL probe pipeline + **re-freeze of
  all golden screenshots** (every golden changes).
- **Why P1:** visual quality, not correctness. Gated behind a deliberate
  product decision because it invalidates the golden baseline.

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

### G7. Draco mesh compression — not supported

- **State:** no Draco decoder in the dependency tree; Draco-compressed
  glTF content will fail to decode.
- **Takes:** draco library on Linux/Windows/Android/iOS/WASM
  toolchains + gltfio decoder hookup.
- **Why P2:** valuable compression win in theory, heavy dependency in
  practice; uncompressed tilesets are fully functional. Revisit if a
  target tileset requires it.

### G8. pnts point size fixed at 1px

- **State:** Filament v1.77 gltfio renders POINTS at a hardcoded 1px
  (P8 verified: 200 red + 120 green + 100 blue, each exactly 1px).
- **Takes:** custom point-cloud material/render path bypassing gltfio.
- **Why P2:** cosmetic; points are visible and correctly colored.

### G9. WASM is a stub

- **State:** the official Filament Web distribution ships filament.js
  with no linkable C++ library (ADR-0006), so the WASM SDK build is a
  compile-only stub. Cannot be fixed from our side.
- **Why P2 / won't fix:** blocked on upstream Filament shipping a C++
  WebAssembly library. Revisit if that ever happens.

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
