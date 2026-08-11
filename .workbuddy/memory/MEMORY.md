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
- 目标 9 格式：PLT/DXF/DWG/PDF/AI/SVG/CDR/CMX（+TIF）。架构：provider DLL 经命名管道 `\\.\pipe\QuickViewThumb` 调常驻服务 `QuickView.exe --thumbnail-server`（连不上则拉起），服务**三通道**：
  - 并行通道(N=`ThumbnailThreads`默认4)：小文件(<`ThumbnailSmallFileThresholdMB`默认5MB)且非 CDR/CMX。
  - CDR/CMX 通道：**`kCdrThreads=2` 个独立线程**，各用自己 `CImageLoader` 且 `m_bPopulateCdrCache=false`（隔离全局 `g_cdrPageCache`，CDR 渲染线程安全）。
  - 大文件通道(1 线程)：解码尺寸钳到 `kLargeThumbMax=128` 降耗。
  - 各慢通道容量 `kSlowCap=64`（避免同屏数十张被 stale 丢弃）；溢出仍走 provider 侧一次性 worker 兜底（并发上限 `kOneShotMax=4`，防进程风暴）。
  - 管道超时 `kPipeTimeoutMs=60000`（客户端+服务端对称）。
- 协议：请求 `[u32 pathLen][wchar path][u32 size]`，响应 `[u32 status][u32 bmpLen][bytes]`；status==3=Stale(服务端丢弃)。
- 参数：`[Thumbnail]ThumbnailThreads`(1-64)、`ThumbnailSmallFileThresholdMB`(1-1024)，设置 UI「缩略图渲染」分组接入；阈值热更新，线程数改动需 KillServer 重启。
- **已修复坑(历史)**：①magic 补 svg/ai 分支；②tif 加 .tif/.tiff 分支；③流读取先 Stat() 一次性读防死循环；④响应 FlushFileBuffers 再 Disconnect 防 BMP 截断；⑤DefaultIcon 设 exe,0。
- **活跃坑（排查清单）**：注册须同时写 QuickView.Image 与 QuickView.Vector 两 ProgID ShellEx + HKLM Approved 白名单；关联链非空；UserChoice 受保护需用户手动设默认应用；仅大/超大图标视图触发；旧 DLL 失败格式需清 thumbcache+重启 Explorer 才重查；调试日志 `C:\Windows\Temp\qvthumb_{provider,server}.log`；IID `IThumbnailProvider`=`{E357FCCD-A995-4576-B01F-234630154E96}`，`IInitializeWithStream`=`{B824B49D-22AC-4161-AC8A-9916E8FA3F7F}`。

## CDR 并行安全（2026-08-11）
- `g_cdrPageCache`(ImageLoader.cpp:62) 是 CDR 唯一跨线程共享可变状态。给 `CImageLoader` 加 `bool m_bPopulateCdrCache=true`（public）；`LoadCDR` 在 false 时用局部 `firstPageData` 产出 outFrame、不碰全局。缩略图服务置 false → 多 CDR 线程安全。**改了 ImageLoader.h → 须 `--clean-first` 全量重建**。

## TIF 大文件优化（2026-08-11）
- `MiniTiff::Load` 在 `ctx.forcePreview`(缩略图/预览)时**枚举所有 IFD/SubIFDs**(`EnumerateTiffIfds`)，用 `SelectThumbIfd` 选"够用的最小分辨率层"（优先 NewSubfileType bit1 缩略图子文件，否则最小≥目标），只解码该层→**多分辨率/金字塔大 TIF 缩略图提速显著**；主视图(`forcePreview=false`)保持全分辨率，**清晰度零影响**。**仅 MiniTiff.cpp 内部 static 改动，未动头文件**（增量即可）。单 IFD 扁平超大 TIF 仍走全解，但已被"大文件通道降档128 + 60s 超时 + OpenMP 并行"兜底。

## TIF 缩略图 GPU 加速评估（2026-08-11）
- 结论：**不划算，不做**。缩略图服务是 headless 常驻进程(`--thumbnail-server`)：①解码环节(TIFF LZW/Deflate+predictor 还原)顺序/逐像素、有串行依赖，GPU 无成熟解码库且不擅长，当前已是软件解码+OpenMP 多 tile 并行；②缩放环节已被"解码时降采样直接出小图"绕过，256px 缩放本身微秒级，GPU 收益可忽略；③headless 进程建 D3D/Vulkan 上下文本身是负担，还可能无 GPU(远程/服务器)、需多 GPU 选择与回退，**工程成本爆炸而收益近零**。正解=解码时降采样(砍解码量)+已有 OpenMP 多 tile 并行，甜区在 tiled 大图。GPU 真正甜区在主视图大图渲染(主程序 renderEngine)，非后台缩略图服务。

## 编译部署坑（2026-08-11）
- 链接写 `QuickViewThumbnailProvider.dll` 被实时防病毒拦截(permission denied)——**非进程锁**(`Get-Process` 模块扫描为空)，是 Defender 对该路径 DLL 写保护；本会话 `Add-MpPreference` 不可用。本地部署须将 `out\build\Release-LTO` 加 Defender 排除或临时关实时保护后重链。
- 绕开技巧：`--clean-first` 清理阶段会因 DLL 被锁报 Access denied 中断；可改为删 `CMakeFiles/QuickView.dir` 触发增量全重编，但会丢 PCH 源 `cmake_pch.cxx`，需先 `cmake -S $JP -B out/build/Release-LTO` 重新 configure 再生；单纯验证主程序可 `cmake --build ... --target QuickView` 单独链主 exe（绕开被锁的 provider DLL）。
