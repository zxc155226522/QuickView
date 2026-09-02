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

inline D2D1_COLOR_F BadgeColorFor(std::wstring_view ext) {
    auto in = [&](std::wstring_view s) -> bool {
        if (ext.size() != s.size()) return false;
        for (size_t i = 0; i < ext.size(); ++i) {
            if (::towupper(ext[i]) != ::towupper(s[i])) return false;
        }
        return true;
    };
    if (in(L"PNG")||in(L"JPG")||in(L"JPEG")||in(L"BMP")||in(L"GIF")||in(L"WEBP")||in(L"HEIC")||in(L"TIF")||in(L"TIFF")||in(L"JXL")||in(L"AVIF")) return D2D1::ColorF(0.063f,0.725f,0.506f); // 翠绿(位图)
    if (in(L"CR2")||in(L"CR3")||in(L"ARW")||in(L"NEF")||in(L"DNG")||in(L"RAF")||in(L"RW2")||in(L"ORF")) return D2D1::ColorF(0.545f,0.361f,0.965f); // 紫(RAW)
    if (in(L"CDR")||in(L"CMX")||in(L"AI")||in(L"SVG")||in(L"SVGZ")||in(L"EPS")) return D2D1::ColorF(0.231f,0.510f,0.965f); // 蓝(矢量)
    if (in(L"PDF")||in(L"TXT")||in(L"DOC")||in(L"DOCX")||in(L"XLS")||in(L"XLSX")||in(L"PPT")||in(L"PPTX")) return D2D1::ColorF(0.937f,0.267f,0.267f); // 玫红(文档)
    if (in(L"PLT")||in(L"DXF")||in(L"DWG")) return D2D1::ColorF(0.024f,0.714f,0.831f); // 青(CAD)
    if (in(L"MP4")||in(L"MOV")||in(L"AVI")||in(L"MKV")||in(L"WEBM")) return D2D1::ColorF(0.957f,0.247f,0.369f); // 粉(视频)
    if (in(L"MP3")||in(L"WAV")||in(L"FLAC")||in(L"OGG")||in(L"M4A")) return D2D1::ColorF(0.133f,0.773f,0.369f); // 绿(音频)
    if (in(L"ZIP")||in(L"RAR")||in(L"7Z")||in(L"TAR")) return D2D1::ColorF(0.961f,0.620f,0.043f); // 琥珀(压缩)
    if (in(L"CPP")||in(L"H")||in(L"PY")||in(L"JS")||in(L"TS")||in(L"JSON")||in(L"XML")||in(L"HTML")) return D2D1::ColorF(0.388f,0.400f,0.945f); // 靛(代码)
    return D2D1::ColorF(0.392f,0.455f,0.545f); // 中性灰(兜底)
}

} // namespace QuickView
