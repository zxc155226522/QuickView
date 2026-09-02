# Implementation Plan, Task List and Thought in Chinese

## 一、问题原因深度分析

### 1. 核心根因：矢量图（DXF/DWG/SVG/PLT 等）被错误地套用了小位图防模糊限制
在 QuickView 的缩放逻辑中，`ComputeBaseFitScaleForVisual(vs, winW, winH)` 以及多处 `fitScale` 计算中存在以下逻辑：
```cpp
float baseFit = std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);
if (g_runtime.LockWindowSize) {
    if (!g_config.UpscaleSmallImagesWhenLocked && baseFit > 1.0f) {
        baseFit = 1.0f;
    }
} else if (vs.VisualSize.width < 200.0f && vs.VisualSize.height < 200.0f && baseFit > 1.0f) {
    baseFit = 1.0f;
}
```
**问题机制**：
- DXF 和 DWG 等 CAD 文件的几何坐标单位是模型单位（如毫米 mm、米 m、厘米 cm、英寸 in）。
  - 例如机械零件包围盒可能为 `50 x 30`（毫米）或 `2.5 x 1.8`（米）；
  - 标准图纸图框尺寸可能为 `297 x 210`（A4）或 `420 x 297`（A3）。
- 当这些文件经 LibreDWG 转换为 SVG XML 时，SVG 的 `width` / `height` 与 `viewBox` 就是包围盒的模型数值（例如 50x30 或 297x210）。
- 在常规分辨率（如 1920x1080）的窗口中，`winW / vs.VisualSize.width` 会远大于 1.0（例如 `1920 / 50 = 38.4` 或 `1920 / 297 = 6.46`）。
- 因为窗口模式下固定了窗口尺寸（`g_runtime.LockWindowSize = true`，或者尺寸小于 200px），`baseFit` 被强行限制为 `1.0f`！
- 导致 CAD 图纸在打开时只以极小的几十到几百像素显示在窗口正中央，完全无法自适应窗口；用户必须双击窗口触发 `PerformZoomFit` 强行覆盖 `view.Zoom` 才能撑满窗口。
- 对于所有矢量格式（SVG、DXF、DWG、PLT、PDF、AI、CDR、CMX 等），矢量图形在数学上具备任意分辨率无损清晰渲染的能力，不存在位图放大发虚的问题，绝对不应该受到 `baseFit <= 1.0f` 或 `< 200px` 的限制，必须按实际视口计算真实的 `fitScale` 实现完整自适应显示。

### 2. DWF / DWG 格式说明
- 用户在描述中提及 `dwf和dxf`，键盘上 `G` 与 `F` 相邻，实际为 CAD 矢量图对（DWG 与 DXF）。
- QuickView 通过 LibreDWG 统一解析 DWG 和 DXF 并转换为 D2D Native SVG 矢量管线。本次修复将一并彻底解决 DXF 与 DWG（以及所有矢量格式）的窗口自适应问题。

---

## 二、修改计划与待改动文件清单

### 文件清单
1. `QuickView/main.cpp`
   - 在 `ComputeBaseFitScaleForVisual` 中增加对矢量图格式（SVG / DXF / DWG / PLT / resvg / mupdf / pdfium 等）的豁免判断，确保矢量图直接返回视口适配比例 `min(winW / vs.VisualSize.width, winH / vs.VisualSize.height)`，不受 `LockWindowSize` 和 `< 200px` 限制。
   - 检查并统一 `main.cpp` 中所有其他涉及 `200.0f` 与 `fitScale > 1.0f` 的相关计算点（如 `CalculateTargetZoom`、`ClampTotalScale`、`PerformZoomFill` 等），确保对矢量图的一致性。
   - 确保新打开 DXF/DWG/SVG 时，初始状态能够正确按视口全屏适配计算 Surface 尺寸与 DComp 变换矩阵。

---

## 三、每处文件改动核心逻辑

### 1. `QuickView/main.cpp`
- **辅助判断函数**：
  检查当前 Primary Pane 是否为矢量资源（`res.isSvg || res.isResvg || res.isMupdf || res.isPdfium`）。
- **`ComputeBaseFitScaleForVisual` 改造**：
  如果是矢量格式，直接返回未受截断的 `std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height)`；仅对纯位图执行小图防拉伸限制。
- **关联的缩放范围与变换计算**：
  在 `GetCurrentTotalScale`、`ClampTotalScale`、`CalculateTargetZoom` 等处对矢量图做一致性处理，使得无论图纸模型坐标是 0.05 还是 500000，缩放与平移都能以视口自适应为基准正常工作。

---

## 四、修改完成后的预期实现效果

1. **DXF / DWG 打开即自适应**：
   无论 CAD 图纸的坐标范围是毫米级（如 50x30）、米级（如 20x15）还是超大建筑图（如 50000x30000），打开文件后立即自动缩放适配窗口视口居中显示，不再需要手动双击适配。
2. **矢量无损缩放体验保持**：
   在自适应的基础上，用户依然可以自由使用滚轮/拖拽进行无损矢量放大和漫游。
3. **普通位图不受影响**：
   小于 200px 的小型位图图标在固定窗口下依然保持防发虚策略，不影响其他格式的显示体验。

---

## 五、任务拆解 (Task List)

- [ ] 任务 1：修改前 Git 版本备份提交（包含精确时间戳和修改计划）
- [ ] 任务 2：在 `QuickView/main.cpp` 中优化 `ComputeBaseFitScaleForVisual` 及相关缩放函数，免除矢量图的小图限制
- [ ] 任务 3：通过 junction `E:\qv_build_tmp` 进行全量编译构建验证
- [ ] 任务 4：修改后 Git 二次提交留存完整代码快照
- [ ] 任务 5：向用户汇报修复结果与效果
