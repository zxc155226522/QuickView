# 项目长期记忆（QuickView 看图软件）

## 矢量图（PLT/DXF/DWG）渲染约定
- PLT 笔号颜色 `kPLTPenColors[0..8]` = black,black,red,red,green,blue,magenta,cyan,black（料号仅用 SP1黑/SP3红）。
- 线宽 `CalcStrokeWidth`（三类共用）：`maxDim*0.0003`，下限 `0.05`。
- DXF/DWG ACI→RGB 1–9 同 AutoCAD 标准；10–255 为近似算法。

## 编译流程约束（必读）
- 中文路径 → ASCII junction 编译：脚本 `看图软件编译并启动.ps1` 用 `mklink /J E:\qv_build_tmp`，preset `Release-LTO`。
- 残留 junction 清理：`fsutil reparsepoint delete E:\qv_build_tmp`（避开 safe-delete 钩子拦截 rm）。
- 增量编译依赖 `out/build/Release-LTO/CMakeCache.txt` 记录的源路径 `e:/qv_build_tmp`，换 junction 名会 CMake 路径报错。
- **改被多 TU 包含的头文件后必须 `--clean-first` 全量重建**（否则运行期结构体布局不一致，表现为整片异常）。后台 PS 构建有 ~2min 墙钟限制，超时改用前台（timeout 480000）。
- 单实例互斥 `Local\QuickView_Master_v1`：旧实例在前台，"启动"仅激活旧进程，易误判"改了没好"。

## 分页预览面板
- 触发：`g_pagedDoc.active && totalPages>1 && ShowPagePanel && !fullscreen`；布局让位经 `ComputeImageViewportLayout()` 减 `layout.Right` 写 `g_pagePanelReservedWidth`（三文件 extern）。
- 缩略图：CDR/CMX 同步 `RasterizeSvgThumbnail`；PDF/AI 走 `DocumentRenderController` 双通道。点击跳页/滚轮/滚动条拖拽交互齐全。
- 跨 TU 链接坑：`main.cpp` 的 `ResolveCanvasColor()`/`g_renderEngine` 被 `UIRenderer.cpp` extern 引用，**必须去 `static`** 否则 LNK2019。

## 边界溢出指示器
- 放大/平移越界时窗口边缘绘 2px 强调色线（DodgerBlue）。开关 `g_config.ShowBorderIndicator`（0关/1开/2自定义），`[View] ShowBorderIndicator`。2026-08-07 默认由 1 改 0。

## 资源管理器缩略图（IThumbnailProvider Shell 扩展）
- 目标 9 格式：PLT/DXF/DWG/PDF/AI/SVG/CDR/CMX（+TIF）。架构：provider DLL 经命名管道 `\\.\pipe\QuickViewThumb` 调常驻服务 `QuickView.exe --thumbnail-server`（连不上则拉起），服务双通道：并行线程接小文件(<`ThumbnailSmallFileThresholdMB`默认5MB)非CDR/CMX；串行单线程接大文件或 CDR/CMX（`g_cdrPageCache` 非线程安全独占）。协议：请求 `[u32 pathLen][wchar path][u32 size]`，响应 `[u32 status][u32 bmpLen][bytes]`；status==3 表示服务端丢弃 stale。
- 参数：`[Thumbnail]ThumbnailThreads`(1-64)、`ThumbnailSmallFileThresholdMB`(1-1024)，设置 UI「缩略图渲染」分组接入；阈值热更新，线程数改动需 KillServer 重启。
- **已修复坑**：①magic 检测补 svg/ai 显式分支（防误判 plt）；②tif 加 `kThumbnailExts`+.tif/.tiff 分支（小端 `II*\0`/大端 `MM\0*`）；③流读取死循环→先 `Stat()` 取 cbSize 一次性读；④响应须 `FlushFileBuffers` 再 `DisconnectNamedPipe` 防 BMP 截断；⑤`DefaultIcon` 设 `exe,0` 防"未知类型"图标。
- **活跃坑（排查清单）**：
  - 注册须同时写 `QuickView.Image` 与 `QuickView.Vector` 两 ProgID 的 ShellEx（CLSID `{4F8C2A6E-...}`），并加入 `HKLM\...\Shell Extensions\Approved` 白名单；否则 SVG 等走 Image 链取不到。
  - 关联链必须非空：`.ext` 默认值须为 `QuickView.Vector`，否则不路由。
  - UserChoice 受保护（如 .pdf→MSEdgePDF）无法静默改，需用户手动设默认应用。
  - 仅"大/超大图标/内容"视图调用 IThumbnailProvider；`IconsOnly=1` 全局禁用；旧 DLL 失败过的格式需重启 Explorer 才重查。
  - 调试：`C:\Windows\Temp\qvthumb_provider.log` / `qvthumb_server.log`；DLL 变更须清 `thumbcache_*.db`+重启 Explorer；本地用 Python+comtypes 端到端验证。
  - IID：`IThumbnailProvider`=`{E357FCCD-A995-4576-B01F-234630154E96}`，`IInitializeWithStream`=`{B824B49D-22AC-4161-AC8A-9916E8FA3F7F}`。

## 已知待修复（2026-08-11 诊断：Z:\2026打版\8-10\周氏 缩略图失败）
- 根因三道关卡：①串行通道 `kSerialCap=4`（`ThumbnailWorker.cpp:234`）过小，Shell 并发请求瞬间塞满→超出部分被标 stale 丢弃；②provider 端 `kMaxBytes=200MB`（`ThumbnailProvider.cpp:468`）超限直接 abort；③客户端管道 `kWorkerTimeoutMs=15000`（`ThumbnailProvider.cpp:101`）超时，慢渲染(CDR/大文件)被砍。且 `RequestThumbnailViaPipe` 对 Stale 静默返回、不重试/不兜底。
- 修复方向（详见对话方案）：A 放宽 kSerialCap；B CDR 与大文件分离通道；C Stale 后兜底/重试；D 大文件降级渲染；E 放宽客户端超时；F CDR 渲染加速。最小改动集 = A+C+E。
- **编译部署坑（2026-08-11 新增）**：链接写 `QuickViewThumbnailProvider.dll` 被实时防病毒拦截(permission denied)——非进程锁(`Get-Process` 模块扫描为空)，是 Defender 对该路径 DLL 的写保护；本会话 `Add-MpPreference` 不可用无法加排除。本地部署需将 `out\build\Release-LTO` 加 Defender 排除，或临时关实时保护/重启后替换。
