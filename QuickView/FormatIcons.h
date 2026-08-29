#pragma once
// ============================================================================
// FormatIcons.h — 每格式专属图标资源表（自动生成 by _gen_format_icons.py，勿手改）
// ============================================================================
// 每个受支持格式一枚专属文件图标：类别色圆形字母章（白字格式缩写，与缩略图右上角
// 类型胶囊同一套类别色语义）。图像资源在 QuickView/icons/<ext>.ico，经
// QuickView.rc #include "format_icons.rc" 编入 exe（资源 ID 100 起）。
// 关联 ProgID 的 DefaultIcon 以 ",-<ID>" 引用。
// 格式清单源自 SupportedExtensions.h 四个分段，与 SUPPORTED_EXTENSIONS 一一对应。
#include "SupportedExtensions.h"

namespace QuickView {

struct FormatIconEntry { std::wstring_view ext; unsigned id; };

inline constexpr FormatIconEntry kFormatIcons[] = {
    {L".jpg", 100},
    {L".jpeg", 101},
    {L".jpe", 102},
    {L".jfif", 103},
    {L".png", 104},
    {L".bmp", 105},
    {L".dib", 106},
    {L".gif", 107},
    {L".tif", 108},
    {L".tiff", 109},
    {L".ico", 110},
    {L".webp", 111},
    {L".avif", 112},
    {L".avifs", 113},
    {L".heic", 114},
    {L".heif", 115},
    {L".svg", 116},
    {L".svgz", 117},
    {L".jxl", 118},
    {L".apng", 119},
    {L".cdr", 120},
    {L".cmx", 121},
    {L".pdf", 122},
    {L".ai", 123},
    {L".plt", 124},
    {L".dxf", 125},
    {L".dwg", 126},
    {L".exr", 127},
    {L".hdr", 128},
    {L".pic", 129},
    {L".psd", 130},
    {L".psb", 131},
    {L".tga", 132},
    {L".icb", 133},
    {L".vda", 134},
    {L".vst", 135},
    {L".pcx", 136},
    {L".qoi", 137},
    {L".wbmp", 138},
    {L".pam", 139},
    {L".pbm", 140},
    {L".pgm", 141},
    {L".ppm", 142},
    {L".pnm", 143},
    {L".wdp", 144},
    {L".hdp", 145},
    {L".jxr", 146},
    {L".hif", 147},
    {L".arw", 148},
    {L".cr2", 149},
    {L".cr3", 150},
    {L".crw", 151},
    {L".dng", 152},
    {L".nef", 153},
    {L".orf", 154},
    {L".raf", 155},
    {L".rw2", 156},
    {L".srw", 157},
    {L".x3f", 158},
    {L".mrw", 159},
    {L".mos", 160},
    {L".kdc", 161},
    {L".dcr", 162},
    {L".sr2", 163},
    {L".pef", 164},
    {L".erf", 165},
    {L".3fr", 166},
    {L".mef", 167},
    {L".nrw", 168},
    {L".raw", 169},
};

// 查扩展名（须带点，如 L".jpg"）对应的图标资源 ID；未定义返回 -1。
constexpr int IconResourceIdForExt(std::wstring_view ext) {
    for (const auto& e : kFormatIcons)
        if (ExtEqualsIgnoreCase(e.ext, ext)) return static_cast<int>(e.id);
    return -1;
}

} // namespace QuickView
