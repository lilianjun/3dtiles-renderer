# ADR-0018: 视锥剔除与 LOD 精化行为验证（P20）

日期：2026-09-29
状态：已验证（Linux/Mesa）

## 背景

P19 验证了缓存驱逐有界，但 3D Tiles 最核心的两个行为此前处于
"声称支持但无证据"状态：

1. **视锥剔除**：相机看不见的 tile 不应被选中/加载。
2. **SSE 精化**：相机拉近时父 tile 应被子 tile 替换（REPLACE），
   拉远后应恢复。

P20 用 P17 的 `Renderer::tileStats()` 做计数口径，用 P16 的轨迹机制
驱动相机，并新增 per-tile 证据口径
`Renderer::selectedTileIds()`（见下）。

## 公共 API

`Renderer::selectedTileIds()`（`include/tiles_renderer/renderer.h`）：

- 返回最近一次 `renderFrame()` 的
  `ViewUpdateResult::tilesToRenderThisFrame` 的 ID 字符串
  （经 cesium-native `TileIdUtilities::createTileIdString`）。
- 诊断用途（frustum/LOD 测试、宿主调试）；ID 字符串格式是
  cesium-native 的，不具契约性，测试只把它当不透明集合成员用。
- render 线程调用；无 tileset 时返回空。

demo 新增 `--print-selected`：每帧打印
`[selected] frame=N ids=<id1>,<id2>,...`（与 `--stats` 的
`[stats]` 行配对解析）。

## 关键发现

### 1. orbit 相机恒看目标点：yaw 旋转不改变可见集合

最初复用 P19 的深 fixture（`p19_deep_tileset`，Z-up 128×128×8
"竖墙"，341 tiles）做 yaw 旋转：`selectedTiles` 确实远小于总量
（27–124 << 341，剔除在工作），但 yaw~0 与 yaw~180 的选中集合
Jaccard 高达 0.81——**集合几乎不变**。

根因（我方相机模型，非上游 bug）：SDK 的 orbit 相机恒看向目标点
（`src/tileset.cpp`：`direction = normalize(target - eye)`），yaw
旋转只是让相机绕目标转圈，看到的始终是中央区域。更糟的是 p19
fixture 在相机 Y-up 世界里是一面竖墙：yaw=0 看正面，yaw=180 看背面，
tile 集合天然高度重合。用 orbit 相机测"转开 180°"本来就不成立。

对策：自生成 `tests/data/p20_frustum_tileset/`（`gen_p20_frustum_tileset.py`）——
Y-up 世界下的**水平地面**（512×512×10 slab，thin 轴为 Y），
4×4 共 16 个 REPLACE 子 tile。低 pitch（15°）+ 远距离（200m）使视锥
具方向性：yaw~0 看 -Z 远端，yaw~180 看 +Z 远端。

### 2. cesium-native 会创建一个空 ID 的 wrapper 根 tile

调试 tile 树发现（已用临时 `TILES_DEBUG_TREE` 打印确认，完事删除）：
explicit tileset 在 JSON root 之上还有一层 wrapper tile
（ID 为空字符串，无 content）。它的 refine 继承 JSON root：

- P3（ADD）：wrapper refine=ADD → 它出现在 `tilesToRenderThisFrame`
  中（`selected=4` 含空 ID）。
- P20-LOD（REPLACE）：wrapper refine=REPLACE → 被 `root.glb` 替换，
  不在 render selection 中（`selected=1` 仅 `root.glb`）。

这是上游正常行为，不是 bug。`tileStats().tilesLoaded` 的树 walk 从
`getRootTile()`（即 wrapper）开始计数，因此 explicit tileset 的
loaded 会比"有 content 的 tile 数"多 1（P20-LOD：FAR 时 loaded=2
= wrapper + root.glb）。

### 3. REPLACE 语义得到干净验证

`tests/data/p20_lod_tileset/`（`gen_p20_lod_tileset.py`）：
root（100×100×10，ge=20，REPLACE）+ 4 子 tile（50×50×10，ge=0）。
相机 FAR(1200m) → NEAR(300m) → FAR(1200m)：

| 阶段 | selected | loaded |
|---|---|---|
| FAR | `{root.glb}` | 2（wrapper+root） |
| NEAR | `{child_0..3.glb}`（**root.glb 不在集合中**） | 6（单调增加） |
| FAR2 | `{root.glb}`（回落） | 6（缓存命中，无重载） |

NEAR 时 root 不在选中集合中——这正是 REPLACE 与 ADD 的区别
（ADD 会是 5 个全选）。FAR2 回落证明选择器不是无脑叠加。

## 验证方法

`tests/frustum_lod_test.py`（ctest `frustum_lod`，<2s；3 遍 bit-identical）：

- **视锥**：121 帧 yaw 扫描（pitch=15°，dist=200m）。
  断言 `max(selected) <= 14 << 17`；yaw~0（±10°）与 yaw~180（±10°）
  的 tile ID 集合：远端角 tile 翻转
  （`tile_0_0`/`tile_3_0` 在 yaw~0 出现、yaw~180 消失；
  `tile_0_3`/`tile_3_3` 反之），Jaccard=0.50。
- **LOD**：FAR/NEAR/FAR2 三阶段稳态窗口断言上表（per-tile 集合相等，
  不是只看总数）。
- `sanitizer_frustum_lod`：经 `tests/run_sanitized.py` 的 ASan/LSan/UBSan
  门禁（缩短运行：视锥只做计数级断言，LOD 只查 NEAR 核心）。

## 实测结果（3 遍一致）

- 视锥：selected 10–13（总量 17）；yaw~0 选中 12 tiles，yaw~180 选中
  12 tiles，交集 8，Jaccard=0.50；四个角 tile 按预期翻转；failed=0。
- LOD：FAR `{root.glb}`/loaded=2 → NEAR `{child_0..3.glb}`/loaded=6
  → FAR2 `{root.glb}`/loaded=6；failed=0。

## 诚实边界

- 结论仅适用于 Linux/Mesa：不断言移动平台行为。
- `selectedTileIds()` 的字符串格式是 cesium-native 的实现细节；
  跨版本可能变化，测试只做集合成员比较。
- 视锥 fixture 是为方向性特制的水平地面；真实 tileset 的剔除效果
  取决于其 bounding volume 形状与相机路径。
- 本测试只覆盖 REPLACE 精化的父→子切换；ADD 精化的叠加语义
  （P3 fixture 已隐含覆盖：`selected=4` 含 wrapper + root + 2 children）
  未做同等严格的时序断言。
- `tilesLoaded` 的计数含无 content 的 wrapper tile（见发现 2）；
  这是口径说明，不是 bug。
