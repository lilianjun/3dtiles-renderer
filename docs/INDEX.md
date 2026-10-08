# 3D Tiles Renderer — 文档总索引

> 本文件是项目文档的入口。2026-10-08 创建，解决"长上下文遗忘"问题。

## 快速导航

| 我要… | 看这里 |
|-------|--------|
| 了解项目是什么 | `README.md` |
| 构建项目 | [构建](#构建) |
| 跑测试 | [测试](#测试) |
| 做 cesium.js 对比 | [一致性测试](#一致性测试-p37) |
| 嵌入 SDK 到 App | `docs/integration.md` |
| 查架构决策 | `docs/adr/`（35 个 ADR） |
| 查 API | `docs/ref/` |

## 构建

**Presets** (`CMakePresets.json`):
- `linux` — 主开发
- `linux-asan` — sanitizer
- `windows` — MSVC
- `android` — NDK r27d
- `ios`
- `windows-mingw` — MinGW 交叉编译门禁（仅 SDK）

**命令**:
```bash
source ~/toolchains/env.sh
cmake --preset linux && cmake --build --preset linux
```

**Linux 运行需要**:
```bash
export LD_LIBRARY_PATH=/home/hatch/toolchains/llvm/lib/x86_64-unknown-linux-gnu:$LD_LIBRARY_PATH
```

**CI** (`.github/workflows/ci.yml`): 5 jobs — Linux / Sanitizers / Windows / Android / iOS

## 测试

```bash
ctest --preset linux
```

- Python 驱动测试 (`*_test.py`) + C++ 测试
- Golden 截图在 `tests/golden/`
- Sanitizer: `tests/run_sanitized.py`

**测试数据生成器** (`tests/data/gen_p*.py`, 14 个):
| 脚本 | 生成内容 |
|------|----------|
| gen_p3.py | 基础 tileset |
| gen_p5.py | glTF 管线 |
| gen_p7.py | i3dm 实例化 |
| gen_p8.py | pnts 点云 |
| gen_p9.py | cmpt 复合 |
| gen_p15.py | PBR 材质 |
| gen_p25.py | KTX2 纹理 |
| gen_p29.py | Draco 压缩 |

**Demo CLI** (`samples/demo/main.cpp`, ~40 flags):
- `--tileset`, `--screenshot`, `--camera-eye/target/up`, `--fov`
- `--background "r,g,b,a"` — **0-1 范围，不是 0-255**
- `--frames`, `--settle-before-screenshot`, `--no-ibl`, `--trajectory`

## 一致性测试 (P37)

**目标**: 与 cesium.js 渲染一致，SSIM≥0.95

**脚本**: `tests/data/benchmarks/conformance_test.py`
```bash
python3 tests/data/benchmarks/conformance_test.py \
  --benchmark <dir> --tileset <tileset.json> \
  --demo ./build/linux/tiles_demo --out /tmp/out
```

**Benchmark 结构**: `tests/data/benchmarks/captured/<name>/`
- `render.png` — cesium.js 参考图
- `params.json` — 相机/背景参数

**参数对齐规范**: `docs/plan/P37-C-param-alignment.md`

**点云测试方法**: `docs/p37-pointcloud-testing.md`（2026-10-08）
- 核心规则：参数从 Cesium 渲染后提取、相机自动构图不许猜、Web 图来自 canvas
- 采集流程已端到端实测：`batch_capture.js`（自动构图 + params.json 导出）
- 作废数字清单（违反规则的旧 SSIM 结论不再引用）

## ADR 索引（35 个）

### 基础架构 (P0-P6)
- 0001 tech-stack — Cesium Native + Filament + SDL3
- 0002 toolchain — 工具链版本锁定
- 0003 sdk-no-sdl — SDK 零 SDL 依赖
- 0004 filament-render-readback — 真实渲染 + 无头截图验证
- 0005 cesium-gltfio-pipeline — 加载渲染管线
- 0006 platform-backends — 四平台后端
- 0007 sanitizer-memory-robustness — 内存健壮性

### 数据格式 (P7-P11)
- 0008 i3dm-instancing — i3dm CPU 实例展开
- 0009 pnts-point-cloud — pnts 原生 POINTS
- 0010 cmpt-composite — cmpt 递归转换
- 0011 tiles-1.1 — 3D Tiles 1.1 支持

### API 演进 (P12-P36)
- 0012 public-api-p12 — resize, lastError
- 0013 pbr-materials — PBR 材质管线
- 0014 trajectory-replay — 相机轨迹回放
- 0015 tile-stats — Tile 统计 API
- 0016 weak-network-testing — 弱网测试
- 0017 memory-bounded-roaming — 内存有界漫游
- 0018 frustum-lod-verification — 视锥 LOD 验证
- 0019 add-region-verification — ADD 细化语义
- 0020 tileset-switch-safety — Tileset 切换安全
- 0022 tileset-load-failfast — 损坏 tileset fail-fast
- 0023 remaining-gaps-roadmap — 剩余缺口路线图
- 0024 ktx2-texture-support — KTX2 纹理
- 0025 image-based-lighting — IBL
- 0026 i3dm-gpu-instancing-boundary — GPU 实例化边界
- 0027 cmpt-crash-guard — cmpt 空模型 SIGSEGV 防御
- 0028 draco-already-supported — Draco 已支持
- 0029 settle-gated-screenshots — settle 门控截图
- 0030 tileset-options-p31 — tileset options 对齐
- 0031 tileset-events-p32 — tile 事件对齐
- 0032 tileset-display-p33 — 显示控制对齐
- 0033 tileset-cache-p34 — 缓存统计对齐
- 0034 tileset-debug-p35 — 调试开关对齐
- 0035 inspector-panel-p36 — Inspector 面板

## 核心规则（li 确立）

1. **cesium.js 是标准答案** — 先读它的源码/spec，不从文件名猜
2. **只收实测结果** — 未经实测的分析结论拒收
3. **无法复现的采集数据 = 错误数据** — 不参与任何结论
4. **动手前先抄代码** — 广泛使用的常规问题直接抄现成实现
5. **常规事项自主决定** — 不问"要不要继续""该怎么做"
6. **向上游发言先给草稿** — li 点头后才能发
7. **外部 agent 分支隔离** — 只 commit 不 push，Muse review/验收/merge
8. **Web 相机不许手写/猜** — 自动构图（viewBoundingSphere = zoomTo 位姿），参数渲染后提取

## 分支状态

- `master` — 主线，P0-P36 完成
- `tilesetio` — P37 进行中（cesium.js 对齐）
- `agent/pointcloud-ssim` — 点云实验分支（已停工，未 merge）

## 待补文档（已知缺口）

1. Harness 脚本使用文档（`compare_ssim.py`）
2. Demo CLI 完整 flags 文档
3. `gen_p*.py` 索引（phase→脚本→输出）
4. ~~Benchmark 采集可复现流程~~ — 已实测解决（2026-10-08）：
   `tests/data/cesiumjs/harness/batch_capture.js`，自动构图 + params.json
   导出，2 fixture（原点点云 + ECEF 龙）一次通过、坏条目正常失败汇总
5. `src/tilesetio/` 模块文档
