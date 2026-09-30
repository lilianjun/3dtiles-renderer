# Tileset API 对齐 cesium.js 规划

> 目标：以 cesium.js `Cesium3DTileset` 为参照，复刻 tileset 相关的接口与功能支持。
> 调研依据：`CesiumGS/cesium` main 分支 `Cesium3DTileset.js` 源码（约 3944 行）+ 官方 ref-doc，
> 完整调研报告见 `~/workspace/cesiumjs-tileset-api-report.md`。
> 底层：cesium-native v0.64.0（`TilesetOptions` 全字段已核实）、Filament v1.77.0。
> 约束：SDK 零 SDL（ADR-0003）；除 `version()` 外所有 API 单渲染线程调用。

## 1. cesium.js API 全貌（摘要）

- **构造选项**约 35 个：`show`、`modelMatrix`、`shadows`、`maximumScreenSpaceError`(16)、
  `cacheBytes`(512MiB)、`maximumCacheOverflowBytes`(512MiB)、`cullWithChildrenBounds`(true, 构造后不可改)、
  `cullRequestsWhileMoving`(true)、`cullRequestsWhileMovingMultiplier`(60)、`preloadWhenHidden`(false)、
  `preloadFlightDestinations`(true)、`preferLeaves`(false)、`dynamicScreenSpaceError`(true)+3 参数、
  `progressiveResolutionHeightFraction`(0.3)、`foveatedScreenSpaceError`(true)+4 参数、
  `skipLevelOfDetail`(false)+5 参数、ellipsoid 等；另有样式/裁剪/点云/矢量/调试等专项选项。
- **属性**：读写（`show`、`modelMatrix`、`maximumScreenSpaceError`、`cacheBytes`、各调优旋钮…）、
  只读（`tilesLoaded`、`totalMemoryUsageInBytes`、`boundingSphere`、`root`、`asset`、
  `extensions`、`timeSinceLoad`…）。
- **事件** 7 个：`tileLoad`、`tileUnload`、`tileFailed`、`tileVisible`、
  `allTilesLoaded`、`loadProgress`、`initialTilesLoaded`。
- **方法**：`fromUrl`/`fromIonAssetId`（异步工厂）、`destroy`/`isDestroyed`、
  `getHeight`、`hasExtension`、`makeStyleDirty`、`trimLoadedTiles`。

## 2. 我方现状

已有（`Renderer` 静态 API）：`loadTileset(url)`、`setOrbitCamera`、`renderedTileCount()`、
`tileStats()`（selectedTiles/tilesLoading/tilesLoaded/tilesFailed/bytesLoaded）、
`setMaxCachedBytes()`（live）、`selectedTileIds()`、`setIblEnabled()`、`lastError()`。

底层可用钩子（已核实）：
- `TilesetOptions` 全字段：`maximumScreenSpaceError`(16.0)、`maximumSimultaneousTileLoads`(20)、
  `preloadAncestors`(true)、`preloadSiblings`(true)、`loadingDescendantLimit`(20)、
  `forbidHoles`(false)、`enableFrustumCulling`(true)、`enableOcclusionCulling`(true)、
  `enableFogCulling`(true)、`maximumCachedBytes`(512MB)、`enableLodTransitionPeriod`(false)+
  `lodTransitionLength`(1.0)、`loadErrorCallback`、`ellipsoid`(WGS84) 等。
  `getOptions()` 返回可变引用，部分字段 live 可改（`maximumCachedBytes` 已在用）。
- 事件钩子：`prepareInMainThread`（tileLoad）、`free()`（tileUnload）、
  `loadErrorCallback`（tileFailed，需跨线程 marshal）、
  `tilesToRenderThisFrame`（tileVisible）、`TileStats`（allTilesLoaded/loadProgress/initialTilesLoaded）。
- `cullWithChildrenBounds` 语义底层已自动处理（Replace+有 children 时启用），无需 API。

## 3. Gap 分析矩阵

| cesium.js 能力 | 状态 | 落点 |
|---|---|---|
| `maximumScreenSpaceError` | 🟡 可直接映射 | P31：构造选项 + live setter（`getOptions()` 下帧生效） |
| `forbidHoles`、`preloadAncestors`、`preloadSiblings`、`enableFrustumCulling`、`enableFogCulling`、`maximumSimultaneousTileLoads`、`loadingDescendantLimit`、`enableLodTransitionPeriod`+`lodTransitionLength`、`ellipsoid` | 🟡 可直接映射 | P31：进 `TilesetOptions` 结构体（构造期；改后需重调 `loadTileset`） |
| `cacheBytes` | ✅ 已有 | `setMaxCachedBytes()`，文档注明映射关系 |
| `cullWithChildrenBounds` | ✅ 底层已有 | cesium-native 内部按 Replace/children 自动推导，不暴露 |
| 7 个事件（`tileLoad`/`tileUnload`/`tileFailed`/`tileVisible`/`allTilesLoaded`/`loadProgress`/`initialTilesLoaded`） | 🟡 钩子齐备 | P32：回调注册，统一在 render 线程派发 |
| `show` / `preloadWhenHidden` | 🟡 可实现 | P33：`setShow()`；隐藏时跳过场景提交（`preloadWhenHidden` 决定是否继续遍历） |
| `modelMatrix` | 🟡 可实现 | P33：与 ECEF rebase 复合（`final = modelMatrix × rebase`，文档写死顺序） |
| `tilesLoaded`、`totalMemoryUsageInBytes`、`boundingSphere`、`timeSinceLoad` | 🟡 可实现 | P33/P34（`totalMemoryUsageInBytes` 语义注明为 content bytes，非 GPU 估计） |
| `hasExtension`、`trimLoadedTiles` | 🟡 需核实底层 | P34（`trimLoadedTiles` 先核实 cesium-native 对应 API） |
| `maximumCacheOverflowBytes`、`memoryAdjustedScreenSpaceError` | 🔴 不复刻 | cesium-native 缓存语义不同（`maximumCachedBytes` 硬上限 + 必需 tile 例外），不硬套 |
| `skipLevelOfDetail` 系列 6 项、`dynamicScreenSpaceError` 4 项、`foveated` 5 项、`progressiveResolutionHeightFraction` | 🔴 不复刻 | CesiumJS 私有遍历策略，cesium-native 无对应；自研 = 重写遍历，成本过高（诚实边界） |
| `cullRequestsWhileMoving`×2、`preferLeaves`、`preloadFlightDestinations` | 🔴 延后 | cesium-native v0.64 无对应选项；需自研遍历逻辑 |
| `shadows` | 🔴 不复刻 | 独立大功能（Filament 阴影），另立项 |
| `style`/`customShader`、clipping、imageryLayers、vector、pick、collision、ion、`getHeight`、`modelUpAxis` | 🔴 不复刻 | 独立功能域或生态依赖 |
| 调试开关全家桶 | 🟡 子集 | P35：`debugShowBoundingVolume`、`debugWireframe`、`debugShowUrl`（+评估 `debugColorizeTiles`） |
| 多 tileset 共存 / `fromUrl` 异步工厂 | 🔴 本轮不做 | 架构变更（静态单例→对象），如需另开 ADR |

## 4. 分阶段实施

### P31：核心 LOD 与加载选项
- 新增 `struct TilesetOptions`（camelCase，贴近 cesium.js 命名）：
  `maximumScreenSpaceError`(16)、`forbidHoles`(false)、`preloadAncestors`(true)、
  `preloadSiblings`(true)、`enableFrustumCulling`(true)、`enableFogCulling`(true)、
  `maximumSimultaneousTileLoads`(20)、`loadingDescendantLimit`(20)、
  `enableLodTransitionPeriod`(false)、`lodTransitionLength`(1.0)、
  `ellipsoidRadii[3]`(WGS84)。
- `loadTileset(url, options)` 重载；老签名保留（默认选项）。
- Live setter：`setMaximumScreenSpaceError(double)`（`getOptions()` 直接改，下帧生效；
  其余为构造期选项，改后重调 `loadTileset`，文档注明）。
- 验收：ctest 全过；新增测试验证构造期透传 + live setter 生效（读 `getOptions()`）；
  sanitizer 跑一遍；SDK 零 SDL。

### P32：事件系统 ✅ 完成（2026-09-30，commit `dda4d31`）
- `struct TilesetEventCallbacks`：`onTileLoad` / `onTileUnload` /
  `onTileFailed({url, message})` / `onTileVisible` / `onAllTilesLoaded` /
  `onLoadProgress(pendingRequests, tilesProcessing)` / `onInitialTilesLoaded`。
- `Renderer::setEventCallbacks()` / `clearEventCallbacks()`。
- 线程约定：所有回调在 render 线程、`renderFrame()` 内派发；
  worker 线程来源（`loadErrorCallback`）先进队列。
- `tileVisible` 只在注册回调时遍历（避免每帧 N 次空转）。
- 回调内禁止调用变更类 `Renderer` API（文档写死；查询类允许）。
- 验收：fixture 计数测试（load/unload/visible 触发；坏 tileset 触发 tileFailed；
  `initialTilesLoaded` 全周期只一次）；ctest 全过。
- 实现要点：单 tree walk 检测 TileLoadState 变迁（Done↔/Failed*）；
  `loadErrorCallback` 用 shared 队列（teardown 时不碰 Impl）；
  替换 tileset 时同步 fire 旧 tileset 的 tileUnload；
  traversal root 空 ID 与 tileLoad/tileUnload 语义一致。
  另修 P22 遗留 demo bug：`--switch-at-frame` 在 renderFrame 失败时重复触发，
  现为 one-shot。

### P33：显示 / 变换 / 只读属性 ✅ 完成（2026-09-30，commit `ca316db`）
- `setShow(bool)` / `isShow()`；`show=false` 时跳过 Filament 场景提交；
  `preloadWhenHidden` 决定是否继续 `updateViewGroup` 遍历。
- `setModelMatrix(const double[16])`（column-major；复合公式
  `render = modelMatrix × tileTransform × RTC × upAxisFix − localOrigin`，
  localOrigin 固定不动——早期"origin 跟 matrix 走"的草案会精确抵消平移，已否决）。
- 只读：`tilesLoaded()`、`boundingSphere(center, radius)`（world 系，应用 modelMatrix）、
  `timeSinceLoadMs()`、`rootTileId()`（unwrap cesium-native 空 ID wrapper）。
- 验收：`show=false` 输出清屏色（像素测试）；modelMatrix 平移后像素变化；
  boundingSphere 与 fixture 已知值对照。
- 诚实边界：selection/LOD 仍用 authored transform（cesium-native 无 runtime root transform API）。

### P34：缓存 / 统计 / 方法对齐 ✅ 完成（2026-10-01，commit `4c6457f`）
- `totalMemoryUsageInBytes()`（语义：content bytes，非 GPU 估计；与 `bytesLoaded` 同值，头文件注明）。
- `trimLoadedTiles()`（one-shot：下一帧临时 zero `maximumCachedBytes` 走 public `loadTiles()` 流程卸载，再 RAII 恢复；不碰 internal/private API）。
- `hasExtension(name)`（`loadTileset` 时缓存 `tileset.json` 的 `extensionsUsed`，精确匹配）。
- `loadTilesetAsync` 经评估决定不做（单线程 render 模型，做真 async 需全 Impl 线程安全重构；header 已文档说明 host 侧 worker 线程方案）。
- 验收：`trimLoadedTiles` 后内存下降可观测（1944→648）；`hasExtension` 对 fixture 正确。
- 附带 P32 加固：事件先收集再派发（与 ADR-0031 声明一致）+ no-throw 契约文档化。

### P35：调试开关（子集）
- `setDebugShowBoundingVolume(bool)`（Filament debug 线画包围体）、
  `setDebugWireframe(bool)`、`setDebugShowUrl(bool)`；评估 `debugColorizeTiles` 可行性。
- 验收：开关不崩；像素测试验证包围体线框出现；全量 debug 全家桶不对齐（文档注明）。

## 5. 风险与约定

- 每个 phase 独立 commit、可独立 revert；代码类 commit 正常走 CI（public 仓库不限分钟）。
- 事件回调重入、线程 marshal、modelMatrix×rebase 顺序是三处最易错点，文档先行。
- 本规划只覆盖 tileset 域；`shadows`、样式语言、裁剪、多 tileset 等另立项，不在本规划内。
