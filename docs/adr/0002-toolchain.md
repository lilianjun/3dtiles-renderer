# ADR-0002: 开发工具链与第三方依赖版本锁定

- 状态：已接受
- 日期：2026-09-29
- 范围：本地开发环境（`~/toolchains`）+ P1 依赖接入（SDL3 / cesium-native / Filament）

## 1. 背景

目标技术栈（ADR-0001）：Cesium Native + Filament + SDL3，C++20 / CMake，
目标平台 Win / Android / iOS / WebAssembly。

本机为 Ubuntu 24.04.5 LTS（x86_64，2 核 / 7.7 GiB RAM），无 GPU。
`apt` 在此环境不可靠（后台任务被运行时用户确认拦截，且 `apt-get download`
无可用候选），因此工具链全部采用**官方预编译二进制**装入 `~/toolchains`，
可复现、可版本锁定。`~/toolchains/env.sh` 一键导入环境。

## 2. 工具链版本（2026-09-29 实测）

| 工具 | 版本 | 位置 | 验证命令 |
|---|---|---|---|
| Ninja | 1.12.1 | `~/toolchains/bin/ninja` | `ninja --version` → 1.12.1 |
| LLVM / Clang | 23.1.2 | `~/toolchains/llvm` | `clang --version` → 23.1.2 |
| Emscripten SDK | 6.0.10（`latest` 别名 2026-09-29 解析结果） | `~/toolchains/emsdk` | `emcc --version` → 6.0.10 |
| Android NDK | r27d（27.3.13750724，r27 LTS 系） | `~/toolchains/android-ndk-r27d` | 最小 CMake 工程 + `android.toolchain.cmake`（arm64-v8a）配置并编译通过 |

备注：
- emsdk 首次安装失败：其自带 Node 解压时 `tar` 试图 `chown` 到 uid/gid 1000，
  被文件系统拒绝。解决：`TAR_OPTIONS="--no-same-owner"` 后重装成功。
- Android NDK 选用 r27 LTS 系（长期支持），r27d 为该系最新补丁。
- Filament 构建必须 `-j2`（内存/磁盘限制）；P1 采用官方预构建包而非源码编译。

## 3. 第三方依赖版本锁定（稳定 tag，不跟 main）

| 依赖 | 锁定版本 | 接入方式 |
|---|---|---|
| SDL3 | `release-3.4.16` | FetchContent 源码构建（shallow clone） |
| cesium-native | `v0.64.0` | FetchContent 源码构建（shallow clone）；其自带 ezvcpkg 自动用 vcpkg 构建约 15 个第三方 port（ada、Async++、blend2d、draco、glm、Ktx、meshoptimizer、OpenSSL、s2、spdlog 等） |
| Filament | `v1.77.0` | 官方 release 预构建包（Linux 用 `filament-v1.77.0-linux.tgz`；android/ios/web/windows 包在 P2 接线） |

决策记录：
- cesium-native：曾有资料提到 v0.66.0（2026-09-08），但 GitHub 标签 API 与
  raw 文件均 404，只有 `v0.64.0` 可验证存在 → 锁定 `v0.64.0`。
- Filament：源码全量构建在 2 核/小内存机器上不可行 → P1 用官方预构建二进制，
  CMake 中以 `filament_prebuilt` INTERFACE target 封装头文件与静态库；
  从源码构建 Filament 留待专用构建机/CI。
- SDL3 无头（headless）构建：本机 X11 开发包不完整，SDL3 的 X11 检查会硬失败；
  P1 默认 `SDL_X11=OFF SDL_WAYLAND=OFF SDL_UNIX_CONSOLE_BUILD=ON`
 （dummy video driver），桌面开发可用 `-DTILES_SDL3_NATIVE_VIDEO=ON` 打开原生后端。

## 4. 集成顺序与验证（由小到大）

1. SDL3 → `cmake --preset linux -DTILES_WITH_SDL3=ON`：configure 约 2m08s，
   构建约 1m18s（-j2）；`tiles_renderer` 输出 `SDL3: 3.4.16`；`ctest` 1/1 通过。
2. cesium-native → 在 1 的基础上加 `-DTILES_WITH_CESIUM_NATIVE=ON`：
   首次 configure 触发 vcpkg 构建全部 port（2 核机器上约数十分钟），
   链接 `Cesium3DTilesSelection`；`tiles_renderer` 输出 `cesium-native: v0.64.0`；
   `ctest` 1/1 通过。
3. Filament → 在 2 的基础上加 `-DTILES_WITH_FILAMENT=ON`：
   下载预构建包并创建 imported targets；`tiles_renderer` 输出 `filament: v1.77.0`；
   `ctest` 1/1 通过。

注意：vcpkg 默认会尝试上传遥测（`z-upload-metrics`），曾被运行时拦截导致
configure 卡住。已在 `~/toolchains/env.sh` 中设置 `VCPKG_DISABLE_METRICS=1`，
所有使用 vcpkg 的构建都应先 `source ~/toolchains/env.sh`。

## 5. 后果

- Linux preset 默认三个 `TILES_WITH_*` 仍为 OFF（零下载即可配置，见 ADR-0001）；
  全依赖验证通过显式 `-D` 打开。
- 首次全依赖 configure 很慢（vcpkg ports），后续增量构建正常。
- Windows / Android / iOS / WASM 的真实构建仍需对应平台 CI 或真机验证
  （iOS 需 macOS + Xcode）。
