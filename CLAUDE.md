# CLAUDE.md - QuickView 项目 AI 指南

> AI 助手在修改本项目代码前,应先阅读本文件加载项目约束。

## 项目概述

- **名称**: QuickView(看图软件)
- **类型**: Windows EXE(C++23, Direct2D/DirectComposition)
- **编译器**: Clang-cl + lld-link(LLVM)
- **构建系统**: CMake + Ninja + vcpkg(自定义 triplet `x64-windows-static-clang`)
- **C++ 标准**: C++23(但第三方库可降级)

## 构建配置

- **CMake Preset**: `Release-LTO`(全量 LTO,禁用异常/RTTI)
- **vcpkg triplet**: `custom-triplet/x64-windows-static-clang.cmake`
- **自定义端口**: `custom-ports/`(dav1d, libavif, libjxl 等)
- **编译脚本**: `看图软件编译并启动.ps1`(创建 junction 避免中文路径 NASM 乱码)

### 中文路径注意事项
- 项目路径 `e:\项目\看图软件` 含中文,NASM 汇编器会乱码
- **必须通过 junction `E:\qv_build_tmp` 编译**,脚本已自动处理

### ⚠️ 编译失败时严禁破坏系统注册状态(必读红线)
当前系统(explorer)注册着 `out\build\Release-LTO\QuickViewThumbnailProvider.dll` 及配套 QuickView.exe,
它是这台电脑的**看图/缩略图生产力工具**。部分脚本的"解锁"步骤(如 `_build_only.ps1` 的 `Unlock-Products`)
会把注册中的 DLL/EXE 改名成 `.bak` 或移走来解除文件锁。

**铁律:编译没有成功之前,绝不替换、不覆盖、不移走系统当前注册的那套文件。**

- 若解锁步骤已把注册文件改成 `.bak`,而随后 CMake 配置或编译失败,必须**立即把 `.bak` 还原回原名**,
  再处理编译错误;绝不能带着"注册指向的文件不存在"的状态结束。
- 只有**编译成功**并通过基本冒烟验证(如 `--thumbnail`、`--export-png` 跑通、无 0xc0000005)后,
  才允许执行 `QuickView缩略图重注册.ps1` 等替换/重注册操作。
- 修改任何编译/部署/解锁脚本时,失败路径必须自动还原 `.bak`,不允许只写成功路径。
- 底线:AI 会话结束时,系统注册的看图软件必须处于可用状态;拿不准就先还原再报告。

### 缩略图全不显示时的排查顺序(2026-09-12 实录)
1. 先查注册:`HKCU:\Software\Classes\CLSID\{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}\InprocServer32`
   是否存在(用 PowerShell 查,Git Bash 转 `reg query` 会因引号/花括号误报"找不到")。
   键整个丢失 = 注册被清(`--uninstall`、设置页重关联、或注册时 DLL 正处于 `.bak` 窗口被跳过),
   跑 `QuickView缩略图重注册.ps1` 即可恢复。
2. 再冒烟:`QuickView.exe --thumbnail --input <文件> --size 256`,stdout 前 4 字节 0=OK。
3. 最后 Explorer 端到端验证:PowerShell `SHCreateItemFromParsingName` + `IShellItemImageFactory::GetImage`
   (测试 .ps1 必须带 UTF-8 BOM,否则中文路径变乱码报 0x80070002)。
4. DLL 只实现 `IInitializeWithFile`,注册**必须带** `DisableProcessIsolation=1`,
   否则 Explorer 走 stream 隔离初始化失败、缩略图静默消失(ps1 与 C++ RegisterAssociations 都要写)。

### 异常/RTTI 禁用规则
- triplet 对**非 boost、非 libraw** 的端口禁用异常(`/EHs-c-`)和 RTTI(`/GR-`)
- 如需为新的 vcpkg 端口启用异常,修改 triplet 中的条件判断

## 已支持的图像格式

JPEG, PNG, WebP, AVIF, JXL, RAW, PSD, SVG, GIF, TIFF, BMP, TGA, EXR, QOI, PCX, NetPBM, **CDR**, **CMX**

## CDR/CMX 格式集成方案

### 数据流
```
.cdr/.cmx 文件
  → libcdr::CDRDocument::parse() / CMXDocument::parse()
  → librevenge::RVNGSVGDrawingGenerator (生成 SVG XML 字符串)
  → RawImageFrame (PixelFormat::SVG_XML)
  → 现有 Direct2D SVG 渲染管线 (无损缩放)
```

### 依赖链
- `libcdr`(源码集成, `third_party/libcdr/`)→ 解析 CDR/CMX
- `librevenge`(源码集成, `third_party/librevenge/`)→ 提供 RVNGFileStream + RVNGSVGDrawingGenerator
- `boost`(vcpkg, header-only)→ libcdr 内部使用 spirit/property_tree/algorithm
- `zlib`(vcpkg, 已有)→ librevenge ZipStream + libcdr CDRInternalStream

### 关键文件

| 文件 | 用途 |
|---|---|
| `third_party/librevenge/CMakeLists.txt` | librevenge 精简 CMake 构建(降级 C++17) |
| `third_party/librevenge/win_compat.h` | Windows POSIX 兼容(S_ISREG/S_ISDIR) |
| `third_party/libcdr/CMakeLists.txt` | libcdr 精简 CMake 构建 |
| `third_party/libcdr/icu_stub/` | ICU stub(字符集检测降级为空实现) |
| `QuickView/ImageLoader.cpp` `LoadCDR()` | CDR→SVG 加载实现 |
| `QuickView/SupportedExtensions.h` | `.cdr`/`.cmx` 扩展名注册 |

### 编译兼容性问题速查

| 问题 | 解决方案 |
|---|---|
| boost-thread `throw` 编译失败 | triplet 对 `boost-*` 端口启用异常 |
| NASM 中文路径乱码 | 通过 junction `E:\qv_build_tmp` 编译 |
| `shared_ptr::unique()` C++23 移除 | librevenge 降级 C++17(`/clang:-std=c++17`) |
| `S_ISREG`/`S_ISDIR` 缺失 | `win_compat.h` + `/FI` 强制包含 |
| ICU 依赖(`unicode/*.h`) | 创建 stub 头文件(空实现) |
| libcdr `zlib.h` 找不到 | libcdr CMakeLists 显式链接 `ZLIB::ZLIB` |
| `try/catch` 在禁用异常环境失败 | 替换为 `strtof` 等 C 风格 API |

## 开发规范

- **修改前**: 结构化修改计划 → Git 备份提交(含时间戳 + 计划内容)
- **修改后**: Git 二次提交保留快照
- **模块化**: 工具函数、配置常量、实体模型、业务逻辑、程序入口分文件
- **输出代码前**: 先输出文件目录索引(极简概括,不贴代码)
- **语言**: 全部回复/思考/方案用中文
- **标识**: 输出方案附 `Implementation Plan, Task List and Thought in Chinese`
