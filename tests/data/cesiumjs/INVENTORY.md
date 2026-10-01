# CesiumJS Test Data Inventory (P37-A)

**总数**: 200 个 tileset
- `Specs/Data/Cesium3DTiles/`: 152（cesium.js 单元测试数据）
- `Apps/SampleData/Cesium3DTiles/`: 21（Sandcastle 示例）
- `3d-tiles-samples/`: 27（官方文档化示例）

详见 `INVENTORY.csv`（每 tileset 一行：来源、路径、格式、特性）。

## 格式支持状态

| 格式 | 数量 | 我们支持 | 说明 |
|------|------|----------|------|
| b3dm | 52 | ✅ | P10 已支持 |
| pnts | 29 | ✅ | P8 已支持（固定 1px） |
| i3dm | 24 | ✅ | P7 已支持（CPU 展开） |
| gltf/glb | 37 | ✅ | P10/P15 已支持 |
| cmpt | 4 | ✅ | P9 已支持 |
| json | 7 | ✅ | 外部 tileset 引用 |
| geom | 26 | ❌ | 分类（classification）专用格式，不做 |
| vctr | 17 | ❌ | 矢量瓦片，不做 |
| geojson | 10 | ❌ | GeoJSON 矢量，不做 |

**可加载目标**: 200 − 26 − 17 − 10 = **147** 个

## 特性支持状态

| 特性 | 数量 | 我们支持 | 说明 |
|------|------|----------|------|
| bv-region/box/sphere | 202 | ✅ | 全部支持 |
| refine ADD/REPLACE | 204 | ✅ | 全部支持 |
| implicit tiling | 10 | ✅ | P10/P11 已支持 |
| batch table hierarchy | 5 | ⚠️ | 部分：层级元数据不解析，几何照渲染 |
| draco | 5 | ✅ | P29 已支持 |
| 3DTILES_content_gltf | 6 | ✅ | 1.1 glTF 内容 |
| 3DTILES_content_voxels | 7 | ❌ | 体素，不做 |
| bounding_volume_cylinder | 2 | ❌ | 圆柱包围体，不做 |
| data-uri content | 1 | ⚠️ | 需验证 cesium-native 是否支持 |

## 版本分布

- 1.0: 120 | 0.0: 42 | 1.1: 38
