# Zhai.PictureView 项目长期笔记

## 项目定位
- Windows EXE 看图软件（WPF, net6.0-windows, 固定 x64）。
- 源码根: `E:\项目\看图软件\Zhai.PictureView_src\`。
- 运行/发布产物: `E:\项目\看图软件\Zhai.PictureView\v1.2.2.27\win-x64\`（已用完整包覆盖旧 2023 版）。
- 规范脚本：`编译并启动.ps1`、`编译发布.ps1`（UTF-8 BOM + Join-Path 拼接；产物落 `发布/yyyy年M月d日H时m分s秒/`）。

## 图片解码架构（双引擎，ImageDecoder.cs）
- 常见格式（jpg/jpeg/png/bmp/gif/ico/webp/wbmp/jfif）→ **SkiaSharp 2.88.3**（`GetWriteableBitmapAsync`，~L264）。
- 其余（psd/ai/pdf/tga/svg...）→ **Magick.NET-Q16-AnyCPU 12.3.0**（`GetDocumentBitmapSourceAsync`，~L324；缩略图另走 `GetMagickThumbnail`）。
- 缩略图：常见格式走 Windows shell `ThumbnailProvider`，其余走 Magick `AdaptiveResize(256,256)`。

## Ghostscript 自包含（关键依赖，已修）
- `.ai` / `.pdf` / `.eps` 走 Magick delegate → 必须 **`gswin64c.exe`**（不只是 `gsdll64.dll`）。
- `src/Libs/Ghostscript/` 打包完整：bin/(gswin64c.exe, gsdll64.dll) + lib/ + Resource/ + iccprofiles/；csproj `<Content Include>` 带目录结构拷到输出 `Ghostscript/`。
- `App.xaml.cs` 启动期把 `Ghostscript\bin` 加 PATH，并保留 `MagickNET.SetGhostscriptDirectory`。
- csproj 固定 `<RuntimeIdentifier>win-x64</RuntimeIdentifier>`（AnyCPU 不拷 Magick 原生库会全挂，PSD 也跟着废）。
- `.gitignore` 对 `src/Libs/Ghostscript/bin/`、`gswin64c.exe`、`gsdll64.dll` 加白名单例外。

## 已确认无 bug 的渲染
- `胡梅尔LOGO.ai`（PDF 1.6）：Magick 72/150/300 DPI 完整正确渲染为 Hummel Logo；"横线断开"实为品牌雪佛龙条纹，非伪影。
- **AI/PDF 栅格化 DPI 已从 150 提升到 300**（2026-07-30 修复），改善低分辨率锯齿横条问题。`ImageDecoder.cs:332` `GetDocumentBitmapSourceAsync`。

## 开发规范要点（用户强约束）
- 改代码前：结构化修改计划 + Git 备份提交（含精确时间戳 + 计划内容）。
- 改代码后：二次 Git 提交保留快照。
- 模块化分文件；输出代码先给文件目录索引（极简概括，不贴代码）。
- 全部回复/思考/方案用中文；输出方案附 `Implementation Plan, Task List and Thought in Chinese`。
- 优先复用第三方库；KISS；事实为本。

## 排查方法论（本项目沉淀）
- native/外部进程依赖问题，系统已装同类库会掩盖"打包库是否真被用"。建独立 net8.0 console、引用同版 NuGet、按同目录结构拷资源直跑 → 最干净的隔离验证（2026-07-30 续诊复用）。
- 改代码前先 `git status`/`git log`；项目有 .git，可直接备份提交。