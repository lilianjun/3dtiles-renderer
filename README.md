# 3dtiles-renderer

跨平台 C++ 3D Tiles 渲染器 **SDK**：一次编码，随处运行 —— Windows / Android / iOS / WebAssembly。

> 当前阶段：**P2 渲染器**（Filament 真实渲染 + 无头截图验证，见 ADR-0004）。

## 架构（ADR-0003）

最终交付物是 **SDK**，不是独立 App：

- **`tiles_renderer`（静态库 SDK）**：对外 API 只接受平台原生窗口句柄
  （HWND / `ANativeWindow*` / `UIView*` / canvas selector / X11 `Window`…）+ 宽高，
  负责渲染（Filament）与 3D Tiles 加载/调度（cesium-native）。**零 SDL 依赖。**
- **`tiles_demo`（SDL3 示例程序）**：用 SDL3 建窗口、收输入，提取原生句柄后调用 SDK；
  是宿主 App 的参考集成，用于四平台验证。

决策依据见 [docs/adr/0003-sdk-no-sdl.md](docs/adr/0003-sdk-no-sdl.md)。

## 技术栈

| 分层 | 选型 | 锁定版本（P1） | 接入方式 |
|---|---|---|---|
| 3D Tiles 加载 / 调度 / LOD | [cesium-native](https://github.com/CesiumGS/cesium-native) | `v0.64.0` | FetchContent 源码构建（ezvcpkg 自动构建其第三方 port） |
| 渲染 | [Filament](https://github.com/google/filament) | `v1.77.0` | 官方预构建二进制包（Linux 已真实渲染；Win/Android/iOS/WASM 接线为 TODO，见 ADR-0004） |
| 窗口 / 输入（仅 demo/测试） | [SDL3](https://github.com/libsdl-org/SDL) | `release-3.4.16` | FetchContent 源码构建；**不进 SDK**（ADR-0003） |
| 语言 | C++20 | | |
| 构建 | CMake ≥ 3.21 + Presets | | 依赖经 `FetchContent` 声明，tag 锁定 |

选型依据见 [docs/adr/0001-tech-stack.md](docs/adr/0001-tech-stack.md)，
工具链与版本锁定见 [docs/adr/0002-toolchain.md](docs/adr/0002-toolchain.md)。

## 目录结构

```
3dtiles-renderer/
├── src/                     # SDK 实现（renderer.cpp）
├── include/tiles_renderer/  # SDK 公共头文件（renderer.h、version.h.in）
├── samples/demo/            # SDL3 示例程序（参考集成，四平台验证用）
├── tests/                   # 测试（smoke；P2+ 按模块加单测）
├── docs/adr/                # 架构决策记录
├── .github/workflows/       # CI：四平台构建矩阵
├── CMakeLists.txt
└── CMakePresets.json        # linux / windows / android / ios / wasm
```

## 构建

### 开发工具链（P1 已搭建，见 docs/adr/0002-toolchain.md）

```bash
source ~/toolchains/env.sh   # ninja 1.12.1 / clang 23.1.2 / emcc 6.0.10 / NDK r27d / VCPKG_DISABLE_METRICS=1
```

| 工具 | 版本 | 位置 |
|---|---|---|
| Ninja | 1.12.1 | `~/toolchains/bin` |
| LLVM / Clang | 23.1.2 | `~/toolchains/llvm` |
| Emscripten SDK | 6.0.10 | `~/toolchains/emsdk` |
| Android NDK | r27d | `~/toolchains/android-ndk-r27d`（`ANDROID_NDK_HOME`） |

### 前置要求

| 平台 | 需要 |
|---|---|
| Linux | cmake ≥ 3.21, gcc ≥ 11 / clang |
| Windows | Visual Studio 2022, cmake |
| Android | Android NDK（设 `ANDROID_NDK_HOME`），API ≥ 24 |
| iOS | macOS + Xcode 15+ |
| WASM | Emscripten SDK（激活后 `emcc` 在 PATH 中） |

### Linux（本机已验证）

```bash
cmake --preset linux
cmake --build --preset linux
ctest --preset linux       # smoke 测试链接 SDK 静态库并运行
```

### 其他平台

```bash
cmake --preset windows   # 在 Windows 上执行
cmake --preset android   # 需 ANDROID_NDK_HOME；缺失时给出警告并以降级桩配置继续
cmake --preset ios       # 需 macOS + Xcode；缺失时给出警告并以降级桩配置继续
cmake --preset wasm      # 需 emcc；缺失时给出警告并以降级桩配置继续
```

> P0 策略：SDK 缺失时**只警告、不硬失败**，保证任何机器都能 configure 成功。
> 真正的交叉工具链在 P1 接入（见路线图）。

### 打开第三方依赖（P1 已接入，默认仍关闭以保持零下载可配置）

```bash
source ~/toolchains/env.sh   # 必须：禁用 vcpkg 遥测，否则 configure 会被拦截
cmake --preset linux -DTILES_WITH_CESIUM_NATIVE=ON -DTILES_WITH_FILAMENT=ON -DTILES_WITH_SDL3=ON -DTILES_SDL3_NATIVE_VIDEO=ON
cmake --build --preset linux -j2   # 2 核机器请用 -j2
xvfb-run -a ctest --preset linux --output-on-failure   # smoke + demo_runs + demo_screenshot
```

`demo_screenshot` 会渲染 30 帧并保存 `build/linux/screenshots/p2_demo.png`
（深蓝背景 + 红色三角形），用 Pillow 做像素级断言。`tiles_demo` 也支持手动截图：

```bash
xvfb-run -a ./build/linux/tiles_demo --frames 30 --width 800 --height 600 \
    --screenshot /tmp/shot.png
```

> 首次全依赖 configure 会触发 vcpkg 构建 cesium-native 的约 15 个第三方 port，
> 在 2 核机器上需要数十分钟，请耐心等待。SDL3 的 X11 视频后端需要
> `-DTILES_SDL3_NATIVE_VIDEO=ON`（及 `libx11-dev libxext-dev libxrandr-dev
> libxcursor-dev libxi-dev libxtst-dev`）；这是渲染验证（Xvfb + 截图测试）
> 的必需开关，默认关闭以保持纯头文件/控制台可配置。

## 路线图

- **P0 仓库骨架**：CMake 四平台 presets、CI 矩阵、ADR、空壳 main + smoke 测试 —— ✅ 已完成
- **P1 依赖接入**：SDK 拆分为静态库（cesium-native + Filament，零 SDL 依赖）与
  SDL3 示例程序（`tiles_demo`）；SDL3 只进 demo 与测试 —— ✅ 已完成
  （本机 linux 全依赖构建 + 测试通过；见 ADR-0003）
- **P2 渲染器**：Filament 真实渲染（Linux/OpenGL + X11 swapchain），深蓝背景 +
  红色三角形最小场景；`renderFrame()->bool`、`readPixels()` 回调截图；
  `xvfb-run` 无头像素断言；`nm` 回归 SDK 零 SDL —— ✅ 已完成
  （本机 linux 全依赖构建 + 3/3 测试通过；见 ADR-0004）
- **P3 LOD 调度**：cesium-native tileset.json 加载、视锥裁剪、瓦片缓存
- **P4 移动端**：Android / iOS 真机优化、触摸输入
- **P5 WASM**：Emscripten 发布、浏览器内运行

## AI 协作

本项目采用 AI 辅助开发：提示词集合见《AI Agent 提示词集合》（P0 配套文档），
每条提示词自带验收标准。人类只做评审与拍板。
