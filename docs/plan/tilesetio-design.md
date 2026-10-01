# tilesetio 设计文档

## 愿景

一个实时的、交互式的 3D Tiles 到渲染数据的转换器。类比 gltfio，但面向 tileset 且是 live 的：
- 相机参数传入 cesium-native
- cesium-native 做 LOD 调度，产出 `CesiumGltf::Model`
- tilesetio 把 Model 转成中立的渲染数据
- Filament 后端把渲染数据上屏

```
Camera ──▶ cesium-native ──▶ tilesetio-core ──▶ tilesetio-filament ──▶ Scene
           (Tileset,         (Model →              (RenderData →
            IPrepareRenderer  RenderData,           VertexBuffer/
            Resources)        中立)                 Renderable)
```

## 核心原则

1. **cesium-native 与 Filament 无直接耦合**
   - `tilesetio-core` 不 include 任何 Filament 头文件
   - `tilesetio-filament` 不 include 任何 cesium-native 头文件
   - 两层之间只通过中立的 `RenderData` 结构体通信

2. **一次解码，直接上屏**
   - 没有 GLB 中间字节，没有 encode/decode 往返
   - 从 `CesiumGltf::Model` 的 accessor 直接读顶点数据

3. **实时交互**
   - 不是一次性 loader，是每帧参与的 live 系统
   - 相机变化 → `updateView` → tile 加载/卸载 → 增量更新 Scene

## 分层

### Layer 1: tilesetio-core（中立）

```cpp
namespace tilesetio {

// 中立的 PBR 材质参数（对标 glTF 2.0 material）
struct MaterialParams {
    float baseColorFactor[4] = {1,1,1,1};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    // 纹理以解码后的像素或 GPU 句柄占位，core 层不碰 GPU
    int baseColorTexture = -1;  // 纹理表索引
    bool doubleSided = false;
    float alphaCutoff = 0.5f;
    enum class AlphaMode { Opaque, Mask, Blend } alphaMode = AlphaMode::Opaque;
};

// 一个可绘制的几何图元
struct PrimitiveData {
    std::vector<float> positions;  // 3*N
    std::vector<float> normals;    // 3*N，可空
    std::vector<float> texcoords;  // 2*N，可空
    std::vector<uint32_t> indices; // 可空（非索引绘制）
    uint32_t materialIndex;
};

// 一个 tile 的完整渲染数据
struct TileRenderData {
    std::vector<PrimitiveData> primitives;
    std::vector<MaterialParams> materials;
    // 节点层级变换（glTF node tree，float 精度已足够）
    std::vector<float> nodeTransforms;  // 16*N, column-major
    std::vector<int> nodeParents;       // -1 = root
    std::vector<uint32_t> primitiveNode; // primitive -> node 索引
    // tile 级变换（tile.getTransform() * RTC_CENTER，double）
    double tileTransform[16];
};

} // namespace tilesetio
```

### Layer 2: cesium-native 适配器

```cpp
// 实现 IPrepareRendererResources，把 cesium-native 的回调转成 TileRenderData
class CesiumTileAdapter : public Cesium3DTilesSelection::IPrepareRendererResources {
public:
    void* prepareInMainThread(Tile&, void* loadThreadResult) override;
    // 内部：从 CesiumGltf::Model 提取 TileRenderData，不经过 GLB
private:
    tilesetio::TileRenderData convertModel(const CesiumGltf::Model& model);
};
```

关键点：
- 直接读 `model.accessors` / `bufferViews` / `buffers` 拿顶点数据
- `model.materials` → `MaterialParams`（只取 PBR 参数，不碰 shader）
- `model.nodes` → 节点变换层级
- tile 的 `getTransform()` + RTC_CENTER → `tileTransform`
- **不碰 Filament，不调 gltfio**

### Layer 3: Filament 后端

```cpp
// 把 TileRenderData 转成 Filament 对象
class FilamentBackend {
public:
    // 为一个 tile 创建所有 Filament 资源，返回 entity 列表
    std::vector<utils::Entity> createTile(
        filament::Engine*, filament::Scene*,
        const tilesetio::TileRenderData& data,
        filament::gltfio::MaterialProvider* materials);
    void destroyTile(filament::Engine*, std::vector<utils::Entity>);
private:
    filament::VertexBuffer* createVertexBuffer(...);
    filament::IndexBuffer* createIndexBuffer(...);
};
```

关键点：
- `VertexBuffer::Builder` 直接灌 `positions`/`normals`/`texcoords`
- `UbershaderProvider` 复用（已验证可用，不依赖 AssetLoader）
- `RenderableManager::Builder` 组装
- `TransformManager` 挂 `tileTransform * nodeTransform`
- **不碰 cesium-native**

## 数据流（每帧）

```
renderFrame():
  1. 相机参数 → tileset->updateViewGroup(viewState)
     (cesium-native 内部做 frustum culling + SSE + LOD 选择)
  2. 新可见 tile → CesiumTileAdapter::prepareInMainThread
     → TileRenderData (中立，无 Filament)
  3. FilamentBackend::createTile(RenderData)
     → VertexBuffer/IndexBuffer/Renderable → scene->addEntity
  4. 卸载 tile → destroyTile → scene->removeEntity + 释放 GPU 资源
  5. renderer->render(swapChain)
```

## 与旧架构的对比

| | 旧（gltfio） | 新（tilesetio） |
|---|---|---|
| 解码次数 | 2（cesium + gltfio） | 1（cesium） |
| GLB 中间字节 | 有 | 无 |
| `_BATCHID` | gltfio 挂掉 | 直接读 accessor，无影响 |
| 扩展支持 | 受 gltfio 限制 | 只受 Filament 底层限制 |
| cesium↔Filament 耦合 | 直接（FilamentPrepareResources 混在一起） | 经 RenderData 解耦 |
| 材质 | Ubershader（复用） | Ubershader（复用） |
| LOD 调度 | cesium-native | cesium-native（不变） |
| 渲染管线 | Filament | Filament（不变） |

## 实施计划

### Phase T1: Core 数据结构 + Model 转换器
- [ ] `src/tilesetio/core.h`: `MaterialParams`, `PrimitiveData`, `TileRenderData`
- [ ] `src/tilesetio/cesium_adapter.cpp`: `convertModel()` 
  - 从 accessor 读 POSITION/NORMAL/TEXCOORD_0/indices
  - 处理 byteStride、componentType（float/vec3 等）
  - material → MaterialParams
  - node → transform 层级
- [ ] 单元测试：用 BatchedColors 的 Model 验证转换正确性

### Phase T2: Filament 后端
- [ ] `src/tilesetio/filament_backend.cpp`: `FilamentBackend`
  - VertexBuffer/IndexBuffer 创建
  - UbershaderProvider 创建 MaterialInstance
  - RenderableManager 组装
  - TransformManager 挂变换
- [ ] 替换 `FilamentPrepareResources::prepareInMainThread` 中的 gltfio 调用
- [ ] BatchedColors 出图验证

### Phase T3: 纹理
- [ ] 纹理数据从 Model 提取（images → 解码 → RGBA）
- [ ] Filament Texture 创建与绑定
- [ ] KTX2/PNG/JPEG 支持

### Phase T4: 全量验证
- [ ] 200 个 tileset 回归
- [ ] cesium.js 图像对比（P37-C2 继续）
- [ ] 性能对比（旧 vs 新：CPU、内存）

### Phase T5: 清理
- [ ] 删除 gltfio AssetLoader 相关代码
- [ ] 删除 `modelToGlb`、`stripMetadataTextureExtensions`
- [ ] 更新 ADR

## 非目标

- 不重写 cesium-native 的 LOD 调度（它已经是对的）
- 不重写 Filament 的渲染管线（cull/sort/draw）
- 不自己写 PBR shader（复用 Ubershader）
- 第一版不支持蒙皮/ morph target（后续迭代）

## T1 Status (2026-10-01)

### Working
- `tilesetio-core`: TileRenderData, PrimitiveData, NodeData, MaterialParams defined.
- `tilesetio-cesium`: CesiumGltf::Model -> RenderData conversion works.
  - Verified: BatchedColors (10 primitives, 240 verts, 10 materials).
  - RTC_CENTER extracted and applied correctly.
  - Transform: modelMatrix * tileTransform * translate(RTC) * upAxisFix - localOrigin.
  - ECEF rebased to origin (translation ~[-0.9, 3.7, -4.1]).

### Blocked: Filament Backend Black Screen
- Entities created successfully (10 renderables, Builder::Success).
- Materials created (10 instances via UbershaderProvider).
- Vertex attributes correct (POSITION+TANGENTS+COLOR+UV0+UV1, no warnings).
- Camera transform verified.
- **But**: Output is black (0 non-black pixels).
- Tried: culling off, double-sided, unlit, lit, default material (no params).
- Hypothesis: UbershaderProvider.createMaterialInstance() requires setup
  that AssetLoader normally does, which we're bypassing. Need to investigate
  what AssetLoader does differently.
- Next: Compare AssetLoader path vs direct backend; OR use Filament
  Material Builder with custom shader instead of ubershader.
