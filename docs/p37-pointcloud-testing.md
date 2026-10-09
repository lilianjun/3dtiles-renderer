# P37 点云测试方法论

## 核心规则（li 确立）

1. **只用 conformance_test.py 跑对比**，不手敲 demo/harness 命令
2. **参数必须从 Cesium 渲染后提取** — 先跑 Cesium，从实际会话中提取相机参数，再喂给自研渲染器；手写参数（即使数值相同）不符合验收流程
3. **相机不许手写/猜** — web 端相机由 Cesium 自动构图（`viewBoundingSphere`，即 zoomTo 的最终位姿），能看到整个 tileset
4. **Web 参考图必须来自 canvas** — `canvas.toDataURL`，不是页面截图
5. **无法复现的采集数据 = 错误数据**，不基于它做优化
6. **先验证基础**：原生 POINTS + 1px 参考必须先过，不过则其他免谈
7. 对比图必须铺满、渲染尺寸 400×300

## 采集流程（已实测验证，2026-10-08）

### 相机：自动构图，不猜参数

`cesium_render.js` 默认自动模式：tileset ready 后调

```javascript
viewer.camera.viewBoundingSphere(tileset.boundingSphere);
```

这是 `viewer.zoomTo(tileset)` 的同步、确定性等价实现（经 Cesium 源码确认：
两者用同一默认 offset——heading 0、pitch -45°、range 按包围球半径+fov
自动计算；`zoomTo` 只是多了飞行器动画）。相机最终位置以渲染后从
`viewer.camera` 提取的值为准。

`--eye/--target/--up` 仅为手动覆盖（调试用），批量采集不用。

### 参数导出：渲染后从 Cesium 会话提取

渲染完成后（`allTilesLoaded` + 静置），从 `window.__viewer.camera` 读取
`position/direction/up`、`frustum.fov`（转回度数）、`aspectRatio`、`near`、
`far`，环境变量 `CAPTURE_PARAMS` 指向输出路径时写 JSON：

**必须用 `positionWC/directionWC/upWC`（世界坐标），不能用
`position/direction/up`。** `viewBoundingSphere` 内部调 `camera.lookAt`，
会把 `camera.transform` 设为包围球中心处的 ENU 坐标系——此后
`camera.position` 等是该**局部系**下的值。实测教训（2026-10-08）：
对 ECEF tileset 用非 WC 版提取，得到的是原点附近的相机（局部系），
而真实相机在 637 万米外；C++ 端按此渲染出一张全黑图，
SSIM 却仍有 0.8184（背景像素掩盖）——典型的"背景掩盖差异"陷阱。

```json
{ "width": 400, "height": 300, "autoFramed": true,
  "camera": { "position": [...], "direction": [...], "up": [...],
              "fov": 60, "aspectRatio": 1.333, "near": 0.1, "far": 10000 },
  "backgroundColor": [0.1, 0.1, 0.1, 1] }
```

`conformance_test.py` 直接消费这个 `params.json`（含 Cesium 水平 FOV→
Filament 垂直 FOV 换算），喂给自研渲染器。

### 取图：canvas.toDataURL（同步渲染 + 同 task 导出）

两个实测发现的坑（已修复并验证）：

1. **Cesium 没设 `preserveDrawingBuffer`**（源码确认无此字段）。
   渲染完成后在另一个 JS task 里调 `toDataURL` 会拿到空白图。
   正确做法：同一 task 内先 `viewer.scene.render()` 再 `toDataURL`。
2. **canvas 尺寸必须在 layout 完成后同步**。页面脚本里调
   `viewer.resize()` 时 layout 还没发生（`clientWidth` 读到 0），canvas
   会停在默认 300×150；且不能直接改 `canvas.width/height`（会清空
   framebuffer）。正确做法：HTML 里给 canvas 定死 px 尺寸
   （替代缺失的 widgets.css 那条 `width:100%;height:100%` 规则），
   tile 加载完成后（layout 已就绪）再调一次 `viewer.resize()`，
   然后同步渲染 + 导出。

### 批量采集

```bash
node tests/data/cesiumjs/harness/batch_capture.js \
  --list tilesets.txt --out captured [--width 400 --height 300] [--jobs 2]
```

`tilesets.txt` 每行 `<name> <tileset.json路径>`——**不带相机参数**，
相机全部由 Cesium 自动构图。每项输出 `<out>/<name>/render.png` +
`params.json`；单项失败不中断，最后打印 `N ok, M failed` 汇总。

性能（实测）：单条约 13 秒（浏览器启动 + SwiftShader 软件渲染，
含固定的 1 秒加载后静置）；`--jobs 2` 并行（默认 2，2 核机器），
200 条约 22 分钟。用 minified Cesium 构建（同版本 1.146.0），输出
与 unminified 逐像素一致。

实测（2026-10-08）：`p8_pnts_cloud`（原点手工点云）+
`TilesetWithDiscreteLOD`（ECEF 官方龙）均一次通过，400×300，
点云 1064 点像素、龙 21883 点像素，四角为背景；故意写坏的条目
正常失败并计入汇总。

## 手工数据测试流程

1. 生成 PNTS（`tests/data/gen_p8_pnts_tileset.py`，确定性，无随机）
2. `batch_capture.js` 采集 Cesium 参考图 + 提取参数（自动构图）
3. 运行 conformance_test.py（读取上一步提取的 params.json）：
   ```bash
   python3 tests/data/benchmarks/conformance_test.py \
     --benchmark <captured>/<name> \
     --tileset <tileset.json> \
     --demo ./build/linux/tiles_demo \
     --out /tmp/out
   ```

## 作废结论（2026-10-08 起不再引用）

以下数字因违反上述规则，全部作废，不得作为结论引用：

- 手工 RGB 点云 SSIM=0.9217 — 用的是手写 params.json，非 Cesium 提取
- PointCloudRGBA SSIM=0.8117 — 采集链路不可复现
- 13 个官方点云基线 SSIM — 采集脚本未留存，无法复现 = 错误数据
- 旧 `tests/data/benchmarks/captured/` 的 201 个目录 — 部分只有
  `dom.html`、无 render.png/params.json，结构描述不得暗示其完整

## 背景参数格式

- Demo `--background` 用 0-1 范围：`"0.1,0.1,0.1,1"`
- 不是 0-255：`"26,26,26,255"` 是错的
