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

namespace QuickView {
namespace {

constexpr double kPi = 3.14159265358979;

// ---------------------------------------------------------------------------
// 文本处理
// ---------------------------------------------------------------------------

// XML 转义, 保留合法 UTF-8 多字节序列 (r2007+ 文本来源)
std::string EscapeXmlKeepUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                    out += '?';
                } else {
                    out += (char)c; // 含合法 UTF-8 多字节与 ASCII
                }
        }
    }
    return out;
}

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

// BITCODE_T → 已转义 XML 文本 (保留合法 UTF-8 多字节)
std::string DwgTextToXml(BITCODE_T s, bool utf16) {
    return EscapeXmlKeepUtf8(DwgTextToUtf8(s, utf16));
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

    // --- 当前渲染目标 (渲染块定义时切换) ---

    std::ostringstream& out() { return *m_out; }
    std::ostringstream& fills() { return *m_fillsCur; }
    void BboxAdd(double x, double y) { m_bboxCur->add(x, y); }

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
                double uMin = knots[degree];
                double uMax = knots[sp->num_knots - degree - 1];
                if (uMax <= uMin) uMax = uMin + 1.0;
                int numSamples = std::max(100, (int)sp->num_ctrl_pts * 20);
                double du = (uMax - uMin) / numSamples;
                out() << "<path d=\"";
                for (int i = 0; i <= numSamples; i++) {
                    double u = (i == numSamples) ? uMax : (uMin + du * i);
                    double px, py;
                    DeBoorSpline(degree, knots, cx, cy,
                                 rational ? w : std::vector<double>(), u, px, py);
                    BboxAdd(px, py);
                    if (i == 0) out() << "M " << FmtFloat(px) << " " << FmtFloat(-py);
                    else        out() << " L " << FmtFloat(px) << " " << FmtFloat(-py);
                }
                out() << "\" stroke=\"" << color
                      << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
                return;
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
                out() << ">\n" << block.svg << "</g>\n";

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
    // 文本
    // -----------------------------------------------------------------
    void EmitText(double x, double y, double h, double rotationDeg,
                  const std::string& xmlText, const std::string& color) {
        BboxAdd(x, y);
        out() << "<text x=\"" << FmtFloat(x) << "\" y=\"" << FmtFloat(-y)
              << "\" font-size=\"" << FmtFloat(h) << "\" fill=\"" << color
              << "\"";
        if (std::abs(rotationDeg) > 0.01)
            out() << " transform=\"rotate(" << FmtFloat(-rotationDeg) << ","
                  << FmtFloat(x) << "," << FmtFloat(-y) << ")\"";
        out() << ">" << xmlText << "</text>\n";
    }

    void RenderText(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_TEXT* t = ent->tio.TEXT;
        if (!t) return;
        double h = t->height > 0 ? t->height : 2.5;
        EmitText(t->ins_pt.x, t->ins_pt.y, h,
                 t->rotation * 180.0 / kPi,
                 DwgTextToXml(t->text_value, m_utf16), ResolveColor(ent));
    }

    void RenderMText(Dwg_Object_Entity* ent) {
        if (!IsVisible(ent)) return;
        Dwg_Entity_MTEXT* t = ent->tio.MTEXT;
        if (!t) return;
        double h = t->text_height > 0 ? t->text_height : 2.5;
        double rotDeg = atan2(t->x_axis_dir.y, t->x_axis_dir.x) * 180.0 / kPi;
        // 先转 UTF-8 → 剥格式码 → 再 XML 转义 (顺序不可颠倒)
        std::string raw = DwgTextToUtf8(t->text, m_utf16);
        std::string txt = EscapeXmlKeepUtf8(StripMTextFormatting(raw));
        EmitText(t->ins_pt.x, t->ins_pt.y, h, rotDeg, txt, ResolveColor(ent));
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
        std::string txt = DwgTextToXml(d->user_text, m_utf16);
        if (txt.empty()) txt = "&lt;&gt;";
        EmitText(x, y, h, d->text_rotation * 180.0 / kPi, txt,
                 ResolveColor(ent));
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
                                double uMin = knots[degree];
                                double uMax =
                                    knots[seg.num_knots - degree - 1];
                                if (uMax <= uMin) uMax = uMin + 1.0;
                                int numSamples =
                                    std::max(50, (int)cxv.size() * 10);
                                double du = (uMax - uMin) / numSamples;
                                for (int k = 0; k <= numSamples; k++) {
                                    double u = (k == numSamples)
                                                   ? uMax
                                                   : (uMin + du * k);
                                    double px, py;
                                    DeBoorSpline(degree, knots, cxv, cyv, wv,
                                                 u, px, py);
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
// 通用解析+渲染: 调用方指定 dxf_read_file 或 dwg_read_file
// ============================================================================
static std::string LoadCadToSVG(const uint8_t* data, size_t size,
                                 int (*readFn)(const char*, Dwg_Data*)) {
    if (!data || size == 0) return {};

    std::string tempPath = WriteTempFile(data, size, "qvcad");
    if (tempPath.empty()) return {};

    std::string result;
    {
        Dwg_Data dwg;
        memset(&dwg, 0, sizeof(dwg));
        int error = readFn(tempPath.c_str(), &dwg);
        // DWG_ERR_CRITICAL=128, 所有 >=128 的位均为致命错误
        if (!(static_cast<unsigned>(error) & ~0x7Fu)) {
            // 文本编码: 仅二进制 DWG R2007+ 为 UTF-16;
            // DXF 即使 R2007+ 也是 UTF-8 (LocalBytesToUtf8 会保留) 或旧代码页
            bool utf16Text = (readFn == dwg_read_file) &&
                             dwg.header.version >= R_2007;
            DwgRenderer renderer(dwg, utf16Text);
            result = renderer.Render();
        }
        dwg_free(&dwg);
    }
    DeleteFileA(tempPath.c_str());

    return result;
}

// DXF (AutoCAD) → SVG (via LibreDWG dxf_read_file)
std::string LoadDXFtoSVG(const uint8_t* data, size_t size) {
    return LoadCadToSVG(data, size, dxf_read_file);
}

// ----------------------------------------------------------------------------
// DWG 异步超时包装: dwg_read_file 对大文件/不兼容版本可能耗时数十秒甚至无限卡住
// 策略: 在后台线程执行解析, 主线程等待最多 kDwgTimeoutMs 毫秒
// 超时后返回空字符串(后台线程继续运行, 结果被忽略)
// 注意: 项目禁用异常(/EHs-c-), 不使用 try/catch
// ----------------------------------------------------------------------------
static constexpr int kDwgTimeoutMs = 8000;  // DWG 解析超时阈值

std::string LoadDWGtoSVG(const uint8_t* data, size_t size) {
    // 快速路径: 空文件直接返回
    if (!data || size == 0) return {};

    // 使用 shared_future 避免 future 析构时阻塞
    // std::async 返回的 future 析构会阻塞等待任务完成,
    // 转为 shared_future 后析构不阻塞
    std::future<std::string> asyncFuture = std::async(
        std::launch::async, [data, size]() -> std::string {
            return LoadCadToSVG(data, size, dwg_read_file);
        });

    // 转移共享状态到 shared_future (asyncFuture 变为空)
    std::shared_future<std::string> future = asyncFuture.share();

    // 等待结果或超时
    if (future.wait_for(std::chrono::milliseconds(kDwgTimeoutMs)) ==
        std::future_status::timeout) {
        // 超时: 返回空字符串
        // shared_future 析构不阻塞, 后台线程继续运行至完成
        return {};
    }

    // 正常完成
    return future.get();
}

} // namespace QuickView
