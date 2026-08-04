#pragma once
// ============================================================================
// VectorLoader.h - PLT(HPGL) / DXF(AutoCAD) / DWG(AutoCAD) → SVG XML 转换器
// ============================================================================
// 将矢量格式文件解析为 SVG XML 字符串，复用现有 D2D 原生 SVG 渲染管线。
// 与 CDR/CMX → librevenge → SVG 的数据流完全一致：
//
//   PLT 文件 → 手写解析器 → SVG XML 字符串
//   DXF/DWG 文件 → libdxfrw (DRW_Interface 回调) → SVG XML 字符串
//                              → RawImageFrame(PixelFormat::SVG_XML)
//                              → Direct2D 无损缩放渲染
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>

namespace QuickView {

/// PLT(HPGL) → SVG XML 转换
/// 返回非空 SVG 字符串表示成功，空字符串表示失败。
std::string LoadPLTtoSVG(const uint8_t* data, size_t size);

/// DXF(AutoCAD) → SVG XML 转换 (via libdxfrw)
/// 返回非空 SVG 字符串表示成功，空字符串表示失败。
std::string LoadDXFtoSVG(const uint8_t* data, size_t size);

/// DWG(AutoCAD) → SVG XML 转换 (via libdxfrw)
/// 返回非空 SVG 字符串表示成功，空字符串表示失败。
std::string LoadDWGtoSVG(const uint8_t* data, size_t size);

} // namespace QuickView
