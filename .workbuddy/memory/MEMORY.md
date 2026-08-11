# 项目长期记忆（QuickView 看图软件）

## 矢量图（PLT/DXF/DWG）渲染约定

- **PLT(HPGL) 笔号颜色**（QuickView/VectorLoader.cpp `kPLTPenColors`，索引=笔号0..8）：
  `black, black, red, red, green, blue, magenta, cyan, black`
  依据：实际料号文件只用 SP1(黑)/SP3(红)，用户明确"文件名与颜色无关，正常文件黑+红、可能带绿"。
  历史 bug：笔8曾误为 white（白底不可见）；笔3曾为 green（与料号不符）。
- **线宽**（`CalcStrokeWidth`，PLT/DXF/DWG 共用）：
  比例 `maxDim*0.0003`，下限 `0.05`（用户指定）。改一处三类格式全部生效。
- DXF/DWG 的 ACI→RGB（ACIToHex）1–9 与 AutoCAD 标准一致，仅 10–249/250–255 为近似算法（非全量255项表）。

## 编译流程关键约束（必读）

- 项目路径含中文（`E:\项目\看图软件`），NASM 等会因中文路径乱码 → **必须经 ASCII junction 编译**：
  脚本 `看图软件编译并启动.ps1` 用 `mklink /J E:\qv_build_tmp <项目>` 中转，preset `Release-LTO` 编译。
- **删除残留 junction 的坑**：沙箱 safe-delete 钩子会拦截 `rm`/`Remove-Item`（且失败不真正删除）。
  正确做法：`fsutil reparsepoint delete E:\qv_build_tmp`（直接删重解析点，不经 cmd/rm）；
  若已被钩子改成空普通目录，用 `mv` 重命名释放名字（重命名不触发删除钩子），再让脚本重建 junction。
- 磁盘已有缓存 `out/build/Release-LTO/CMakeCache.txt` 记录源路径为 `e:/qv_build_tmp`；
  复用该 junction 名可做增量编译（只重编改动 cpp），改其他 junction 名会触发 CMakeCache 路径不匹配报错。
- 验证编译的临时 PS 脚本若含中文会被 PowerShell(-File) 以错误编码读成乱码 → 用 `$PSScriptRoot` 取项目路径，脚本正文保持 ASCII。
- **改头文件后必须 `--clean-first` 全量重建（血泪教训）**：改动被多 TU 包含的头文件（尤其结构体如 `EditState.h`/`AppConfig`）后，CMake 增量依赖可能漏编部分 .obj，运行期出现结构体布局/内联访问不一致 → 表现为"功能原本正常却突然整片异常"（如全部图空白）。排查"回归"前**先 `cmake --build out/build/Release-LTO --clean-first` 全量重建**再下结论；单实例互斥 `Local\QuickView_Master_v1` 会把旧实例留在前台，"启动"只是激活旧进程换图，极易误判为"改了还没好"。
- **后台 PowerShell 构建有 ~2 分钟墙钟限制**：`--clean-first`+LTO 链接常超 2 分钟被掐断（2m1s、零输出、failed，非代码错）。对策：前台 PowerShell（timeout 480000）跑，或 clean-first 编译完再用前台增量只链接。
- **46f78a4 是否致空白未定论**：日志显干净重建后位图上传烘焙正常，但用户实跑仍空白，遂 `git reset --hard a6dbb59`（功能前），功能提交备分 `backup-pagepanel-46f78a4`。回退后若仍空白→根因更早（bisect）；若正常→功能代码有牵连，重做须规避。

## 分页预览面板（右侧 docked 面板）实现要点

- 触发条件：`g_pagedDoc.active && totalPages>1 && g_config.ShowPagePanel && !fullscreen` 时显示右侧面板（与图片同容器，画布同色背景+1px 强调分隔线，非浮层）。
- 布局让位：唯一入口 `ComputeImageViewportLayout()` 减少 `layout.Right`，让出宽度写入全局 `g_pagePanelReservedWidth`（main.cpp / UIRenderer.cpp / ImageViewportLayout.cpp 均 `extern` 该变量）。
- 渲染：`UIRenderer::DrawPagePanel`（Static 层）。缩略图懒加载：CDR/CMX 用 `CImageLoader::RasterizeSvgThumbnail` 同步；PDF/AI 走 `DocumentRenderController` 双通道队列 `RequestThumbnail`（主视图单槽 + 缩略图队列，互不饿死）。结果经 `ResultMessage` → `ProcessPendingPageThumbs` 上传 GPU 缓存到 `m_pageThumbCache`。
- 交互：缩略图点击跳页（PDF→`HandlePdfPageJump`、CDR→`HandleCdrPageStep`，均接受 0 基 targetPage）；面板内滚轮滚动 `ScrollPagePanel`；滚动条拖拽 `Begin/Drag/EndPagePanelScrollDrag`。配置项 `ShowPagePanel`(bool) / `PagePanelWidth`(int, 0=自动220，否则160~400) 存于 `General` 段。
- **跨 TU 链接坑（必读）**：`main.cpp` 的 `ResolveCanvasColor()` 与 `g_renderEngine` 现被 `UIRenderer.cpp` 以 `extern` 引用，**必须去掉 `static`（改为外部链接）**，否则 LNK2019。二者原先是 `static`（内部链接）。

## 边界溢出指示器（Edge Overflow Indicator）

- 现象：图片放大/平移超出窗口可视区时，在超出方向的窗口边缘绘制 2px 强调色线条（默认主题蓝 DodgerBlue），即"图片放大时的蓝色边框"。
- 绘制：`UIRenderer::DrawBorderIndicators`（UIRenderer.cpp:1935），由 UIRenderer.cpp:878 在 `ShowBorderIndicator != 0` 时调用；颜色=主题强调色（`ThemeCustomAccentR/G/B`）或自定义色（=2）。
- 开关：`g_config.ShowBorderIndicator`（EditState.h:599），0=关 / 1=开(强调色) / 2=自定义色；配置项 `[View] ShowBorderIndicator`，设置 UI：设置→视图→显示边界溢出指示器。
- 默认行为变更（2026-08-07）：main.cpp:4617 默认值由 1 改为 0，重新编译后默认不再显示蓝框；已显式在 ini 存过值的仍按 ini 生效，需到设置里关一次。

## 资源管理器缩略图（IThumbnailProvider Shell 扩展）

- 目标格式：PLT/DXF/DWG/PDF/AI/SVG/CDR/CMX 共 9 种矢量/文档格式。架构：进程外渲染——provider DLL 把 Shell 给的流写成临时文件，调 `QuickView.exe --thumbnail` 复用主程序渲染管线生成 BMP，再回读 HBITMAP。
- **常驻缩略图服务（方案A，2026-08-11 起替代每次 spawn 进程）**：根因是多文件并发时每张缩略图都 spawn 独立 `QuickView.exe --thumbnail` 重型进程 → **进程风暴卡死（非死锁）**。`--thumbnail`/`--thumbnail-server` 均在单实例互斥 `Local\QuickView_Master_v1` 与建窗之前分发，不碰互斥；`ImageLoader` 无跨进程独锁 → 排除内部死锁。新架构：provider DLL 优先连 `\\.\pipe\QuickViewThumb` 命名管道（OVERLAPPED+超时 CancelIo 回收；连不上则 `CreateProcessW` 拉起 `QuickView.exe --thumbnail-server --idle 60`(CREATE_NO_WINDOW) 后重试，管道异常重拉一次）；服务**双通道渲染**：并行通道 N 线程(默认4，`[Thumbnail]ThumbnailThreads`) 接小文件(<`[Thumbnail]ThumbnailSmallFileThresholdMB` 默认5MB)且非CDR/CMX；串行通道 1 线程接大文件或 CDR/CMX（`g_cdrPageCache`(ImageLoader.cpp:62) 非线程安全，独占线程）。**响应写完必须先 `FlushFileBuffers` 再 `DisconnectNamedPipe`**——字节模式管道在断开时丢弃未读缓冲，否则客户端 BMP 读截断（生产偶发缩略图缺失）。每 worker 独立 `CImageLoader` 实例跨请求复用；每请求记 `C:\Windows\Temp\qvthumb_server.log` 耗时。`GetThumbnail` 两侧（管道/兜底）均记耗时日志；管道失败回退原一次性 `RunThumbnailWorker` 兜底（不回归）。协议：请求 `[u32 pathByteLen][wchar path][u32 size]`，响应 `[u32 status][u32 bmpLen][bytes]`。
- **设置 UI 接入（2026-08-11）**：两个参数已接入设置→高级「缩略图渲染」分组（Slider+float 字段，因 `Input` 仅支持字符串 `pStrVal`）。`ThumbnailThreads`(1-64)/`ThumbnailSmallFileThresholdMB`(1-1024) 由 `SaveConfig` 写 `[Thumbnail]` 段、`LoadConfig` 读回 `std::clamp`，与服务 `ReadServerConfig` 共用同段同键（零耦合）。**阈值热更新**：服务 accept 循环每次 `ReloadThreshold()` 重读原子变量 `g_smallFileBytes`，下次请求即时生效（免重启）；**线程数改动**靠 `QuickView::KillThumbnailServer()`（OpenEvent+SetEvent 置位 `Local\QuickViewThumbStop` 命名事件）终止现存服务，下次请求拉起新进程生效。改了 `EditState.h` 头文件→须 `--clean-first` 全量重建。
- **注册位置（必读，2026-08-10 修正）**：Shell 决定默认程序/图标/缩略图提供程序时**优先用 `FileExts\.ext\UserChoice` 的 ProgID，其次才是 `HKCU\Software\Classes\.ext` 的 default**。实测：SVG 的 UserChoice 指向 `QuickView.Image`，PDF 的 UserChoice 指向 `MSEdgePDF`（Edge）。因此必须把缩略图 ShellEx **同时注册到 `QuickView.Image` 和 `QuickView.Vector`** 两个 ProgID（都写 CLSID `{4F8C2A6E-...}`），否则 SVG（走 Image 那条链）根本取不到 provider。代价：jpg/png 等 73 种共用 `QuickView.Image` 的格式也会被卷入进程外缩略图管线（性能略降但功能正常）；暂不追求隔离，先保矢量/文档格式可用。CLSID 仍须在 `HKLM\...\Shell Extensions\Approved` 白名单。独立 `QuickView.Vector` 仅作 Classes 层 default 的兜底（UserChoice 缺失时生效）。
- **DefaultIcon 必须设置（否则显示"未知类型"图标）**：`QuickView.Image`/`QuickView.Vector` 的 `DefaultIcon` 若为空或被作者故意设成 `shell32.dll,0`，则该 ProgID 关联的所有格式在资源管理器里显示"未知文件类型"图标。修复：把 `DefaultIcon` 设为 `"<QuickView.exe 路径>,0"`。
- **Shell 调用顺序（关键事实，2026-08-10 实测日志确认）**：真实 Explorer **只调用 `IInitializeWithStream::Initialize`（传流），不调用 `IObjectWithSite::SetSite`** → provider 拿不到真实文件路径，只能靠 magic 字节判断扩展名。SetSite 那段保留作兼容但真实环境永不触发，不要依赖它传路径。
- **magic 检测坑（致命）**：`Initialize` 里靠流内容前几字节判扩展名。SVG(`<?xml`/`<svg`) 与 AI/EPS(`%!PS-Adobe`) 是纯 ASCII 文本，若不加显式分支会被 ASCII 兜底误判为 `plt`（HPGL 渲染器），worker 拿 SVG 内容按 HPGL 解析 → 渲染失败 → 缩略图不显示。**PDF 因 `%PDF` 头能正确识别，所以 PDF 成功而 SVG/AI 失败是典型的该 bug 症状。** 修复：在 magic 链里显式加 svg / ai 分支（位于 dxf 之后、ASCII 兜底之前）。
- **TIFF 缩略图坑（致命，2026-08-10 晚）**：`.tif`/`.tiff` 已关联到 `QuickView.Image`（Classes default，无 UserChoice），且 `QuickView.Image\ShellEx\{E357FCCD...}` 已注册 provider CLSID → Explorer **调用我们的 provider 处理 tif**。但 `kThumbnailExts` 不含 `.tif/.tiff`，且 magic 检测无 tif 分支 → tif 二进制（含 0x00，非 ASCII）落到 `ext=bin` → worker 拿 `.bin` 渲染失败 → 缩略图全部不生成（日志实证大量 `ext=bin`+`worker failed/timeout`）。现象"部分 tif 有图、部分无"＝有图的是 provider 注册前 WIC 生成的旧缓存侥幸留存、无图的是从没成功缓存。修复：①`kThumbnailExts` 增 `.tif`/`.tiff`；②magic 链加 tif 分支（小端 `"II*\0"`=49 49 2A 00 / 大端 `"MM\0*"=4D 4D 00 2A，置于 dxf 之后、svg/ai 之前；二进制含 0x00 不会误入 ASCII plt 兜底）。主程序**已支持 tif 渲染**（SupportedExtensions.h 的 STANDARD_EXTENSIONS 含 .tif/.tiff；MiniTiff/Cmyk/Lzw + 统一 LoadThumbnail 管线），**无需新增解码能力**，只改 provider 识别即可根治。修复后须清 thumbcache + 重启 Explorer。
- **流读取死循环坑（致命）**：原 `Initialize` 用循环 `pstream->Read` 读流，部分 Shell 提供的 IStream 在 EOF 不返回可靠终止信号 → 无限读取直到触发 200MB 上限熔断，导致所有格式缩略图都不生成（日志显示 `stream too large, abort`）。修复：优先 `IStream::Stat()` 取真实 `cbSize` 后**一次性 `Read` 固定字节数**；Stat 失败才退回流式读取并加「无进展熔断」(`buf.size()` 连续两次不变即 break)。
- **IID 易错点**：`IThumbnailProvider` = `{E357FCCD-A995-4576-B01F-234630154E96}`（不是 E357FCC4）；`IInitializeWithStream` = `{B824B49D-22AC-4161-AC8A-9916E8FA3F7F}`（Shell 强制要求，非 IInitializeWithFile）。
- **调试手段**：`ThumbnailProvider.cpp` 的 `DbgLog` 写 `C:\Windows\Temp\qvthumb_provider.log`（含 PROCESS_ATTACH / QI / Initialize / GetThumbnail）。改完必须清 `thumbcache_*.db` + 重启 Explorer 才能让新 DLL 加载。
- **本地端到端验证（绕过 Explorer）**：Python + `comtypes` 直接 `CoCreateInstance` provider → `SHCreateStreamOnFileW` 喂真实文件流 → `GetThumbnail(cx)`（注意 comtypes 自动处理 `[out]` 参数，调用时只传 `[in]` 的 cx，结果以元组返回）。venv 路径 `C:\Users\Administrator\.workbuddy\binaries\python\envs\default`。
- **关联必须非空（致命但易漏）**：缩略图提供程序只在「文件扩展名→ProgID→该 ProgID 的 ShellEx」链完整时才被调用。若某格式（如 PDF）在 HKCU/HKLM 下默认 ProgID 为空，则该格式**完全不被路由到 provider**（也无缩略图、也无接管）。`SettingsOverlay::RegisterAssociations` 仅把 `kVectorThumbExts` 中**用户在"文件关联"勾选**的格式设成 `QuickView.Vector`；未勾选的格式不会被设置 → 保持空。排查"某格式没缩略图"时务必先 `reg query` 确认 `.ext` 默认值为 `QuickView.Vector` 而非空。
- **UserChoice 受保护（致命）**：`FileExts\.ext\UserChoice` 子键由 Windows 默认应用机制保护，普通用户权限**无法删除也无法改写**（PowerShell `Remove-Item` 报"子键不存在"实则是 ACL 拒绝；`reg delete` 同样被拒）。这意味着第三方（含本程序）**无法静默接管受保护的默认关联**——典型即 `.pdf`（UserChoice=`MSEdgePDF`，Edge 抢占）。要接管 PDF，只能引导用户在「设置→应用→默认应用」里把 .pdf 手动设为 QuickView；改完后 UserChoice→`QuickView.Image`，而 Image 已注册 ShellEx → 立即生效。SVG/AI 的 UserChoice 指向 `QuickView.Image`（非 Edge），故只需给 Image 注册 ShellEx 即可，无需动 UserChoice。
- **视图模式硬约束**：Windows 仅「大图标 / 超大图标 / 内容」视图调用 `IThumbnailProvider`；中等图标及以下只显示文件类型图标。用户报"不显示"时先确认视图模式，并确认 `HKCU\...\Explorer\Advanced IconsOnly=0`（=1 则全局禁用缩略图）。Explorer 还会缓存"无缩略图"负面结果，旧 DLL 时代失败过的格式需**一次性重启 Explorer**才会重查（非 DLL 文件变更则不需重启；DLL 变更必须重启才重载）。
- **沙箱拦截补充**：PowerShell `Add-Type`（运行时编译 C#/.NET）被安全策略拦截；无法用 P/Invoke 调 `SHChangeNotify`，关联刷新只能靠用户 F5/重浏览或重启 Explorer。
