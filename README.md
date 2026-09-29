# 3dtiles-renderer

跨平台 C++ 3D Tiles 渲染器：一次编码，随处运行 —— Windows / Android / iOS / WebAssembly。

> 当前阶段：**P0 仓库骨架**（可编译、可测试的空壳）。渲染器主体在 P1 起逐步接入。

## 技术栈

| 分层 | 选型 | 说明 |
|---|---|---|
| 3D Tiles 加载 / 调度 / LOD | [cesium-native](https://github.com/CesiumGS/cesium-native) | Cesium 官方 C++ 库，不重复造轮子 |
| 渲染 | [Filament](https://github.com/google/filament) | Google PBR 渲染器，支持 Vulkan / Metal / OpenGL / WebGL |
| 窗口 / 输入 | [SDL3](https://github.com/libsdl-org/SDL) | 四平台窗口与输入抽象 |
| 语言 | C++20 | |
| 构建 | CMake ≥ 3.21 + Presets | 依赖经 `FetchContent` 声明 |

选型依据见 [docs/adr/0001-tech-stack.md](docs/adr/0001-tech-stack.md)。

## 目录结构

```
3dtiles-renderer/
├── src/                 # 入口与实现（P0: main.cpp 桩）
├── include/tiles_renderer/  # 公共头文件（P0: version.h.in）
├── tests/               # 测试（P0: smoke 桩；P1+ 按模块加单测）
├── docs/adr/            # 架构决策记录
├── .github/workflows/   # CI：四平台构建矩阵
├── CMakeLists.txt
└── CMakePresets.json    # linux / windows / android / ios / wasm
```

## 构建

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
./build/linux/tiles_renderer
ctest --preset linux
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

### 打开第三方依赖（P1 用，P0 默认关闭）

```bash
cmake --preset linux -DTILES_WITH_CESIUM_NATIVE=ON -DTILES_WITH_FILAMENT=ON -DTILES_WITH_SDL3=ON
```

## 路线图

- **P0 仓库骨架**（本阶段）：CMake 四平台 presets、CI 矩阵、ADR、空壳 main + smoke 测试 —— ✅ 进行中
- **P1 依赖接入**：FetchContent 真正拉取 cesium-native / Filament / SDL3 并链接；SDL3 空窗口能跑
- **P2 渲染器**：Filament PBR 管线，渲染首个 glTF 模型
- **P3 LOD 调度**：cesium-native tileset.json 加载、视锥裁剪、瓦片缓存
- **P4 移动端**：Android / iOS 真机优化、触摸输入
- **P5 WASM**：Emscripten 发布、浏览器内运行

## AI 协作

本项目采用 AI 辅助开发：提示词集合见《AI Agent 提示词集合》（P0 配套文档），
每条提示词自带验收标准。人类只做评审与拍板。
