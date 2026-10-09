# 3D Tiles 功能能力清单（CesiumJS 1.146 对照）

> 目标：以 CesiumJS 1.146（`packages/engine/Source/Scene/Cesium3DTileset.js`）为"正确答案"，
> 整理 tileset 相关的全部功能能力，作为后续复刻到本 C++ 渲染器的依据。
> 调研方式：直接阅读源码事实，不猜测。
>
> **文档组织优先级**（按复刻顺序）：
> 1. **构造参数** —— `Cesium3DTileset.ConstructorOptions` 完整速查（复刻 API 的第一步）
> 2. **方法** —— `Cesium3DTileset.prototype` 方法速查
> 3. **属性** —— 公开属性速查
> 4. **能力域详解** —— 按功能域的行为细节（§1–§8 原有内容）
>
> 状态标记说明（我方渲染器 `src/tileset.cpp` + `src/tilesetio/`）：
> - ✅ 已支持 —— 已有对应实现
> - 🟡 部分支持 —— 有但残缺，或经底层间接支持
> - ❌ 缺失 —— 无实现
> - ❓ 待确认 —— 未核实
> - 🚫 已决策不复刻 —— `docs/tileset-api-roadmap.md` 明确排除（诚实边界）
>
> 优先级（从"让渲染器正确渲染 3D Tiles 数据"的角度）：
> - **P0** —— 必须复刻：影响正确性/大面积数据不可渲染
> - **P1** —— 重要：常用功能，影响可用性
> - **P2** —— 可选：锦上添花或小众场景

---

## 第一部分：构造参数（`Cesium3DTileset.ConstructorOptions`）

> 来源：`Cesium3DTileset.js` 第 74–158 行 `ConstructorOptions` typedef JSDoc。
> 类型/默认值均为源码原文。`url`/`ionAssetId` 二选一为数据源（二者皆无则抛错）。

### A. 数据源

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `url` | `Resource\|string` | — | tileset.json 的 URL（与 `fromUrl` 对应） | ✅（构造参数） | P0 |
| `ionAssetId` | `number` | — | Cesium ion 资产 ID（`fromIonAssetId`） | ❌（无 ion 概念） | P2 |

### B. 基础显示

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `show` | `boolean` | `true` | 是否显示 tileset | ✅（`setShow`/`isShow`） | P0 |
| `modelMatrix` | `Matrix4` | `IDENTITY` | 变换 tileset 根 tile 的 4x4 矩阵 | ✅（`setModelMatrix`） | P0 |
| `modelUpAxis` | `Axis` | `Axis.Y` | 加载模型内容时视为"上"的轴 | ✅（cesium-native upAxis 处理） | P0 |
| `modelForwardAxis` | `Axis` | `Axis.X` | 加载模型内容时视为"前"的轴 | ✅（cesium-native） | P1 |
| `shadows` | `ShadowMode` | `ENABLED` | `DISABLED(0)`/`ENABLED(1)`/`CAST_ONLY(2)`/`RECEIVE_ONLY(3)`；投射时渲染两次 | ❌（已决策另立项） | P1 |
| `splitDirection` | `SplitDirection` | `NONE` | 卷帘分割显示 | ❌ | P2 |
| `showCreditsOnScreen` | `boolean` | `false` | 是否在屏上显示 tileset credits | ❌ | P2 |
| `projectTo2D` | `boolean` | `false` | 是否精确投影到 2D（更耗内存）；创建后不可改 | ❌ | P2 |

### C. LOD 驱动（核心）

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `maximumScreenSpaceError` | `number` | `16` | LOD 主驱动；tile SSE 超过则继续细化 | ✅（构造选项 + live setter） | P0 |
| `cacheBytes` | `number` | `536870912` (512MiB) | tile 缓存修剪水位（当前视图不需要的 tile） | ✅（`setMaxCachedBytes()`） | P0 |
| `maximumCacheOverflowBytes` | `number` | `536870912` | 缓存超用水位（视图必需 tile 可临时超用） | 🟡（语义不同，不硬套） | P1 |

### D. 遍历优化

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `cullWithChildrenBounds` | `boolean` | `true` | 孩子 bbox 全在父内时复用父的 culling | ✅（cesium-native 内部） | P0 |
| `cullRequestsWhileMoving` | `boolean` | `true` | 相机快速移动时不请求小瓦片（仅静态 tileset） | ❌（cesium-native v0.64 无） | P2 |
| `cullRequestsWhileMovingMultiplier` | `number` | `60.0` | 上项的激进程度乘子 | ❌ | P2 |
| `preferLeaves` | `boolean` | `false` | 优先加载叶子（反转 depth 优先级） | ❌ | P2 |
| `preloadWhenHidden` | `boolean` | `false` | `show=false` 时仍预加载（不渲染） | 🟡（`setShow(false)` 路径已处理） | P1 |
| `preloadFlightDestinations` | `boolean` | `true` | 相机飞行目的地预加载 | ❌ | P2 |
| `progressiveResolutionHeightFraction` | `number` | `0.3` | 乘到 SSE 公式的 height 上，渐进分辨率 | 🚫 已决策不复刻 | P2 |
| `skipLevelOfDetail` | `boolean` | `false` | 是否跳层加载 | 🚫 已决策不复刻 | P2 |
| `baseScreenSpaceError` | `number` | `1024` | skipLOD：达到该 SSE 才开始跳层 | 🚫 | P2 |
| `skipScreenSpaceErrorFactor` | `number` | `16` | skipLOD：跳层的 SSE 倍数阈值 | 🚫 | P2 |
| `skipLevels` | `number` | `1` | skipLOD：最少跳过的层数 | 🚫 | P2 |
| `immediatelyLoadDesiredLevelOfDetail` | `boolean` | `false` | skipLOD：只下载达标 tile，忽略跳层因子 | 🚫 | P2 |
| `loadSiblings` | `boolean` | `false` | skipLOD：遍历时是否下载可见 tile 的兄弟 | 🚫 | P2 |
| `dynamicScreenSpaceError` | `boolean` | `true` | 街景地平线视角用低分辨率远处 tile | 🚫 已决策不复刻 | P2 |
| `dynamicScreenSpaceErrorDensity` | `number` | `2.0e-4` | 类 Fog density，控制优化生效距离 | 🚫 | P2 |
| `dynamicScreenSpaceErrorFactor` | `number` | `24.0` | 优化强度（越大远处越粗） | 🚫 | P2 |
| `dynamicScreenSpaceErrorHeightFalloff` | `number` | `0.25` | tileset 高度比，决定"街景高度" | 🚫 | P2 |
| `foveatedScreenSpaceError` | `boolean` | `true` | 屏幕中心优先，边缘放宽 SSE | 🚫 已决策不复刻 | P2 |
| `foveatedConeSize` | `number` | `0.1` | 注视锥大小（0=视线，1=全 FOV 即禁用） | 🚫 | P2 |
| `foveatedMinimumScreenSpaceErrorRelaxation` | `number` | `0.0` | 锥外 tile 的 SSE 放宽起始值 | 🚫 | P2 |
| `foveatedInterpolationCallback` | `function` | `Math.lerp` | 锥外 SSE 放宽的插值函数 | 🚫 | P2 |
| `foveatedTimeDelay` | `number` | `0.2` | 相机停止后延迟加载边缘 tile（秒） | 🚫 | P2 |

### E. 材质与光照

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `lightColor` | `Cartesian3` | `undefined`（用场景光） | 模型着色光源颜色 | 🚫（全 unlit 材质，无光照计算，不适用） | P2 |
| `imageBasedLighting` | `ImageBasedLighting` | — | tileset 级 IBL 管理对象 | 🟡（`setIblEnabled()` 全局开关） | P1 |
| `environmentMapOptions` | `object` | — | 动态环境贴图管理选项 | ❌ | P2 |
| `backFaceCulling` | `boolean` | `true` | true 时由 glTF `doubleSided` 决定，false 时禁用背面剔除 | 🟡（doubleSided 已处理；无全局开关） | P1 |
| `customShader` | `CustomShader` | — | 自定义 shader（`Model` 管线） | ❌ | P2 |
| `enableShowOutline` | `boolean` | `true` | 是否启用 `CESIUM_primitive_outline` 处理（false 跳过几何处理） | 🟡（outline 渲染已实现；开关未暴露） | P1 |
| `showOutline` | `boolean` | `true` | 是否显示 outline | 🟡（同上） | P1 |
| `outlineColor` | `Color` | `BLACK` | outline 颜色 | ✅（`setOutlineColor`/`outlineColor`） | P2 |

### F. 裁剪与分类

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `clippingPlanes` | `ClippingPlaneCollection` | — | 选择性裁剪渲染 | ❌（已决策不复刻） | P1 |
| `clippingPolygons` | `ClippingPolygonCollection` | — | 多边形裁剪（变更触发 rebake） | ❌（已决策不复刻） | P2 |
| `classificationType` | `ClassificationType` | — | 地形/3D Tiles/两者被分类；设置后禁用 skipLOD | ❌（classification 缺失） | P2 |

### G. 矢量（Vector）

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `vectorClassificationOnly` | `boolean` | `false` | 仅用矢量 tile 做分类 | ❌ | P2 |
| `vectorKeepDecodedPositions` | `boolean` | `false` | 矢量 tile 在内存保留解码位置（配合 `getPolylinePositions`） | ❌ | P2 |
| `vectorBlendOption` | `BlendOption` | `TRANSLUCENT` | 矢量图元混合方式（仅 OPAQUE/TRANSLUCENT） | ❌ | P2 |
| `heightReference` | `HeightReference` | — | 矢量要素的高度参考（需 `scene`） | ❌ | P2 |
| `scene` | `Scene` | — | 渲染场景（`heightReference` 钳制到地形/3D Tiles 时必需） | ❌（无 Scene 概念） | P2 |

### H. 点云

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `pointCloudShading` | `object` | — | 构造 `PointCloudShading`：`attenuation`、`geometricErrorScale`(1.0)、`maximumAttenuation`（默认=SSE）、`baseResolution`、`eyeDomeLighting`(true)、`eyeDomeLightingStrength`、`eyeDomeLightingRadius`、`backFaceCulling`(false)、`normalShading`(true) | ❌（点云整体搁置） | P1 |

### I. 要素 ID（ picking / styling ）

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `featureIdLabel` | `string\|number` | `"featureId_0"` | picking/styling 用的 feature ID 集标签；整数 N 自动转 `"featureId_N"`；per-instance 优先于 per-primitive | ❌ | P2 |
| `instanceFeatureIdLabel` | `string\|number` | `"instanceFeatureId_0"` | 实例 feature ID 集标签 | ❌ | P2 |

### J. 拾取与碰撞

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `enableCollision` | `boolean` | `false` | 相机碰撞/高度钳制到 tileset 表面 | ❌ | P2 |
| `enablePick` | `boolean` | `false` | WebGL1 下允许 `pick`（更耗内存）；WebGL2 忽略 | ❌ | P2 |
| `asynchronouslyLoadImagery` | `boolean` | `false` | 贴在 tileset 上的影像异步加载（true 则先显示原纹理） | ❌ | P2 |

### K. 椭球体

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `ellipsoid` | `Ellipsoid` | `WGS84` | 地球椭球体（尺寸/形状） | ✅（cesium-native 内部 WGS84） | P0 |

### L. 调试（`debug*`）

| 参数 | 类型 | 默认值 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|---|
| `debugHeatmapTilePropertyName` | `string` | — | 按指定 tile 变量值做 heatmap 着色 | ❌ | P2 |
| `debugFreezeFrame` | `boolean` | `false` | 只用上一帧 tile 渲染，不做新遍历 | ❌ | P2 |
| `debugColorizeTiles` | `boolean` | `false` | 每 tile 随机色 | ❌ | P2 |
| `enableDebugWireframe` | `boolean` | `false` | WebGL1 下 `debugWireframe` 的前置开关（创建后不可改） | ❌（Filament 无运行时线框切换） | P2 |
| `debugWireframe` | `boolean` | `false` | 每 tile 内容线框渲染 | ❌ | P2 |
| `edgeDisplayMode` | `EdgeDisplayMode` | `SURFACES_ONLY` | `EXT_mesh_primitive_edge_visibility` 边的显示方式 | ❌ | P2 |
| `debugShowBoundingVolume` | `boolean` | `false` | 渲染每 tile 包围体 | ✅（`setDebugShowBoundingVolume`） | P1 |
| `debugShowContentBoundingVolume` | `boolean` | `false` | 渲染每 tile content 包围体 | ✅（P36） | P1 |
| `debugShowViewerRequestVolume` | `boolean` | `false` | 渲染 viewer request volume（黄色） | ✅（setter 已实现） | P2 |
| `debugShowGeometricError` | `boolean` | `false` | label 显示每 tile geometric error | ❌ | P2 |
| `debugShowRenderingStatistics` | `boolean` | `false` | label 显示 command/point/triangle/feature 数 | ❌ | P2 |
| `debugShowMemoryUsage` | `boolean` | `false` | label 显示纹理+几何内存 MB | ❌ | P2 |
| `debugShowUrl` | `boolean` | `false` | label 显示 tile url | 🟡（stderr，无屏内 label） | P2 |

---

## 第二部分：方法（`Cesium3DTileset.prototype`）

> 来源：`Cesium3DTileset.js` 中 `Cesium3DTileset.prototype.xxx =` 定义（17 个）。

### 静态工厂

| 方法 | 签名 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `fromUrl` | `(url, options?) → Promise<Cesium3DTileset>` | 从 URL 加载 | ✅（构造+`load()` 两步） | P0 |
| `fromIonAssetId` | `(assetId, options?) → Promise<Cesium3DTileset>` | 从 Cesium ion 资产加载 | ❌ | P2 |

### 生命周期

| 方法 | 签名 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `update` | `(frameState)` | 每帧遍历+渲染（主入口） | ✅（`update(view, ...)`） | P0 |
| `prePassesUpdate` / `postPassesUpdate` | `(frameState)` | 多 pass 前后的更新钩子 | 🟡（单 pass，无此概念） | P2 |
| `updateForPass` | `(frameState, passState)` | 指定 pass 的更新 | ❌ | P2 |
| `trimLoadedTiles` | `()` | 下帧卸载本帧未选中 tile | ✅（P34） | P1 |
| `destroy` / `isDestroyed` | `()` | 销毁/是否已销毁 | ✅（RAII 析构） | P0 |

### 查询

| 方法 | 签名 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `hasExtension` | `(name) → boolean` | tileset 是否声明某扩展 | ✅（P34） | P1 |
| `isGltfExtensionUsed` / `isGltfExtensionRequired` | `(name) → boolean` | glTF 层扩展声明检查 | 🟡（走 cesium-native） | P2 |
| `getTraversal` | `() → traversal` | 获取遍历器（调试/测试用） | ❌ | P2 |
| `pick` | `(ray, ...)` | 射线拾取 tile feature | ❌ | P2 |

### 高度/地形相关

| 方法 | 签名 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `updateHeight` | `(cartographic, callback)` | 异步查询 tileset 表面高度 | ❌ | P2 |
| `getHeight` | `(cartographic) → Promise<number>` | 同上（Promise 版） | ❌ | P2 |

### 样式

| 方法 | 签名 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `makeStyleDirty` | `()` | 标记样式脏，下帧重应用 | ❌（样式未实现） | P2 |

### 内部加载

| 方法 | 签名 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `loadTileset` | `(resource, json)` | 解析 tileset.json 并建树（`fromUrl` 内部调用） | ✅（cesium-native 内部） | P0 |

---

## 第三部分：属性（`Object.defineProperties`，47 个）

> 来源：`Cesium3DTileset.js` 第 1142 行 `defineProperties` 块。只列公开 API（不含 `isCesium3DTileset` 等内部标记）。

### 只读状态

| 属性 | 类型 | 说明 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `asset` | `object` | tileset.json 的 asset 对象 | 🟡（未暴露） | P2 |
| `extensions` | `object` | 扩展声明对象 | 🟡（`hasExtension()` 有；对象未暴露） | P2 |
| `extras` | `any` | tileset.json 的 extras | ❌ | P2 |
| `properties` | `object` | batch table 属性字典（1.0） | ❌ | P2 |
| `basePath` | `string` | tileset 基路径 | 🟡（内部有） | P2 |
| `resource` | `Resource` | tileset.json 的 resource | 🟡（内部有） | P2 |
| `root` | `Cesium3DTile` | 树根 tile（替代已移除的 `readyPromise` 做就绪判断） | 🟡（`rootTileId()`；无 tile 对象） | P2 |
| `boundingSphere` | `BoundingSphere` | 世界系包围球（含 modelMatrix） | ✅（P33） | P1 |
| `schema` | `MetadataSchema` | 1.1 metadata schema | ❌ | P1 |
| `metadata` | `TilesetMetadata` | tileset 级元数据 | ❌ | P1 |
| `metadataExtension` | `object` | `3DTILES_metadata` 扩展对象 | ❌ | P2 |
| `statistics` | `Cesium3DTilesetStatistics` | 统计（选中/加载/可见 tile 数等） | 🟡（部分计数有） | P1 |
| `totalMemoryUsageInBytes` | `number` | content 字节数（非 GPU 估计） | ✅（P34） | P1 |
| `timeSinceLoad` | `number` | 加载耗时（ms） | ✅（`timeSinceLoadMs()` P33） | P2 |
| `tilesLoaded` | `boolean` | 是否全部加载完成（只读） | ✅（`tilesLoaded()` P33） | P0 |
| `hasMixedContent` | `boolean` | 是否含多种 content 类型 | ❌ | P2 |
| `isSkippingLevelOfDetail` | `boolean` | 当前是否处于 skipLOD 模式 | 🚫（skipLOD 不复刻） | — |
| `memoryAdjustedScreenSpaceError` | `number` | 内存自适应调整后的 SSE | 🟡（无自适应；硬上限有） | P1 |

### 可读写（构造参数的 live 镜像）

以下属性与构造参数同名，构造后仍可读写（部分有 setter 副作用）：

| 属性 | 对应构造参数 | 我方状态 |
|---|---|---|
| `show` | `show` | ✅ |
| `modelMatrix` | `modelMatrix` | ✅ |
| `maximumScreenSpaceError` | `maximumScreenSpaceError` | ✅（live setter） |
| `cacheBytes` | `cacheBytes` | ✅（`setMaxCachedBytes()`） |
| `maximumCacheOverflowBytes` | `maximumCacheOverflowBytes` | 🟡 |
| `shadows` | `shadows` | ❌ |
| `clippingPlanes` / `clippingPolygons` | 同名 | ❌ |
| `classificationType` | `classificationType` | ❌ |
| `imageBasedLighting` | `imageBasedLighting` | 🟡 |
| `customShader` | `customShader` | ❌ |
| `style` | —（非构造参数，`Cesium3DTileStyle` 对象） | ❌ |
| `styleEngine` | —（内部 `Cesium3DTileStyleEngine`） | ❌ |
| `pointCloudShading` | `pointCloudShading` | ❌ |
| `featureIdLabel` / `instanceFeatureIdLabel` | 同名 | ❌ |
| `ellipsoid` | `ellipsoid` | 🟡 |
| `scene` | `scene` | ❌ |
| `heightReference` | `heightReference` | ❌ |
| `foveatedConeSize` / `foveatedMinimumScreenSpaceErrorRelaxation` | 同名 | 🚫 |
| `vectorBlendOption` / `vectorClassificationOnly` / `vectorKeepDecodedPositions` | 同名 | ❌ |
| `showCreditsOnScreen` | `showCreditsOnScreen` | ❌ |
| `asynchronouslyLoadImagery` | `asynchronouslyLoadImagery` | ❌ |
| `imageryLayers` / `imageryLayersModificationCounter` | —（运行时） | ❌ |
| `environmentMapManager` | —（运行时） | ❌ |
| `clippingPlanesOriginMatrix` | —（运行时） | ❌ |

### 事件（`Event` 对象，非属性但属公开 API）

| 事件 | 触发时机 | 我方状态 | 优先级 |
|---|---|---|---|
| `tileLoad` / `tileUnload` / `tileFailed` | tile 加载/卸载/失败 | ✅（P32） | P0 |
| `tileVisible` | tile 变为可见 | ✅（P32） | P1 |
| `allTilesLoaded` / `initialTilesLoaded` | 全部/初始加载完成 | ✅（P32） | P0 |
| `loadProgress` | `(pendingRequests, tilesProcessing)` | ✅（P32） | P1 |

---

## 第四部分：能力域详解

> 以下为按功能域整理的源码行为细节（复刻实现时的行为依据）。
> 与第一、二部分重叠处以第一、二部分为准（API 速查），此处侧重"怎么做"的语义。

### 1. 加载与解析

| 能力 | CesiumJS（类/选项） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| tileset.json 版本校验 | `Cesium3DTileset.loadTileset` | `asset.version` 仅接受 `"0.0"`/`"1.0"`/`"1.1"`，其他抛 `RuntimeError`；`asset.tilesetVersion` 追加为所有资源请求的 `?v=` 参数 | 🟡（cesium-native 解析；`?v=` 行为待确认） | P1 |
| 扩展声明检查 | `extensionsUsed` / `extensionsRequired`，`hasExtension(name)` | `extensionsRequired` 逐个过白名单 `supportedExtensions`，不支持直接抛错；白名单含 `3DTILES_metadata`、`3DTILES_implicit_tiling`、`3DTILES_content_gltf`、`3DTILES_multiple_contents`、`3DTILES_bounding_volume_S2`、`3DTILES_batch_table_hierarchy` 等 | 🟡（`hasExtension()` 已实现 P34；白名单校验走 cesium-native） | P1 |
| geometricError 缺失回退 | `Cesium3DTile` | tile 缺 `geometricError` 记日志并回退到父 tile（无父则用根） | ✅（cesium-native） | P0 |
| refine 语义 | `Cesium3DTileRefine` | `ADD`(0)/`REPLACE`(1)；小写接受但记 deprecation；未指定继承父，无父默认 REPLACE | ✅（cesium-native） | P0 |
| boundingVolume 类型 | `Cesium3DTile.createBoundingVolume` | box（center+3x3 halfAxes→`TileOrientedBoundingBox`）、region（EPSG:4979 六元组→`TileBoundingRegion`，**不受** tileset transform 变换）、sphere；`3DTILES_bounding_volume_S2`→`TileBoundingS2Cell`；未定义直接抛错 | 🟡（box/region/sphere 经 cesium-native；S2 缺失） | P0（S2 为 P2） |
| viewerRequestVolume | `Cesium3DTile.insideViewerRequestVolume` | 相机进入该体积（distanceToCamera==0）才允许请求/渲染该 tile 内容 | 🟡（cesium-native 有支持；我方未专项验证） | P0 |
| content.uri / content.url | `Cesium3DTile` | 1.0 用 `content.uri`；旧 `content.url` 仍读但记 deprecation；`uri === ""` 视为无内容（Empty） | ✅（cesium-native） | P0 |
| content.boundingVolume | `Cesium3DTile` | content 自带包围体（只包要素，children 可能超出），`tile.contentBoundingVolume` 无则回退 tile 包围体 | ✅（cesium-native；P36 debug 可视化已验证） | P1 |
| 外部 tileset.json | `Tileset3DTileContent.fromJson` | tile content 指向另一 tileset.json，`loadTileset(resource, json, parentTile)` 并入本树；该 content 无 feature（`getFeature`→undefined，`applyStyle` 空操作） | 🟡（可加载；**traversal 未选中外部 tile**，已知 3.2% 差异） | **P0** |
| 空 tile | `Empty3DTileContent` | 无 content 的层级占位 tile；ready 恒 true；全部操作空实现 | ✅ | P0 |
| 3D Tiles 1.1 `contents` 数组 | `Multiple3DTileContent` | 多个内层 content 独立请求、独立 resource；**全部请求可一次性调度才发送**（防部分泄漏）；全为外部 tileset 时 `hasRenderableContent=false`；metadata/group 传播给子 content | ❌（用户已搁置；4 个 fixture 受影响） | **P0** |
| 隐式分块 | `ImplicitTileset` / `Implicit3DTileContent` / `ImplicitSubtree` | `subtreeUriTemplate`（如 `{level}/{x}/{y}.subtree`）、`contentUriTemplates[]`（多 content）；`ImplicitAvailabilityBitstream`（constant/bitstream）判定可用性；底部 tile 的子 subtree 用 placeholder 懒加载；`ImplicitSubtreeCache` LRU | ❌ | **P0** |
| subtree 二进制/JSON | `subt` / `subtreeJson` → `Implicit3DTileContent.fromSubtreeJson` | 同上 | ❌ | P0 |
| 请求调度 | `RequestScheduler`，`maximumRequestsPerServer=18` | tile 内容请求 `throttle+throttleByServer`，`RequestType.TILES3D`；每帧各 pass 后 `requestTiles` 按优先级发起；低优先级取消下帧重试 | 🟡（`maximumSimultaneousTileLoads=20`、`loadingDescendantLimit=20` 已暴露；无按 server 限流） | P1 |
| 请求取消 | `cancelOutOfViewRequests` | 每帧取消上一帧后未被 touch 且仍在 LOADING 的 tile 请求 | ✅（cesium-native 内部） | P1 |
| 过期重请求 | `tile.updateExpiration` | content 带 `expireDate`/`expireDuration`，过期后重新请求 | ❓（cesium-native 是否透出待确认） | P2 |
| 1.146 已移除 `readyPromise` | — | 就绪判断改用 `root` getter + `allTilesLoaded`/`tilesLoaded` | ✅（无 readyPromise 概念；`tilesLoaded` 对应） | P2 |

### 2. 遍历与选择

| 能力 | CesiumJS（类/选项） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| SSE 计算 | `Cesium3DTile.getScreenSpaceError` | 透视：`geometricError × height / (distance × sseDenominator)`，`height` 乘 `progressiveResolutionHeightFraction`；结果除以 `pixelRatio`；`geometricError==0` 叶子直接 0；`useParentGeometricError` 时用父级 error 配子 bbox（ADD 提前达标） | ✅（cesium-native） | P0 |
| 基础遍历 | `Cesium3DTilesetBaseTraversal` | REPLACE tile 需**全部**孩子加载完成才替换（空 tile 豁免）；children 按到相机距离排序（early-Z）；external/implicit tile 穿透遍历 | ✅（cesium-native） | P0 |
| `cullWithChildrenBounds`（true） | 构造选项 | 孩子 bbox 全在父内时复用父的 culling mask（`Cesium3DTileOptimizations`） | ✅（cesium-native 内部按 Replace/children 自动推导） | P0 |
| 视锥剔除 / 雾剔除 / 遮挡剔除 | `enableFrustumCulling` 等 | 注意：**CesiumJS 遍历中没有独立的 fog culling**，fog 只出现在 dynamic SSE 公式里 | ✅（三项皆为构造选项；雾剔除走 cesium-native） | P0 |
| 内存自适应 SSE | `memoryAdjustedScreenSpaceError` | `totalMemoryUsageInBytes > cacheBytes + maximumCacheOverflowBytes` 时 SSE ×1.02 逐帧放宽，低于 cacheBytes 时 /1.02 回落 | 🟡（有 `maximumCachedBytes` 硬上限；无自适应 SSE；`maximumCacheOverflowBytes` 语义不同，已决策不硬套） | P1 |
| `forbidHoles` / `preloadAncestors` / `preloadSiblings` | cesium-native `TilesetOptions` | 细化时不留空洞；预加载祖先/兄弟 | ✅（已暴露为构造选项） | P0 |
| `preloadWhenHidden` / `preloadFlightDestinations` | 构造选项 | 隐藏时是否继续遍历；飞行目的地预加载 | 🟡（`preloadWhenHidden` 在 `setShow(false)` 路径已处理；flight 目的地无） | P1 |
| `debugFreezeFrame` | debug 选项 | 只用上一帧 tile 渲染，不做新遍历/请求 | ❌ | P2 |
| `classificationType` 禁用 skipLOD | 构造选项 | 设置 classification 后 `isSkippingLevelOfDetail` 为 false | ❌（classification 本身缺失） | P2 |

> 注：`skipLevelOfDetail` 系 5 参数、`dynamicScreenSpaceError` 系 3 参数、`foveatedScreenSpaceError` 系 4 参数、
> `progressiveResolutionHeightFraction` 均已在第一部分 D 节列出，此处不重复。

### 3. 渲染（内容类型与管线选项）

| 能力 | CesiumJS（类/选项） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| b3dm | `Model3DTileContent.fromB3dm` → `Model.fromB3dm` | 转 glTF 后走 Model 管线 | ✅ | P0 |
| i3dm | `Model3DTileContent.fromI3dm` | 同上 | 🟡（CPU 展开实例，非 GPU instancing；ECEF 双重变换已修） | P0 |
| pnts | `Model3DTileContent.fromPnts` | 点云转 glTF 走 Model 管线 | 🟡（基础点渲染；用户已搁置精细对齐） | P1 |
| cmpt | `Composite3DTileContent.fromTileType` | 内层 tile 逐个走 factory；子 resource 加 `?compositeIndex=i`（嵌套如 `0_1_1`）；metadata/style/update/pick 循环委派 | 🟡（自研 merge 为单 glTF；**i3dm 实例坐标系仍有 6.8% 差异**） | **P0** |
| glb / gltf（`3DTILES_content_gltf`） | `Model3DTileContent.fromGltf` | 1.1 直接内嵌 glTF | ✅ | P0 |
| geom / vctr（3D Tiles 2.0 草稿） | `Geometry3DTileContent` / `Vector3DTileContent` | 实验性格式 | ❌ | P2 |
| voxel（`3DTILES_content_voxels`） | `Cesium3DTilesVoxelProvider` + `VoxelContent` | 要求根 content 带扩展；`VoxelPrimitive` 渲染，box/cylinder/ellipsoid 三形状 | ❌（1.146 对该扩展支持不完整，我方无） | P2 |
| 高斯泼溅 | `GaussianSplat3DTileContent` | 需 `KHR_gaussian_splatting` + `KHR_gaussian_splatting_compression_spz_2`；SPZ 解码 POSITION/ROTATION/SCALE + SH_DEGREE 半精度打包 | ❌ | P2 |
| geoJson（`MAXAR_content_geojson`） | `Model3DTileContent.fromGeoJson` | 矢量转模型 | ❌ | P2 |
| Draco | `KHR_draco_mesh_compression`（`GltfLoader`） | attribute 走 draco 解码路径（byteOffset/byteStride 被忽略） | ✅（cesium-native 内置解码） | P0 |
| meshopt | `EXT_meshopt_compression`（`findMeshoptExtension`） | bufferView/buffer 任一可带，bufferView 优先 | ❓ 待确认 | P1 |
| `3DTILES_batch_table_hierarchy` | `BatchTableHierarchy` | 按层级向上查找父类属性；feature 继承属性只读 | 🟡（解析 crash 已绕过；层级继承语义未完整实现） | P1 |

### 4. 样式

| 能力 | CesiumJS（类） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `Cesium3DTileStyle` JSON | `show`/`color`/`pointSize`/`pointOutlineColor` 等 + `meta` + `defines` | 属性值可为字面量、表达式字符串（`${Height} >= 100`）、`conditions` 数组（首个为真者胜出） | ❌（已决策不复刻） | P1 |
| 表达式语言 | `Expression` / `ConditionsExpression` | `${property}` 引用要素属性；算术/比较/逻辑/三元、vec、数学函数、`color()`/`regExp()` 内置函数 | ❌ | P2 |
| shader 化样式 | `StyleExpression.getShaderFunction` | Model 内容的 color/show/pointSize 编译为 shader 函数（`_colorShaderFunction` 等） | ❌ | P2 |
| 样式引擎 | `Cesium3DTileStyleEngine` | 脏标记 `_styleDirty`；全量 vs 增量（`_selectedTilesToStyle`）应用；`lastStyleTime` 防重复；统计 styled 要素/tile 数 | ❌ | P2 |
| 要素句柄 | `Cesium3DTileFeature` / `ModelFeature` | `getProperty`/`setProperty`（写 batch table，`featurePropertiesDirty`）、`getPropertyInherited`（feature→tile→group→tileset 逐层，语义优先） | ❌ | P1 |

### 5. 元数据

| 能力 | CesiumJS（类） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| tileset 元数据 | `Cesium3DTilesetMetadata` | 含 `schema`（`MetadataSchema`）、`metadata`（`TilesetMetadata`）、`groups`（`GroupMetadata[]`）、`statistics` | ❌ | P1 |
| schema / class / property / enum | `MetadataSchema` / `MetadataClass` / `MetadataClassProperty` / `MetadataEnum` | class 定义 property 字典（类型、分量数、是否数组、semantic、默认值、min/max） | ❌ | P1 |
| tile / content / group 元数据 | `findTileMetadata` / `findContentMetadata` / `findGroupMetadata` | 优先 `extensions["3DTILES_metadata"]`，否则 1.1 `metadata` 字段；`metadataJson.class` 查 schema | ❌ | P1 |
| `EXT_structural_metadata` | `GltfStructuralMetadataLoader` / `parseStructuralMetadata` / `MetadataPipelineStage` | property table/texture/attribute 绑定进 shader；`Model.js` 另兼容 legacy `EXT_feature_metadata` | ❌ | P1 |
| `EXT_mesh_features` | `Model.js`（`featureIdLabel`，默认 `featureId_0`） | feature ID 选择（per-instance 优先于 per-primitive），用于 picking/styling | ❌ | P2 |
| batch table 运行时 | `Cesium3DTileBatchTable` | 每 feature 的 show/color/属性存取 | 🟡（batchId 读取已用；show/color 运行时改写无） | P1 |

### 6. 扩展（汇总）

| 扩展 | CesiumJS 处理点 | 我方状态 | 优先级 |
|---|---|---|---|
| `3DTILES_content_gltf` | glb/gltf tile content（`preprocess3DTileContent` 把 magic `glTF` 重命名为 `glb`） | ✅ | P0 |
| `3DTILES_multiple_contents` | `Multiple3DTileContent`（见第四部分 §1） | ❌（搁置） | P0 |
| `3DTILES_implicit_tiling` | `ImplicitTileset`（见第四部分 §1） | ❌ | P0 |
| `3DTILES_metadata` | `Cesium3DTilesetMetadata`（见 §5） | ❌ | P1 |
| `3DTILES_batch_table_hierarchy` | `BatchTableHierarchy`（见 §3） | 🟡 | P1 |
| `3DTILES_content_voxels` | `Cesium3DTilesVoxelProvider` | ❌ | P2 |
| `3DTILES_bounding_volume_S2` | `TileBoundingS2Cell` | ❌ | P2 |
| `3DTILES_draco_point_compression` | 白名单（点云 draco） | 🟡（经 cesium-native） | P1 |
| `3DTILES_content_gltf_vector` | 已弃用（v1.142 后移除） | ❌（无需复刻） | — |
| `EXT_structural_metadata` / `EXT_mesh_features` | `Scene/Model/` 管线 | ❌ | P1 |
| `EXT_feature_metadata`（legacy） | `Model.js` 兼容 | ❌ | P2 |
| `KHR_draco_mesh_compression` | `GltfLoader` | ✅（cesium-native） | P0 |
| `EXT_meshopt_compression` | `findMeshoptExtension` / `GltfBufferViewLoader` | ❓ | P1 |
| `KHR_gaussian_splatting` (+ spz_2) | `GaussianSplat3DTileContent` | ❌ | P2 |
| `CESIUM_primitive_outline` | `showOutline`/`outlineColor` | 🟡（渲染已实现；开关未暴露） | P1 |
| `MAXAR_content_geojson` | geoJson content | ❌ | P2 |

### 7. 调试

调试开关已在第一部分 L 节完整列出（13 项），此处仅补充非开关类调试能力：

| 能力 | CesiumJS | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| heatmap | `Cesium3DTilesetHeatmap` | 按指定 tile 变量值 6 色梯度着色（深灰→蓝→粉→红→橙→黄），`tile._debugColor` | ❌ | P2 |

### 8. 性能相关选项

核心参数已在第一部分 B/C 节列出（`maximumScreenSpaceError`、`cacheBytes`、`maximumCacheOverflowBytes`），此处补充：

| 能力 | CesiumJS | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `maximumSimultaneousTileLoads`（20）/ `loadingDescendantLimit`（20） | cesium-native `TilesetOptions` | 加载并发上限 | ✅（构造选项） | P0 |
| `enableLodTransitionPeriod` + `lodTransitionLength`（1.0s） | 构造选项 | LOD 渐变淡入淡出 | ✅（构造选项，默认关闭） | P2 |

---

## 附：最重要的 5 个缺失能力（P0，按正确性影响排序）

1. **`contents` 数组（`3DTILES_multiple_contents` / `Multiple3DTileContent`）** —— 3D Tiles 1.1 核心结构；缺失则整类 1.1 tileset 无法正确渲染（当前 4 个 fixture 受影响，用户已搁置但属必须复刻）。
2. **隐式分块（`3DTILES_implicit_tiling`）** —— SUBTREE + template URI + 可用性位流；缺失则所有隐式 tileset（大规模城市场景常用）完全不可用。
3. **外部 tileset 的遍历选中** —— 可加载但 `prepareInMainThread` 收不到外部 tile（已知 3.2% 差异根因），属 traversal bug 而非功能缺失，修好即达标。
4. **cmpt 内 i3dm 实例坐标系** —— 自研 merge 把 b3dm/i3dm 压成单 glTF，实例 deconjugation 仍有 6.8% 差异；需与 CesiumJS 的 inner-content 独立矩阵逐项对齐。
5. **`viewerRequestVolume` 的请求门控语义** —— 部分 tileset 依赖它控制内容可见性；当前经 cesium-native 间接支持但未经专项验证，行为偏差会导致"该出不出"的正确性问题。

> 注：`skipLevelOfDetail` / `dynamicScreenSpaceError` / `foveatedScreenSpaceError` / `style` / `shadows` / `clippingPlanes`
> 在 `docs/tileset-api-roadmap.md` 中已有明确"不复刻/另立项"决策，本清单如实记录但不建议推翻。
>
> 源码事实纠正（v1.146）：已移除 `readyPromise`（改用 `root` + `allTilesLoaded`/`tilesLoaded`）；
> 遍历中没有独立的 fog culling（fog 只出现在 dynamic SSE 公式里）；
> `extensionsRequired` 有白名单校验。
