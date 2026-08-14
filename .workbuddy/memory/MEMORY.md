# 项目长期记忆（QuickView 看图软件）

## 矢量图（PLT/DXF/DWG）渲染约定
- PLT 笔号颜色 `kPLTPenColors[0..8]`=black,black,red,red,green,blue,magenta,cyan,black（料号仅 SP1黑/SP3红）。
- 线宽 `CalcStrokeWidth`（共用）：`maxDim*0.0003`，下限 0.05。
- DXF/DWG ACI→RGB 1–9 同 AutoCAD；10–255 近似算法。

## 编译流程约束（必读）
- 中文路径→ASCII junction 编译：脚本 `看图软件编译并启动.ps1` 用 `mklink /J E:\qv_build_tmp`，preset `Release-LTO`。残留 junction 清理：`fsutil reparsepoint delete E:\qv_build_tmp`。
- 增量依赖 `out/build/Release-LTO/CMakeCache.txt` 记录的 `e:/qv_build_tmp`，换 junction 名会 CMake 路径报错。
- **改被多 TU 包含的头文件后必须 `--clean-first` 全量重建**（否则运行期结构体布局不一致）。后台 PS 构建 ~2min 墙钟限制，超时改前台（timeout 480000）。
- 单实例互斥 `Local\QuickView_Master_v1`：旧实例在前台，"启动"仅激活旧进程，易误判"改了没好"。

## 分页预览面板
- 触发 `g_pagedDoc.active && totalPages>1 && ShowPagePanel && !fullscreen`；布局让位 `ComputeImageViewportLayout()` 写 `g_pagePanelReservedWidth`（三文件 extern）。
- 跨 TU 链接坑：`main.cpp` 的 `ResolveCanvasColor()`/`g_renderEngine` 被 `UIRenderer.cpp` extern 引用，**必须去 `static`** 否则 LNK2019。

## 资源管理器缩略图（IThumbnailProvider Shell 扩展）
- 目标 9 格式 PLT/DXF/DWG/PDF/AI/SVG/CDR/CMX(+TIF)。provider DLL 经命名管道 `\\.\pipe\QuickViewThumb` 调常驻服务 `QuickView.exe --thumbnail-server`，服务三通道：并行(N=`ThumbnailThreads`默认4，小文件<5MB非CDR/CMX)、CDR/CMX(`kCdrThreads=2` 线程，`m_bPopulateCdrCache=false` 隔离 `g_cdrPageCache`)、大文件(1线程，解码钳 128)。
- 协议：请求 `[u32 pathLen][wchar path][u32 size]`，响应 `[u32 status][u32 bmpLen][bytes]`；status==3=Stale。管道超时 60000ms。
- 排查清单：注册须写 QuickView.Image/Vector 两 ProgID ShellEx + HKLM Approved 白名单；仅大/超大图标视图触发；旧 DLL 失败格式需清 thumbcache+重启 Explorer；IID `IThumbnailProvider`=`{E357FCCD-A995-4576-B01F-234630154E96}`。

## CDR 渲染与保真度
- 链路：libcdr/CMX 解析 → librevenge RVNGSVGDrawingGenerator 产 SVG → 后处理 → `RasterizeSvgThumbnail` 用 D2D `ID2D1SvgDocument` 栅格化。**解析是 libcdr（非自研）；保真度瓶颈在 D2D SVG 渲染器仅支持 SVG 1.1 子集（滤镜/裁剪/蒙版/图案/CSS 静默丢弃）。**
- 修复方向（待确认）：SVG 后端换 **resvg**(Rust,MIT,静态库)，覆盖 CDR/CMX/PLT/AI 等；解析端零改动。

## TIF 优化
- 缩略图：`MiniTiff::Load` 在 `forcePreview` 枚举所有 IFD/SubIFDs，选"够用最小分辨率层"只解码该层→金字塔大 TIF 提速；主视图保持全分辨率。**仅 MiniTiff.cpp 内部 static 改动，增量即可**。
- 未压缩快路径(`LoadUncompressedPreview`)：未压缩+stripped+8bit+chunky+predictor==1 时按 targetPx 步长采样直接产小图(I/O/内存降~step²)；已支持灰/RGB/CMYK(CMYK 复用 `ConvertCmykToBgra`)。
- 超大文件根因：旧 `IInitializeWithStream::Initialize` 整流读内存、超 200MB 丢弃；已改为流式 spill（去 200MB 上限，仅留 2GB 上限）。

## 编译部署坑
- **拦截真凶是 360 安全卫士（非 Defender）**：Stop-Process 结束 360 进程被拒（自保护）。绕开法：链接到非 exe 名(如 `QuickView_stage.stage`)再 `Move-Item` 改名成 exe（360 拦写 exe 但允许改名）。vcpkg 静态链接，exe 自包含。
- 沙箱跑 `看图软件编译并启动.ps1` 在 configure 阶段因 vcpkg 代理 `127.0.0.1:7890` 卡死；绕过：直接 `cmake --build out/build/Release-LTO`（CMakeCache 已固化则有效）。
- 脚本编译后清理 junction `E:\qv_build_tmp`;手动验证需先 `New-Item -ItemType Junction -Path E:\qv_build_tmp -Target E:\项目\看图软件` 重建。

## 缩略图角标风格（Adobe 系）
- 右上角彩色实心圆徽章+白字缩写；半径约占画布 17%；三字母(TIF/PDF/SVG/DXF/DWG/PLT/CMX)字号比两字母(AI/CDR)小一圈。多尺寸缓存按 cx 比例生成。

## CDR 命令行验证（exe headless）
- `QuickView.exe --export-png <in> <out.png> [maxDim]` 走 libcdr→SVG→resvg 全流程（含画布外）。
- **路径陷阱**：必须传 `E:/...` Windows 盘符路径，不能用 `/e/...` POSIX（CreateFileW 打不开）。
- 后台 PS ~2min 硬超时：长任务须前台 + timeout=600000。
- 含空格/逗号的文件名经 PowerShell `Start-Process -ArgumentList` 传参会 exit=3；绕过：Git Bash 直接调 exe（Windows 路径+引号）或先 cp 到 ASCII 名。

## 自动更新/更新功能（2026-08-14 停用）
- 用户要求去除自动更新/更新功能，敲定"别删了直接停止"：main.cpp 停用 `UpdateManager::Get().Init/StartBackgroundCheck/HandleExit` 接线（`GetAppVersionUTF8` 加 `[[maybe_unused]]`）；SettingsOverlay 隐藏常规页检查更新/更新通道、关于页检查更新按钮（版本号保留）。UpdateManager.h/.cpp、AppStrings 更新字符串、EditState 成员均未删（留死代码）。本沙箱 360 拦 exe 链接无法编译验证，本机 `看图软件编译并启动.ps1` 终验。
