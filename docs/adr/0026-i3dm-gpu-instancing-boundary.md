# ADR-0026: i3dm GPU instancing — investigated, boundary stands (P27)

Date: 2026-09-30
Status: accepted (boundary, not a fix)

## Context

P7 (ADR-0008) delivers correct i3dm by expanding
`EXT_mesh_gpu_instancing` into plain glTF nodes on the CPU: N instances =
N draw calls. P27 set out to replace that path with real GPU instancing —
one GPU-instanced draw per mesh, screenshots bit-identical to the CPU
expansion. The task explicitly allowed the boundary outcome: if Filament
v1.77 + gltfio cannot be hooked up reliably, document the boundary
instead of shipping a half-baked implementation.

This ADR records the investigation (done against the exact pinned
toolchain: Filament v1.77.0, Cesium Native v0.64.0, SDL3 3.4.16) and why
the P7 CPU path stays.

## Investigation

### Path 1: manual `InstanceBuffer` — blocked by the ubershader

Filament v1.77 does expose the primitives the task description assumed:

- `filament::InstanceBuffer` exists
  (`include/filament/InstanceBuffer.h`).
- `RenderableManager::Builder::instances(size_t, InstanceBuffer*)`
  exists — but it must be passed **at Builder time**; there is **no**
  public runtime setter to attach an instance buffer to an already-built
  renderable (the assumed `RenderableManager::setInstances` does not
  exist in v1.77).
- Per-instance count with transforms is capped by
  `Engine::getMaxAutomaticInstances()` — 64 on most platforms — so 256
  instances could never be "one draw per mesh" anyway (minimum 4 batches).

The harder block is the shader. `RenderableManager.h` documents, for
both `instances()` overloads:

> *The material must set its `instanced` parameter to `true` in order to
> use `getInstanceIndex()` in the vertex or fragment shader to get the
> instance index and possibly adjust the position or transform.*

The engine does **not** apply `InstanceBuffer` transforms by itself —
the backend only issues `glDrawElementsInstanced`; per-instance
transforms reach pixels only through shader code that reads them.
Verified against v1.77.0 sources:

- the gltfio ubershader template
  (`libs/gltfio/materials/base.mat.in`, tag v1.77.0) contains **zero**
  references to instancing;
- the prebuilt `matc` rejects an `instancing` material key
  (`Ignoring config entry (unknown key)`), so the compiled uberz
  variants cannot have instancing enabled either;
- `MaterialBuilder::instanced(bool)` is a **compile-time** property
  baked into generated shader code (`MATERIAL_HAS_INSTANCES`
  define) — there is no runtime mechanism to inject instance handling
  into an already-compiled material.

Consequence: attaching an `InstanceBuffer` to a gltfio-created
renderable cannot work — the ubershader would render every instance
with the same transform. And gltfio offers no hook anyway:
`AssetLoader` builds renderables internally, and no public API exposes
the `Builder` before `build()` or lets us rebuild a renderable from a
gltfio asset while preserving its mesh/material/texture/PBR state.

The only way to make manual instancing render correctly would be a
custom instancing-capable PBR material — i.e. re-implementing the
ubershader (lit model, textures, normal mapping, P26 IBL) just for i3dm
tiles. That duplicates P15/P26 for a performance optimization we cannot
even measure (ADR-0023 G6: Mesa software rendering, no perf signal).
Rejected.

### Path 2: automatic instancing (`RenderPass::instanceify`) — real but opportunistic

Filament has a second, separate mechanism: `RenderPass` merges
consecutive draw commands with identical material instance + geometry +
raster state into one instanced draw, staging each command's
`PerRenderableData` (which carries the world transform) into a
`data[CONFIG_MAX_INSTANCES]` UBO array. It is:

- **off by default** (`mAutomaticInstancingEnabled = false`), enabled
  by one public call: `Engine::setAutomaticInstancingEnabled(true)`;
- **material-agnostic** — works with the ubershader, no shader change
  needed, screenshots bit-identical by construction (same transforms,
  fewer draw calls);
- **opportunistic, not guaranteed**: merging requires commands to be
  *consecutive after sorting*, and the sort key puts a 10-bit Z-bucket
  **above** the 32-bit material-id — instances spread in depth sort
  apart and do not merge (the source even carries a TODO: *"we need to
  add a 'primitive id' in the low-bits of material-id, so that
  auto-instancing can work better"*). It cannot promise "one draw per
  mesh" for 3D-distributed instances;
- a **global engine flag** affecting every tile, not just i3dm, with no
  measurable benefit on our Mesa verification stack.

It is a legitimate future optimization, but enabling it would not
"replace the CPU expansion path" (the task's bar) and would add an
unmeasurable global behavior change. Not done in P27.

### Path 3: bake instances into one mesh (vertex duplication)

Merging N transformed copies into a single vertex buffer would give one
draw per mesh, but it is CPU-side vertex duplication, not GPU
instancing — 256x vertex memory, still a "CPU expansion", just at a
different level. Does not meet the task's intent. Rejected.

## Decision

**Keep the P7 CPU expansion as the correctness path. P27 closes as a
documented boundary, not a code change.**

- No new code, no new tests, no public API change, no behavior change.
- The i3dm fixtures from P7 (12/8/256 instances, POSITION/SCALE/
  ROTATION, RTC_CENTER, rebase, corrupt-input gates) remain the
  correctness bar and are not regressed (ctest 26/26 re-verified).
- ADR-0023 G6 is updated to cite this investigation.

## When to revisit

1. **A Filament version whose gltfio executes `EXT_mesh_gpu_instancing`
   natively** (or exposes a Builder hook / post-build instance-buffer
   attachment) — then the expansion can sit behind a version check,
   as ADR-0008 already anticipated.
2. **Real-device perf signal** (ADR-0023 G1): if i3dm draw calls ever
   show up in a profile on Android/iOS/Windows, reconsider enabling
   `setAutomaticInstancingEnabled(true)` as a measured optimization —
   but validate bit-identical goldens first, since it is a global flag.

## Consequences

- `docs/adr/0023-remaining-gaps-roadmap.md` G6 now points here.
- README/CHANGELOG note P27 as "investigated, boundary stands".
- Honest scope, unchanged: i3dm renders correctly via CPU expansion
  (N draw calls); GPU instancing awaits upstream support or a measured
  reason to flip the automatic-instancing flag.
