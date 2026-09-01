// ============================================================================
// DwgLoader.cpp - DXF/DWG(AutoCAD) → SVG XML 转换器 (via GNU LibreDWG)
// ============================================================================
// LibreDWG 解析 DXF(ASCII/Binary) 与 DWG(R1.4-R2018)，统一产出 Dwg_Data，
// 本文件遍历 *Model_Space 实体链生成 SVG XML，复用 D2D 原生 SVG 渲染管线。
//
// 渲染策略:
//   双层渲染 — 填充类图形(HATCH 实心填充/图案填充线/SOLID/TRACE)画在底层，
//   线框实体画在上层，填充永不遮挡线框。
//   HATCH 区分 is_solid_fill(实心填充)与图案填充(按 deflines 定义线绘制，
//   用 clipPath 裁剪到边界内)。
// Y 轴翻转: y' = -y (CAD Y朝上 → SVG Y朝下)
// 颜色: 真彩(Dwg_Color.rgb) > ACI 索引 > BYLAYER(查图层色)
// 文本: r2007+ 为 UTF-16(转合法 UTF-8)，早期版本为本地编码(非 ASCII 置 '?')
// ============================================================================
#include "pch.h"
#include "VectorLoader.h"
#include "VectorCommon.h"
#include <libredwg/dwg.h>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <future>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>

namespace QuickView {
namespace {

constexpr double kPi = 3.14159265358979;

// ---------------------------------------------------------------------------
// 文本处理
// ---------------------------------------------------------------------------

// 早期版本 (本地代码页) 字节 → UTF-8。
// R2007+ 的 DXF 文本本身是 UTF-8 → 合法 UTF-8 直接保留;
// 否则中文环境常见 GBK (ANSI_936) → 转换; 无代码页信息时退化非 ASCII 置 '?'
std::string LocalBytesToUtf8(const char* s) {
    if (!s) return {};
    size_t len = strlen(s);
    if (len == 0) return {};
    bool hasHigh = false;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)s[i] > 0x7F) { hasHigh = true; break; }
    }
    if (!hasHigh) return std::string(s, len);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            s, (int)len, nullptr, 0) > 0) {
        return std::string(s, len); // 已是合法 UTF-8
    }
    int wn = MultiByteToWideChar(936, 0, s, (int)len, nullptr, 0);
    if (wn > 0) {
        std::wstring w(wn, L'\0');
        MultiByteToWideChar(936, 0, s, (int)len, &w[0], wn);
        int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn,
                                    nullptr, 0, nullptr, nullptr);
        if (n > 0) {
            std::string out(n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), wn,
                                &out[0], n, nullptr, nullptr);
            return out;
        }
    }
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; i++)
        out += ((unsigned char)s[i] > 0x7F) ? '?' : s[i];
    return out;
}

// BITCODE_T → 原始 UTF-8 字节 (不转义; r2007+ 源为 UTF-16)
std::string DwgTextToUtf8(BITCODE_T s, bool utf16) {
    if (!s) return {};
    if (utf16) {
        const unsigned short* w = (const unsigned short*)s;
        std::string utf8;
        for (; *w; ++w) {
            unsigned ch = *w;
            if (ch < 0x80) {
                utf8 += (char)ch;
            } else if (ch < 0x800) {
                utf8 += (char)(0xC0 | (ch >> 6));
                utf8 += (char)(0x80 | (ch & 0x3F));
            } else {
                utf8 += (char)(0xE0 | (ch >> 12));
                utf8 += (char)(0x80 | ((ch >> 6) & 0x3F));
                utf8 += (char)(0x80 | (ch & 0x3F));
            }
        }
        return utf8;
    }
    return LocalBytesToUtf8(s); // 早期版本: 本地代码页字节
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::wstring LowerWide(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

// ---------------------------------------------------------------------------
// 文字轮廓化
// CAD 帧走 Direct2D 原生 ID2D1SvgDocument 管线, 而它只实现 SVG 1.1 的排版子集
// —— 不做 <text> 布局, 文字会被整段吞掉。故在生成 SVG 阶段就用 GDI
// GetGlyphOutline(GGO_NATIVE) 取 TrueType 原生轮廓, 展开成 <path>。
// 轮廓统一按 em=1000 采集 (y 轴朝上、基线 y=0), 之后再缩放到 CAD 文字高度。
// ---------------------------------------------------------------------------
constexpr LONG kGlyphEm = 1000;
// lpMat2 传 NULL 会让 GetGlyphOutline 一律返回 GDI_ERROR, 必须显式给单位矩阵
const MAT2 kIdentMat2 = { {0, 1}, {0, 0}, {0, 0}, {0, 1} };

struct GlyphSeg {
    char op = 'M';        // 'M' 'L' 'C' 'Z'
    double x[3] = {0, 0, 0};
    double y[3] = {0, 0, 0};
};

struct GlyphShape {
    std::vector<GlyphSeg> segs;
    double advance = 0;
    double minX = 0, minY = 0, maxX = 0, maxY = 0;   // em, 相对基线原点
};

struct GlyphPoint {
    double x = 0, y = 0;
    bool quad = false;    // true = TT_PRIM_QSPLINE 的控制点, false = 直线端点
};

double Fixed1616ToDouble(FIXED f) { return f.value + f.fract / 65536.0; }

// 一条轮廓的顶点序列 → 闭合子路径。GDI 把 TrueType 轮廓摊平成 (off-curve,
// on-curve) 交替序列: 首点可能是 off-curve (此时 pfxStart 已是隐含中点),
// 末尾也可能悬空一个 off-curve, 需回到轮廓起点闭合。
void EmitContour(GlyphShape& g, const POINTFX& start,
                 const std::vector<GlyphPoint>& items) {
    double sx = Fixed1616ToDouble(start.x), sy = Fixed1616ToDouble(start.y);
    double cx = sx, cy = sy;
    GlyphSeg m; m.op = 'M'; m.x[0] = cx; m.y[0] = cy;
    g.segs.push_back(m);
    size_t i = 0;
    while (i < items.size()) {
        if (!items[i].quad) {   // TT_PRIM_LINE
            GlyphSeg s; s.op = 'L'; s.x[0] = items[i].x; s.y[0] = items[i].y;
            g.segs.push_back(s);
            cx = s.x[0]; cy = s.y[0];
            i++;
            continue;
        }
        double qx = items[i].x, qy = items[i].y;
        i++;
        double ex, ey;
        if (i < items.size() && !items[i].quad) {
            ex = items[i].x; ey = items[i].y;
            i++;
        } else if (i < items.size()) {
            ex = (qx + items[i].x) / 2; ey = (qy + items[i].y) / 2;
        } else {
            ex = sx; ey = sy;
        }
        GlyphSeg s; s.op = 'C';
        s.x[0] = cx + (2.0 / 3.0) * (qx - cx); s.y[0] = cy + (2.0 / 3.0) * (qy - cy);
        s.x[1] = ex + (2.0 / 3.0) * (qx - ex); s.y[1] = ey + (2.0 / 3.0) * (qy - ey);
        s.x[2] = ex; s.y[2] = ey;
        g.segs.push_back(s);
        cx = ex; cy = ey;
    }
    GlyphSeg z; z.op = 'Z';
    g.segs.push_back(z);
}

void AppendPathNum(std::string& d, double v, int decimals) {
    char buf[40];
    int n = snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    if (n <= 0) return;
    int e = n;
    char* dot = (decimals > 0) ? (char*)memchr(buf, '.', (size_t)n) : nullptr;
    if (dot) {
        int stop = (int)(dot - buf);
        while (e > stop && buf[e - 1] == '0') buf[--e] = '\0';
        if (e > 0 && buf[e - 1] == '.') buf[--e] = '\0';
    }
    d.append(buf, e);
}

class TextOutliner {
public:
    TextOutliner() { m_dc = CreateCompatibleDC(nullptr); }
    ~TextOutliner() {
        for (auto* f : m_faces) {
            if (f->font) DeleteObject(f->font);
            delete f;
        }
        if (m_dc) DeleteDC(m_dc);
    }
    TextOutliner(const TextOutliner&) = delete;
    TextOutliner& operator=(const TextOutliner&) = delete;

    // face 为空 → 默认字体。d 为局部坐标路径 (x 沿基线, y 朝下), local 为其包围盒
    bool BuildText(const std::wstring& text, const std::wstring& face, double height,
                   double widthFactor, double oblique, std::string& d, BBox& local);

private:
    struct Face {
        HFONT font = nullptr;
        double cap = 700;     // 'H' 高度 (em), 用于把 CAD 文字高度换算成 em
        size_t id = 0;
    };

    static HFONT MakeFont(const wchar_t* name) {
        LOGFONTW lf;
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -kGlyphEm;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = PROOF_QUALITY;   // 按设计形状取轮廓, 不做像素网格 hinting
        lf.lfPitchAndFamily = FF_DONTCARE | DEFAULT_PITCH;
        wcsncpy_s(lf.lfFaceName, name, LF_FACESIZE - 1);
        lf.lfFaceName[LF_FACESIZE - 1] = 0;
        return CreateFontIndirectW(&lf);
    }

    bool UseFace(Face* f) {
        if (!f || !f->font) return false;
        if (m_selected != f->font) {
            if (!SelectObject(m_dc, f->font)) return false;
            m_selected = f->font;
        }
        return true;
    }

    Face* GetFace(const std::wstring& requested);
    const GlyphShape* FetchGlyph(Face* face, wchar_t ch);

    struct CachedText {
        std::string d;
        BBox box;
    };

    HDC m_dc = nullptr;
    HFONT m_selected = nullptr;
    std::vector<Face*> m_faces;
    std::unordered_map<std::wstring, Face*> m_faceByName;
    std::unordered_map<unsigned long long, GlyphShape> m_glyphs;
    std::unordered_map<std::string, CachedText> m_strings;   // 重复文字串复用
};

TextOutliner::Face* TextOutliner::GetFace(const std::wstring& requested) {
    if (!m_dc) return nullptr;
    std::wstring key = LowerWide(requested.empty() ? L"arial" : requested);
    auto hit = m_faceByName.find(key);
    if (hit != m_faceByName.end()) return hit->second;

    HFONT font = MakeFont(key.c_str());
    if (!font) return nullptr;
    if (!SelectObject(m_dc, font)) { DeleteObject(font); return nullptr; }
    m_selected = font;
    // GDI 会把不存在的族名替换成别的字体: 改用真实族名, 让缓存键与实际字形一致
    wchar_t actual[LF_FACESIZE] = {0};
    if (GetTextFaceW(m_dc, LF_FACESIZE, actual) > 0) {
        std::wstring real = LowerWide(actual);
        if (!real.empty() && real != key) {
            hit = m_faceByName.find(real);
            if (hit != m_faceByName.end()) {
                m_selected = nullptr;
                DeleteObject(font);
                return hit->second;
            }
            DeleteObject(font);
            font = MakeFont(real.c_str());
            key = real;
            if (!font) return nullptr;
            if (!SelectObject(m_dc, font)) { DeleteObject(font); return nullptr; }
            m_selected = font;
        }
    }

    Face* f = new Face();
    f->font = font;
    f->id = m_faces.size();
    GLYPHMETRICS gm;
    memset(&gm, 0, sizeof(gm));
    if (GetGlyphOutlineW(m_dc, L'H', GGO_NATIVE, &gm, 0, nullptr, &kIdentMat2) != GDI_ERROR
        && gm.gmBlackBoxY > 0) {
        f->cap = gm.gmBlackBoxY;
    }
    m_faces.push_back(f);
    m_faceByName[key] = f;
    return f;
}

const GlyphShape* TextOutliner::FetchGlyph(Face* face, wchar_t ch) {
    unsigned long long key = ((unsigned long long)face->id << 32) | (unsigned short)ch;
    auto hit = m_glyphs.find(key);
    if (hit != m_glyphs.end()) return &hit->second;

    GlyphShape g;
    if (!UseFace(face)) return &m_glyphs.emplace(key, g).first->second;
    GLYPHMETRICS gm;
    memset(&gm, 0, sizeof(gm));
    DWORD sz = GetGlyphOutlineW(m_dc, ch, GGO_NATIVE, &gm, 0, nullptr, &kIdentMat2);
    if (sz == GDI_ERROR) {
        // 无轮廓字形(空格等): 退回只取度量
        memset(&gm, 0, sizeof(gm));
        if (GetGlyphOutlineW(m_dc, ch, GGO_METRICS, &gm, 0, nullptr, &kIdentMat2) == GDI_ERROR)
            memset(&gm, 0, sizeof(gm));
        sz = 0;
    }
    if (gm.gmBlackBoxX || gm.gmBlackBoxY) {
        g.advance = gm.gmCellIncX;
        g.minX = gm.gmptGlyphOrigin.x;
        g.maxX = g.minX + (double)gm.gmBlackBoxX;
        g.maxY = gm.gmptGlyphOrigin.y;
        g.minY = g.maxY - (double)gm.gmBlackBoxY;
    }
    if (sz > 0) {
        std::vector<unsigned char> buf(sz);
        if (GetGlyphOutlineW(m_dc, ch, GGO_NATIVE, &gm, sz, buf.data(), &kIdentMat2)
            != GDI_ERROR) {
            const unsigned char* data = buf.data();
            DWORD off = 0;
            while (off + sizeof(TTPOLYGONHEADER) <= sz) {
                const TTPOLYGONHEADER* ph = (const TTPOLYGONHEADER*)(data + off);
                if (ph->dwType != TT_POLYGON_TYPE
                    || ph->cb < sizeof(TTPOLYGONHEADER) || off + ph->cb > sz) break;
                DWORD end = off + ph->cb;
                std::vector<GlyphPoint> items;
                DWORD coff = off + sizeof(TTPOLYGONHEADER);
                while (coff + 2 * sizeof(WORD) <= end) {
                    const TTPOLYCURVE* pc = (const TTPOLYCURVE*)(data + coff);
                    if (pc->wType != TT_PRIM_LINE && pc->wType != TT_PRIM_QSPLINE) break;
                    bool quad = (pc->wType == TT_PRIM_QSPLINE);
                    size_t bytes = 2 * sizeof(WORD) + (size_t)pc->cpfx * sizeof(POINTFX);
                    if (coff + bytes > end) break;
                    for (WORD i = 0; i < pc->cpfx; i++)
                        items.push_back({ Fixed1616ToDouble(pc->apfx[i].x),
                                          Fixed1616ToDouble(pc->apfx[i].y), quad });
                    coff += (DWORD)bytes;
                }
                EmitContour(g, ph->pfxStart, items);
                off = end;
            }
        }
    }
    return &m_glyphs.emplace(key, std::move(g)).first->second;
}

bool TextOutliner::BuildText(const std::wstring& text, const std::wstring& faceName,
                             double height, double widthFactor, double oblique,
                             std::string& d, BBox& local) {
    d.clear();
    local = BBox();
    if (text.empty() || height <= 0) return false;

    // 重复文字串在工程图里极常见 (测试 DXF: 210 个 TEXT 仅 13 种文本)
    std::string key;
    key.reserve(faceName.size() * 2 + text.size() * 2 + 24);
    key.append((const char*)faceName.data(), faceName.size() * sizeof(wchar_t));
    key.push_back('\1');
    key.append((const char*)text.data(), text.size() * sizeof(wchar_t));
    key.push_back('\1');
    const double nums[3] = { height, widthFactor, oblique };
    key.append((const char*)nums, sizeof(nums));
    auto cached = m_strings.find(key);
    if (cached != m_strings.end()) {
        d = cached->second.d;
        local = cached->second.box;
        return !d.empty();
    }

    Face* face = GetFace(faceName);
    if (!face) return false;

    const double wf = widthFactor > 0.01 ? widthFactor : 1.0;
    const double shear = oblique ? tan(oblique) : 0.0;
    const double s = height / face->cap;          // em → CAD 单位
    const double lineFeed = height * (5.0 / 3.0); // MTEXT 默认行距 (5 取 3)
    // 精度随字高走: 约 1/10000 字高, 与图纸单位无关
    int decimals = (int)lround(log10(10000.0 / height));
    if (decimals < 0) decimals = 0; else if (decimals > 7) decimals = 7;
    double gx = 0;                                // em, 已含字宽因子
    int line = 0;
    bool any = false;

    for (wchar_t ch : text) {
        if (ch == L'\n') { line++; gx = 0; continue; }
        if (ch < 0x20) continue;
        if (ch >= 0xD800 && ch <= 0xDFFF) continue;   // 代理对: CAD 文字不涉及
        const GlyphShape* g = FetchGlyph(face, ch);
        if (!g) continue;
        for (const GlyphSeg& seg : g->segs) {
            if (seg.op == 'Z') { d += " Z"; continue; }
            int npts = seg.op == 'C' ? 3 : 1;
            d += ' ';
            d += seg.op;
            for (int i = 0; i < npts; i++) {
                double u = (seg.x[i] * wf + gx + shear * seg.y[i]) * s;
                double v = line * lineFeed - seg.y[i] * s;
                if (i) d += ' ';
                AppendPathNum(d, u, decimals);
                d += ' ';
                AppendPathNum(d, v, decimals);
                local.add(u, v);
            }
            any = true;
        }
        gx += g->advance * wf;
    }
    if (!any) d.clear();
    m_strings.emplace(std::move(key), CachedText{ d, local });
    return any;
}

// ---------------------------------------------------------------------------
// 样条离散 — 弦高自适应细分
// ---------------------------------------------------------------------------
// 固定"每个控制点 N 个采样"的策略在工程图上会失控: 一张图的样条控制点总数可达
// 数万 (向量化底图/复杂轮廓), 乘以 20 后离散出上百万段, SVG 达数十 MB, 渲染耗时
// 超过加载超时而被判为打不开。这里改为按曲线自身尺寸的相对弦高来离散: 平直区间
// 一个 knot 区间只出一个点, 弯曲处自动加密, 放大若干倍仍看不出折线化。
constexpr double kSplineFlatRelTol = 1e-4;
// 单个 knot 区间最多细分 2^5 段: 实测深度 6 与 5 输出逐字节一致 (容差已收敛),
// 而向量化底图这类锯齿样条若放开深度会一路细分到上限, 该上限即其代价闸门
constexpr int kSplineFlatMaxDepth = 5;
constexpr size_t kSplineFlatMaxPts = 6000; // 单条样条硬上限, 防御畸形数据

void FlattenSegment(int degree, const std::vector<double>& knots,
                    const std::vector<double>& cx, const std::vector<double>& cy,
                    const std::vector<double>& w, double tol, int depth,
                    double u0, double x0, double y0,
                    double u1, double x1, double y1,
                    std::vector<std::pair<double, double>>& pts) {
    if (depth <= 0 || pts.size() >= kSplineFlatMaxPts) return;

    double um = 0.5 * (u0 + u1);
    double xm, ym;
    DeBoorSpline(degree, knots, cx, cy, w, um, xm, ym);

    // 弦的法向偏差取 1/4、1/2、3/4 三处最大值: 只看中点会放过中点恰好在弦上、
    // 两端却外鼓的 S 形 (带拐点) 区间
    double dx = x1 - x0, dy = y1 - y0;
    double len2 = dx * dx + dy * dy;
    auto chordDev = [&](double x, double y) {
        if (len2 <= 0.0) return std::hypot(x - x0, y - y0);
        double t = ((x - x0) * dx + (y - y0) * dy) / len2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        return std::hypot(x - (x0 + t * dx), y - (y0 + t * dy));
    };
    double dist = chordDev(xm, ym);
    if (dist <= tol) {
        double qx, qy;
        double uq1 = u0 + (u1 - u0) * 0.25;
        DeBoorSpline(degree, knots, cx, cy, w, uq1, qx, qy);
        dist = chordDev(qx, qy);
        if (dist <= tol) {
            double uq3 = u0 + (u1 - u0) * 0.75;
            DeBoorSpline(degree, knots, cx, cy, w, uq3, qx, qy);
            dist = std::max(dist, chordDev(qx, qy));
        }
    }
    if (dist <= tol) return;

    FlattenSegment(degree, knots, cx, cy, w, tol, depth - 1,
                   u0, x0, y0, um, xm, ym, pts);
    pts.push_back({xm, ym});
    FlattenSegment(degree, knots, cx, cy, w, tol, depth - 1,
                   um, xm, ym, u1, x1, y1, pts);
}

// 输出 [uMin,uMax] 上的离散点 (含端点, 按 u 升序); 空 vector 表示数据不可用
std::vector<std::pair<double, double>> FlattenSpline(
    int degree, const std::vector<double>& knots,
    const std::vector<double>& cx, const std::vector<double>& cy,
    const std::vector<double>& w) {
    std::vector<std::pair<double, double>> pts;
    if (degree < 1 || knots.size() < 2 || cx.size() < 2) return pts;

    double uMin = knots[degree];
    double uMax = knots[knots.size() - degree - 1];
    if (!(uMax > uMin)) return pts;

    // 控制点包围盒: B 样条整条曲线落在控制点多边形凸包内, 故其尺寸可作为容差基准
    double minX = cx[0], maxX = cx[0], minY = cy[0], maxY = cy[0];
    for (size_t i = 1; i < cx.size(); i++) {
        if (cx[i] < minX) minX = cx[i];
        if (cx[i] > maxX) maxX = cx[i];
        if (cy[i] < minY) minY = cy[i];
        if (cy[i] > maxY) maxY = cy[i];
    }
    // 容差必须是纯几何量: 混入参数区间会让"参数跨度大而绘图尺寸小"的样条被
    // 当成完全平直处理, 真实曲率整段丢失
    double tol = std::hypot(maxX - minX, maxY - minY) * kSplineFlatRelTol;

    // B 样条仅在 knot 处可能失去连续性, 以互异 knot 值分段
    std::vector<double> bp;
    bp.reserve(cx.size() + 1);
    bp.push_back(uMin);
    for (size_t i = degree + 1; i + degree + 1 < knots.size(); i++) {
        if (knots[i] > bp.back() && knots[i] < uMax) bp.push_back(knots[i]);
    }
    bp.push_back(uMax);

    double px, py;
    DeBoorSpline(degree, knots, cx, cy, w, bp[0], px, py);
    pts.push_back({px, py});
    std::vector<std::pair<double, double>> mid;
    for (size_t i = 1; i < bp.size(); i++) {
        double qx, qy;
        DeBoorSpline(degree, knots, cx, cy, w, bp[i], qx, qy);
        mid.clear();
        FlattenSegment(degree, knots, cx, cy, w, tol, kSplineFlatMaxDepth,
                       bp[i - 1], px, py, bp[i], qx, qy, mid);
        pts.insert(pts.end(), mid.begin(), mid.end());
        pts.push_back({qx, qy});
        px = qx;
        py = qy;
        if (pts.size() >= kSplineFlatMaxPts) break;
    }
    return pts;
}

// ---------------------------------------------------------------------------
// DwgRenderer — Dwg_Data 遍历与 SVG 生成
// ---------------------------------------------------------------------------
class DwgRenderer {
public:
    explicit DwgRenderer(Dwg_Data& dwg, bool utf16Text)
        : m_dwg(dwg), m_utf16(utf16Text) {}

    std::string Render() {
        BuildLayerTable();

        // 模型空间实体链
        Dwg_Object_Ref* mspaceRef = dwg_model_space_ref(&m_dwg);
        if (!mspaceRef || !mspaceRef->obj) {
            // 兜底: 直接遍历全部对象, 渲染属于模型空间的实体
            RenderAllObjectsFallback();
        } else {
            Dwg_Object* obj = get_first_owned_entity(mspaceRef->obj);
            while (obj) {
                if (obj->supertype == DWG_SUPERTYPE_ENTITY)
                    RenderEntity(obj);
                obj = get_next_owned_entity(mspaceRef->obj, obj);
            }
        }

        if (!m_bbox.valid) return {};

        std::string body;
        body.reserve(m_defs.str().size() + m_fills.str().size() +
                     m_body.str().size() + 32);
        body += m_defs.str();
        body += m_fills.str();
        body += m_body.str();
        return AssembleSvg(m_bbox, std::move(body));
    }

private:
    Dwg_Data& m_dwg;
    bool m_utf16;
    BBox m_bbox;
    std::ostringstream m_body;   // 线框层 (上层)
    std::ostringstream m_fills;  // 填充层 (下层, 主空间)
    std::ostringstream m_defs;   // clipPath 定义
    int m_clipId = 0;            // clipPath 自增 id

    // --- 当前渲染目标 (渲染块定义时整体切换) ---
    std::ostringstream* m_out = &m_body;
    std::ostringstream* m_fillsCur = &m_fills;
    BBox* m_bboxCur = &m_bbox;

    // --- 图层表 (key = LAYER 对象 index) ---
    struct LayerInfo {
        int aci = 7;
        unsigned rgb = 0;
        bool hasRgb = false;
        bool visible = true;
    };
    std::unordered_map<long, LayerInfo> m_layers;

    // --- 块定义缓存 (key = BLOCK_HEADER 对象 index) ---
    struct BlockDef {
        std::string svg;
        BBox bbox;
        bool valid = false;
    };
    std::unordered_map<long, BlockDef> m_blocks;
    std::unordered_set<long> m_blockActive; // 递归环保护

    // --- 文字样式表 (key = STYLE 对象指针, nullptr = 默认字体) ---
    struct StyleFont {
        std::wstring face;          // 空 → 默认 TTF
        double widthFactor = 1.0;
        double oblique = 0.0;       // 弧度
    };
    std::unordered_map<const Dwg_Object*, StyleFont> m_styles;
    TextOutliner m_text;

    // --- 当前渲染目标 (渲染块定义时切换) ---

    std::ostringstream& out() { return *m_out; }
    std::ostringstream& fills() { return *m_fillsCur; }
    void BboxAdd(double x, double y) { m_bboxCur->add(x, y); }

    // 写折线顶点序列 (模型坐标, Y 轴翻转)。first 表示尚未落任何点, 用引用传入
    // 以便多段边界续接同一条 path。容差足够小时相邻点会落到同一个 %.3f 输出值
    // 上, 这类重合点只放大路径不改外形, 直接跳过。
    void EmitFlatPoints(std::ostringstream& sink, BBox* bbox,
                        const std::vector<std::pair<double, double>>& pts,
                        bool& first) {
        long long lastIx = 0, lastIy = 0;
        for (const auto& p : pts) {
            long long ix = std::llround(p.first * 1000.0);
            long long iy = std::llround(p.second * 1000.0);
            if (!first && ix == lastIx && iy == lastIy) continue;
            if (bbox) bbox->add(p.first, p.second);
            sink << (first ? "M " : " L ") << FmtFloat(p.first) << " "
                 << FmtFloat(-p.second);
            first = false;
            lastIx = ix;
            lastIy = iy;
        }
    }

    // -----------------------------------------------------------------
    // 图层
    // -----------------------------------------------------------------
    void BuildLayerTable() {
        for (BITCODE_RL i = 0; i < m_dwg.num_objects; i++) {
            Dwg_Object* obj = &m_dwg.object[i];
            if (obj->supertype != DWG_SUPERTYPE_OBJECT) continue;
            if (obj->fixedtype != DWG_TYPE_LAYER) continue;
            Dwg_Object_LAYER* lay = obj->tio.object->tio.LAYER;
            if (!lay) continue;
            LayerInfo info;
            info.hasRgb = ColorIsTrueColor(lay->color);
            info.rgb = info.hasRgb ? (lay->color.rgb & 0xFFFFFFu) : 0;
            info.aci = lay->color.index;
            info.visible = !lay->off && !lay->frozen;
            m_layers[(long)obj->index] = info;
        }
    }

    static bool ColorIsTrueColor(const Dwg_Color& c) {
        return c.method == DWG_COLOR_METHOD_TRUECOLOR && (c.rgb & 0xFFFFFFu) != 0;
    }

    const LayerInfo* FindLayer(Dwg_Object_Entity* ent) {
        if (!ent || !ent->layer) return nullptr;
        Dwg_Object* obj = ent->layer->obj;
        if (!obj) obj = dwg_ref_object(&m_dwg, ent->layer);
        if (!obj) return nullptr;
        auto it = m_layers.find((long)obj->index);
        return (it != m_layers.end()) ? &it->second : nullptr;
    }

    bool IsVisible(Dwg_Object_Entity* ent) {
        if (!ent) return false;
        if (ent->invisible) return false;
        const LayerInfo* lay = FindLayer(ent);
        if (lay && !lay->visible) return false;
        return true;
    }

    // ACI → 预览色: 预览画布固定白底 (main.cpp whiteBg=true), ACI 7 (黑/白自适应)
    // 按白底约定渲染为黑色, 否则无图层表的精简 DXF 全部默认白色 → 白底不可见
    static std::string AciToHexPreview(int aci) {
        if (aci == 7) return "#000000";
        return ACIToHex(aci);
    }

    // 单一颜色解析 (不含 BYLAYER)
    std::string ColorToHex(const Dwg_Color& c, int fallbackAci = 7) {
        if (ColorIsTrueColor(c)) {
            char buf[8];
            snprintf(buf, sizeof(buf), "#%06X", c.rgb & 0xFFFFFFu);
            return buf;
        }
        int aci = c.index;
        if (aci == 256 || aci == 0 || c.method == DWG_COLOR_METHOD_BYLAYER ||
            c.method == DWG_COLOR_METHOD_BYBLOCK || c.method == 0) {
            aci = fallbackAci;
        }
        return AciToHexPreview(aci);
    }

    // 实体颜色解析 (含 BYLAYER → 图层色)
    std::string ResolveColor(Dwg_Object_Entity* ent) {
        const Dwg_Color& c = ent->color;
        if (ColorIsTrueColor(c)) return ColorToHex(c);
        // BYLAYER / BYBLOCK / 未指定 → 取图层颜色
        const LayerInfo* lay = FindLayer(ent);
        if (lay) {
            if (lay->hasRgb) {
                char buf[8];
                snprintf(buf, sizeof(buf), "#%06X", lay->rgb);
                return buf;
            }
            return AciToHexPreview(lay->aci);
        }
        int aci = c.index;
        if (aci == 256 || aci == 0) aci = 7;
        return AciToHexPreview(aci);
    }

    // -----------------------------------------------------------------
    // 兜底: 无模型空间引用时遍历全部对象
    // -----------------------------------------------------------------
    void RenderAllObjectsFallback() {
        for (BITCODE_RL i = 0; i < m_dwg.num_objects; i++) {
            Dwg_Object* obj = &m_dwg.object[i];
            if (obj->supertype != DWG_SUPERTYPE_ENTITY) continue;
            // 仅渲染直接归属模型空间的实体 (entmode 2 = MSPACE)
            Dwg_Object_Entity* ent = obj->tio.entity;
            if (!ent) continue;
            if (ent->entmode == 2 || (ent->entmode == 3 && IsMspaceOwner(ent)))
                RenderEntity(obj);
        }
    }

    bool IsMspaceOwner(Dwg_Object_Entity* ent) {
        if (!ent->ownerhandle) return false;
        Dwg_Object* owner = ent->ownerhandle->obj;
        if (!owner) return false;
        if (owner->fixedtype != DWG_TYPE_BLOCK_HEADER) return false;
        Dwg_Object_BLOCK_HEADER* bh = owner->tio.object->tio.BLOCK_HEADER;
        if (!bh || !bh->name) return false;
        if (m_utf16) {
            // BITCODE_TU 为 uint16_t 串, 不能用 wchar_t 版 wcsstr, 手动比较
            static const char name[] = "Model_Space";
            const BITCODE_TU w = (BITCODE_TU)bh->name;
            if (w[0] == L'*') {
                for (size_t j = 0;; j++) {
                    bool all = true;
                    for (size_t k = 0; name[k]; k++) {
                        if (w[j + k] != (unsigned char)name[k]) { all = false; break; }
                    }
                    if (all) return true;
                    if (w[j] == 0) break;
                }
            }
            return false;
        }
        return strstr(bh->name, "Model_Space") != nullptr;
    }

    // -----------------------------------------------------------------
    // 块定义渲染 (递归, 带缓存与环保护)
    // -----------------------------------------------------------------
    const BlockDef& RenderBlockDef(Dwg_Object* bhObj) {
        long key = (long)bhObj->index;
        auto it = m_blocks.find(key);
        if (it != m_blocks.end()) return it->second;

        BlockDef& def = m_blocks[key];
        if (!m_blockActive.insert(key).second) {
            return def; // 递归环, 放弃
        }

        std::ostringstream blockOut;
        std::ostringstream blockFills;
        BBox blockBbox;
        std::ostringstream* prevOut = m_out;
        std::ostringstream* prevFills = m_fillsCur;
        BBox* prevBbox = m_bboxCur;
        m_out = &blockOut;
        m_fillsCur = &blockFills;
        m_bboxCur = &blockBbox;

        Dwg_Object* obj = get_first_owned_entity(bhObj);
        while (obj) {
            if (obj->supertype == DWG_SUPERTYPE_ENTITY)
                RenderEntity(obj);
            obj = get_next_owned_entity(bhObj, obj);
        }

        m_out = prevOut;
        m_fillsCur = prevFills;
        m_bboxCur = prevBbox;
        m_blockActive.erase(key);

        // 填充在下, 线框在上
        def.svg = blockFills.str();
        def.svg += blockOut.str();
        def.bbox = blockBbox;
        def.valid = blockBbox.valid;
        return def;
    }

    // -----------------------------------------------------------------
    // 实体分发
    // -----------------------------------------------------------------
    void RenderEntity(Dwg_Object* obj) {
        Dwg_Object_Entity* ent = obj->tio.entity;
        if (!ent) return;

        switch (obj->fixedtype) {
            case DWG_TYPE_LINE:        RenderLine(ent); break;
            case DWG_TYPE_CIRCLE:      RenderCircle(ent); break;
            case DWG_TYPE_ARC:         RenderArc(ent); break;
            case DWG_TYPE_ELLIPSE:     RenderEllipse(ent); break;
            case DWG_TYPE_POINT:       RenderPoint(ent); break;
            case DWG_TYPE_LWPOLYLINE:  RenderLWPolyline(ent); break;
            case DWG_TYPE_POLYLINE_2D:
            case DWG_TYPE_POLYLINE_3D: RenderPolyline(obj); break;
            case DWG_TYPE_SPLINE:      RenderSpline(ent); break;
            case DWG_TYPE_INSERT:      RenderInsert(ent, false); break;
            case DWG_TYPE_MINSERT:     RenderInsert(ent, true); break;
            case DWG_TYPE_TEXT:        RenderText(ent); break;
            case DWG_TYPE_MTEXT:       RenderMText(ent); break;
            case DWG_TYPE_HATCH:       RenderHatch(ent); break;
            case DWG_TYPE_SOLID:
            case DWG_TYPE_TRACE:       RenderSolidQuad(ent, obj->fixedtype == DWG_TYPE_SOLID); break;
            case DWG_TYPE__3DFACE:     Render3DFace(ent); break;
            case DWG_TYPE_LEADER:      RenderLeader(ent); break;
            case DWG_TYPE_RAY:
            case DWG_TYPE_XLINE:       RenderRay(ent); break;
            case DWG_TYPE_DIMENSION_LINEAR:
            case DWG_TYPE_DIMENSION_ALIGNED:
            case DWG_TYPE_DIMENSION_ANG2LN:
            case DWG_TYPE_DIMENSION_ANG3PT:
            case DWG_TYPE_DIMENSION_DIAMETER:
            case DWG_TYPE_DIMENSION_RADIUS:
            case DWG_TYPE_DIMENSION_ORDINATE:
                RenderDimension(ent); break;
            default:
                break; // VERTEX/SEQEND/VIEWPORT/IMAGE 等跳过
        }
    }

    // -----------------------------------------------------------------
    // 基本实体
    // -----------------------------------------------------------------
    void RenderPoint(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_POINT* p = ent->tio.POINT;
        if (!p) return;
        BboxAdd(p->x, p->y);
        double r = m_bboxCur->maxDim() * 0.002;
        if (r < 0.5) r = 0.5;
        out() << "<circle cx=\"" << FmtFloat(p->x) << "\" cy=\""
              << FmtFloat(-p->y) << "\" r=\"" << FmtFloat(r)
              << "\" fill=\"" << ResolveColor(ent) << "\"/>\n";
    }

    void RenderLine(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_LINE* l = ent->tio.LINE;
        if (!l) return;
        EmitLine(l->start.x, l->start.y, l->end.x, l->end.y, ResolveColor(ent));
    }

    void EmitLine(double x1, double y1, double x2, double y2,
                  const std::string& color) {
        BboxAdd(x1, y1); BboxAdd(x2, y2);
        out() << "<line x1=\"" << FmtFloat(x1) << "\" y1=\"" << FmtFloat(-y1)
              << "\" x2=\"" << FmtFloat(x2) << "\" y2=\"" << FmtFloat(-y2)
              << "\" stroke=\"" << color
              << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void RenderRay(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_RAY* r = ent->tio.RAY;
        if (!r) return;
        double x1 = r->point.x, y1 = r->point.y;
        double dx = r->vector.x, dy = r->vector.y;
        double len = std::hypot(dx, dy);
        if (len < 1e-6) return;
        double scale = 1e6 / len;
        EmitLine(x1, y1, x1 + dx * scale, y1 + dy * scale, ResolveColor(ent));
    }

    void RenderCircle(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_CIRCLE* c = ent->tio.CIRCLE;
        if (!c || c->radius <= 0) return;
        BboxAdd(c->center.x - c->radius, c->center.y - c->radius);
        BboxAdd(c->center.x + c->radius, c->center.y + c->radius);
        out() << "<circle cx=\"" << FmtFloat(c->center.x) << "\" cy=\""
              << FmtFloat(-c->center.y) << "\" r=\"" << FmtFloat(c->radius)
              << "\" stroke=\"" << ResolveColor(ent)
              << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void RenderArc(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_ARC* a = ent->tio.ARC;
        if (!a || a->radius <= 0) return;
        double cx = a->center.x, cy = a->center.y, r = a->radius;
        double sa = a->start_angle, ea = a->end_angle;
        double x1 = cx + r * cos(sa), y1 = cy + r * sin(sa);
        double x2 = cx + r * cos(ea), y2 = cy + r * sin(ea);
        double arcAngle = ea - sa;
        if (arcAngle < 0) arcAngle += 2.0 * kPi;
        int largeArc = (arcAngle > kPi) ? 1 : 0;
        BboxAdd(x1, y1); BboxAdd(x2, y2);
        BboxAdd(cx - r, cy - r); BboxAdd(cx + r, cy + r);
        // CAD 圆弧逆时针(Y朝上) → Y翻转后顺时针 → sweep=1
        out() << "<path d=\"M " << FmtFloat(x1) << " " << FmtFloat(-y1)
              << " A " << FmtFloat(r) << " " << FmtFloat(r) << " 0 "
              << largeArc << " 1 " << FmtFloat(x2) << " " << FmtFloat(-y2)
              << "\" stroke=\"" << ResolveColor(ent)
              << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void RenderEllipse(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_ELLIPSE* e = ent->tio.ELLIPSE;
        if (!e) return;
        double cx = e->center.x, cy = e->center.y;
        double mx = e->sm_axis.x, my = e->sm_axis.y;
        double majorLen = std::hypot(mx, my);
        if (majorLen < 1e-6) return;
        double rotAngle = atan2(my, mx);
        double minorLen = majorLen * e->axis_ratio;
        double stParam = e->start_angle; // 参数角
        double enParam = e->end_angle;
        bool full = (std::abs(enParam - stParam - 2.0 * kPi) < 1e-6) ||
                    (std::abs(enParam - stParam) < 1e-6 && enParam > 6.0);

        std::string color = ResolveColor(ent);
        if (full && std::abs(rotAngle) < 1e-6) {
            BboxAdd(cx - majorLen, cy - minorLen);
            BboxAdd(cx + majorLen, cy + minorLen);
            out() << "<ellipse cx=\"" << FmtFloat(cx) << "\" cy=\""
                  << FmtFloat(-cy) << "\" rx=\"" << FmtFloat(majorLen)
                  << "\" ry=\"" << FmtFloat(minorLen)
                  << "\" stroke=\"" << color
                  << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
            return;
        }

        int numSamples = full ? 128
            : std::max(32, (int)(64 * (enParam - stParam) / (2.0 * kPi)));
        double cosR = cos(rotAngle), sinR = sin(rotAngle);
        out() << "<path d=\"";
        for (int i = 0; i <= numSamples; i++) {
            double t = stParam + (enParam - stParam) * i / numSamples;
            double ex = majorLen * cos(t), ey = minorLen * sin(t);
            double px = cx + ex * cosR - ey * sinR;
            double py = cy + ex * sinR + ey * cosR;
            BboxAdd(px, py);
            if (i == 0) out() << "M " << FmtFloat(px) << " " << FmtFloat(-py);
            else        out() << " L " << FmtFloat(px) << " " << FmtFloat(-py);
        }
        if (full) out() << " Z";
        out() << "\" stroke=\"" << color
              << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    // -----------------------------------------------------------------
    // 多段线
    // -----------------------------------------------------------------
    // 输出带 bulge 的顶点序列; bulge 位于"当前点→下一点"段上
    void EmitPolylinePath(const std::vector<double>& xs,
                          const std::vector<double>& ys,
                          const std::vector<double>& bulges,
                          bool closed, const std::string& color) {
        if (xs.empty()) return;
        out() << "<path d=\"";
        size_t n = xs.size();
        size_t segCount = closed ? n : n - 1;
        BboxAdd(xs[0], ys[0]);
        out() << "M " << FmtFloat(xs[0]) << " " << FmtFloat(-ys[0]);
        for (size_t i = 1; i <= segCount; i++) {
            double x = xs[i % n], y = ys[i % n];
            double bulge = (i - 1 < bulges.size()) ? bulges[i - 1] : 0.0;
            double px = xs[(i - 1) % n], py = ys[(i - 1) % n];
            BboxAdd(x, y);
            if (std::abs(bulge) > 1e-6) {
                double dx = x - px, dy = y - py;
                double dist = std::hypot(dx, dy);
                if (dist > 1e-6) {
                    double theta = 4.0 * std::atan(std::abs(bulge));
                    double r = dist / (2.0 * std::sin(theta / 2.0));
                    int largeArc = (theta > kPi) ? 1 : 0;
                    // 正bulge=逆时针(Y朝上) → Y翻转后顺时针 → sweep=1
                    int sweep = (bulge > 0) ? 1 : 0;
                    out() << " A " << FmtFloat(r) << " " << FmtFloat(r)
                          << " 0 " << largeArc << " " << sweep
                          << " " << FmtFloat(x) << " " << FmtFloat(-y);
                    continue;
                }
            }
            out() << " L " << FmtFloat(x) << " " << FmtFloat(-y);
        }
        if (closed) out() << " Z";
        out() << "\" stroke=\"" << color
              << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void RenderLWPolyline(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_LWPOLYLINE* pl = ent->tio.LWPOLYLINE;
        if (!pl || !pl->points || pl->num_points == 0) return;
        bool closed = (pl->flag & 512) != 0; // DWG: 512=closed (DXF 70 位1)
        std::vector<double> xs, ys, bulges;
        xs.reserve(pl->num_points);
        ys.reserve(pl->num_points);
        for (BITCODE_BL i = 0; i < pl->num_points; i++) {
            xs.push_back(pl->points[i].x);
            ys.push_back(pl->points[i].y);
        }
        if (pl->bulges && pl->num_bulges > 0) {
            for (BITCODE_BL i = 0; i < pl->num_bulges; i++)
                bulges.push_back(pl->bulges[i]);
        }
        EmitPolylinePath(xs, ys, bulges, closed, ResolveColor(ent));
    }

    void RenderPolyline(Dwg_Object* obj) {
        Dwg_Object_Entity* ent = obj->tio.entity;
        if (!ent || !IsVisible(ent)) return;
        Dwg_Entity_POLYLINE_2D* pl = ent->tio.POLYLINE_2D;
        if (!pl) return;
        bool closed = (pl->flag & 1) != 0;
        std::vector<double> xs, ys, bulges;
        // DWG: 引用数组已填 → 按引用取顶点
        if (pl->vertex && pl->num_owned > 0) {
            for (BITCODE_BL i = 0; i < pl->num_owned; i++) {
                Dwg_Object_Ref* ref = pl->vertex[i];
                if (!ref) continue;
                Dwg_Object* vobj = ref->obj ? ref->obj : dwg_ref_object(&m_dwg, ref);
                if (!vobj || vobj->supertype != DWG_SUPERTYPE_ENTITY) continue;
                Dwg_Object_Entity* vent = vobj->tio.entity;
                if (!vent) continue;
                if (vobj->fixedtype == DWG_TYPE_VERTEX_2D ||
                    vobj->fixedtype == DWG_TYPE_VERTEX_3D ||
                    vobj->fixedtype == DWG_TYPE_VERTEX_MESH ||
                    vobj->fixedtype == DWG_TYPE_VERTEX_PFACE) {
                    Dwg_Entity_VERTEX_2D* v = vent->tio.VERTEX_2D;
                    if (!v) continue;
                    xs.push_back(v->point.x);
                    ys.push_back(v->point.y);
                    // VERTEX_3D 无 bulge 字段, 仅 VERTEX_2D 有效
                    bulges.push_back(vobj->fixedtype == DWG_TYPE_VERTEX_2D
                                         ? v->bulge : 0.0);
                }
            }
        }
        // DXF (LibreDWG dxf_read_file): vertex 引用数组不填, VERTEX 也无 ownerhandle。
        // DXF 文件顺序保证 VERTEX 紧跟其 POLYLINE、以 SEQEND 结束 → 顺序扫描兜底。
        if (xs.size() < 2) {
            xs.clear(); ys.clear(); bulges.clear();
            BITCODE_RL start = (BITCODE_RL)(obj - m_dwg.object) + 1;
            for (BITCODE_RL i = start; i < m_dwg.num_objects; i++) {
                Dwg_Object* vobj = &m_dwg.object[i];
                if (vobj->fixedtype == DWG_TYPE_SEQEND) break;
                if (vobj->supertype != DWG_SUPERTYPE_ENTITY) break;
                if (vobj->fixedtype != DWG_TYPE_VERTEX_2D &&
                    vobj->fixedtype != DWG_TYPE_VERTEX_3D &&
                    vobj->fixedtype != DWG_TYPE_VERTEX_MESH &&
                    vobj->fixedtype != DWG_TYPE_VERTEX_PFACE) break;
                Dwg_Object_Entity* vent = vobj->tio.entity;
                if (!vent) continue;
                Dwg_Entity_VERTEX_2D* v = vent->tio.VERTEX_2D;
                if (!v) continue;
                xs.push_back(v->point.x);
                ys.push_back(v->point.y);
                bulges.push_back(vobj->fixedtype == DWG_TYPE_VERTEX_2D
                                     ? v->bulge : 0.0);
            }
        }
        if (xs.size() < 2) return;
        EmitPolylinePath(xs, ys, bulges, closed, ResolveColor(ent));
    }

    // -----------------------------------------------------------------
    // 样条
    // -----------------------------------------------------------------
    void RenderSpline(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_SPLINE* sp = ent->tio.SPLINE;
        if (!sp) return;

        std::string color = ResolveColor(ent);

        // scenario 2: 仅控制点(bezier); 有拟合点而无控制点时用拟合点连线
        // 防护: knots/ctrl_pts 需非空, degree 需满足 knots[degree] 与
        // knots[num_knots-degree-1] 均在界内 (畸形文件防御)
        if (sp->num_ctrl_pts >= 2 && sp->num_knots >= 2 && sp->knots && sp->ctrl_pts) {
            int degree = sp->degree;
            if (degree < 1) degree = 3;
            if (degree > (int)sp->num_knots - 2) degree = (int)sp->num_knots - 2;
            if (degree >= 1) {
                std::vector<double> knots(sp->knots, sp->knots + sp->num_knots);
                std::vector<double> cx, cy, w;
                cx.reserve(sp->num_ctrl_pts);
                cy.reserve(sp->num_ctrl_pts);
                for (BITCODE_BL i = 0; i < sp->num_ctrl_pts; i++) {
                    cx.push_back(sp->ctrl_pts[i].x);
                    cy.push_back(sp->ctrl_pts[i].y);
                    w.push_back(sp->ctrl_pts[i].w);
                }
                bool rational = sp->rational && !w.empty();
                static const std::vector<double> kNoWeights;
                std::vector<std::pair<double, double>> pts = FlattenSpline(
                    degree, knots, cx, cy, rational ? w : kNoWeights);
                if (pts.size() >= 2) {
                    out() << "<path d=\"";
                    bool first = true;
                    EmitFlatPoints(out(), m_bboxCur, pts, first);
                    out() << "\" stroke=\"" << color
                          << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
                    return;
                }
            }
        }
        if (sp->num_fit_pts >= 2 && sp->fit_pts) {
            out() << "<path d=\"";
            for (BITCODE_BL i = 0; i < sp->num_fit_pts; i++) {
                double px = sp->fit_pts[i].x, py = sp->fit_pts[i].y;
                BboxAdd(px, py);
                if (i == 0) out() << "M " << FmtFloat(px) << " " << FmtFloat(-py);
                else        out() << " L " << FmtFloat(px) << " " << FmtFloat(-py);
            }
            out() << "\" stroke=\"" << color
                  << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
        }
    }

    // -----------------------------------------------------------------
    // 块引用 (INSERT/MINSERT)
    // -----------------------------------------------------------------
    void RenderInsert(Dwg_Object_Entity* ent, bool /*minsert*/) {
        if (!IsVisible(ent)) return;
        // INSERT 与 MINSERT 字段布局一致
        Dwg_Entity_INSERT* ins = ent->tio.INSERT;
        if (!ins || !ins->block_header) return;
        Dwg_Object* bhObj = ins->block_header->obj;
        if (!bhObj) bhObj = dwg_ref_object(&m_dwg, ins->block_header);
        if (!bhObj || bhObj->fixedtype != DWG_TYPE_BLOCK_HEADER) return;

        const BlockDef& block = RenderBlockDef(bhObj);
        if (block.svg.empty()) return;

        double px = ins->ins_pt.x, py = ins->ins_pt.y;
        double sx = ins->scale.x, sy = ins->scale.y;
        double angle_deg = ins->rotation * 180.0 / kPi;
        double ang = ins->rotation;
        int cols = ins->num_cols > 0 ? ins->num_cols : 1;
        int rows = ins->num_rows > 0 ? ins->num_rows : 1;

        for (int col = 0; col < cols; col++) {
            for (int row = 0; row < rows; row++) {
                double ox = px + col * ins->col_spacing;
                double oy = py + row * ins->row_spacing;
                out() << "<g transform=\"translate(" << FmtFloat(ox) << ","
                      << FmtFloat(-oy) << ")";
                if (std::abs(sx - 1.0) > 1e-9 || std::abs(sy - 1.0) > 1e-9)
                    out() << " scale(" << FmtFloat(sx) << "," << FmtFloat(sy)
                          << ")";
                if (std::abs(angle_deg) > 0.001)
                    out() << " rotate(" << FmtFloat(-angle_deg) << ")";
                out() << "\">\n" << block.svg << "</g>\n";

                if (block.valid) {
                    double cosA = cos(ang), sinA = sin(ang);
                    double corners[4][2] = {
                        {block.bbox.minX * sx, block.bbox.minY * sy},
                        {block.bbox.maxX * sx, block.bbox.minY * sy},
                        {block.bbox.minX * sx, block.bbox.maxY * sy},
                        {block.bbox.maxX * sx, block.bbox.maxY * sy}
                    };
                    for (auto& c : corners) {
                        double rx = c[0] * cosA + c[1] * sinA;
                        double ry = -c[0] * sinA + c[1] * cosA;
                        BboxAdd(ox + rx, oy + ry);
                    }
                } else {
                    BboxAdd(ox, oy);
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // 文本 → 轮廓
    // -----------------------------------------------------------------
    // SHX/PS 等线字体无 TrueType 轮廓, 返回空让调用方退回默认 TTF
    static std::wstring FaceFromFileName(const std::string& file) {
        if (file.empty()) return {};
        size_t slash = file.find_last_of("\\/");
        std::string stem = (slash == std::string::npos) ? file : file.substr(slash + 1);
        size_t dot = stem.find_last_of('.');
        if (dot == std::string::npos) return Utf8ToWide(stem);
        std::string ext = stem.substr(dot + 1);
        for (auto& c : ext) c = (char)towlower((unsigned char)c);
        if (ext == "shx" || ext == "mxs" || ext == "shz" || ext == "pfb"
            || ext == "pfm" || ext == "afm")
            return {};
        return Utf8ToWide(stem.substr(0, dot));
    }

    // DIMSTYLE 等非 STYLE 的句柄一律退回默认字体
    const StyleFont& ResolveStyle(BITCODE_H ref) {
        Dwg_Object* obj = ref ? ref->obj : nullptr;
        if (!obj && ref) obj = dwg_ref_object(&m_dwg, ref);
        if (!obj || obj->fixedtype != DWG_TYPE_STYLE) {
            obj = nullptr;
        }
        auto hit = m_styles.find(obj);
        if (hit != m_styles.end()) return hit->second;

        StyleFont sf;
        if (obj && obj->tio.object && obj->tio.object->tio.STYLE) {
            Dwg_Object_STYLE* st = obj->tio.object->tio.STYLE;
            if (st->width_factor > 0.01) sf.widthFactor = st->width_factor;
            sf.oblique = st->oblique_angle;
            if (!(st->flag & 32))   // is_shx
                sf.face = FaceFromFileName(DwgTextToUtf8(st->font_file, m_utf16));
        }
        return m_styles.emplace(obj, sf).first->second;
    }

    // hAlign: 0 左 1 中 2 右; vAlign: 0 基线 1 下 2 中 3 上
    void EmitText(double x, double y, double h, double rotDeg, const StyleFont& sf,
                  int hAlign, int vAlign, const std::string& utf8,
                  const std::string& color) {
        if (utf8.empty() || h <= 0) { BboxAdd(x, y); return; }
        std::string d;
        BBox local;
        if (!m_text.BuildText(Utf8ToWide(utf8), sf.face, h, sf.widthFactor,
                              sf.oblique, d, local)) {
            BboxAdd(x, y);
            return;
        }
        double dx = hAlign == 1 ? -(local.minX + local.maxX) / 2
                                : (hAlign == 2 ? -local.maxX : 0.0);
        double dv = vAlign == 1 ? -local.maxY
                    : vAlign == 2 ? -(local.minY + local.maxY) / 2
                    : vAlign == 3 ? -local.minY : 0.0;

        // 局部 (u,v): u 沿基线, v 朝下 → CAD 偏移 (u·cosθ + v·sinθ, u·sinθ − v·cosθ)
        double rad = rotDeg * kPi / 180.0;
        double c = cos(rad), s = sin(rad);
        double tx = x + dx * c + dv * s;
        double ty = y + dx * s - dv * c;

        out() << "<g transform=\"translate(" << FmtFloat(tx) << "," << FmtFloat(-ty)
              << ")";
        if (std::abs(rotDeg) > 0.01)
            out() << " rotate(" << FmtFloat(-rotDeg) << ")";
        out() << "\"><path d=\"" << d << "\" fill=\"" << color
              << "\" stroke=\"none\"/></g>\n";

        const double corners[4][2] = {
            {local.minX, local.minY}, {local.maxX, local.minY},
            {local.minX, local.maxY}, {local.maxX, local.maxY}
        };
        for (auto& p : corners)
            BboxAdd(tx + p[0] * c + p[1] * s, ty + p[0] * s - p[1] * c);
    }

    void RenderText(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_TEXT* t = ent->tio.TEXT;
        if (!t) return;
        double h = t->height > 0 ? t->height : 2.5;
        int ha = t->horiz_alignment, va = t->vert_alignment;
        // 对齐非默认时, DXF 11 才是真正的锚点
        double ax = t->ins_pt.x, ay = t->ins_pt.y;
        if ((ha || va)
            && (std::abs(t->alignment_pt.x) > 1e-6 || std::abs(t->alignment_pt.y) > 1e-6)) {
            ax = t->alignment_pt.x;
            ay = t->alignment_pt.y;
        }
        if (ha == 4) { ha = 1; va = 2; }          // Middle
        if (ha == 3 || ha == 5) ha = 0;           // Aligned / Fit
        if (ha > 2) ha = 0;
        if (va > 3) va = 0;

        StyleFont sf = ResolveStyle(t->style);
        if (t->width_factor > 0.01) sf.widthFactor = t->width_factor;
        if (t->oblique_angle) sf.oblique = t->oblique_angle;
        EmitText(ax, ay, h, t->rotation * 180.0 / kPi, sf, ha, va,
                 DwgTextToUtf8(t->text_value, m_utf16), ResolveColor(ent));
    }

    void RenderMText(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_MTEXT* t = ent->tio.MTEXT;
        if (!t) return;
        double h = t->text_height > 0 ? t->text_height : 2.5;
        double rotDeg = atan2(t->x_axis_dir.y, t->x_axis_dir.x) * 180.0 / kPi;
        // \P 先换成真实换行, 再剥其余格式码 (顺序不可颠倒)
        std::string raw = DwgTextToUtf8(t->text, m_utf16);
        for (size_t i = 0; i + 1 < raw.size(); i++) {
            if (raw[i] == '\\' && (raw[i + 1] == 'P' || raw[i + 1] == 'p')) {
                raw[i] = '\n';
                raw.erase(i + 1, 1);
            }
        }
        int att = (t->attachment >= 1 && t->attachment <= 9) ? t->attachment : 1;
        int ha = (att - 1) % 3;                    // 0 左 1 中 2 右
        int row = (att - 1) / 3;                   // 0 上 1 中 2 下
        int va = row == 0 ? 3 : row == 1 ? 2 : 1;
        EmitText(t->ins_pt.x, t->ins_pt.y, h, rotDeg, ResolveStyle(t->style),
                 ha, va, StripMTextFormatting(raw), ResolveColor(ent));
    }

    // -----------------------------------------------------------------
    // 填充四边形 (SOLID=实心填充, TRACE=填充描边)
    // -----------------------------------------------------------------
    void RenderSolidQuad(Dwg_Object_Entity* ent, bool filled) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_SOLID* s = ent->tio.SOLID;
        if (!s) return;
        // SOLID 顶点顺序: 1,2,4,3 (AutoCAD 之字形)
        double px[4] = {s->corner1.x, s->corner2.x, s->corner4.x, s->corner3.x};
        double py[4] = {s->corner1.y, s->corner2.y, s->corner4.y, s->corner3.y};
        for (int i = 0; i < 4; i++) BboxAdd(px[i], py[i]);
        std::string color = ResolveColor(ent);
        std::ostringstream& target = filled ? fills() : *m_out;
        target << "<polygon points=\""
               << FmtFloat(px[0]) << "," << FmtFloat(-py[0]) << " "
               << FmtFloat(px[1]) << "," << FmtFloat(-py[1]) << " "
               << FmtFloat(px[2]) << "," << FmtFloat(-py[2]) << " "
               << FmtFloat(px[3]) << "," << FmtFloat(-py[3])
               << "\" fill=\"" << color << "\"";
        if (filled)
            target << " stroke=\"none\"/>\n";
        else
            target << " stroke=\"" << color
                   << "\" stroke-width=\"__SW__\"/>\n";
    }

    void Render3DFace(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity__3DFACE* f = ent->tio._3DFACE;
        if (!f) return;
        double px[4] = {f->corner1.x, f->corner2.x, f->corner3.x, f->corner4.x};
        double py[4] = {f->corner1.y, f->corner2.y, f->corner3.y, f->corner4.y};
        for (int i = 0; i < 4; i++) BboxAdd(px[i], py[i]);
        out() << "<polygon points=\""
              << FmtFloat(px[0]) << "," << FmtFloat(-py[0]) << " "
              << FmtFloat(px[1]) << "," << FmtFloat(-py[1]) << " "
              << FmtFloat(px[2]) << "," << FmtFloat(-py[2]) << " "
              << FmtFloat(px[3]) << "," << FmtFloat(-py[3])
              << "\" stroke=\"" << ResolveColor(ent)
              << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    // -----------------------------------------------------------------
    // 引线
    // -----------------------------------------------------------------
    void RenderLeader(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_LEADER* l = ent->tio.LEADER;
        if (!l || !l->points || l->num_points < 2) return;
        out() << "<path d=\"";
        for (BITCODE_BL i = 0; i < l->num_points; i++) {
            double x = l->points[i].x, y = l->points[i].y;
            BboxAdd(x, y);
            if (i == 0) out() << "M " << FmtFloat(x) << " " << FmtFloat(-y);
            else        out() << " L " << FmtFloat(x) << " " << FmtFloat(-y);
        }
        out() << "\" stroke=\"" << ResolveColor(ent)
              << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    // -----------------------------------------------------------------
    // 尺寸标注 (简化: 渲染文字)
    // -----------------------------------------------------------------
    void RenderDimension(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        // 所有 DIMENSION 实体公共前缀布局一致, 统一按 LINEAR 布局读取
        Dwg_Entity_DIMENSION_LINEAR* d = ent->tio.DIMENSION_LINEAR;
        if (!d) return;
        double x = d->text_midpt.x, y = d->text_midpt.y;
        if (std::abs(x) < 1e-6 && std::abs(y) < 1e-6) {
            x = d->def_pt.x; y = d->def_pt.y;
        }
        double h = 2.5;
        std::string txt = DwgTextToUtf8(d->user_text, m_utf16);
        if (txt.empty()) txt = "<>";
        // DIMENSION 的句柄是 DIMSTYLE, 不是文字样式 → 用默认字体
        EmitText(x, y, h, d->text_rotation * 180.0 / kPi, ResolveStyle(nullptr),
                 1, 2, txt, ResolveColor(ent));
    }

    // -----------------------------------------------------------------
    // HATCH 填充 (核心: 实心填充与图案填充分开处理)
    // -----------------------------------------------------------------
    void RenderHatch(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_HATCH* h = ent->tio.HATCH;
        if (!h || !h->paths || h->num_paths == 0) return;

        std::string color = ResolveColor(ent);
        BBox hatchBbox;
        std::ostringstream pathSS;

        for (BITCODE_BL p = 0; p < h->num_paths; p++) {
            const Dwg_HATCH_Path& path = h->paths[p];
            bool firstPoint = true;

            if ((path.flag & 2) && path.polyline_paths) {
                // 多段线边界路径
                BITCODE_BL n = path.num_segs_or_paths;
                for (BITCODE_BL i = 0; i < n; i++) {
                    const Dwg_HATCH_PolylinePath& pp = path.polyline_paths[i];
                    double x = pp.point.x, y = pp.point.y;
                    hatchBbox.add(x, y);
                    if (firstPoint) {
                        pathSS << "M " << FmtFloat(x) << " " << FmtFloat(-y)
                               << " ";
                        firstPoint = false;
                    } else {
                        double bulge =
                            path.bulges_present ? pp.bulge : 0.0;
                        if (std::abs(bulge) > 1e-6 && i > 0) {
                            const Dwg_HATCH_PolylinePath& prev =
                                path.polyline_paths[i - 1];
                            double dx = x - prev.point.x,
                                   dy = y - prev.point.y;
                            double dist = std::hypot(dx, dy);
                            if (dist > 1e-6) {
                                double theta =
                                    4.0 * std::atan(std::abs(bulge));
                                double r =
                                    dist / (2.0 * std::sin(theta / 2.0));
                                int largeArc = (theta > kPi) ? 1 : 0;
                                int sweep = (bulge > 0) ? 1 : 0;
                                pathSS << "A " << FmtFloat(r) << " "
                                       << FmtFloat(r) << " 0 " << largeArc
                                       << " " << sweep << " "
                                       << FmtFloat(x) << " " << FmtFloat(-y)
                                       << " ";
                                continue;
                            }
                        }
                        pathSS << "L " << FmtFloat(x) << " " << FmtFloat(-y)
                               << " ";
                    }
                }
            } else if (path.segs) {
                // 分段边界路径 (线/圆弧/椭圆弧/样条)
                for (BITCODE_BL i = 0; i < path.num_segs_or_paths; i++) {
                    const Dwg_HATCH_PathSeg& seg = path.segs[i];
                    switch (seg.curve_type) {
                        case 1: { // 线
                            double x1 = seg.first_endpoint.x,
                                   y1 = seg.first_endpoint.y;
                            double x2 = seg.second_endpoint.x,
                                   y2 = seg.second_endpoint.y;
                            hatchBbox.add(x1, y1);
                            hatchBbox.add(x2, y2);
                            if (firstPoint) {
                                pathSS << "M " << FmtFloat(x1) << " "
                                       << FmtFloat(-y1) << " ";
                                firstPoint = false;
                            }
                            pathSS << "L " << FmtFloat(x2) << " "
                                   << FmtFloat(-y2) << " ";
                            break;
                        }
                        case 2: { // 圆弧
                            double cx = seg.center.x, cy = seg.center.y,
                                   r = seg.radius;
                            if (r <= 0) break;
                            double sa = seg.start_angle, ea = seg.end_angle;
                            double x1 = cx + r * cos(sa),
                                   y1 = cy + r * sin(sa);
                            double x2 = cx + r * cos(ea),
                                   y2 = cy + r * sin(ea);
                            hatchBbox.add(x1, y1);
                            hatchBbox.add(x2, y2);
                            hatchBbox.add(cx - r, cy - r);
                            hatchBbox.add(cx + r, cy + r);
                            if (firstPoint) {
                                pathSS << "M " << FmtFloat(x1) << " "
                                       << FmtFloat(-y1) << " ";
                                firstPoint = false;
                            }
                            double arcAngle = ea - sa;
                            if (arcAngle < 0) arcAngle += 2.0 * kPi;
                            int largeArc = (arcAngle > kPi) ? 1 : 0;
                            // CCW(Y朝上)翻转后顺时针→sweep=1; CW则0
                            int sweep = seg.is_ccw ? 1 : 0;
                            pathSS << "A " << FmtFloat(r) << " " << FmtFloat(r)
                                   << " 0 " << largeArc << " " << sweep << " "
                                   << FmtFloat(x2) << " " << FmtFloat(-y2)
                                   << " ";
                            break;
                        }
                        case 3: { // 椭圆弧 (center + 主轴向量 + 比率)
                            double cx = seg.center.x, cy = seg.center.y;
                            double mx = seg.endpoint.x, my = seg.endpoint.y;
                            double majorLen = std::hypot(mx, my);
                            if (majorLen < 1e-6) break;
                            double rot = atan2(my, mx);
                            double minorLen = majorLen * seg.minor_major_ratio;
                            double cosR = cos(rot), sinR = sin(rot);
                            int numSamples = 32;
                            for (int k = 0; k <= numSamples; k++) {
                                double t = seg.start_angle +
                                           (seg.end_angle - seg.start_angle) *
                                               k / numSamples;
                                double ex = majorLen * cos(t);
                                double ey = minorLen * sin(t);
                                double px = cx + ex * cosR - ey * sinR;
                                double py = cy + ex * sinR + ey * cosR;
                                hatchBbox.add(px, py);
                                if (firstPoint) {
                                    pathSS << "M " << FmtFloat(px) << " "
                                           << FmtFloat(-py) << " ";
                                    firstPoint = false;
                                } else {
                                    pathSS << "L " << FmtFloat(px) << " "
                                           << FmtFloat(-py) << " ";
                                }
                            }
                            break;
                        }
                        case 4: { // 样条
                            if (seg.num_control_points >= 2 &&
                                seg.num_knots >= 2 && seg.knots &&
                                seg.control_points) {
                                int degree = seg.degree;
                                if (degree < 1) degree = 3;
                                if (degree > (int)seg.num_knots - 2)
                                    degree = (int)seg.num_knots - 2;
                                std::vector<double> knots(
                                    seg.knots, seg.knots + seg.num_knots);
                                std::vector<double> cxv, cyv, wv;
                                for (BITCODE_BL k = 0;
                                     k < seg.num_control_points; k++) {
                                    cxv.push_back(
                                        seg.control_points[k].point.x);
                                    cyv.push_back(
                                        seg.control_points[k].point.y);
                                    wv.push_back(seg.control_points[k].weight);
                                }
                                std::vector<std::pair<double, double>> pts =
                                    FlattenSpline(degree, knots, cxv, cyv, wv);
                                EmitFlatPoints(pathSS, &hatchBbox, pts,
                                               firstPoint);
                            } else if (seg.num_fitpts >= 2 && seg.fitpts) {
                                for (BITCODE_BL k = 0; k < seg.num_fitpts;
                                     k++) {
                                    double px = seg.fitpts[k].x,
                                           py = seg.fitpts[k].y;
                                    hatchBbox.add(px, py);
                                    if (firstPoint) {
                                        pathSS << "M " << FmtFloat(px) << " "
                                               << FmtFloat(-py) << " ";
                                        firstPoint = false;
                                    } else {
                                        pathSS << "L " << FmtFloat(px) << " "
                                               << FmtFloat(-py) << " ";
                                    }
                                }
                            }
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
            if (!firstPoint && !(path.flag & 0x20)) // is_open 不闭合
                pathSS << "Z ";
        }

        std::string pathD = pathSS.str();
        if (pathD.length() < 3) return;

        // bbox 汇总
        if (hatchBbox.valid) {
            m_bboxCur->add(hatchBbox.minX, hatchBbox.minY);
            m_bboxCur->add(hatchBbox.maxX, hatchBbox.maxY);
        }

        if (h->is_solid_fill) {
            // 实心填充 → 底层
            fills() << "<path d=\"" << pathD << "\" fill=\"" << color
                    << "\" fill-rule=\"evenodd\" stroke=\"none\"/>\n";
        } else {
            // 图案填充 → clipPath 裁剪的填充定义线 (底层)
            if (h->num_deflines > 0 && h->deflines && hatchBbox.valid) {
                int clipId = ++m_clipId;
                m_defs << "<clipPath id=\"h" << clipId << "\"><path d=\""
                       << pathD << "\"/></clipPath>\n";
                fills() << "<g clip-path=\"url(#h" << clipId
                        << ")\" stroke=\"" << color
                        << "\" stroke-width=\"__SW__\">\n";
                double big = hatchBbox.maxDim() * 1.5 + 1.0;
                for (int dl = 0; dl < h->num_deflines; dl++) {
                    const Dwg_HATCH_DefLine& defl = h->deflines[dl];
                    double aRad = defl.angle * kPi / 180.0;
                    double dx = cos(aRad), dy = sin(aRad);
                    // 定义线步进向量 (offset 在图案局部坐标系)
                    double sxv = defl.offset.x * dx - defl.offset.y * dy;
                    double syv = defl.offset.x * dy + defl.offset.y * dx;
                    double stepLen = std::hypot(sxv, syv);
                    double bx = defl.pt0.x, by = defl.pt0.y;
                    if (stepLen > 1e-9) {
                        // 覆盖 hatch bbox 的线数范围
                        double ux = sxv / stepLen, uy = syv / stepLen;
                        double corners[4][2] = {
                            {hatchBbox.minX, hatchBbox.minY},
                            {hatchBbox.maxX, hatchBbox.minY},
                            {hatchBbox.minX, hatchBbox.maxY},
                            {hatchBbox.maxX, hatchBbox.maxY}};
                        double minProj = 1e18, maxProj = -1e18;
                        for (auto& c : corners) {
                            double pr = (c[0] - bx) * ux + (c[1] - by) * uy;
                            if (pr < minProj) minProj = pr;
                            if (pr > maxProj) maxProj = pr;
                        }
                        int kMin = (int)std::floor(minProj / stepLen) - 1;
                        int kMax = (int)std::ceil(maxProj / stepLen) + 1;
                        for (int k = kMin; k <= kMax; k++) {
                            double lx = bx + k * sxv, ly = by + k * syv;
                            fills() << "<line x1=\""
                                    << FmtFloat(lx - dx * big) << "\" y1=\""
                                    << FmtFloat(-(ly - dy * big))
                                    << "\" x2=\"" << FmtFloat(lx + dx * big)
                                    << "\" y2=\""
                                    << FmtFloat(-(ly + dy * big)) << "\"/>\n";
                        }
                    } else {
                        fills() << "<line x1=\"" << FmtFloat(bx - dx * big)
                                << "\" y1=\"" << FmtFloat(-(by - dy * big))
                                << "\" x2=\"" << FmtFloat(bx + dx * big)
                                << "\" y2=\"" << FmtFloat(-(by + dy * big))
                                << "\"/>\n";
                    }
                }
                fills() << "</g>\n";
            }
            // 边界描边 (线框层, 确保非关联填充边界可见)
            out() << "<path d=\"" << pathD << "\" stroke=\"" << color
                  << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
        }
    }
};

} // namespace

// ============================================================================
// 通用解析+渲染: dwg_read_memory 直接解析内存缓冲 (无临时文件中转)
// ============================================================================
static std::string LoadCadToSVG(const uint8_t* data, size_t size, bool isDwg) {
    if (!data || size == 0) return {};

    std::string result;
    {
        Dwg_Data dwg;
        memset(&dwg, 0, sizeof(dwg));
        const auto t0 = std::chrono::steady_clock::now();
        int error = dwg_read_memory(data, size, &dwg);
        const int readMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
        // DWG_ERR_CRITICAL=128, 所有 >=128 的位均为致命错误
        if (!(static_cast<unsigned>(error) & ~0x7Fu)) {
            // 文本编码: 仅二进制 DWG R2007+ 为 UTF-16;
            // DXF 即使 R2007+ 也是 UTF-8 (LocalBytesToUtf8 会保留) 或旧代码页
            bool utf16Text = isDwg && dwg.header.version >= R_2007;
            const auto t1 = std::chrono::steady_clock::now();
            DwgRenderer renderer(dwg, utf16Text);
            result = renderer.Render();
            const int renderMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t1).count();
            char diag[192];
            snprintf(diag, sizeof(diag),
                     "[CAD-DIAG] %s split: libredwg_read=%d ms svg_render=%d ms\n",
                     isDwg ? "DWG" : "DXF", readMs, renderMs);
            OutputDebugStringA(diag);
        }
        dwg_free(&dwg);
    }

    return result;
}

// DXF (AutoCAD) → SVG (via LibreDWG dwg_read_memory)
std::string LoadDXFtoSVG(const uint8_t* data, size_t size) {
    return LoadCadToSVG(data, size, false);
}

// ----------------------------------------------------------------------------
// DWG 异步超时包装: 大文件/不兼容版本解析可能耗时数十秒甚至无限卡住
// 策略: 在后台线程执行解析, 调用线程等待最多 kDwgTimeoutMs 毫秒
// 超时后置位取消标志, 后台解析在下一个对象检查点中止 (DWG_ERR_CANCELLED),
// 不再空跑到结束; 返回空字符串
// 注意: 项目禁用异常(/EHs-c-), 不使用 try/catch
// ----------------------------------------------------------------------------
static constexpr int kDwgTimeoutMs = 8000;  // DWG 解析超时阈值

namespace {
struct DwgCancelContext {
    std::atomic<bool> cancelled{ false };
};
int DwgCancelCheck(void* ctx) {
    return static_cast<DwgCancelContext*>(ctx)->cancelled.load(
               std::memory_order_relaxed) ? 1 : 0;
}
} // namespace

std::string LoadDWGtoSVG(const uint8_t* data, size_t size) {
    // 快速路径: 空文件直接返回
    if (!data || size == 0) return {};

    auto cancelCtx = std::make_shared<DwgCancelContext>();

    // 使用 shared_future 避免 future 析构时阻塞
    // std::async 返回的 future 析构会阻塞等待任务完成,
    // 转为 shared_future 后析构不阻塞
    std::future<std::string> asyncFuture = std::async(
        std::launch::async, [data, size, cancelCtx]() -> std::string {
            std::string result;
            Dwg_Data dwg;
            memset(&dwg, 0, sizeof(dwg));
            dwg.cancel_check = &DwgCancelCheck;
            dwg.hook_ctx = cancelCtx.get();
            int error = dwg_read_memory(data, size, &dwg);
            if (!(static_cast<unsigned>(error) & ~0x7Fu)) {
                DwgRenderer renderer(dwg, dwg.header.version >= R_2007);
                result = renderer.Render();
            }
            dwg_free(&dwg);
            return result;
        });

    // 转移共享状态到 shared_future (asyncFuture 变为空)
    std::shared_future<std::string> future = asyncFuture.share();

    // 等待结果或超时
    if (future.wait_for(std::chrono::milliseconds(kDwgTimeoutMs)) ==
        std::future_status::timeout) {
        // 超时: 通知后台解析中止并返回空字符串
        // shared_future 析构不阻塞; 后台线程在下一检查点退出
        cancelCtx->cancelled.store(true, std::memory_order_relaxed);
        return {};
    }

    // 正常完成
    return future.get();
}

} // namespace QuickView
