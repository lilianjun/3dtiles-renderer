# P37 点云测试方法论

## 核心规则（2026-10-08 确立）

1. **只用 conformance_test.py 跑对比**，不手敲 demo/harness 命令
2. **params.json 必须从 Cesium 实际会话提取**，不手写
3. **无法复现的采集数据 = 错误数据**，不基于它做优化
4. **先验证基础**：原生 POINTS + 1px 参考必须先过，不过则其他免谈

## Harness 修改（tests/data/cesiumjs/harness/cesium_render.js）

### 1. 隐藏 Cesium logo
```javascript
// 在 HTML <style> 中添加：
.cesium-viewer-bottom{display:none !important;}
```

### 2. 确保画布铺满（page.evaluate，在 waitForFunction 之后、screenshot 之前）
```javascript
await page.evaluate((w, h) => {
    const canvas = document.querySelector('#cesiumContainer canvas');
    if (canvas) {
        canvas.width = w;
        canvas.height = h;
        canvas.style.width = w + 'px';
        canvas.style.height = h + 'px';
    }
    const container = document.getElementById('cesiumContainer');
    if (container) {
        container.style.width = w + 'px';
        container.style.height = h + 'px';
    }
}, width, height);
```

## 手工数据测试流程

1. 生成 PNTS（Python struct，参考 /tmp/my_pc 生成脚本）
2. 创建 tileset.json（boundingVolume sphere）
3. 用 harness 渲染 Cesium 参考（自动 framing，提取真实相机参数）
4. 手写 params.json（position/direction/up/fov/aspectRatio/near/far/backgroundColor）
5. 运行 conformance_test.py：
   ```bash
   python3 tests/data/benchmarks/conformance_test.py \
     --benchmark /path/to/bench \
     --tileset /path/to/tileset.json \
     --demo ./build/linux/tiles_demo \
     --out /tmp/out
   ```

## 已验证结论

- 不透明 RGB 点云：SSIM=0.9217（手工数据，两边一致）
- PointCloudRGBA（官方 1px 基线）：SSIM=0.8117（原生 POINTS）
- 13 个官方点云基线是 2px（采集事故），不可用
- Alpha blending 在 Filament 中未生效（硬编码 alpha=0.3 无变化）

## 背景参数格式

- Demo `--background` 用 0-1 范围：`"0.1,0.1,0.1,1"`
- 不是 0-255：`"26,26,26,255"` 是错的
