# ADR-0015: Tile 流式统计诊断 API（P17）

## 背景

项目方案 MVP 要求"屏幕诊断：FPS、选中/加载/失败 tile 数、CPU/GPU 内存估算"。
P16 之前 SDK 对外唯一的观测点是 `renderedTileCount()`（P3）——
tile 选中多少、加载中多少、失败多少、吃了多少字节，全是黑盒。
一个流式渲染器没有这套诊断，宿主 App 无法做加载指示、失败告警和容量规划。

## 决策

新增 `Renderer::tileStats()`，返回 `Renderer::TileStats`：

| 字段 | 来源 | 语义 |
|---|---|---|
| `selectedTiles` | `ViewUpdateResult::tilesToRenderThisFrame.size()` | 上次遍历选中渲染的 tile 数 |
| `tilesLoading` | worker + main load queue 长度之和 | 遍历认领、内容尚未就绪的 tile 数 |
| `tilesLoaded` | 实例化 tile 树 walk，`TileLoadState::Done` 计数 | 内容真正加载完成的 tile 数 |
| `tilesFailed` | 同一 walk，`Failed` + `FailedTemporarily` 计数 | 加载失败的 tile 数（含可重试的暂时失败） |
| `bytesLoaded` | `Tileset::getTotalDataBytes()` | 当前持有的 tile 内容字节数 |

全 `-1` 表示"没有加载 tileset"。和其它所有 API 一样（`version()` 除外），
**必须在 render 线程调用**（ADR-0012）——它只读 `renderFrame()` 写下的状态，
不做任何场景变更。

## 字段选择理由

- **选中/加载中/失败**：方案原文点名的三个数，一一对应。
- **`tilesLoaded` 为什么不用 `forEachLoadedTile`**：
  那个枚举器的语义是"被引用或状态非 Unloaded 的 tile"（见
  `LoadedTileEnumerator.cpp::meetsCriteriaForEnumeration`），
  会把正在加载中的 tile 也算进去——流式过程中"loaded"会撒谎。
  我们已经在 walk 整棵树，顺手按 `Done` 精确计数，成本不增加。
- **`selectedTiles` vs `renderedTileCount()`**：两者故意不同。
  遍历选中（4）包含常选但无自有内容的 root tile；
  `renderedTileCount()`（3）只计有实际渲染资源的 tile。
  前者回答"调度器想画什么"，后者回答"场景里真有什么"。
- **`bytesLoaded` 是内容字节，不是 GPU 显存**：注释里写明，避免误读。

## 诚实不做的两项

- **FPS**：构建机是 Mesa 软件渲染，FPS 数字无意义；真机 FPS 是宿主用
  平台计时器量的东西，不属于 SDK 的职责。不做。
- **CPU/GPU 内存估算**：cesium-native 只暴露内容字节（`getTotalDataBytes`），
  Filament v1.77 没有公开的 per-scene GPU 显存统计口。
  自己拼凑"估算"等于编数字，不做；`bytesLoaded` 是目前唯一诚实可给的容量信号。

## 实现位置

`TilesetRenderer::tileStats()`（`src/tileset.cpp`），遍历诊断在
`updateTiles()` 里从 `ViewUpdateResult` 捕获（`tileset.cpp` 唯一能拿到
`ViewUpdateResult` 的地方）。demo 用 `--stats` 每帧打印一行
`[stats] frame=… selected=…`，`tests/stats_test.py` 解析断言。
失败计数 walk 是 O(已知 tile)，注释提醒不要在大 tileset 上每帧调用。
