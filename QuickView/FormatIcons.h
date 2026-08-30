#pragma once
// ============================================================================
// FormatIcons.h — 每格式专属图标资源表（自动生成 by _gen_format_icons.py，勿手改）
// ============================================================================
// 每个受支持格式一枚专属文件图标：类别色圆形字母章（白字格式缩写，与缩略图右上角
// 类型胶囊同一套类别色语义）。图像资源在 QuickView/icons/<ext>.ico，经
// QuickView.rc #include "format_icons.rc" 编入 exe（资源 ID 200 起）。
// 关联 ProgID 的 DefaultIcon 以 ",-<ID>" 引用。
// 格式清单源自 SupportedExtensions.h 四个分段，与 SUPPORTED_EXTENSIONS 一一对应。
#include "SupportedExtensions.h"

namespace QuickView {

struct FormatIconEntry { std::wstring_view ext; unsigned id; };

inline constexpr FormatIconEntry kFormatIcons[] = {
    {L".jpg", 200},
    {L".jpeg", 201},
    {L".jpe", 202},
    {L".jfif", 203},
    {L".png", 204},
    {L".bmp", 205},
    {L".dib", 206},
    {L".gif", 207},
    {L".tif", 208},
    {L".tiff", 209},
    {L".ico", 210},
    {L".webp", 211},
    {L".avif", 212},
    {L".avifs", 213},
    {L".heic", 214},
    {L".heif", 215},
    {L".svg", 216},
    {L".svgz", 217},
    {L".jxl", 218},
    {L".apng", 219},
    {L".cdr", 220},
    {L".cmx", 221},
    {L".pdf", 222},
    {L".ai", 223},
    {L".plt", 224},
    {L".dxf", 225},
    {L".dwg", 226},
    {L".exr", 227},
    {L".hdr", 228},
    {L".pic", 229},
    {L".psd", 230},
    {L".psb", 231},
    {L".tga", 232},
    {L".icb", 233},
    {L".vda", 234},
    {L".vst", 235},
    {L".pcx", 236},
    {L".qoi", 237},
    {L".wbmp", 238},
    {L".pam", 239},
    {L".pbm", 240},
    {L".pgm", 241},
    {L".ppm", 242},
    {L".pnm", 243},
    {L".wdp", 244},
    {L".hdp", 245},
    {L".jxr", 246},
    {L".hif", 247},
    {L".arw", 248},
    {L".cr2", 249},
    {L".cr3", 250},
    {L".crw", 251},
    {L".dng", 252},
    {L".nef", 253},
    {L".orf", 254},
    {L".raf", 255},
    {L".rw2", 256},
    {L".srw", 257},
    {L".x3f", 258},
    {L".mrw", 259},
    {L".mos", 260},
    {L".kdc", 261},
    {L".dcr", 262},
    {L".sr2", 263},
    {L".pef", 264},
    {L".erf", 265},
    {L".3fr", 266},
    {L".mef", 267},
    {L".nrw", 268},
    {L".raw", 269},
};

// 查扩展名（须带点，如 L".jpg"）对应的图标资源 ID；未定义返回 -1。
constexpr int IconResourceIdForExt(std::wstring_view ext) {
    for (const auto& e : kFormatIcons)
        if (ExtEqualsIgnoreCase(e.ext, ext)) return static_cast<int>(e.id);
    return -1;
}

} // namespace QuickView
