# 3D Tiles 功能能力清单（CesiumJS 1.146 对照）

> 目标：以 CesiumJS 1.146（`packages/engine/Source/Scene/`）为"正确答案"，
> 整理 tileset 相关的全部功能能力，作为后续复刻到本 C++ 渲染器的依据。
> 调研方式：直接阅读源码事实，不猜测。源码版本：CesiumJS 1.146。
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

## 1. 加载与解析

| 能力 | CesiumJS（类/选项） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| tileset.json 版本校验 | `Cesium3DTileset.loadTileset` | `asset.version` 仅接受 `"0.0"`/`"1.0"`/`"1.1"`，其他抛 `RuntimeError`；`asset.tilesetVersion` 追加为所有资源请求的 `?v=` 参数 | 🟡（cesium-native 解析；`?v=` 行为待确认） | P1 |
| 扩展声明检查 | `extensionsUsed` / `extensionsRequired`，`hasExtension(name)` | `extensionsRequired` 逐个过白名单 `supportedExtensions`，不支持直接抛错；白名单含 `3DTILES_metadata`、`3DTILES_implicit_tiling`、`3DTILES_content_gltf`、`3DTILES_multiple_contents`、`3DTILES_bounding_volume_S2`、`3DTILES_batch_table_hierarchy` 等 | 🟡（`hasExtension()` 已实现 P34；白名单校验走 cesium-native） | P1 |
| geometricError 缺失回退 | `Cesium3DTile` | tile 缺 `geometricError` 记日志并回退到父 tile（无父则用根） | ✅（cesium-native） | P0 |
| refine 语义 | `Cesium3DTileRefine` | `ADD`(0)/`REPLACE`(1)；小写接受但记 deprecation；未指定继承父，无父默认 REPLACE | ✅（cesium-native） | P0 |
| boundingVolume 类型 | `Cesium3DTile.createBoundingVolume` | box（center+3x3 halfAxes→`TileOrientedBoundingBox`）、region（EPSG:4979 六元组→`TileBoundingRegion`，**不受** tileset transform 变换）、sphere；`3DTILES_bounding_volume_S2`→`TileBoundingS2Cell`；未定义直接抛错 | 🟡（box/region/sphere 经 cesium-native；S2 缺失） | P0（S2 为 P2） |
| viewerRequestVolume | `Cesium3DTile.insideViewerRequestVolume` | 相机进入该体积（distanceToCamera==0）才允许请求/渲染该 tile 内容 | 🟡（cesium-native 有支持；我方未专项验证） | P0 |
| content.uri / content.url | `Cesium3DTile` | 1.0 用 `content.uri`；旧 `content.url` 仍读但记 deprecation；`uri === ""` 视为无内容（Empty） | ✅（cesium-native） | P0 |
| content.boundingVolume | `Cesium3DTile` | content 自带包围体（只包要素，children 可能超出），`tile.contentBoundingVolume` 无则回退 tile 包围体 | 🟡（未专项验证） | P1 |
| 外部 tileset.json | `Tileset3DTileContent.fromJson` | tile content 指向另一 tileset.json，`loadTileset(resource, json, parentTile)` 并入本树；该 content 无 feature（`getFeature`→undefined，`applyStyle` 空操作） | 🟡（可加载；**traversal 未选中外部 tile**，已知 3.2% 差异） | **P0** |
| 空 tile | `Empty3DTileContent` | 无 content 的层级占位 tile；ready 恒 true；全部操作空实现 | ✅ | P0 |
| 3D Tiles 1.1 `contents` 数组 | `Multiple3DTileContent` | 多个内层 content 独立请求、独立 resource；**全部请求可一次性调度才发送**（防部分泄漏）；全为外部 tileset 时 `hasRenderableContent=false`；metadata/group 传播给子 content | ❌（用户已搁置；4 个 fixture 受影响） | **P0** |
| 隐式分块 | `ImplicitTileset` / `Implicit3DTileContent` / `ImplicitSubtree` | `subtreeUriTemplate`（如 `{level}/{x}/{y}.subtree`）、`contentUriTemplates[]`（多 content）；`ImplicitAvailabilityBitstream`（constant/bitstream）判定可用性；底部 tile 的子 subtree 用 placeholder 懒加载；`ImplicitSubtreeCache` LRU | ❌ | **P0** |
| subtree 二进制/JSON | `subt` / `subtreeJson` → `Implicit3DTileContent.fromSubtreeJson` | 同上 | ❌ | P0 |
| 加载事件 | `tileLoad`/`tileUnload`/`tileFailed`/`tileVisible`/`allTilesLoaded`/`loadProgress`/`initialTilesLoaded`，只读 `tilesLoaded` | 7 事件；`tileFailed` 携带 error（含 tile）；`loadProgress(pendingRequests, tilesProcessing)` | ✅（P32 七事件已实现；`tilesLoaded()` 已实现 P33） | P0 |
| 1.146 已移除 `readyPromise` | — | 就绪判断改用 `root` getter + `allTilesLoaded`/`tilesLoaded` | ✅（无 readyPromise 概念；`tilesLoaded` 对应） | P2 |
| 请求调度 | `RequestScheduler`，`maximumRequestsPerServer=18` | tile 内容请求 `throttle+throttleByServer`，`RequestType.TILES3D`；每帧各 pass 后 `requestTiles` 按优先级发起；低优先级取消下帧重试 | 🟡（`maximumSimultaneousTileLoads=20`、`loadingDescendantLimit=20` 已暴露；无按 server 限流） | P1 |
| 请求取消 | `cancelOutOfViewRequests` | 每帧取消上一帧后未被 touch 且仍在 LOADING 的 tile 请求 | ✅（cesium-native 内部） | P1 |
| 过期重请求 | `tile.updateExpiration` | content 带 `expireDate`/`expireDuration`，过期后重新请求 | ❓（cesium-native 是否透出待确认） | P2 |

---

## 2. 遍历与选择

| 能力 | CesiumJS（类/选项） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| SSE 计算 | `Cesium3DTile.getScreenSpaceError` | 透视：`geometricError × height / (distance × sseDenominator)`，`height` 乘 `progressiveResolutionHeightFraction`；结果除以 `pixelRatio`；`geometricError==0` 叶子直接 0；`useParentGeometricError` 时用父级 error 配子 bbox（ADD 提前达标） | ✅（cesium-native） | P0 |
| 基础遍历 | `Cesium3DTilesetBaseTraversal` | REPLACE tile 需**全部**孩子加载完成才替换（空 tile 豁免）；children 按到相机距离排序（early-Z）；external/implicit tile 穿透遍历 | ✅（cesium-native） | P0 |
| `maximumScreenSpaceError`（默认 16） | 构造选项 | LOD 主驱动；tile SSE 超过则继续细化 | ✅（构造选项 + live setter） | P0 |
| `skipLevelOfDetail` + 5 参数（`baseScreenSpaceError`=1024、`skipScreenSpaceErrorFactor`=16、`skipLevels`=1、`immediatelyLoadDesiredLevelOfDetail`、`loadSiblings`） | `Cesium3DTilesetSkipTraversal` | 跳层加载：SSE>base 的走 base traversal；达到 skipping 阈值（`tile.SSE < ancestor.SSE/factor` 且深度差>skipLevels）跳过中间层；父子可同时渲染填空 | 🚫 已决策不复刻（cesium-native 无对应；自研=重写遍历） | P2 |
| `dynamicScreenSpaceError` + 3 参数（density 2.0e-4、factor 24、heightFalloff 0.25） | `Cesium3DTile.getScreenSpaceError` | `error -= CesiumMath.fog(distance, density) × factor`；density 每帧按相机高度占 tileset 高度比例缩放；效果"像把雾拉近相机"，远处用更粗 LOD | 🚫 已决策不复刻 | P2 |
| `foveatedScreenSpaceError` + 4 参数 | `Cesium3DTile.isPriorityDeferred` | 按视线与 tile 的夹角算 `_foveatedFactor`，锥外 tile 放宽 SSE 并延迟请求（`foveatedTimeDelay`=0.2s） | 🚫 已决策不复刻 | P2 |
| `progressiveResolutionHeightFraction`（0.3） | 构造选项 | 乘到 SSE 公式的 height 上，渐进分辨率 | 🚫 已决策不复刻 | P2 |
| `cullWithChildrenBounds`（true） | 构造选项 | 孩子 bbox 全在父内时复用父的 culling mask（`Cesium3DTileOptimizations`） | ✅（cesium-native 内部按 Replace/children 自动推导） | P0 |
| `cullRequestsWhileMoving`（+multiplier 60） | 构造选项 | 相机移动快时不请求小瓦片；仅静态 tileset 生效 | ❌（cesium-native v0.64 无对应） | P2 |
| `preferLeaves` | 构造选项 | 优先加载叶子（反转 depth 优先级） | ❌ | P2 |
| `forbidHoles` / `preloadAncestors` / `preloadSiblings` | cesium-native `TilesetOptions` | 细化时不留空洞；预加载祖先/兄弟 | ✅（已暴露为构造选项） | P0 |
| `preloadWhenHidden` / `preloadFlightDestinations` | 构造选项 | 隐藏时是否继续遍历；飞行目的地预加载 | 🟡（`preloadWhenHidden` 在 `setShow(false)` 路径已处理；flight 目的地无） | P1 |
| 视锥剔除 / 雾剔除 / 遮挡剔除 | `enableFrustumCulling` 等 | 注意：**CesiumJS 遍历中没有独立的 fog culling**，fog 只出现在 dynamic SSE 公式里 | ✅（三项皆为构造选项；雾剔除走 cesium-native） | P0 |
| 内存自适应 SSE | `memoryAdjustedScreenSpaceError` | `totalMemoryUsageInBytes > cacheBytes + maximumCacheOverflowBytes` 时 SSE ×1.02 逐帧放宽，低于 cacheBytes 时 /1.02 回落 | 🟡（有 `maximumCachedBytes` 硬上限；无自适应 SSE；`maximumCacheOverflowBytes` 语义不同，已决策不硬套） | P1 |
| `debugFreezeFrame` | debug 选项 | 只用上一帧 tile 渲染，不做新遍历/请求 | ❌ | P2 |
| `classificationType` 禁用 skipLOD | 构造选项 | 设置 classification 后 `isSkippingLevelOfDetail` 为 false | ❌（classification 本身缺失） | P2 |

---

## 3. 渲染（内容类型与管线选项）

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
| `modelMatrix` | 构造选项 | 变换 tileset 根（root computedTransform 的父级起点），默认单位矩阵 | ✅（`setModelMatrix`；复合顺序已文档化） | P0 |
| `clippingPlanes` / `clippingPolygons` | 构造选项 | `ClippingPlaneCollection` / `ClippingPolygonCollection` 选择性裁剪渲染；polygon 变更触发 rebake | ❌（已决策不复刻） | P1 |
| `shadows`（`ShadowMode`） | 构造选项 | `DISABLED(0)`/`ENABLED(1)`/`CAST_ONLY(2)`/`RECEIVE_ONLY(3)`；默认 ENABLED；投射时渲染两次（相机+光源视角） | ❌（已决策另立项；Filament 阴影独立大功能） | P1 |
| `imageBasedLighting` | 构造选项 | tileset 级 IBL 管理对象 | 🟡（`setIblEnabled()` 全局开关；无逐 tileset 对象） | P1 |
| `lightColor` | 构造选项 | shading 光源颜色；undefined 时用 scene 光源 | ❌ | P2 |
| `backFaceCulling`（默认 true） | 构造选项 | true 时由 glTF 材质 `doubleSided` 决定，false 时禁用背面剔除 | 🟡（材质 doubleSided 已处理；无全局开关） | P1 |
| `show` | 构造选项 | 是否显示 | ✅（`setShow`/`isShow`） | P0 |
| `showOutline` / `outlineColor` / `enableShowOutline` | 构造选项 | `CESIUM_primitive_outline` 轮廓线显示；默认显示、黑色 | 🟡（outline LINES 渲染已实现；开关选项未暴露） | P1 |
| `splitDirection` | 构造选项 | 卷帘分割显示 | ❌ | P2 |
| 点云着色 | `PointCloudShading`：`attenuation`、`geometricErrorScale`（1.0）、`maximumAttenuation`（默认=SSE）、`baseResolution`、`eyeDomeLighting`（true）/`eyeDomeLightingStrength`/`eyeDomeLightingRadius`、`backFaceCulling`（false）、`normalShading`（true） | 基于 geometric error 的点衰减；EDL 需 WebGL1 扩展否则忽略 | ❌（点云整体搁置） | P1 |
| Draco | `KHR_draco_mesh_compression`（`GltfLoader`） | attribute 走 draco 解码路径（byteOffset/byteStride 被忽略） | ✅（cesium-native 内置解码） | P0 |
| meshopt | `EXT_meshopt_compression`（`findMeshoptExtension`） | bufferView/buffer 任一可带，bufferView 优先 | ❓ 待确认 | P1 |
| `3DTILES_batch_table_hierarchy` | `BatchTableHierarchy` | 按层级向上查找父类属性；feature 继承属性只读 | 🟡（解析 crash 已绕过；层级继承语义未完整实现） | P1 |

---

## 4. 样式

| 能力 | CesiumJS（类） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `Cesium3DTileStyle` JSON | `show`/`color`/`pointSize`/`pointOutlineColor` 等 + `meta` + `defines` | 属性值可为字面量、表达式字符串（`${Height} >= 100`）、`conditions` 数组（首个为真者胜出） | ❌（已决策不复刻） | P1 |
| 表达式语言 | `Expression` / `ConditionsExpression` | `${property}` 引用要素属性；算术/比较/逻辑/三元、vec、数学函数、`color()`/`regExp()` 内置函数 | ❌ | P2 |
| shader 化样式 | `StyleExpression.getShaderFunction` | Model 内容的 color/show/pointSize 编译为 shader 函数（`_colorShaderFunction` 等） | ❌ | P2 |
| 样式引擎 | `Cesium3DTileStyleEngine` | 脏标记 `_styleDirty`；全量 vs 增量（`_selectedTilesToStyle`）应用；`lastStyleTime` 防重复；统计 styled 要素/tile 数 | ❌ | P2 |
| 要素句柄 | `Cesium3DTileFeature` / `ModelFeature` | `getProperty`/`setProperty`（写 batch table，`featurePropertiesDirty`）、`getPropertyInherited`（feature→tile→group→tileset 逐层，语义优先） | ❌ | P1 |

---

## 5. 元数据

| 能力 | CesiumJS（类） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| tileset 元数据 | `Cesium3DTilesetMetadata` | 含 `schema`（`MetadataSchema`）、`metadata`（`TilesetMetadata`）、`groups`（`GroupMetadata[]`）、`statistics` | ❌ | P1 |
| schema / class / property / enum | `MetadataSchema` / `MetadataClass` / `MetadataClassProperty` / `MetadataEnum` | class 定义 property 字典（类型、分量数、是否数组、semantic、默认值、min/max） | ❌ | P1 |
| tile / content / group 元数据 | `findTileMetadata` / `findContentMetadata` / `findGroupMetadata` | 优先 `extensions["3DTILES_metadata"]`，否则 1.1 `metadata` 字段；`metadataJson.class` 查 schema | ❌ | P1 |
| `EXT_structural_metadata` | `GltfStructuralMetadataLoader` / `parseStructuralMetadata` / `MetadataPipelineStage` | property table/texture/attribute 绑定进 shader；`Model.js` 另兼容 legacy `EXT_feature_metadata` | ❌ | P1 |
| `EXT_mesh_features` | `Model.js`（`featureIdLabel`，默认 `featureId_0`） | feature ID 选择（per-instance 优先于 per-primitive），用于 picking/styling | ❌ | P2 |
| batch table 运行时 | `Cesium3DTileBatchTable` | 每 feature 的 show/color/属性存取 | 🟡（batchId 读取已用；show/color 运行时改写无） | P1 |

---

## 6. 扩展（汇总）

| 扩展 | CesiumJS 处理点 | 我方状态 | 优先级 |
|---|---|---|---|
| `3DTILES_content_gltf` | glb/gltf tile content（`preprocess3DTileContent` 把 magic `glTF` 重命名为 `glb`） | ✅ | P0 |
| `3DTILES_multiple_contents` | `Multiple3DTileContent`（见 §1） | ❌（搁置） | P0 |
| `3DTILES_implicit_tiling` | `ImplicitTileset`（见 §1） | ❌ | P0 |
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

---

## 7. 调试

| 能力 | CesiumJS（选项） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `debugShowBoundingVolume` | false | 每个 tile 渲染 bounding volume 线框 | ✅（`setDebugShowBoundingVolume`） | P1 |
| `debugShowContentBoundingVolume` | false | 每个 tile 渲染 content bounding volume 线框 | ✅（P36 已实现） | P1 |
| `debugShowViewerRequestVolume` | false | 渲染 viewer request volume（黄色） | ✅（setter 已实现） | P2 |
| `debugFreezeFrame` | false | 冻结遍历，只用上一帧 tile | ❌ | P2 |
| `debugColorizeTiles` | false | 每 tile 随机色（ADD 下父子交错可视化） | ❌ | P2 |
| `debugWireframe` / `enableDebugWireframe` | false | content 线框渲染；WebGL1 需先 enable | ❌（Filament v1.77 无运行时线框切换，头文件已注明不提供） | P2 |
| `debugShowGeometricError` | false | label 显示每 tile geometric error | ❌ | P2 |
| `debugShowRenderingStatistics` | false | label 显示 command/point/triangle/feature 数 | ❌ | P2 |
| `debugShowMemoryUsage` | false | label 显示纹理+几何内存 MB | ❌ | P2 |
| `debugShowUrl` | false | label 显示 tile url | 🟡（`setDebugShowUrl` 打 stderr，无屏内 label 系统） | P2 |
| heatmap | `debugHeatmapTilePropertyName` + `Cesium3DTilesetHeatmap` | 按指定 tile 变量值 6 色梯度着色（深灰→蓝→粉→红→橙→黄），`tile._debugColor` | ❌ | P2 |

---

## 8. 性能相关选项

| 能力 | CesiumJS（选项） | 行为要点 | 我方状态 | 优先级 |
|---|---|---|---|---|
| `maximumScreenSpaceError`（16） | LOD 主驱动 | 见 §2 | ✅ | P0 |
| `cacheBytes`（512MiB）/ `maximumCacheOverflowBytes`（512MiB） | 缓存水位 | 超出后 `unloadTiles` 淘汰 LRU；overflow 允许视图必需 tile 临时超用 | 🟡（`setMaxCachedBytes()` live；`trimLoadedTiles()` P34；overflow 语义不同不硬套） | P0 |
| `maximumSimultaneousTileLoads`（20）/ `loadingDescendantLimit`（20） | 加载并发 | 通过 cesium-native `TilesetOptions` | ✅（构造选项） | P0 |
| `totalMemoryUsageInBytes`（只读） | 统计 | content bytes（非 GPU 估计） | ✅（P34，与 `bytesLoaded` 同值） | P1 |
| `trimLoadedTiles()` | 方法 | 下帧卸载本帧未选中 tile | ✅（P34，zero-cache RAII 方案） | P1 |
| `enableLodTransitionPeriod` + `lodTransitionLength`（1.0s） | LOD 渐变 | 细节层级间淡入淡出 | ✅（构造选项，默认关闭） | P2 |
| `timeSinceLoad`（只读） | 统计 | 加载耗时 | ✅（`timeSinceLoadMs()` P33） | P2 |
| `boundingSphere`（只读） | 统计 | world 系包围球（含 modelMatrix） | ✅（P33） | P1 |
| `root`（只读） | 树根 tile 句柄 | 替代已移除的 `readyPromise` 做就绪判断 | 🟡（`rootTileId()` P33；无 tile 对象句柄） | P2 |

---

## 附：最重要的 5 个缺失能力（P0，按正确性影响排序）

1. **`contents` 数组（`3DTILES_multiple_contents` / `Multiple3DTileContent`）** —— 3D Tiles 1.1 核心结构；缺失则整类 1.1 tileset 无法正确渲染（当前 4 个 fixture 受影响，用户已搁置但属必须复刻）。
2. **隐式分块（`3DTILES_implicit_tiling`）** —— SUBTREE + template URI + 可用性位流；缺失则所有隐式 tileset（大规模城市场景常用）完全不可用。
3. **外部 tileset 的遍历选中** —— 可加载但 `prepareInMainThread` 收不到外部 tile（已知 3.2% 差异根因），属 traversal bug 而非功能缺失，修好即达标。
4. **cmpt 内 i3dm 实例坐标系** —— 自研 merge 把 b3dm/i3dm 压成单 glTF，实例 deconjugation 仍有 6.8% 差异；需与 CesiumJS 的 inner-content 独立矩阵逐项对齐。
5. **`viewerRequestVolume` 的请求门控语义** —— 部分 tileset 依赖它控制内容可见性；当前经 cesium-native 间接支持但未经专项验证，行为偏差会导致"该出不出的"正确性问题。

> 注：`skipLevelOfDetail` / `dynamicScreenSpaceError` / `foveatedScreenSpaceError` / `style` / `shadows` / `clippingPlanes`
> 在 `docs/tileset-api-roadmap.md` 中已有明确"不复刻/另立项"决策，本清单如实记录但不建议推翻。
