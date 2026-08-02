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
