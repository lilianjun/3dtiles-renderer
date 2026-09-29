# ADR-0014: 确定性相机轨迹回放（P16）

## 背景

项目方案 MVP 要求"确定性相机轨迹回放"和"固定轨迹截图"。
P2–P15 的每个截图测试都是"写死一组 orbit 参数 → 渲染 N 帧 → 断言最后一帧"。
相机一动，截图就不可复现；没有轨迹概念，就没有"同一条相机路径"的回归能力。

## 决策

轨迹播放器住在 **demo 层**（`samples/demo/trajectory.h`），**不进入 SDK 公共 API**。

### 为什么不是 SDK

1. **最小 API 原则**（ADR-0003、ADR-0012）：P12 审计时已明确拒绝 `lookAt` ——
   相机控制是宿主 App 的职责，SDK 只接受"本帧相机是什么"（`setOrbitCamera`）。
   轨迹回放本质上是"脚本化的宿主相机输入"，和鼠标拖拽 orbit 是同一类调用，
   只是数值来自脚本而非手指。把它做进 SDK 等于替宿主做输入，越界。
2. **零新增 API 风险**：`setOrbitCamera` 已有，轨迹播放不需要 SDK 任何改动。
3. **可移植性**：真机上的宿主 App 自己实现轨迹驱动（调同一个 API），
   demo 的实现是参考做法，不是唯一做法。

### 为什么不是纯测试工具（Python）

逐帧相机控制必须发生在渲染循环里 —— Python 侧只能调起 demo 进程，
最终还是要 demo 支持 `--trajectory`。播放器放在 demo 里，
测试（`tests/trajectory_test.py`）和人工/真机 smoke（`--trajectory` + `--frame-dir`）
共用同一份实现，不存在两份插值逻辑分叉的风险。

## 轨迹格式

CSV 关键帧，`frame,yaw,pitch,distance`，`#` 开头为注释行：

```
# frame,yaw,pitch,distance
0,30,18,20
4,90,18,20
8,180,25,14
11,270,18,20
```

- `frame` 是**逻辑帧序号**（第几次 `renderFrame()` 成功），与墙钟无关。
- 关键帧按 frame 升序；播放时按段插值，超出末关键帧后钳制在末帧。
- 只表达 `setOrbitCamera` 已有的参数（yaw/pitch/distance，绕 tileset 局部原点），
  不引入 free-look —— 与 P12 的 API 决策一致。

## 插值：smoothstep（而非线性）

段内参数 `t ∈ [0,1]` 走 `t*t*(3-2*t)`。线性插值在关键帧处速度不连续，
相机"急停急起"；smoothstep 保证关键帧处速度为零，flythrough 观感平滑，
且仍然是纯函数、可复现。yaw 取最短角路径（350°→10° 走 +20° 而非 -340°）。

## 确定性策略

1. **逻辑帧驱动**：轨迹序号只在 `renderFrame()` 成功时推进；
   driver 忙碌导致的重试不计入（与现有 demo 主循环一致）。
2. **预热**：轨迹开始前，用首关键帧相机跑固定 60 帧预热
   （P3 golden 已证明该预算下本地 tiny tileset 的 tile 选择稳定；
   P11 证明此栈上重渲染 bit-identical）。
3. **异步加载隔离**：cesium-native 的 tile I/O 跑在 worker 线程，
   但比较的是"预热完成后同一轨迹的截图"，此时本地 fixture 的所需 tile
   已全部就绪；`trajectory_determinism` 跑两次全量、逐帧 md5，
   任何加载时序泄漏都会被抓住。
4. **无时间相关材质**：fixture 无动画、无 time-based shader，
   Filament 引擎时间不影响像素。

## 明确不做

- 实时墙钟回放 / 输入录制（那是宿主 App 的交互层，不是确定性测试工具）。
- 轨迹驱动 tile 预加载（streaming 策略是 cesium-native 的事）。
- 把轨迹帧冻进 golden 之外的"视频"产物（`--frame-dir` 导出 PNG 序列已够真机 smoke 用）。

## 后果

- `tiles_demo --trajectory <csv> [--frame-dir <dir>] [--warmup N]`。
- `tests/trajectory_test.py` → ctest `trajectory_determinism`：两次运行逐帧 bit-identical。
- golden 新增 `p16_traj_f00/f05/f11`（P3 tileset，12 帧 orbit+push-in 轨迹的首/中/末帧）。
