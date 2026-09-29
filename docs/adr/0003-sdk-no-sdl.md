# ADR-0003: SDK 本体零 SDL 依赖 —— SDL3 仅用于示例与测试

- 状态：已接受
- 日期：2026-09-29
- 相关：ADR-0001（技术栈 Cesium Native + Filament + SDL3）、ADR-0002（工具链与版本锁定）

## 背景

P1 初期曾把 `tiles_renderer` 做成一个直接链接 SDL3 的可执行文件：
SDK 自己建窗口、自己跑循环。这在验证依赖时最省事，但它回答错了问题——
最终交付物是 **SDK**，不是独立 App。

## 决策

1. `tiles_renderer` 是**静态库 SDK**，对外 API（`include/tiles_renderer/renderer.h`）
   只接受**平台原生窗口句柄 + 宽高**：
   - Windows: `HWND`，Android: `ANativeWindow*`，iOS: `UIView*`，
     Web: canvas selector（如 `"#canvas"`），Linux: X11 `Window` / `wl_surface*`
   - SDK 负责渲染（Filament）与 3D Tiles 加载/调度/LOD（cesium-native）
2. **SDL3 永不进入 SDK**：它的 FetchContent 声明与链接只出现在
   `samples/demo`（示例程序）和 `tests`（测试）中。
3. 示例程序 `tiles_demo`：用 SDL3 建窗口、收输入，提取原生句柄后调用 SDK。
   它是宿主 App 的**参考集成**，用于四个目标平台的验证。
4. SDK 永远不创建窗口、不跑事件循环。

## 原因

1. **宿主 App 自有窗口与生命周期**。游戏引擎、原生 App、浏览器页面都有自己的
   窗口管理；SDK 若自带 SDL3，会与宿主的窗口/事件循环冲突，还强加一个窗口库依赖。
2. **Filament 本来就吃原生句柄**。`createSwapChain` 接受 `void*` 原生窗口；
   SDL3 只是"方便拿到句柄 + 输入"的工具，不是渲染必需品。
3. **集成面最小**：SDK 零 SDL 依赖，宿主可用 GLFW / Qt / 原生 API，
   按自己节奏集成。
4. **测试分层清晰**：SDK 单测不依赖窗口系统；SDL 相关行为只在 demo 与 smoke 测试覆盖。

## 后果

- CMake 目标划分：
  - `tiles_renderer` (STATIC)：`src/renderer.cpp`，PUBLIC 链接
    `Cesium3DTilesSelection` + `filament_prebuilt`
  - `tiles_demo` (exe)：`samples/demo/main.cpp`，链接 `tiles_renderer` + `SDL3::SDL3-static`
   （仅 `TILES_WITH_SDL3=ON` 时构建）
  - `tests`：smoke 测试链接 SDK（SDL3 仅作为消费者可选链接）
- P2 渲染器工作全部在 SDK 内进行；demo 只负责"给句柄、跑循环、收输入"。
- 无头 CI 上 demo 使用 SDL dummy video driver：SDL 3.4 起 dummy/offscreen 驱动为
  opt-in（需显式 `SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy")`，不会自动选中），
  demo 在 `SDL_Init` 失败时自动回退到 dummy 驱动；拿不到原生句柄时跳过 SDK 初始化
  并以 exit 0 结束（只验证 SDL3 + SDK 链接通路）。
- CI 的 "Run binary" 步骤改为运行 `tiles_demo`（有 SDL3 时）/ 依赖 ctest。
