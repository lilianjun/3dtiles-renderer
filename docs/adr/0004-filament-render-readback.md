# ADR-0004: Filament 真实渲染与无头截图验证（P2）

- 状态：已接受（2026-09-29）
- 范围：`tiles_renderer` SDK 的 Linux 渲染后端、`tiles_demo` 截图链路、CTest 像素断言

## 背景

P2 要让 SDK 真正画出东西，并在无 GPU 的 CI 机器上自动验证“画出来了”。
约束：SDK 保持零 SDL（ADR-0003）；构建机只有 Mesa llvmpipe + Xvfb。

## 决策

1. **Linux 后端 = Filament OpenGL + 原生 X11 句柄**
   `Renderer::initialize` 用 `Engine::create(Backend::OPENGL)`，
   `createSwapChain((void*)x11Window)`。Windows/Android/iOS/WASM 保留
   明确 TODO 并返回 `false`（不硬失败，见 `src/renderer.cpp`）。
2. **最小可见场景**：深蓝清屏色 `(0.1, 0.2, 0.45)` + 一个纯红 unlit 三角形。
   材质用 `matc`（Filament 自带 `bin/matc`，`-a opengl`）离线编译，
   `cmake/embed_filamat.py` 把 `.filamat` 转成 C++ 头文件编入 SDK，
   运行时零数据文件。
3. **截图走 `Renderer::readPixels` 回调，不截屏文件**：公共 API 新增
   `readPixels(vector<uint8_t>& RGBA, w, h)`（top-left 原点），PNG 编码
   留给调用方（demo 用 stb_image_write）。Filament 类型不泄漏到公共头。
4. **readback 的三个坑**（实测结论）：
   - `beginFrame()` 非阻塞：驱动忙时返回 `false`（llvmpipe 上紧循环
     120 次只有 2 次成功），必须按“成功帧计数 + 超时”重试，不能按
     尝试次数计数；
   - `readPixels` 的用户回调由驱动线程经 fence 入队到主线程消息队列，
     必须每帧调 `Engine::pumpMessageQueues()` 才会触发；
     （源码：`DriverBase::scheduleCallback` → `purge()` ← `pumpMessageQueues()`）
   - 回调触发前持续 pump `beginFrame/render/endFrame`，15s 超时兜底。
5. **预构建包是 clang/libc++ 构建的**：链接时追加系统 `libc++`/`libc++abi`
  （动态）与包内 `libzstd.a`。我们自己的目标仍用 libstdc++；
   Filament 公共 API 边界无 `std::` 类型（`Engine::Config` 全 POD），
   双运行时共存实测无问题。

## 验证

- `xvfb-run -a ctest --preset linux`：`smoke` + `demo_runs` +
  `demo_screenshot`（新）。
- `demo_screenshot`：`tests/screenshot_test.py` 跑 demo 渲染 30 帧并截图，
  Pillow 断言 800×600、非全黑、含深蓝清屏色（容差区间）与红色三角
  （>100 像素）。无 `DISPLAY` 时脚本自己套 `xvfb-run`，两种调用方式都过。
- 截图保留在 `build/linux/screenshots/`。
- `nm libtiles_renderer.a | grep -i sdl` 无输出（SDK 零 SDL 回归检查）。

## 后果

- Linux 有了真实渲染通路；P3 可直接在 `Scene` 里挂 cesium-native 瓦片。
- 其他三平台 + WASM 的 swapchain 接线仍是 TODO（P4/P5）。
- `readPixels` 只用于测试/调试，不建议逐帧调用（文档已注明）。
