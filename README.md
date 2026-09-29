# 3dtiles-renderer

跨平台 C++ 3D Tiles 渲染器 **SDK**：一次编码，随处运行 —— Windows / Android / iOS / WebAssembly。

> 当前阶段：**P3 数据上屏**（真实 tileset.json + GLB 经 Cesium 调度、gltfio 解码、Filament 渲染，见 ADR-0005）。

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

### 其他平台（P4：工具链自动接线）

```bash
cmake --preset android   # 需 ANDROID_NDK_HOME（r27d）；自动接 NDK toolchain + Filament android 预编译包
cmake --preset ios       # 需 macOS + Xcode；自动做 iOS arm64 交叉 + Filament xcframework
cmake --preset wasm      # 需 EMSDK；自动接 Emscripten（Filament 官方 web 包无 C++ 库，SDK 为 stub）
cmake --preset windows   # 在 Windows 上执行；自动接 Filament windows 预编译包
```

> P0 策略保留：工具链缺失时**只警告、不硬失败**，以降级桩配置继续。
> 各平台 backend 选择与预编译包布局调查见 ADR-0006。

### 平台状态矩阵（P4）

| 平台 | Backend | 本地编译验证 | CI 编译验证 | 运行验证 |
|---|---|---|---|---|
| Linux | OpenGL | ✅ gcc 全依赖 | ✅ 全依赖 + `xvfb-run ctest` 4/4 | ✅ xvfb + Mesa 像素断言 |
| Android | Vulkan | ✅ NDK r27d arm64（SDK + linkcheck.so 链接通过） | ✅ | ❌ 无真机 |
| Windows | Vulkan | ❌ 本地无 MSVC | ✅ MSVC + Filament 预编译包 | ❌ 无实机 |
| iOS | Metal | ❌ 本地无 Xcode | ✅ iOS arm64 交叉 + xcframework | ❌ 无设备 |
| Web (WASM) | WebGL（预留） | ✅ emcc SDK stub 交叉编译；renderer.cpp WASM 分支经 emcc + 真实 Filament 头文件语法验证 | ✅ | ❌ 无浏览器验证；官方 v1.77.0 web 包只有 `filament.js`，C++ 接线待办 |

> 诚实边界：只有 Linux 无头渲染经过真实像素验证；Android 链接通过不等于
> 真机运行成功；Web 的 Filament C++ 接线需自行源码构建（见 ADR-0006）。

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

### P3：tileset 数据上屏

```bash
# 默认加载可执行文件旁的自生成测试 tileset（tests/data/p3_box_tileset/）
xvfb-run -a ./build/linux/tiles_demo --frames 60 \
    --tileset tests/data/p3_box_tileset/tileset.json \
    --screenshot /tmp/p3.png
# --no-tileset 回到 P2 红三角模式；鼠标拖拽 orbit、滚轮缩放
```

测试数据由 `tests/data/gen_p3_tileset.py` 自生成（root 灰 10m 盒 +
child_a 橙 4m 盒 + child_b 青 4m 盒，ADD refine），不依赖外网。
`tileset_screenshot` 测试断言 3 tiles 上屏、截图含橙色与青色像素。

**P5 补齐三个 P3 缺口**（见 ADR-0005）：

- **HTTP/HTTPS**：`RoutingAssetAccessor` — `http(s)://` 走 cesium-native
  `CesiumCurl`（libcurl），本地路径/`file://` 行为不变；
  `http_tileset_screenshot` 用本地 `python3 -m http.server` 验证。
- **b3dm**：`B3dmToGltfConverter` 产出的 `CesiumGltf::Model` 经
  `CesiumGltfWriter::writeGlb` 重新序列化后交给 gltfio；
  b3dm 的 `RTC_CENTER`（`CESIUM_RTC` 扩展）以双精度参与 transform 计算；
  `b3dm_tileset_screenshot` 断言 b3dm 橙色瓦片上屏。
  i3dm 仍为 TODO（同路径理论可用，但无测试数据覆盖）。
- **ECEF→local-origin rebase**：tileset 加载后取 root bounding volume 中心
  为双精度原点；瓦片**选择**（Cesium ViewState）在真实世界坐标（double）
  进行，**渲染**时每 tile transform 减去原点再转 float32，Filament 相机
  围绕 (0,0,0)。`rebase_tileset_screenshot` 用 root 平移 123456789.0 m 的
  `far` tileset 验证：rebase 后与 `near` 的橙色质心偏差 ≤ 25 px。
  （这是通用 local-origin rebase，不是严格旋转 ENU 基；详见 ADR-0005。）

> **边界（诚实说明）**：i3dm 未实现（待办）；代理/证书走系统默认；
> rebase 不是完整 ENU 姿态变换。详见 ADR-0005。

### P6：内存与资源健壮性（见 ADR-0007）

```bash
# 1) 普通回归（9/9）：smoke、demo、截图、tileset、http、b3dm、rebase、lifecycle、fault_inputs
cmake --preset linux -DTILES_WITH_CESIUM_NATIVE=ON -DTILES_WITH_FILAMENT=ON -DTILES_WITH_SDL3=ON -DTILES_SDL3_NATIVE_VIDEO=ON
cmake --build --preset linux -j2
xvfb-run -a ctest --preset linux -I 1,9 --output-on-failure

# 2) sanitizer 门禁（6/6）：ASan + LSan + UBSan，只插桩自有 targets
cmake --preset linux-asan          # Debug + TILES_SANITIZE=ON，构建目录 build/linux-asan
cmake --build --preset linux-asan -j2
xvfb-run -a ctest --preset linux-asan -R sanitizer_ --output-on-failure
```

`TILES_SANITIZE=ON` 给 `tiles_renderer`/`tiles_demo`/测试加
`-fsanitize=address,undefined -fno-sanitize=vptr -fno-sanitize-recover=all`
（第三方保持无插桩；`-fno-sanitize=vptr` 是因为 Filament/cesium-native
预编译库均为 `-fno-rtti`，UBSan vptr 检查对其必然误报——已验证为误报）。
`tests/lsan.supp` 只收录确认过的第三方泄漏（当前 1 条：
cesium-native `CurlAssetAccessor` 的 handle 缓存泄漏），自有代码泄漏一律修复。
另新增故障注入测试（不存在路径/损坏 tileset.json/损坏 glb）与 API 生命周期测试
（未初始化调用、重复 initialize、shutdown 后调用、shutdown 后重初始化）。

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
- **P3 数据上屏**：cesium-native tileset.json 加载 + `updateViewGroup`/`loadTiles`
  调度、gltfio ubershader 解码 GLB、Filament PBR 渲染；自生成测试 tileset
  （灰/橙/青三盒）；`ViewUpdateResult` 驱动可见性；`xvfb-run` 像素断言 —— ✅ 已完成
  （本机 linux 全依赖构建 + 4/4 测试通过；SDK 零 SDL；见 ADR-0005）
- **P4 四端接线与 CI**：各平台 Filament backend 选定（Linux/OpenGL、
  Android/Vulkan、Windows/Vulkan、iOS/Metal、Web 预留 WebGL）；
  工具链自动接线（NDK r27d / Emscripten / Xcode）；Filament v1.77.0
  预编译包按平台接线（android/windows/ios 真链接，web 官方包无 C++ 库故 stub）；
  CI 四平台真实构建 —— ✅ 已完成
  （本地：linux 4/4、android NDK 链接通过、wasm emcc 交叉通过；
  windows/ios 编译靠 CI；运行验证仅 linux 无头；见 ADR-0006）
- **P5 真机与 Web**：Android APK / iOS app 真机冒烟、Windows 实机、
  浏览器运行；Filament for Web 源码构建或 filament.js 桥接；
  P3 遗留（i3dm、完整 ENU 姿态变换）—— HTTP(S)、b3dm、local-origin rebase
  已在本阶段完成并验证（见 ADR-0005）
- **P6 内存与资源健壮性**：ASan+LSan+UBSan 门禁（`linux-asan` preset，
  只插桩自有 targets）；故障注入（坏路径/坏 tileset/坏 glb 优雅处理）；
  API 生命周期测试；`registerAllTileContentTypes()` 改 `std::call_once`；
  Filament teardown 顺序修正（Renderer 先于 SwapChain）—— ✅ 已完成
  （sanitizer 6/6、普通 9/9，SDK 零 SDL；见 ADR-0007）

## AI 协作

本项目采用 AI 辅助开发：提示词集合见《AI Agent 提示词集合》（P0 配套文档），
每条提示词自带验收标准。人类只做评审与拍板。
