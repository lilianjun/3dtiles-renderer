# ADR-0006：四平台 backend 选择与 Filament 预编译包接线（P4）

- 状态：已接受
- 日期：2026-09-29
- 背景：ADR-0003、ADR-0004

## 决策

1. 每个平台固定一个 Filament backend，由 `TILES_PLATFORM_*` 编译期宏在
   `Renderer::initialize()` 内选择，不再由运行期参数决定：

   | 平台 | Backend | 窗口句柄来源 |
   |---|---|---|
   | Linux | `Backend::OPENGL` | X11 `Window`（`NativeWindowHandle`） |
   | Android | `Backend::VULKAN` | `ANativeWindow*`（Java 层 `Surface` 传下） |
   | Windows | `Backend::VULKAN` | `HWND`（`void*` 透传） |
   | iOS | `Backend::METAL` | `UIView*`（`void*` 透传） |
   | Web (WASM) | `Backend::DEFAULT`（= WebGL） | canvas CSS selector（`const char*` 长期存活字符串） |

   选择依据：Filament 官方各平台示例的默认 backend；Android 上 Vulkan 是
   Filament 移动端主路径（API 24+ 可用）；iOS 只有 Metal 可用；
   Windows 上 Vulkan 避免 OpenGL 驱动碎片化；WASM 只有 WebGL。

2. SDK 公开的 `NativeWindowHandle` 保持平台相关类型
   （`include/tiles_renderer/renderer.h`），各平台传参即原生句柄，
   不做二次封装——宿主拥有窗口，SDK 只做绘制（ADR-0003）。

3. 各平台 Filament 预编译包（v1.77.0，`gh release` 直接确认）由 CMake
   在 configure 时下载解压，按平台接线：

   | 平台 | 包 | 关键布局发现 |
   |---|---|---|
   | linux | `filament-v1.77.0-linux.tgz` | `lib/x86_64/*.a`，`bin/matc` |
   | android | `filament-v1.77.0-android-native.tgz` | `lib/arm64-v8a/*.a`（`libbluevk.a`，**无 bluegl**；`gltfio` 实名为 `libgltfio_core.a`），`include/` |
   | windows | `filament-v1.77.0-windows.tgz` | `lib/x86_64/md/*.lib`（注意多一层 `md/`，对应 /MD 运行时），`bin/matc.exe` |
   | ios | `filament-v1.77.0-ios.tgz` | `.xcframework`，用 `ios-arm64` slice（`ios-arm64/libfilament.a` + `Headers/`） |
   | web | `filament-v1.77.0-web.tgz` | **只有 `filament.js` / `filament.wasm` / `filament.d.ts`，无 C++ 头文件与静态库** |

   材质统一用**宿主** `matc -a all` 编译（`matc` 不可交叉运行，必须用
   host 工具），跨平台 `filamat` 二进制兼容。

## 关键工程结论

- **Android 真机链路可链接**：NDK r27d + `arm64-v8a` 静态库组，
  `libtiles_renderer.a` 与 `tiles_renderer_linkcheck.so` 本地构建通过。
  链接中发现 `Engine::create()` 的 backend switch 会把所有 driver
  （含 `OpenGLDriver`）拉进链接，因此 NDK 系统库 `GLESv3` / `EGL`
  是 SDK 在 Android 上的**真实传递依赖**，已写入 `filament_prebuilt`
  的 INTERFACE。注意：链接通过 ≠ 真机运行验证（无 Android 真机，
  见下）。
- **Web（WASM）边界**：官方 v1.77.0 web 包不是 C++ SDK，当前 Emscripten
  预设只能做 SDK stub 的真交叉编译（`libtiles_renderer.a` for wasm32，
  本地 emcc 构建通过）；renderer.cpp 的 WASM 分支
  （`Backend::DEFAULT` + canvas selector）已用 emcc + 真实 Filament 头文件
  做 `-fsyntax-only` 语法验证。要真跑 WebGL，需自行从源码构建 Filament
  for Web（或走 `filament.js` 桥接），留作后续事项——**不声称已接通**。
- **工具链自动接线**：`ANDROID_NDK_HOME` / `EMSDK` 存在即自动用真实
  toolchain；缺失则警告 + host stub（P0 策略不变）。`TILES_CROSS_TOOLCHAIN`
  每轮 configure 从已缓存状态**确定性推导**（不靠 cache 记忆），
  保证 re-configure 与 fresh configure 行为一致（修过一次
  re-configure 回退到 stub 的 bug）。
- **NDK 版本统一为 r27d**（本地与 CI 一致，原 CI 用 r26d）。
- **CI 触发分支修正为 `master`**（仓库默认分支是 `master`，原 workflow
  监听 `main` 导致 push 从未触发 CI）。

## 验证矩阵（P4 验收时实际状态）

| 平台 | 本地编译 | CI 编译 | 运行验证 |
|---|---|---|---|
| Linux | ✅ gcc 全依赖 | ✅ 全依赖 + xvfb ctest 4/4 | ✅ xvfb + Mesa 像素断言 |
| Android | ✅ NDK r27d arm64（SDK + linkcheck.so）| ✅ | ❌ 无真机 |
| Windows | ❌ 本地无 MSVC | ✅ MSVC + Filament | ❌ 无实机 |
| iOS | ❌ 本地无 Xcode | ✅ iOS arm64 交叉 | ❌ 无设备 |
| WASM | ✅ emcc stub 交叉编译 | ✅ | ❌ 无浏览器验证；Filament C++ 未接 |

## CI 实战记录（2026-09-29，PR #1）

- Windows runner（`windows-2025-vs2026` 镜像）默认捡到 MinGW，
  链不动 Filament 的 MSVC `/MD` 预编译 `.lib`（ABI 不兼容）。
  修法：preset 不再写 `generator`（删掉原来的 `Unix Makefiles`），
  让 CMake 自动选最新安装的 Visual Studio；另加
  `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL`
  与 Filament 的 `md/` 包对齐（防 LNK2038），以及 `/Zc:__cplusplus`
 （MSVC 默认 `__cplusplus` 恒为 `199711L`，smoke test 的 C++20
  static_assert 会误杀）。
- iOS：xcframework 布局下没有 `link_directories`，`zstd` 必须走
  xcframework 全路径（与其它 archive 一样进 `_filament_libs`），
  不能按裸名进 `_filament_syslibs`。
- iOS 链接另需 `-framework OpenGLES`：`Engine::create()` 的 backend
  switch 会把 GL driver 工具函数拉进链接（与 Android 上
  GLESv3/EGL 同一类问题）。
- 以上三处都是 `tiles_renderer_linkcheck` 先在本机/CI 暴露出来的——
  说明"强制真链接"的验收目标达到了设计目的。

## 后续事项

- P3 遗留：HTTP(S) asset accessor、b3dm/i3dm 内容类型、ECEF→ENU rebase。
- Web：Filament 源码构建 for Web，或 `filament.js` 桥接方案。
- 真机/实机冒烟：Android APK、iOS app、Windows exe、浏览器。
