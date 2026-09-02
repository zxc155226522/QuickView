#pragma once
// ============================================================================
// VectorLoader.h - PLT(HPGL) / DXF(AutoCAD) / DWG(AutoCAD) → SVG XML 转换器
// ============================================================================
// 将矢量格式文件解析为 SVG XML 字符串，复用现有 D2D 原生 SVG 渲染管线：
//
//   PLT 文件 → 手写解析器 (VectorLoader.cpp) → SVG XML 字符串
//   DXF/DWG 文件 → GNU LibreDWG (DwgLoader.cpp) → SVG XML 字符串
//                              → RawImageFrame(PixelFormat::SVG_XML)
//                              → Direct2D 无损缩放渲染
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include "ImageTypes.h"

namespace QuickView {

/// PLT(HPGL) → SVG XML 转换
/// 返回非空 SVG 字符串表示成功，空字符串表示失败。
std::string LoadPLTtoSVG(const uint8_t* data, size_t size);

/// DXF(AutoCAD) → SVG XML 转换 (via LibreDWG, 实现在 DwgLoader.cpp)
/// 返回非空 SVG 字符串表示成功，空字符串表示失败。
std::string LoadDXFtoSVG(const uint8_t* data, size_t size);

/// DWG(AutoCAD) → SVG XML 转换 (via LibreDWG, 实现在 DwgLoader.cpp)
/// 返回非空 SVG 字符串表示成功，空字符串表示失败。
std::string LoadDWGtoSVG(const uint8_t* data, size_t size);

/// DXF → SVG (带取消谓词: 渲染循环中定期检查, 返回 true 表示用户取消)
std::string LoadDXFtoSVG(const uint8_t* data, size_t size,
                         SimplePredicate checkCancel);

/// DWG → SVG (带取消谓词, 无超时上限, 用户取消是唯一终止手段)
std::string LoadDWGtoSVG(const uint8_t* data, size_t size,
                         SimplePredicate checkCancel);

/// 从 DWG 二进制文件中快速提取内嵌预览图 (BMP / PNG 数据)
/// 命中 AutoCAD 内嵌预览时耗时仅数毫秒，返回 true 并填充 outImageData。
bool ExtractDwgEmbeddedPreview(const uint8_t* data, size_t size,
                               std::vector<uint8_t>& outImageData,
                               bool* outIsPng = nullptr);

} // namespace QuickView
