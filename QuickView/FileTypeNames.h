#pragma once
// ============================================================================
// FileTypeNames.h — 每扩展名的「资源管理器类型名」对照表
// ============================================================================
// 背景：QuickView 用两个通用 ProgID（QuickView.Image / QuickView.Vector）承载
// 所有格式的打开关联，FriendlyTypeName 也只有两份。资源管理器的「类型」列和
// 「分组依据 → 类型」读取的是当前生效 ProgID 的 FriendlyTypeName —— 结果是整个
// 文件夹的 jpg/png/webp 全部归组为 "QuickView Image Viewer"，无法区分类型。
//
// 解法（同 VLC 的做法）：为每个扩展名建立独立 ProgID（QuickView.jpg /
// QuickView.png / ...），各自携带本表给出的类型名。同一种格式的别名共用一个
// 名字（.jpeg/.jpe/.jfif → "JPEG 文件"），分组时自然归到同一组。
//
// 缩略图不受影响：provider 在 .ext 级 ShellEx 注册（最高优先级），与 ProgID 无关。
// ============================================================================

#include "SupportedExtensions.h"

namespace QuickView {

struct FileTypeEntry {
    std::wstring_view ext;
    std::wstring_view typeName;
};

// 同一格式的别名必须用同一个 typeName，保证「按类型分组」时归到同一组。
inline constexpr FileTypeEntry kFileTypeNames[] = {
    // 标准栅格
    { L".jpg",   L"JPG 文件" },
    { L".jpeg",  L"JPEG 文件" },
    { L".jpe",   L"JPEG 文件" },
    { L".jfif",  L"JPEG 文件" },
    { L".png",   L"PNG 文件" },
    { L".bmp",   L"BMP 文件" },
    { L".dib",   L"BMP 文件" },
    { L".gif",   L"GIF 文件" },
    { L".tif",   L"TIFF 文件" },
    { L".tiff",  L"TIFF 文件" },
    { L".ico",   L"ICO 文件" },
    // Web / 现代格式
    { L".webp",  L"WebP 文件" },
    { L".avif",  L"AVIF 文件" },
    { L".avifs", L"AVIF 文件" },
    { L".heic",  L"HEIC 文件" },
    { L".heif",  L"HEIF 文件" },
    { L".hif",   L"HEIF 文件" },
    { L".svg",   L"SVG 文件" },
    { L".svgz",  L"SVG 文件" },
    { L".jxl",   L"JPEG XL 文件" },
    { L".apng",  L"APNG 文件" },
    { L".cdr",   L"CDR 文件" },
    { L".cmx",   L"CMX 文件" },
    { L".pdf",   L"PDF 文件" },
    { L".ai",    L"AI 文件" },
    { L".plt",   L"PLT 文件" },
    { L".dxf",   L"DXF 文件" },
    { L".dwg",   L"DWG 文件" },
    // 专业 / HDR / 传统格式
    { L".exr",   L"EXR 文件" },
    { L".hdr",   L"HDR 文件" },
    { L".pic",   L"PIC 文件" },
    { L".psd",   L"PSD 文件" },
    { L".psb",   L"PSB 文件" },
    { L".tga",   L"TGA 文件" },
    { L".icb",   L"TGA 文件" },
    { L".vda",   L"TGA 文件" },
    { L".vst",   L"TGA 文件" },
    { L".pcx",   L"PCX 文件" },
    { L".qoi",   L"QOI 文件" },
    { L".wbmp",  L"WBMP 文件" },
    { L".pam",   L"PAM 文件" },
    { L".pbm",   L"PBM 文件" },
    { L".pgm",   L"PGM 文件" },
    { L".ppm",   L"PPM 文件" },
    { L".pnm",   L"PNM 文件" },
    { L".wdp",   L"JXR 文件" },
    { L".hdp",   L"JXR 文件" },
    { L".jxr",   L"JXR 文件" },
    // 相机 RAW（SUPPORTED_EXTENSIONS 内的 22 种；每种独立命名以便区分）
    { L".arw",   L"ARW 文件" },
    { L".cr2",   L"CR2 文件" },
    { L".cr3",   L"CR3 文件" },
    { L".crw",   L"CRW 文件" },
    { L".dng",   L"DNG 文件" },
    { L".nef",   L"NEF 文件" },
    { L".orf",   L"ORF 文件" },
    { L".raf",   L"RAF 文件" },
    { L".rw2",   L"RW2 文件" },
    { L".srw",   L"SRW 文件" },
    { L".x3f",   L"X3F 文件" },
    { L".mrw",   L"MRW 文件" },
    { L".mos",   L"MOS 文件" },
    { L".kdc",   L"KDC 文件" },
    { L".dcr",   L"DCR 文件" },
    { L".sr2",   L"SR2 文件" },
    { L".pef",   L"PEF 文件" },
    { L".erf",   L"ERF 文件" },
    { L".3fr",   L"3FR 文件" },
    { L".mef",   L"MEF 文件" },
    { L".nrw",   L"NRW 文件" },
    { L".raw",   L"RAW 文件" },
};

// 查表（constexpr）：命中返回表内类型名；未命中返回空视图。
constexpr std::wstring_view FriendlyTypeNameForExt(std::wstring_view ext) {
    for (const auto& e : kFileTypeNames)
        if (ExtEqualsIgnoreCase(e.ext, ext)) return e.typeName;
    return {};
}

// 运行时入口：表内命中用静态名；未命中（理论上不会发生，表已覆盖全部受支持
// 扩展名，护栏见下）回退为大写扩展名 + " 文件"（如 ".XYZ" → "XYZ 文件"）。
inline std::wstring MakeFriendlyTypeName(std::wstring_view ext) {
    if (std::wstring_view hit = FriendlyTypeNameForExt(ext); !hit.empty())
        return std::wstring(hit);
    std::wstring name;
    for (wchar_t c : ext)
        if (c != L'.') name.push_back(ToUpperAscii(c));
    name += L" 文件";
    return name;
}

// 表覆盖完整性护栏：SUPPORTED_EXTENSIONS 中每一项都必须能在表中查到。
static_assert([]() consteval {
    for (std::wstring_view s : SUPPORTED_EXTENSIONS) {
        bool found = false;
        for (const auto& e : kFileTypeNames)
            if (ExtEqualsIgnoreCase(e.ext, s)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}(), "kFileTypeNames 必须覆盖 SUPPORTED_EXTENSIONS 的每一项");

// 表内条目数与 SUPPORTED_EXTENSIONS 的关系：别名合并后条目更少，但每个受支持
// 扩展名至少出现一次（上面的 static_assert 已保证）。

} // namespace QuickView
