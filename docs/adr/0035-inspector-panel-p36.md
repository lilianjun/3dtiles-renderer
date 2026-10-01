# ADR-0035: 复刻 Cesium3DTilesInspector（demo 交互式调试面板）

日期：2026-10-01
状态：已接受（用户明确要求 "要"）

## 背景

用户指出 cesium.js 自带 `Cesium3DTilesInspector` 面板（`@cesium/widgets`
包，Knockout DOM widget），功能很多。P31–P35 复刻的是
`Cesium3DTileset` 的 API 层；Inspector 是独立的 UI 层，我们完全没做。

完整控件清单见 `~/workspace/cesium-3dtiles-inspector-checklist.md`
（subagent 从 `CesiumGS/cesium` main 分支源码枚举）。

## 范围决策

只复刻**通用 3D Tiles 概念**的 section；cesium.js 私有概念明确不做：

| Section | 决策 |
|---|---|
| Tileset（trim cache） | ✅ 做（已有 `trimLoadedTiles()`） |
| Display（包围盒/内容体/请求体开关） | ✅ 做；Colorize/Wireframe 像 cesium.js 一样显示为 disabled + 原因 |
| Update（Freeze Frame、Max SSE slider） | ✅ 做；Dynamic SSE 不做（P31 已决定不复刻私有遍历策略） |
| Logging（FPS、Statistics 文本） | ✅ 做 |
| Tile Debug Labels（per-tile 标注） | v2（需要逐 tile 投影 + 文本 overlay，工作量大；v1 先做面板本体） |
| Style（样式语言） | ❌ 不做（cesium.js 私有概念） |
| Optimization（skip LOD 等） | ❌ 不做（P31 已决定） |
| Point Cloud Shading | ❌ 不做（cesium.js 私有 shading 模型） |
| Picking / Pick Statistics | ❌ 不做（无 picking 子系统） |

## 架构决策

- **UI 工具包**：Dear ImGui（vcpkg `imgui` port，demo-only 依赖）。
  SDK 保持零 ImGui、零 SDL（ADR-0003 不动）。
- **输入后端**：`imgui_impl_sdl3`（官方 backend，处理鼠标/键盘；
  `io.WantCaptureMouse` 决定 orbit 拖拽是否让路给面板）。
- **渲染后端**：自写 minimal Filament backend（单窗口 overlay）：
  - ImGui draw data → 动态 VertexBuffer/IndexBuffer；
  - font atlas → RGBA8 Filament Texture；
  - UI 材质用 matc 编译（unlit、transparent、depthTest off），clip rect
    用 fragment discard 实现（每 draw command 一个 material instance
    传 clip uniforms）；
  - 第二个 Filament View（不清屏）画 UI layer，顺序在 3D view 之后。
- **SDK 扩展点**（最小、demo-gated）：
  - `Renderer::setOverlayCallback(std::function<void()>)` ——
    `renderFrame()` 内 3D view 渲染完、present 前调用；不设置则零开销。
  - `Renderer::nativeEngineHandle()` —— 返回 `void*`（实为
    `filament::Engine*`），demo 侧 cast。公共头不新增 filament include。
- **新 SDK API**（P36-A）：
  - `setDebugFreezeFrame(bool)` / `isDebugFreezeFrame()` —— 跳过
    tile selection 更新，只渲染上一帧已选 tile。
  - `setDebugShowContentBoundingVolume(bool)` —— content 包围体线框。
  - `setDebugShowViewerRequestVolume(bool)` —— viewer request volume 线框
    （cesium-native tile 元数据里有；若某 tile 无则跳过）。
  - 统计扩展：`TileStats` 加 `tilesVisited`、`pendingRequests`、
    `tilesProcessing`（内部已有计数，只是没暴露）。
- **构建门控**：`TILES_WITH_IMGUI` CMake 选项，桌面默认 ON，
  Android/iOS 默认 OFF（移动 CI 不编 ImGui，保持快）。
- **demo 模式**：`--inspector` 进入交互模式（窗口常驻 + 面板）；
  `--inspector-smoke N` 给 CI 用：headless 跑 N 帧自动退出，供截图验收。

## 分阶段

- **P36-A**：SDK 缺口 API + overlay hook + 单元测试（沿用现有
  `tileset_*_test.py` 模式），独立 commit。**✅ 已完成（2026-10-01）**：
  6 部分测试全过（content volume / tile BV / request volume / freeze /
  extended stats / live modelMatrix），sanitizer 干净，SDK 零 SDL/零 ImGui。
- **P36-B**：demo `--inspector` 交互循环 + ImGui SDL3 输入 + Filament
  渲染后端 + UI 材质，独立 commit。
- **P36-C**：面板 section 接线（Display/Update/Logging/Tileset），
  独立 commit。
- **验收**：`--inspector-smoke` xvfb 截图（面板像素存在性断言）+
  全量 ctest 回归 + sanitizer + MinGW SDK 门 + SDK 零 SDL/零 ImGui
  符号检查。CI 全绿后 push。

## 诚实边界

- Colorize/Wireframe 复用 P35 结论：面板上显示为 disabled，tooltip 写
  原因（与 cesium.js 对 `enableDebugWireframe` 未设置的 disabled 处理一致）。
- Tile Debug Labels（5.2–5.5）v1 不做，面板上不出现，避免半吊子。
- FPS 是 demo 侧墙钟统计，不是 cesium.js PerformanceDisplay 的翻版。
- 统计口径是我们的 `TileStats` 语义（content bytes），不是 cesium.js
  的 GPU 显存口径；面板标注写清楚。
