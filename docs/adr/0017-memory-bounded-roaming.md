# ADR-0017: 相机漫游内存有界性（P19）

日期：2026-09-29
状态：已验证（Linux/Mesa）

## 背景

P19 验证项目方案要求的"相机往返后内存趋稳"：长时间相机漫游时，
Cesium tile 内容缓存的 LRU 驱逐必须生效，`tilesLoaded` 有界，进程 RSS
不无限增长。

## 公共 API

`Renderer::setMaxCachedBytes(std::int64_t bytes)`（`include/tiles_renderer/renderer.h`）：

- render 线程调用；作用于下一次 `loadTileset()`，也支持已加载 tileset 的
  live 修改（下一帧生效）。
- `<= 0` 恢复 Cesium 默认 512MB。
- 语义是 **content cache**（CPU 侧 tile 内容字节），不是 GPU 显存。
  Cesium v0.64.0 的 `TilesetOptions::maximumCachedBytes` 默认 512MB，
  `Tileset::loadTiles()` 每帧调用
  `unloadCachedBytes(_options.maximumCachedBytes, ...)` 执行 LRU 驱逐。

实现细节（`src/tileset.cpp`, `src/renderer.cpp`）：

- `TilesetRenderer::Impl::maxCachedBytes`（默认 -1）；构造
  `TilesetOptions` 时正数写入 `options.maximumCachedBytes`。
- live 修改经 `tileset->getOptions().maximumCachedBytes`（非常量重载）。
- `Renderer::setMaxCachedBytes()` 先存入 `g_pendingMaxCachedBytes`；
  `loadTileset()` 在创建 tileset 前应用——否则 tileset 尚未存在时调用会被丢弃
  （初版 bug，已修）。
- demo 新增 `--cache-budget <bytes>` 和 `--print-rss`
 （Linux `/proc/self/statm`，`[rss] start/end=...KB`，end 在
  `Renderer::shutdown()` 前测量）。

## 关键发现：浅树 fixture 无法触发驱逐（上游行为）

最初用 3 层 fixture（`p19_roam_tileset`，21 tiles）测试时，即使把 budget
设到 800KB（远低于 2.76MB 总量），`loaded` 恒为 23、`bytes` 恒为
2,760,912，unload queue 恒空——**驱逐从未发生**。

根因（cesium-native v0.64.0，非我方代码）：

1. `TilesetSelection.cpp::visitTileIfNeeded()` 先调
   `traversalState.beginNode(&tile)`，**之后**才做 frustum culling。
2. `TreeTraversalState` 以 `Tile::Pointer`（intrusive refcount）持有所有
   `beginNode` 过的 tile（含被 cull 的），`finishNode` 不移除，
   要到下下帧 `beginTraversal` 交换后才释放。
3. `TilesetContentManager::isContentReferenced()` 见 refcount>1 即判为被引用，
   tile 永不进入 `markTileEligibleForContentUnloading`。
4. 浅而宽的树（3 层、ADD）每帧 traversal 触及全部 tile → 全部被 pin 住
   （实测 `prevTrav=23 curTrav=23`，即使 `visited` 只有 11）→
   `maximumCachedBytes` 被静默忽略。

推论：**有效缓存下限 = 每帧 traversal 触及的 working set**，而非
`maximumCachedBytes`。深树中，被 cull 的高层分支整个子树不再被触及，
其 tile 在 2 帧后变为 eligible，LRU 驱逐正常工作。

建议后续向上游提 issue（`TreeTraversalState` 对 cull-but-touched tile
持有 intrusive 引用，违背了 `maximumCachedBytes` 的文档语义）。

## 验证方法

Fixture：`tests/data/p19_deep_tileset/`（`gen_p19_deep_tileset.py` 生成，
确定性无 RNG）：

- implicit QUADTREE，level 0–4，共 1+4+16+64+256 = **341 tiles**，
  约 **15.6MB**（每 tile ~46KB subdivided box）。
- root 128×128；geometricError 逐层减半，保证 dist 14 相机 refine 到 L4。
- 轨迹 `tests/data/trajectories/p19_deep.csv`：dist 14 / pitch 30，
  yaw 0→360°，3 圈共 432 帧（+60 帧 warmup）。

测试 `tests/memory_roam_test.py`：本地 HTTP server 记录每个请求，
证明"有界性来自驱逐而非工作集小"：

- distinct `.glb` 请求数 → 相机实际覆盖的 tile 数。
- total hits > distinct → 被驱逐后重新加载的次数（churn）。
- 每帧解析 `[stats]`：`tilesLoaded`/`bytesLoaded` 最大值与每圈末值。
- `--print-rss` 记录进程 RSS 前后。

## 实测结果（3 遍，bit 级稳定）

Budget **8MB**（working set ~4MB < 8MB < 总量 15.6MB）：

| 指标 | 实测值 |
|---|---|
| distinct glb 请求 | 253（三遍一致） |
| total glb hits | 629–642（≈380 次驱逐后重载） |
| max tilesLoaded | 179（三遍一致） |
| max bytesLoaded | 7,977,744（三遍 bit-identical，< 8MB） |
| 每圈末 loaded | 167–176（波动 < 6%，无上 drift） |
| failed | 0 |
| 6 圈长跑 | loaded=179 / bytes=7,977,744 每圈末完全一致 |

对照：默认 512MB budget 下同一轨迹 loaded=255 / bytes=11.4MB。
8MB budget 把常驻集从 255 压到 179，且 6 圈零漂移——**驱逐生效、
内存有界**。

RSS（Linux `/proc/self/statm`）：113MB → ~260MB（3 圈）。

- tile 内容字节有界（8MB），RSS 增长**不是** tile 泄漏：
  LSan 在 150 帧 sanitizer roam 中零报告。
- 无 tileset 基线（纯 P2 三角形）跑 900 帧 RSS 也 +49MB
  （~54KB/frame）：这是 Filament/Mesa 软件栈基线 render loop 的行为，
  P2 时代已存在，非 P19 引入，不在 P19 范围内。
- 因此 P19 断言 RSS end < 400MB（防灾难性泄漏），ADR 在此诚实记录数值，
  不声称"RSS 完全趋稳"。

## 诚实边界

- content cache ≠ GPU 显存：本测试只约束 CPU 侧 tile 内容字节；
  Filament GPU 资源由 `free()`（`destroyAsset`）随 content unload 释放，
  已验证调用路径，但 Mesa 下未做显存级度量。
- 结论仅适用于 Linux/Mesa 软件渲染：不断言移动平台（Android/iOS）
  的 GPU/系统内存行为；不断言性能与功耗。
- 浅树 + 小 working set 场景下 `maximumCachedBytes` 可能被 traversal
  pin 效应架空（见上），调用方应保证 budget > 每帧触及工作集。

## 门禁

- `memory_roam`（普通）：3 圈，断言上表全部指标，TIMEOUT 600。
- `sanitizer_memory_roam`：150 帧 ASan/LSan/UBSan，复用
  `tests/run_sanitized.py` 与 `tests/lsan.supp`（覆盖率阈值放宽，
  RSS 断言跳过——ASan shadow/quarantine 下 RSS 无代表性）。
- CTest：19 → 20（普通）；sanitizer 门禁新增 1 项。
