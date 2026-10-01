# P37 规划：CesiumJS 测试数据一致性套件

> 目标：把 cesium.js 的 3D Tiles 测试数据全拿过来，在我们的渲染器里跑通加载与渲染，渲染结果与 cesium.js 一致。

## 1. 数据源（按优先级）

| # | 来源 | 路径 | 规模 | 说明 |
|---|------|------|------|------|
| 1 | `CesiumGS/cesium` | `Specs/Data/Cesium3DTiles/` | ~246 个文件 | **核心**：cesium.js 单元测试用的小 tileset，覆盖最全。社区共识："should provide a pretty good coverage of features" |
| 2 | `CesiumGS/cesium` | `Apps/SampleData/Cesium3DTiles/` | ~27 个文件 | Sandcastle 示例数据，稍大、更真实 |
| 3 | `CesiumGS/3d-tiles-samples` | `1.0/`, `1.1/`, `glTF/` | ~15 个 tileset | 官方文档化示例：1.1 隐式切分、元数据、glTF 扩展 |

**获取方式**：shallow clone（`--depth 1 --filter=blob:none --sparse`），只取需要的目录，不进 git 历史。

**不拿**：NASA Curiosity、bertt 样例——体量大、非官方测试数据，优先级靠后。

## 2. 数据盘点（P37-A：先分类，再动手）

按两个维度建立清单 `tests/data/cesiumjs/INVENTORY.md`：

**维度一：tile 格式**
- b3dm / i3dm / pnts / cmpt / glb-direct / vctr / geojson

**维度二：特性**
- batch table / batch table hierarchy
- draco / ktx2 / rtcCenter / transform
- implicit tiling（quadtree/octree）
- 3D Tiles 1.1 metadata（schema/class/property）
- request volume / expiration / refine 策略

每个条目标注：**我们当前支持 / 部分支持 / 不支持**。不支持的进待办，不阻塞。

## 3. 加载一致性（P37-B）

对每个 tileset：
1. `loadTileset` 不崩溃、不返回失败
2. 跑 N 帧，`tilesLoaded > 0`（有内容被加载）
3. 记录 `TileStats`：visited / loaded / failed
4. `failed > 0` 的逐个分析：是数据问题、我们缺特性、还是 bug

**验收**：清单里"应支持"的 tileset 全部 `failed == 0`；"不支持特性"的有明确标注和 issue。

## 4. 渲染一致性（P37-C，核心难点）

### 4.1 为什么不能像素级逐点比对

- cesium.js = WebGL 渲染器；我们 = Filament（Vulkan/OpenGL）
- shader 实现、光照模型、tone mapping、抗锯齿、纹理过滤都有差异
- **像素逐点相等是不现实的目标**，业界做法都是带容差的结构相似性

### 4.2 方案：双渲染器截图对比

**前置条件：参数一致性清单**（li 强调：这是渲染一致性的前提）

| 参数 | 我们侧 | cesium.js 侧 | 对齐方式 |
|------|--------|--------------|----------|
| 相机位置 | eye（世界坐标） | `camera.position` | 同一 ECEF/local 坐标换算 |
| 相机朝向 | lookAt target + up | `camera.lookAt` | 同一 target/up 向量 |
| 视场角 | fovY | `camera.frustum.fov` | 同一度数 |
| 宽高比 | viewport w/h | canvas w/h | 同一分辨率（如 800×600） |
| 近/远平面 | near/far | `frustum.near/far` | 同一数值 |
| 背景色 | clear color | `scene.backgroundColor` | 同一 RGBA |
| 天空盒 | 关闭 | `scene.skyBox = undefined` | 都关，避免大气散射差异 |
| 光照 | 太阳方向 + IBL 开关 | `scene.sun` / imageBasedLighting | 太阳方向对齐；IBL 都关（变量太多） |
| 色调映射 | Filament 默认 | cesium.js 默认 | 记录差异，不强求一致 |
| DPR | 1 | `viewer.resolutionScale = 1.0` | 都按 1 |

> 原则：先把**能对齐的全部对齐**，对不齐的（如 tone mapping 曲线）记录为已知差异，不计入失败。

### 4.3 预期差异（先声明，不算失败）

以下差异是渲染器实现差异，不视为不一致：
- tone mapping 曲线不同导致的整体亮度偏移
- 抗锯齿算法不同导致的边缘 1-2px 差异
- pnts 点大小：我们固定 1px，cesium.js 有 attenuation
- i3dm：我们 CPU 展开，cesium.js GPU instancing（视觉应一致，性能不比）

## 5. 实施步骤

| 步骤 | 内容 | 产出 |
|------|------|------|
| P37-A | 数据获取 + 盘点清单 | `tests/data/cesiumjs/` + `INVENTORY.md` |
| P37-B | 加载一致性 harness + 全量跑 | `cesiumjs_load_test.py`，失败分析报告 |
| P37-C1 | 双渲染截图 harness（我们侧） | 固定相机截图模式 |
| P37-C2 | cesium.js headless 渲染 harness | Puppeteer 脚本 + 截图 |
| P37-C3 | 对比 + 差异分析 + 修 bug | 一致性报告，SSIM 达标 |
| P37-D | 文档：ADR + 诚实边界更新 | ADR-0036，README 更新 |

## 6. 验收标准

1. **加载**：INVENTORY 标注"应支持"的 tileset，`failed == 0`，`loaded > 0`
2. **渲染**："应支持"且"可渲染"的 tileset，SSIM ≥ 0.95 或人工确认一致
3. **回归**：新增测试进 ctest，不破坏现有 34 个测试
4. **文档**：每个不一致项有根因（渲染器差异 / 缺特性 / bug 已修）

## 7. 风险

- **cesium.js headless 环境**：需要 Chrome + WebGL（swiftshader），CI 跑可能慢，考虑只在本地跑、大数据集抽样
- **相机对齐**：ECEF 坐标系、大场景 tileset 的相机计算要仔细验证
- **1.1 隐式切分/元数据**：cesium-native v0.64.0 支持，但我们的 Filament 渲染侧可能没完全接好，P37-B 会暴露

## 8. 不做的

- NASA/bertt 大数据集（体量大，非测试数据）
- 性能对比（只比正确性，不比帧率）
- cesium.js 的 styling/picking 等交互功能（只比静态渲染）
