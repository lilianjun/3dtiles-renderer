# 隐式分块（3DTILES_implicit_tiling）设计文档

> 状态：设计阶段，无 fixture 可验证。P0 优先级（大规模城市场景常用）。
> 参考：CesiumJS 1.146 `ImplicitTileset.js` / `Implicit3DTileContent.js` / `ImplicitSubtree.js`，
> 3D Tiles 1.1 spec。

## 1. 概述

隐式分块用模板 URI + 可用性位流描述规则性细分（如四叉树/八叉树），避免为每块写 JSON。
适用于大规模城市、点云、地形等规则格网数据。

## 2. 数据结构（tileset.json）

```json
{
  "root": {
    "boundingVolume": { "region": [...] },  // 仅 box 或 region
    "geometricError": 100,
    "refine": "REPLACE",
    "implicitTiling": {
      "subdivisionScheme": "QUADTREE",  // 或 "OCTREE"
      "subtreeLevels": 3,              // 每个 subtree 覆盖的层数
      "maximumLevel": 10,              // 可选，最大层级
      "subtrees": {
        "uriTemplate": "{level}/{x}/{y}.subtree",
        // 或 "uri": "0/0/0.subtree"（单 subtree）
      }
    },
    "content": {
      "uriTemplate": "{level}/{x}/{y}/{z}.b3dm",  // 1.1 风格
      // 或 "uri": "..."（固定）
    },
    // 可选：tile metadata 模板
    "metadata": { "class": "tileClass", ... }
  }
}
```

## 3. CesiumJS 实现要点

### 3.1 ImplicitTileset
- 解析 `implicitTiling`：subdivisionScheme、subtreeLevels、subtrees.uriTemplate
- 存储 root 的 boundingVolume、geometricError、content uriTemplate
- 提供 `getTileCoordinates(level, x, y, z)` → 计算子 tile 的 boundingVolume（按 scheme 细分）

### 3.2 Subtree 加载
- URL 模板替换：`{level}`、`{x}`、`{y}`、`{z}`（OCTREE 有 z）
- Subtree 文件格式：JSON（`.subtree` JSON 版）或二进制 `subt`
  - 包含：tile availability bitstream、content availability bitstreams、child subtree availability
- `ImplicitAvailabilityBitstream`：`constant`（全有/全无）或 `bitstream`（位流解码）

### 3.3 内容寻址
- `contentUriTemplates[]`：多 content 时每个有独立模板
- 模板变量：`{level}`、`{x}`、`{y}`、`{z}`
- 示例：`"{level}/{x}/{y}.b3dm"`

### 3.4 遍历集成
- `Implicit3DTileContent`：tile 的 content 为隐式占位，`hasRenderableContent=false`
- 子 tile 按需生成（`createTileChildren`），不预先创建整树
- 底部 tile 的子 subtree 用 placeholder 懒加载
- `ImplicitSubtreeCache`：LRU 缓存已加载的 subtree

### 3.5 元数据
- Subtree 可带 tile metadata（`ImplicitSubtreeMetadata`）
- 通过 `metadataSchema` 解析

## 4. 我方实现方案

### 4.1 架构
```
TilesetRenderer
  └── ImplicitTilesetHandler（新）
        ├── 解析 implicitTiling（tileset.json 加载时）
        ├── SubtreeCache（LRU，复用现有缓存机制）
        ├── URI 模板展开器
        └── Tile 生成器（按需创建 Tile 对象）
```

### 4.2 关键步骤
1. **检测**：`loadTileset` 时检查 root 是否有 `implicitTiling`
2. **解析**：提取 subdivisionScheme、subtreeLevels、uriTemplate
3. **Subtree 加载**：实现 `SubtreeLoader`，支持 JSON 和二进制 `subt`
   - 位流解码：`ImplicitAvailabilityBitstream`
4. **Tile 生成**：在 `createTileChildren` 时（cesium-native 回调），根据坐标生成子 tile
   - 计算 boundingVolume：按 scheme 细分父 volume
   - 设置 content URI：模板展开
5. **缓存**：Subtree LRU 缓存，避免重复加载

### 4.3 与 cesium-native 的关系
- cesium-native v0.64.0 是否支持隐式分块？需验证。
- 若不支持，需自研 `TilesetContentLoader` 或预处理展开。
- 若支持，只需确保我方 `prepareInLoadThread` 能处理生成的 tile。

### 4.4 测试策略
- 无现成 fixture，需从 CesiumJS sample 数据获取隐式分块示例
- 或按 spec 构造最小测试 tileset

## 5. 工作量评估
- Subtree 解析（JSON + 二进制）：中等
- 位流解码：小
- URI 模板：小
- Tile 生成与遍历集成：大（需理解 cesium-native 的 tile 创建流程）
- 总计：约 2-3 周（单人）

## 6. 优先级说明
P0 是因为隐式分块是 3D Tiles 1.1 核心特性，大规模数据常用。但无 fixture 可验证，
建议在有实际数据需求时再实现。
