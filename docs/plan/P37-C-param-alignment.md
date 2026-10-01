# P37-C 参数一致性规范

> li 要求：渲染一致性的前提是参数一致性。相机、背景、渲染状态必须确保一致。

## 1. 相机参数

| 参数 | 我们 (Filament) | cesium.js | 对齐方法 |
|------|----------------|-----------|----------|
| eye | `camera->lookAt(eye, ...)` | `camera.position` | 同一坐标值直接复制 |
| target | `camera->lookAt(..., target, ...)` | `camera.lookAt` 的 target | 同一坐标值直接复制 |
| up | `camera->lookAt(..., up)` | `camera.up` | 同一向量，归一化 |
| fovY | `camera->setProjection(fov, aspect, near, far)` | `camera.frustum.fov` (radians) | 同一度数转弧度 |
| aspect | `width / height` | `canvas.width / canvas.height` | 同一分辨率 |
| near | 投影矩阵 near | `frustum.near` | 同一数值 |
| far | 投影矩阵 far | `frustum.far` | 同一数值 |

**SDK 缺口**：目前只有 `setOrbitCamera(yaw, pitch, distance)`，需新增
`Renderer::setCamera(eye[3], target[3], up[3])` 完整 lookAt。

**demo 缺口**：需新增 `--camera-eye x y z --camera-target x y z --camera-up x y z --fov deg`。

**坐标系注意**：
- 我们的 tileset 用 local-origin 平移（ECEF → local）
- cesium.js 用完整 ECEF
- 相机参数必须在**同一坐标系**下给出：用 tileset 的 `boundingSphere` 中心算出 local 坐标，两边都用 local

## 2. 背景与环境

| 参数 | 我们 | cesium.js | 对齐方法 |
|------|------|-----------|----------|
| 背景色 | `renderer->setClearColor(r,g,b,a)` | `scene.backgroundColor` | 同一 RGBA |
| 天空盒 | 不创建 skybox | `scene.skyBox = undefined` | 都关 |
| 大气散射 | 无 | `scene.skyAtmosphere = false` | cesium 侧关 |
| 太阳光 | 方向光方向 | `scene.sun` 位置 | 同一方向向量 |
| IBL | `--no-ibl` | `scene.imageBasedLighting` 关 | 都关（变量太多，先对齐直接光） |
| 阴影 | 默认关 | `scene.shadowMap.enabled = false` | 都关 |

**SDK 缺口**：需确认 `setClearColor` 是否存在，不存在则新增。
**demo 缺口**：需新增 `--background r g b a`。

## 3. 渲染状态

| 参数 | 我们 | cesium.js | 对齐方法 |
|------|------|-----------|----------|
| 分辨率 | `--width --height` | canvas 尺寸 | 同一数值，如 800×600 |
| DPR | 1（无头默认） | `viewer.resolutionScale = 1.0` + `useBrowserRecommendedResolution = false` | 都按 1 |
| MSAA | Filament 默认 | `scene.msaaSamples` | 都设 4x 或都关，记录 |
| 色调映射 | Filament ACES | cesium.js 默认（无/线性） | **对不齐**，记录为已知差异 |
| 输出色彩空间 | sRGB | sRGB | 一致 |

## 4. Tileset 加载参数

| 参数 | 我们 | cesium.js | 对齐方法 |
|------|------|-----------|----------|
| maximumScreenSpaceError | `TilesetOptions` | `maximumScreenSpaceError` | 同一数值（16） |
| 相机冻结 | `--debug-freeze-frame` | 首帧后不再动相机 | 都用静态相机 |

## 5. 对齐流程（每个 tileset）

```
1. 读 tileset.json → boundingSphere（center + radius）
2. 计算相机：eye = center + dir * radius * 2.5（dir 固定，如 (1,1,1) 归一化）
   target = center，up = (0,0,1) 或 (0,1,0)（看包围体类型）
3. 换算到 local 坐标（减 local-origin）
4. 我们：demo --camera-eye ... --camera-target ... --background 0 0 0 1 --no-ibl --fov 60
5. cesium.js：Puppeteer 设置同样参数，截屏
6. SSIM 对比
```

## 6. 已知对不齐（不算失败）

- tone mapping 曲线（Filament ACES vs cesium.js）
- 抗锯齿算法差异（边缘 1-2px）
- pnts 点大小（我们 1px vs cesium attenuation）
- 文字/标注（我们无屏内文字系统）
