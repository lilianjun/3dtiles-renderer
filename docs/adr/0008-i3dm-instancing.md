# ADR-0008: i3dm real support via CPU instance expansion (P7)

Date: 2026-09-29
Status: accepted

## Context

P5 closed the b3dm gap but left i3dm (Instanced 3D Model) as a TODO. P7
delivers real i3dm support end-to-end:

```
i3dm bytes -> cesium-native I3dmToGltfConverter
            -> CesiumGltf::Model with EXT_mesh_gpu_instancing
            -> SDK modelToGlb -> gltfio AssetLoader -> Filament Scene
```

Test data is self-generated and deterministic
(`tests/data/gen_p7_i3dm_tileset.py`, no network), covering POSITION /
SCALE / per-instance rotation (NORMAL_UP + NORMAL_RIGHT) / 256 instances /
RTC_CENTER / a 123456789 m tile-transform rebase.

## Decision

### 1. i3dm header is 32 bytes, not 28

During development we initially packed a 28-byte header (the b3dm layout
minus `glTFFormat`) and got `GLB extends past the end of the buffer` from
the converter. Per the 3D Tiles 1.0 spec the i3dm header is
magic(4) + 7x uint32 = **32 bytes**. The generator packs
`struct.pack("<4sIIIIIII", ...)` and documents the b3dm/i3dm difference.
Lesson recorded: test the converter against self-made bytes early; a
one-field header mistake surfaces far from its cause.

### 2. Trust the converter's documented output, verify with pixels

We read cesium-native v0.64.0 `I3dmToGltfConverter.cpp` and confirmed it
outputs a `CesiumGltf::Model` whose mesh node carries
`EXT_mesh_gpu_instancing` with `TRANSLATION` / `ROTATION` / `SCALE`
accessors, supporting `INSTANCES_LENGTH`, `POSITION`(/`_QUANTIZED`),
`SCALE`(/`_NON_UNIFORM`), `NORMAL_UP`/`NORMAL_RIGHT`, and `RTC_CENTER`.
It recenters instance positions by their mean and writes the mean (+
RTC_CENTER) into the glTF root node translation. Our fixtures then
**verify behavior with screenshots**, not with code-reading alone —
which is what caught decision 3.

### 3. Filament v1.77 gltfio does not execute EXT_mesh_gpu_instancing — expand on the CPU

Early in P7 we assumed gltfio implements the extension because the
extension *name* exists in the binary. A rendered screenshot proved
otherwise: Filament drew the base mesh exactly once and dropped the
instance attributes. (Symbol presence is not feature presence.)

Decision: the render bridge **expands GPU instancing into plain glTF
nodes, prioritizing correctness** (`expandGpuInstancing()` in
`src/tileset.cpp`):

- for each instance, clone the instanced node with baked
  `baseNodeMatrix * instanceTRS`;
- instances without TRS attributes get the base node's transform;
- identity TRS fields are written as defaults so the writer omits the
  keys (an empty `"translation":[]` array made gltfio's
  `createAsset` fail — a real crash-adjacent bug we hit and fixed);
- the extension is stripped from the node and from the model's
  used/required extension lists.

**Honesty note, by design:** this costs N draw calls instead of one
instanced draw. It is a correctness-first bridge, not a performance
optimization. The 256-instance test asserts correctness (loads, renders,
no crash) and must never be quoted as a performance number. A future
Filament with real instancing support can drop the expansion behind a
version check.

### 4. Merge all buffers when re-serializing to GLB

P5's `modelToGlb()` only wrote `model.buffers[0]`. i3dm-converted models
carry buffer 0 (mesh) + buffer 1 (instance TRANSLATION/ROTATION/SCALE),
so naive re-serialization silently dropped every instance. The fix merges
all buffers into one GLB BIN chunk with 4-byte alignment, rewrites every
`bufferView.byteOffset` / `buffer`, and updates `byteLength`. This also
hardens the b3dm path (multi-buffer models in general).

### 5. Apply the converter's up-axis conjugation at the tile root

The i3dm converter computes per-instance matrices as
`toTileInv * instanceTransform * toTile`, where `toTile` includes the
glTF Y-up -> Z-up rotation — it assumes the runtime applies the same
up-axis rotation at the content root. Our Filament bridge never did, so:

- small-coordinate instances rendered offset from their own bounding
  volume (visible as tilted/stacked boxes);
- RTC_CENTER instances landed millions of meters away (empty frame).

Fix: when the converted model carries `EXT_mesh_gpu_instancing`, the
tile root node gets
`tileTransform * translate(rtcCenter) * upAxisToZUp` composed before the
double-precision local-origin subtraction (`upAxisToZUp()` mirrors
cesium-native's X/Y/Z-up matrices). Evidence: the main fixture's layout
snapped to the authored ground-plane grid, and the RTC fixture now
renders.

### 6. Large-coordinate rebase is bit-exact for the tested values

- `p7_i3dm_rebase`: identical i3dm, `far` adds a 123456789.0 m root
  transform. Screenshots are **bit-identical** (0 differing pixels).
- `p7_i3dm_rtc`: `RTC_CENTER = [1210000.0, -4736290.5, 4081600.0]`
  vs a no-RTC reference with pre-shifted positions. Screenshots are
  **bit-identical** — the converter's up-axis-conjugated RTC offset is
  cancelled exactly by the SDK's double-precision rebase.

Scope honesty: the rebase fixture tileset keeps the bounding volume in
tile-LOCAL coordinates (the spec's rule; the tile transform carries the
offset). An earlier draft added the offset to the volume as well, which
would have double-translated it — caught in review, not by a test
failure, because the wrong volume only perturbs culling/origin.
Bit-exactness is proven only for the chosen (integer/0.5) coordinates;
arbitrary sub-meter RTC values are not covered.

### 7. Fault injection: corrupt i3dm fails gracefully

`fault_test.py` gains three cases (same contract as corrupt .glb: tile
skipped, demo exit 0, never a crash):
- **D** truncated mid-header;
- **E** wrong magic (`xxxx`);
- **F** `INSTANCES_LENGTH` doubled past the POSITION data (the converter
  warns `Matrix decompose failed` and substitutes identity — no crash).

## Consequences

- `tests/i3dm_test.py` (ctest `i3dm_tileset_screenshot`): 12 orange
  (POSITION+SCALE) + 8 teal (per-instance yaw) instances across 2 tiles
  with blob-count assertions (several blobs per color proves expansion,
  not one box per tile); 256-instance stress (correctness only);
  rebase near/far bit-identical; RTC vs reference bit-identical.
- `sanitizer_i3dm` added to the linux-asan gate (now 7 scenarios);
  `fault_inputs` covers the three corrupt-i3dm cases.
- README format matrix updated; the i3dm TODO is closed.
- Known limits: CPU expansion = N draw calls (not GPU instancing);
  instancing detection keys on the extension being present on a converted
  model (`fromI3dm`); nested instanced-node graphs not covered;
  Android/Windows/iOS have no on-device verification (as before).
