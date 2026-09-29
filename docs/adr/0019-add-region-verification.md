# ADR-0019: ADD 精化语义与 region 包围体验证（P21）

日期：2026-09-29
状态：已验证（Linux/Mesa）

## 背景

P20 严格验证了 REPLACE 精化，但 ADD 只留了边界说明；且全部 fixture 的
bounding volume 只用过 `box`，3D Tiles 1.0 的 **`region`**（地理经纬度
矩形，单位弧度）处于"声称支持但零证据"状态。P21 补这两个缺口，复用
P20 的口径（`tileStats()` + `selectedTileIds()` + `--print-selected`）。

## ADD vs REPLACE 对比观测

`tests/data/p21_add_tileset/`（`gen_p21_add_tileset.py`）：与 P20 的
`p20_lod_tileset` 完全相同的几何（root 100×100×10 ge=20 + 4 子 tile
50×50×10 ge=0），唯一差别 `refine: "ADD"`。同一条轨迹
（`p20_lod.csv`：FAR 1200m → NEAR 300m → FAR 1200m）：

| 阶段 | ADD selected（去 wrapper 空 ID 后） | REPLACE selected（P20） |
|---|---|---|
| FAR | `{root.glb}` | `{root.glb}` |
| NEAR | `{root.glb, child_0..3.glb}`（**root 在场**） | `{child_0..3.glb}`（root 不在场） |
| FAR2 | `{root.glb}`（回落） | `{root.glb}`（回落） |

loaded：2 → 6 → 6（单调增后缓存命中），与 REPLACE 一致。

这是 ADD/REPLACE 语义差异的回归门禁：同一个几何、同一条轨迹，
NEAR 阶段的选中集合是否包含 root 就是两种语义的分水岭。
测试断言写成集合相等（不是只看数量），REPLACE 的行为由 P20 的
`frustum_lod` 锁定，ADD 的由 P21 的 `add_region` 锁定。

附带确认 ADR-0018 的发现 2：ADD 下空 ID wrapper tile 确实出现在
`tilesToRenderThisFrame` 中（`ids=,root.glb`），REPLACE 下不出现——
wrapper 的 refine 继承 JSON root，两套语义下行为一致，无例外。

## region 包围体

`tests/data/p21_region_tileset/`（`gen_p21_region_tileset.py`）：

- 位置：lat=0、lon=0（赤道/本初子午线），WGS84 下 ECEF 恰为
  `(6378137, 0, 0)`，手算可验。
- root：region `[-h,-h,+h,+h]`（h=0.0005° 弧度，约 111m 见方），
  ge=20，refine=ADD，`transform` 为平移 `(6378137,0,0)` 的列主序 4x4。
- 2 个子 tile：region 的西/东两半，ge=0。
- 内容：本地原点附近的小盒子（root 50×50×20，子 tile 25×50×20）；
  tile transform 把它们搬到 ECEF 量级。

SDK 零改动走通，路径全是既有代码：

1. `computeLocalOrigin` 的 `BoundingRegion` 分支（P5 已有）取 region
   OBB 中心 → 本地原点 `(6.37814e6, 0, 0)`，demo 日志实证。
2. 渲染变换 `tileTransform * translate(rtcCenter) - localOrigin`
   把内容拉回原点附近；orbit 相机目标即本地原点。
3. cesium-native 的 traversal 对 region 做视锥剔除与 SSE 精化，
   与 box 无异：NEAR（300m）时 `selected={root,child_0,child_1}`，
   `loaded=4`（wrapper+root+2 children），`failed=0`。

截图（400×300，NEAR 稳态帧）与 `--no-tileset` 同轨迹参考图对比：
5382 像素不同（4.5%），盒子清晰可见；3 遍 md5 bit-identical。

## 验证方法

`tests/add_region_test.py`（ctest `add_region`，~3s；3 遍 PASS）：

- **ADD**：FAR/NEAR/FAR2 三阶段稳态窗口，selected 集合相等断言
  （上表），loaded 单调 2→6、FAR2 缓存 6。
- **region**：NEAR 稳态 `loaded==4`、`failed==0`、
  `selected=={root.glb,child_0.glb,child_1.glb}`；截图 vs 空参考
  >1000 像素不同（实测 5382）。
- 解析逻辑从 `frustum_lod_test.py` import（`parse_output`、
  `run_demo`），不复制。
- `sanitizer_add_region`：ASan/LSan/UBSan 门禁（缩短运行：ADD 查
  NEAR 核心集合，region 查 loaded/failed；截图只在普通测试做）。

## 实测结果（3 遍一致）

- ADD：FAR `{root.glb}`/loaded=2 → NEAR `{root.glb + 4 children}`/
  loaded=6 → FAR2 `{root.glb}`/loaded=6；failed=0。
- region：NEAR `{root.glb,child_0.glb,child_1.glb}`/loaded=4；
  failed=0；截图差 5382 像素。

## 诚实边界

- 结论仅适用于 Linux/Mesa；不断言移动平台行为。
- region fixture 选在 lat=0/lon=0 是为了手算可验 ECEF；真实地理
  tileset 的 region 精度/曲率行为未超出本 fixture 的覆盖。
- region 只验证了"加载、选中、画出、稳定"；region 参与视锥剔除的
  per-tile 集合翻转（如 P20 对 box 做的）未单独做——region 的剔除
  路径与 box 共用 cesium-native 的 traversal。
- `selectedTileIds()` 字符串格式仍是 cesium 实现细节（ADR-0018）。
