# ADR-001: 技术选型 — Cesium Native + Filament + SDL3

- **状态**: 已接受
- **日期**: 2026-09-29
- **阶段**: P0

## 背景

项目目标：用 C++ 从零构建 3D Tiles 渲染器，支持 Windows / Android / iOS / WebAssembly 四平台，
在 AI 辅助下完成编码与测试，要求 AI 多动手、人类少动手。

核心难题有三：
1. 3D Tiles 规范复杂（tileset.json、隐式切分、LOD 调度、glTF 内容）—— 自研加载器工作量巨大；
2. 四平台图形 API 各不相同（D3D / Metal / Vulkan / GLES / WebGL）—— 自研渲染抽象层成本高；
3. 四平台窗口与输入差异大 —— 需要统一抽象。

## 决策

| 分层 | 选型 | 版本基线 |
|---|---|---|
| 3D Tiles 加载 / 调度 / LOD / glTF 解析 | **Cesium Native** | main 分支（Apache-2.0） |
| 渲染（PBR、后端抽象） | **Filament** | main 分支（Apache-2.0） |
| 窗口 / 输入 | **SDL3** | main 分支（zlib 许可） |
| 语言标准 | **C++20** | |
| 构建与依赖 | **CMake ≥ 3.21 + FetchContent** | 无外部包管理器依赖 |

## 理由

- **Cesium Native** 是 Cesium 官方 C++ 库，为 Cesium for Unreal / Unity 等插件提供底层支撑，
  覆盖 3D Tiles 运行时流式加载、glTF 序列化、WGS84 高精度数学。自研等价功能至少数人月，
  直接复用把人力集中在渲染器本身。
- **Filament** 是 Google 的跨平台 PBR 渲染器，原生支持 Android / iOS / Windows，
  并可通过 Emscripten 编译到 WebGL，恰好覆盖四平台；PBR 材质体系与 3D Tiles 1.x（glTF 内容）天然契合。
- **SDL3** 提供四平台统一的窗口、事件与输入抽象，且对 Android / iOS / Emscripten 均有官方支持，
  避免为每个平台手写平台层。
- **FetchContent** 让依赖声明即代码，CI 与开发者机器行为一致；P0 用 `TILES_WITH_*` 开关默认关闭，
  保证骨架零下载即可构建，P1 再打开真正接入。

## 备选（已否决）

- **bgfx + 自研 3D Tiles 加载器**：bgfx 后端覆盖广，但 3D Tiles 加载/调度仍需自研，工作量不可接受。
- **纯自研渲染器**：与"AI 多动手、人类少动手"的目标冲突，风险最高。

## 后果

- 正面：P0 即可跑通构建链；P1 起 AI 按模块接入成熟库，验收标准清晰。
- 负面：三个大依赖的编译时间较长（CI 需做缓存，P1 处理）；需跟随上游 main 分支的 API 变化，
  必要时锁定到稳定 tag（P1 决策）。
- 若后续实测发现 Filament 在某平台不满足性能预算，可发起 ADR-002 重新评估渲染层，
  本 ADR 的验收协议（自动验收、平台矩阵）不受影响。
