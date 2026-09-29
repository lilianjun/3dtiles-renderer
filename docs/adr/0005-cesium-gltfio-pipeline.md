# ADR-0005: Cesium Native → gltfio → Filament 渲染管线（P3）

日期：2026-09-29
状态：已接受（P3 完成）

## 背景

P2 验证了 Filament 真实渲染（深蓝清屏 + 纯红 unlit 三角形）。
P3 需要把真实 3D Tiles 数据（tileset.json + GLB 内容）渲染上屏，
证明「加载 → 调度 → LOD → 解码 → 上屏」整条链路打通。

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

4. **tile transform**：`tile.getTransform()`（double 4x4）→ float，
   写入 glTF root 节点的 TransformManager。P3 测试数据在原点附近，
   真实 ECEF tileset 需要 ENU/local-origin rebase（TODO，见下）。

5. **gltfio ubershader**：用 `createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA)`，
   预构建材质，无运行时编译。PBR 光照需要方向光强度 ~1e5 lux 量级
   （Filament 物理单位；3.0 会全黑）。

6. **测试数据自生成**：`tests/data/gen_p3_tileset.py` 生成确定性 tileset
   （root 灰 10m + child_a 橙 4m + child_b 青 4m，ADD refine），
   不依赖外网。位置放在 +X 右侧以避开 Mesa 软件 GL 路径观测到的
   负 X 几何不显示问题（仅测试数据摆放，见 TODO）。

## 范围（诚实边界）

- **内容格式**：当前从完成请求中复制原始 GLB 字节交给 gltfio。
  自生成裸 GLB 可工作；b3dm/i3dm 等经 Cesium 转换后的 `CesiumGltf::Model`
  尚未真正转入 gltfio（TODO）。
- **网络**：`LocalFileAssetAccessor` 仅支持本地路径和 `file://`。
  HTTP/HTTPS 未实现（TODO）；CLI 的 `--tileset` 参数实为本地路径。
- **坐标**：tile transform 直接 double→float，无 ENU rebase。
  真实地理坐标（ECEF 大数值）会导致 float 精度崩坏（TODO）。
- **`registerAllTileContentTypes()` 线程安全**：当前用函数内 `static bool`
  防重复，可改为 `std::call_once`（TODO）。

## 验证

- `tests/tileset_test.py`：xvfb + Mesa 软渲染 60 帧，断言 3 tiles、
  截图非全黑、深蓝背景存在、橙色 + 青色像素同时存在。
- `ctest` 4/4 通过（含 P2 的 3 个测试）。
- `nm -u libtiles_renderer.a | grep -ci SDL` == 0。

## 后续（P4/P5）

- HTTP/HTTPS AssetAccessor（或明确文档限定本地路径）。
- ECEF → ENU local-origin rebase。
- b3dm/i3dm → gltfio 的 Model 转换路径。
- Windows/Android/iOS/WASM 的 swapchain 接线（ADR-0004 已列）。
