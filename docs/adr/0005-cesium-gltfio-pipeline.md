# ADR-0005: Cesium Native → gltfio → Filament 渲染管线（P3→P5）

日期：2026-09-29
状态：已接受（P3 完成；P5 补齐三个缺口）

## 背景

P2 验证了 Filament 真实渲染（深蓝清屏 + 纯红 unlit 三角形）。
P3 需要把真实 3D Tiles 数据（tileset.json + GLB 内容）渲染上屏，
证明「加载 → 调度 → LOD → 解码 → 上屏」整条链路打通。
P5 补齐 P3 留下的三个缺口：HTTP/HTTPS 加载、b3dm 内容、
ECEF→ENU 式 local-origin rebase。

## 决策

渲染管线固定为：

```
Cesium Tileset::updateViewGroup(viewState)
  → Tileset::loadTiles()            // 实际执行加载队列（只调 updateViewGroup 不会加载！）
  → IPrepareRendererResources       // per-tile：AssetLoader::createAsset → loadResources()
  → gltfio ubershader provider      // 预构建材质，无需 filamat JIT
  → Filament Scene::addEntities     // 可见性由 ViewUpdateResult 驱动
```

关键细节（都是踩坑后固定的）：

1. **`registerAllTileContentTypes()` 必须调用**：否则 GLB content 的 converter
   未注册，会被错误当 JSON 解析，tile 永远加载失败。

2. **`loadTiles()` 必须每帧调用**：`updateViewGroup` 只填充加载队列，
   不调用 `loadTiles()` 则内容永远不会真正加载。

3. **可见性必须用 `ViewUpdateResult.tilesToRenderThisFrame`**（+ `tilesFadingOut`），
   不能用 `tile.isRenderable()`。后者对所有已加载 tile 为 true，会导致
   未被本帧选中的 tile 也进 Scene（P3 初期因此全部加入）。

4. **tile transform（P5 更新）**：不再直接 double→float。
   `world = tile.getTransform() * translate(rtcCenter) - localOrigin`
   全程 double 计算后才转 float32，写入 glTF root 节点的 TransformManager。
   `rtcCenter` 来自 b3dm 的 `CESIUM_RTC` 扩展（见第 7 条）；
   `localOrigin` 是 P5 rebase 原点（见第 8 条）。

5. **gltfio ubershader**：用 `createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA)`，
   预构建材质，无运行时编译。PBR 光照需要方向光强度 ~1e5 lux 量级
   （Filament 物理单位；3.0 会全黑）。

6. **测试数据自生成**：`tests/data/gen_p3_tileset.py` 生成确定性 tileset
   （root 灰 10m + child_a 橙 4m + child_b 青 4m，ADD refine），
   不依赖外网。位置放在 +X 右侧以避开 Mesa 软件 GL 路径观测到的
   负 X 几何不显示问题（仅测试数据摆放，见 TODO）。
   P5 新增 `tests/data/gen_p5_tilesets.py`：b3dm tileset 与
   near/far rebase tileset。

7. **b3dm 内容（P5）**：`prepareInLoadThread` 不再从原始响应中截 GLB。
   cesium-native 的 `B3dmToGltfConverter` 把 b3dm 转成 `CesiumGltf::Model`
   后，我们用 `CesiumGltfWriter::GltfWriter::writeGlb` 把 Model 重新
   序列化为 GLB 再交给 gltfio（真实路径，绕过转换不行）。
   b3dm feature table 的 `RTC_CENTER` 转成的 `CESIUM_RTC` 扩展被提取为
   双精度 rtcCenter，应用到第 4 条的 transform 计算（而不是塞进 float
   glTF 节点），扩展本身从 Model 中剥离（gltfio 不支持）。

8. **local-origin rebase（P5）**：tileset 加载完成后，取 root tile
   bounding volume 中心（OBB/Sphere/BoundingRegion(+loose)）作为
   双精度 rebase 原点。
   - 瓦片**选择**：Cesium `ViewState` 的 target/eye = `origin + 本地 orbit`，
     全程 double，LOD 逻辑看到的是真实世界坐标。
   - 瓦片**渲染**：每个 tile 的 transform 减去 origin 后再转 float32，
     Filament 相机固定围绕 (0,0,0) orbit。
   - 对本地小坐标 tileset，origin ≈ (0,0,0)，行为与 P3 一致。
   - 这是通用的 local-origin rebase，不是严格的旋转 ENU 基
     （east/north/up 姿态）。文档/命名统一称「ECEF→local-origin rebase」，
     不虚称完整 ENU 姿态变换。
   - 验证：`tests/data/p5_rebase_tileset/far` 的 root 带
     123456789.0 m 平移（float32 会变成 123456792.0，偏差 ~3 m 且
     瓦片会在 1.2e8 m 外不可见）；rebase 后与 `near` 的橙色质心
     偏差 ≤ 25 px。

9. **HTTP/HTTPS 加载（P5）**：新增 `RoutingAssetAccessor` —
   `http(s)://` 走 cesium-native 的 `CesiumCurl::CurlAssetAccessor`，
   其余（本地路径 / `file://`）走原有 `LocalFileAssetAccessor`。
   本地路径行为保持确定性不变。测试用本地 `python3 -m http.server`
   随机端口，不依赖外网。

## 范围（诚实边界）

- **内容格式**：b3dm 已真实支持（converter → Model → writeGlb → gltfio，
  截图断言通过）。**i3dm 尚未实现**：走同样的 `contentKind` Model 路径
  理论上可用，但没有生成器数据和测试覆盖，留 TODO。
  （注：P3 时期「b3dm 只看原始响应魔数」的问题在 P5 已彻底修掉 —
  现在优先用转换后的 Model 输出。）
- **网络**：http(s) 通过 libcurl 实现；代理/证书走系统默认。
  `CurlAssetAccessor::tick()` 为空实现（libcurl 自带线程），无需每帧调用。
- **坐标**：local-origin rebase 解决大数值精度问题，但**不是**完整的
  ENU 姿态变换（无 east/north/up 旋转基）；cesium-geospatial 的
  `Ellipsoid`-based WGS84→ENU 变换仍为 TODO。
- **`registerAllTileContentTypes()` 线程安全**：当前用函数内 `static bool`
  防重复，可改为 `std::call_once`（TODO）。
- **其它**：见 ADR-0004/ADR-0006（四平台接线、WASM stub）。

## 验证

- `tests/tileset_test.py`：xvfb + Mesa 软渲染 60 帧，断言 tiles ≥ 1、
  截图非全黑、深蓝背景存在、期望颜色像素存在（P5 新增 `--expect` 过滤）。
- P5 新增 3 个独立 ctest：
  - `http_tileset_screenshot`：本地 HTTP 服务器 + 同一像素断言。
  - `b3dm_tileset_screenshot`：b3dm child_a（橙）+ glb child_b（青）双断言。
  - `rebase_tileset_screenshot`：near/far 双渲染，橙色质心偏差 ≤ 25 px。
- Linux `ctest` 7/7 通过（P2/P3 的 4 个 + P5 的 3 个）。
- `nm -u libtiles_renderer.a | grep -ci SDL` == 0（SDK 零 SDL 保持）。

## 后续

- i3dm 生成器数据 + 测试覆盖（走 b3dm 同路径）。
- 完整的 WGS84→ENU 姿态变换（cesium-geospatial Ellipsoid）。
- Windows/Android/iOS/WASM 的真机/真渲染验证（ADR-0004 已列）。
