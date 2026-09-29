# Golden screenshot regression baseline (P11)

Frozen, human-verified renders — one per phase — that the `golden_regression`
ctest re-renders and compares pixel-for-pixel on every run. Any future
render-pipeline regression fails loudly instead of drifting silently.

## The set

All 800x600 PNG, rendered with `tiles_demo` under `xvfb` + Mesa software GL
(via `stb_image_write`, deterministic encoding). Frozen 2026-09-29 from
renders each phase's own pixel assertions had already accepted.
Re-frozen 2026-09-29 (P26): the 9 lit entries re-rendered with the default
procedural IBL now on — every changed image human-reviewed (geometry pixel
counts identical, only lighting changed); `p2_demo` and `p8_pnts` (unlit)
are unchanged. See ADR-0025.

| golden | phase | fixture | frames | what it proves |
|---|---|---|---|---|
| `p2_demo.png` | P2 | `--no-tileset` (clear + red unlit triangle) | 30 | real Filament engine/swapchain/render path |
| `p3_tileset.png` | P3 | `tests/data/p3_box_tileset` | 60 | cesium-native scheduling → gltfio → Filament |
| `p5_b3dm.png` | P5 | `tests/data/p5_b3dm_tileset` | 60 | b3dm → Model → writeGlb → gltfio |
| `p7_i3dm.png` | P7 | `tests/data/p7_i3dm_tileset` | 60 | i3dm instancing via CPU expansion (ADR-0008) |
| `p8_pnts.png` | P8 | `tests/data/p8_pnts_cloud` | 60 | pnts POINTS, 1 px/point exact colors |
| `p9_cmpt.png` | P9 | `tests/data/p9_cmpt_tileset` | 60 | cmpt: b3dm + pnts + i3dm in one frame |
| `p10_11_glb.png` | P10 | `tests/data/p10_11_glb` | 90 | 1.1 bare-glb content (`3DTILES_content_gltf`) |
| `p10_11_implicit.png` | P10 | `tests/data/p10_11_implicit` | 90 | 1.1 implicit QUADTREE, 5 tiles |
| `p16_traj_f00.png` | P16 | `p3_box_tileset` + `trajectories/p16_orbit_push.csv` | traj frame 0 | deterministic replay: first frame |
| `p16_traj_f05.png` | P16 | `p3_box_tileset` + `trajectories/p16_orbit_push.csv` | traj frame 5 | deterministic replay: middle frame |
| `p16_traj_f11.png` | P16 | `p3_box_tileset` + `trajectories/p16_orbit_push.csv` | traj frame 11 | deterministic replay: last frame |

Total size ~192 KB. Kept small on purpose: only the single most
representative render per phase (rebase/RTC/many-instance variants stay in
their phase tests, not here).

## Comparison policy

- **Local gate: strict equality (0 differing pixels).** Re-rendering is
  bit-identical run to run on this stack (verified for all 11 entries at
  freeze time: same md5 across runs).
- **CI escape hatch:** env `GOLDEN_MAX_DIFF_FRAC`, default 0, hard ceiling
  0.005 (0.5%). Exists for GL stacks that are not bit-identical across
  machines (mesa version / driver differences). Any nonzero diff is always
  printed with its exact pixel count — tolerance can never silently hide a
  regression. CI sets `GOLDEN_MAX_DIFF_FRAC=0.005` on the Linux job: the
  first completed CI run after P11 (P18's) showed 25–1389 differing pixels
  (max 0.29% on `p16_traj_f11`) against the dev machine's Mesa 25.2.8 —
  deterministic edge-rasterization variance, same scenes bit-identical
  locally. A real regression (missing box, wrong color, broken lighting)
  moves tens of thousands of pixels, so 0.5% still gates.
- Comparison is done by `tests/golden_test.py` (manifest + renderer +
  differ); it needs `Pillow` and `numpy`, same as the other screenshot
  tests.

## Updating goldens

Regeneration is a deliberate, reviewed act — see
`tests/golden/regenerate.py --help` and the policy in its docstring.
Short version: only when the render pipeline changed *intentionally*;
inspect every changed image yourself; explain the change in the commit
message (or an ADR). Never regenerate just to make a failing gate pass.

## Why sanitizer builds skip goldens

`golden_regression` is registered only when `TILES_SANITIZE=OFF`. ASan/UBSan
instrumentation changes codegen and can perturb floating-point scheduling
enough to add pixel noise; sanitizer builds exist to catch memory bugs, not
to hold the pixel baseline. Memory correctness and pixel correctness are
separate gates, each strict in its own lane.
