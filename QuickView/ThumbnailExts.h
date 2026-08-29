#pragma once
// ============================================================================
// ThumbnailExts.h — 缩略图 handler 覆盖范围的单一真相源 (single source of truth)
// ============================================================================
// 下列每个扩展名都会在 Shell 的 .ext 级（扩展名键下的 ShellEx 子键）注册
// QuickView 的 IThumbnailProvider。原因：
//   * provider 把渲染委派给 "QuickView.exe --thumbnail"，复用完整应用渲染管线，
//     并在最上层绘制「类型角标」。
//   * 在 .ext 级注册（Shell 解析链的最高优先级，先于 ProgID / UserChoice）能保证
//     无论哪个程序持有该格式的默认应用哈希，角标都稳定显示。
//
// 清单从 QuickView::SUPPORTED_EXTENSIONS（全部可浏览图像格式）派生。归档容器
// （.zip/.cbz/...）已不再受支持，不在 SUPPORTED_EXTENSIONS 中。因为是从
// SUPPORTED_EXTENSIONS 派生，所以它永远不可能与之漂移：任何新增的可浏览格式都会
// 自动获得 provider 覆盖。
// ============================================================================
#include "SupportedExtensions.h"

namespace QuickView {

// 全部可浏览图像格式（即 SUPPORTED_EXTENSIONS，保留过滤逻辑作为防漂移护栏）。
inline constexpr auto kThumbnailExts = []() consteval {
    constexpr size_t N = SUPPORTED_EXTENSIONS.size();
    std::array<std::wstring_view, N> out{};
    size_t i = 0;
    for (std::wstring_view e : SUPPORTED_EXTENSIONS) {
        bool isArchive = false;
        for (std::wstring_view a : ARCHIVE_EXTENSIONS)
            if (ExtEqualsIgnoreCase(e, a)) { isArchive = true; break; }
        if (!isArchive) out[i++] = e;
    }
    return out;
}();

inline constexpr size_t kThumbnailExtsCount = std::size(kThumbnailExts);

// 文档 / 矢量格式：仅为「打开（双击）」动词映射到专用 ProgID QuickView.Vector。
// （缩略图覆盖与此无关，见上方 kThumbnailExts。）保留独立列表是因为
// 「矢量 vs 位图」的打开行为分类是与缩略图不同的关注点。
inline constexpr std::wstring_view kVectorExts[] = {
    L".cdr", L".cmx", L".plt", L".dxf", L".dwg", L".pdf", L".ai", L".svg", L".svgz"
};

// 防漂移断言：每个缩略图扩展名都必须是受支持扩展名。
static_assert([]() consteval {
    for (std::wstring_view t : kThumbnailExts) {
        bool found = false;
        for (std::wstring_view s : SUPPORTED_EXTENSIONS)
            if (ExtEqualsIgnoreCase(t, s)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}(), "kThumbnailExts 必须是 SUPPORTED_EXTENSIONS 的子集");

} // namespace QuickView
