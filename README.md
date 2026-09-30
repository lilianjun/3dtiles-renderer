# 3dtiles-renderer

跨平台 C++ 3D Tiles 渲染器 **SDK**：一次编码，随处运行 —— Windows / Android / iOS。

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
| 渲染 | [Filament](https://github.com/google/filament) | `v1.77.0` | 官方预构建二进制包（Linux 已真实渲染；Win/Android/iOS 接线为 TODO，见 ADR-0004） |
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
└── CMakePresets.json        # linux / windows / android / ios
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

> 诚实边界：只有 Linux 无头渲染经过真实像素验证；Android 链接通过不等于
> 真机运行成功。Web 端不在本仓库范围内（见下）。

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
  i3dm 见 P7（ADR-0008）。
- **ECEF→local-origin rebase**：tileset 加载后取 root bounding volume 中心
  为双精度原点；瓦片**选择**（Cesium ViewState）在真实世界坐标（double）
  进行，**渲染**时每 tile transform 减去原点再转 float32，Filament 相机
  围绕 (0,0,0)。`rebase_tileset_screenshot` 用 root 平移 123456789.0 m 的
  `far` tileset 验证：rebase 后与 `near` 的橙色质心偏差 ≤ 25 px。
  （这是通用 local-origin rebase，不是严格旋转 ENU 基；详见 ADR-0005。）

> **边界（诚实说明）**：i3dm 见 P7（ADR-0008），GPU instancing 经 P27
> 调研确认为边界（ADR-0026：Filament v1.77 ubershader 无 instancing
> shader 支持、gltfio 无 hook，CPU 展开保持为正确性路径）；pnts 见
> P8（ADR-0009）；cmpt 见 P9（ADR-0010）；3D Tiles 1.1 见 P10
> （ADR-0011）；代理/证书走系统默认；rebase 不是完整 ENU 姿态变换。
> 详见 ADR-0005、ADR-0008、ADR-0009、ADR-0010、ADR-0011、ADR-0026。

### P6：内存与资源健壮性（见 ADR-0007）

```bash
# 1) 普通回归（25/25）：smoke、demo、截图、tileset、http、b3dm、rebase、
#    i3dm、pnts、cmpt、tiles11、golden_regression、lifecycle、
#    fault_inputs（P6/P28：损坏输入优雅失败；P28 新增 P/Q/R/S：曾 SIGSEGV
#    的结构性损坏 cmpt 经 converter guard 转为 failed tile）、
#    host_integration（P12：宿主视角 API 集成检查）、
#    pbr_materials_screenshot（P15：PBR 材质管线验证）、
#    trajectory_determinism（P16：确定性相机轨迹回放）、
#    tile_stats（P17：tile 流式统计诊断）、
#    weak_network（P18：弱网韧性：慢网/取消/中断/断网恢复，~8s）、
#    memory_roam（P19：3 圈相机漫游内存有界，驱逐验证）、
#    frustum_lod（P20：视锥剔除 per-tile 集合翻转 + REPLACE 精化切换，<2s）、
#    add_region（P21：ADD 精化 root 不退场 vs REPLACE 对比门禁 +
#    region 地理包围体加载/渲染，~3s）、
#    tileset_switch（P22：loadTileset 再入/切换安全：干净切换像素证明 +
#    失败重试不破坏现 tileset + 同路径重载 + 慢网加载中切换，~20s）、
#    tileset_failfast（P23：损坏 tileset.json 毫秒级失败、旧场景像素无扰动 +
#    慢网不误杀）、
#    ktx2（P25：KTX2/UASTC 纹理真实渲染、与 PNG 对照像素断言 +
#    损坏 KTX2 优雅失败）、
#    ibl（P26：默认 IBL 开/关 A/B：金属明显变亮并出现环境反射、
#    dielectric 不爆 + 几何无变化）、
#    draco（P29：Draco 压缩网格真实渲染、与未压缩双胞胎 bit-identical +
#    损坏 Draco 优雅失败；解码器来自 cesium-native 自带 vcpkg draco，零新依赖）
cmake --preset linux -DTILES_WITH_CESIUM_NATIVE=ON -DTILES_WITH_FILAMENT=ON -DTILES_WITH_SDL3=ON -DTILES_SDL3_NATIVE_VIDEO=ON
cmake --build --preset linux -j2
xvfb-run -a ctest --preset linux -I 1,9 --output-on-failure

# 2) sanitizer 门禁（21/21）：ASan + LSan + UBSan，只插桩自有 targets
#    （P18 新增 sanitizer_weaknet_abort：加载中途 teardown，门禁无报告；
#     P19 新增 sanitizer_memory_roam：150 帧漫游内存有界，门禁无报告；
#     P20 新增 sanitizer_frustum_lod：视锥/LOD 场景门禁无报告；
#     P21 新增 sanitizer_add_region：ADD/region 场景门禁无报告；
#     P22 新增 sanitizer_tileset_switch：切换 teardown 路径门禁无报告；
#     P23 新增 sanitizer_tileset_failfast：损坏 tileset 预检路径门禁无报告；
#     P25 新增 sanitizer_ktx2：KTX2 转码/上传/shutdown 门禁无报告；
#     P26 新增 sanitizer_ibl：IBL cubemap/SH/IndirectLight/mipmap 路径门禁无报告；
#     P29 新增 sanitizer_draco：Draco 解码/渲染/损坏路径门禁无报告）
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

## SDK 安装与外部集成（P13）

```bash
source ~/toolchains/env.sh
cmake --preset linux -DTILES_WITH_CESIUM_NATIVE=ON -DTILES_WITH_FILAMENT=ON
cmake --build --preset linux -j2
cmake --install build/linux --prefix /opt/tiles_renderer   # 或任意 prefix
```

安装内容：`libtiles_renderer.a`、公共头（含生成的 `version.h`）、
`tiles_rendererConfig.cmake`（`find_package(tiles_renderer)` 用），以及
cesium-native / Filament / vcpkg 第三方库的头文件与静态库——外部项目只需：

```cmake
find_package(tiles_renderer CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE tiles_renderer::tiles_renderer)
```

`Renderer::version()` 的版本号唯一来源是顶层 `project(VERSION)`（当前
`0.1.0`），`CHANGELOG.md` 按 Keep a Changelog 记录各阶段真实变更。

发布边界（诚实版）：**源码集成发行，不提供预编译二进制包**。消费方从
源码构建 SDK 后再安装；安装产物不可跨平台搬运。Filament 官方预编译库用
clang/libc++ 构建，Linux 消费方链接时会带上 `c++`/`c++abi`（与消费方自己
的 libstdc++ 共存，见 `CMakeLists.txt` 注释）；安装的是 release 第三方
库，`debug/` 指向 release 树做兼容（不承诺 debug 信息保真）。
`docs/integration.md` 有四平台宿主集成指南。

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
  Android/Vulkan、Windows/Vulkan、iOS/Metal）；
  工具链自动接线（NDK r27d / Xcode）；Filament v1.77.0
  预编译包按平台接线（android/windows/ios 真链接）；
  CI 真实构建 ✅ 已完成
  （本地：linux 全依赖构建、android NDK 链接通过；
  windows/ios 编译靠 CI；运行验证仅 linux 无头；见 ADR-0006）
- **P5 真机与 Web**：Android APK / iOS app 真机冒烟、Windows 实机；
  Web 端不在本 C++ 仓库范围内：2026-09-30 经 li 决策，WASM stub
  构建已整体移除，未来 web 渲染器走独立 JS 项目（JS 写加载与数据解析、
  filament.js 渲染），待 C++ SDK 完成后重估（见 ADR-0023 G9）；
  P3 遗留（i3dm、完整 ENU 姿态变换）—— HTTP(S)、b3dm、local-origin rebase
  已在本阶段完成并验证（见 ADR-0005）
- **P6 内存与资源健壮性**：ASan+LSan+UBSan 门禁（`linux-asan` preset，
  只插桩自有 targets）；故障注入（坏路径/坏 tileset/坏 glb 优雅处理）；
  API 生命周期测试；`registerAllTileContentTypes()` 改 `std::call_once`；
  Filament teardown 顺序修正（Renderer 先于 SwapChain）—— ✅ 已完成
  （sanitizer 6/6、普通 9/9，SDK 零 SDL；见 ADR-0007）
- **P7 i3dm 真实支持**：cesium-native `I3dmToGltfConverter` 产出
  `EXT_mesh_gpu_instancing`，SDK 把实例**展开为普通 glTF nodes**
  （Filament v1.77 gltfio 只解析扩展名、不执行实例属性）；
  多 buffer 合并、converter 的 up-axis 共轭补偿（tile root 附加
  Y-up→Z-up 旋转）、RTC_CENTER 双精度 rebase；
  `i3dm_tileset_screenshot` 断言 12 橙色 + 8 旋转青色实例上屏、
  256 实例不崩、rebase near/far 与 RTC/reference 截图 bit-identical；
  故障注入加截断/坏 magic/INSTANCES_LENGTH 越界 i3dm 优雅失败—— ✅ 已完成
  （sanitizer 7/7、普通 10/10，SDK 零 SDL；见 ADR-0008）
- **P8 pnts 真实支持**：cesium-native `PntsToGltfConverter` 产出 POINTS
  primitive，Filament v1.77 gltfio + ubershader **原生渲染点云**——无需
  SDK 代码改动（P5 的 RTC 提取 + P7 的多 buffer 合并已覆盖；upAxisFix
  仅对 `EXT_mesh_gpu_instancing` 生效）；每点恰好 1 像素、颜色精确；
  `pnts_tileset_screenshot` 断言 420 点三色像素数、60000 点不崩、
  rebase near/far 与 RTC/reference 截图 bit-identical；
  故障注入加截断/坏 magic/POINTS_LENGTH 越界 pnts 优雅失败—— ✅ 已完成
  （sanitizer 19/19、普通 11/11，SDK 零 SDL；见 ADR-0009）
- **P9 cmpt 真实支持**：cesium-native `CmptToGltfConverter` 递归转换
  内嵌 tile 并 `Model::merge` 合并为一个 Model——无需 SDK 代码改动
  （P5 的 RTC 提取 + P7 的多 buffer 合并 + P7 的实例展开 + P8 的原生
  POINTS 渲染已全覆盖）；`cmpt_tileset_screenshot` 断言同一帧含 b3dm
  橙色盒子、pnts 64 点（每点 1px）、i3dm 6 实例（多 blob）；
  故障注入加截断/坏 magic/tilesLength 不符 cmpt 优雅失败；
  `sanitizer_cmpt` 进 linux-asan 门禁—— ✅ 已完成
  （sanitizer 21/21、普通 12/12，SDK 零 SDL；见 ADR-0010）

- **P10 3D Tiles 1.1 真实支持**：`asset.version: "1.1"` tileset——裸 `.glb`
  tile content（`3DTILES_content_gltf`）经 magic `"glTF"` 走
  `BinaryToGltfConverter` 进同一 render bridge；implicit QUADTREE
  （`3DTILES_implicit_tiling`，subtreeLevels/availableLevels 2）经
  cesium-native `ImplicitQuadtreeLoader` 加载手写 JSON subtree
  （constant availability）并按 `{level}_{x}_{y}` 模板取 tile content——
  均无需 SDK 代码改动；`tiles11_screenshot` 断言裸 glb 双色盒子与
  implicit 5 tiles（红/绿/蓝/黄/品红）上屏；
  故障注入加损坏 glb/缺失 subtree/非法 subdivisionScheme 优雅失败；
  `sanitizer_11` 进 linux-asan 门禁—— ✅ 已完成
  （见 ADR-0011）

- **P13 SDK 安装打包**：`cmake --install` 安装公共头 +
  `libtiles_renderer.a` + `tiles_rendererConfig.cmake`
 （`find_package(tiles_renderer)` → `tiles_renderer::tiles_renderer`）；
  cesium-native 自带 config 与 vcpkg 第三方包 config 一并安装，
  Filament 预编译 archives/headers 随 SDK 安装并由 config 绝对路径引用；
  外部最小项目验证 configure+link+`version()` 输出 `0.1.0`；
  `CHANGELOG.md`（Keep a Changelog，P0–P13 真实记录）；
  发行策略：源码 FetchContent 集成、不提供预编译二进制包—— ✅ 已完成
  （普通 ctest 17/17，SDK 零 SDL）

## AI 协作

本项目采用 AI 辅助开发：提示词集合见《AI Agent 提示词集合》（P0 配套文档），
每条提示词自带验收标准。人类只做评审与拍板。

## License

MIT — see [LICENSE](LICENSE).
