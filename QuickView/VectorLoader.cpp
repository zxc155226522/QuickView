// ============================================================================
// VectorLoader.cpp - PLT(HPGL) / DXF(AutoCAD) / DWG(AutoCAD) → SVG XML 转换器
// ============================================================================
// PLT: 手写 HPGL 解析器
// DXF/DWG: libdxfrw 库 (DRW_Interface 回调式)，支持全部实体类型
// Y 轴翻转策略: 直接在坐标中取负 (y' = -y)
// viewBox 从实际绘制坐标计算，不依赖文件头元数据。
// stroke-width 按坐标系比例设置，避免大坐标系下线宽不可见。
// 非 ASCII 文本用 SanitizeXmlText 净化，防止 D2D XML 解析器遇到
// GBK 编码的中文等非 UTF-8 字节导致整个文档解析失败。
// ============================================================================
#include "pch.h"
#include "VectorLoader.h"
#include <cmath>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <string_view>
#include <unordered_map>
// libdxfrw: DXF/DWG 解析库 (安装到 include/libdxfrw/)
#include <libdxfrw.h>
#include <libdwgr.h>

namespace QuickView {

// ============================================================================
// 通用工具
// ============================================================================

static std::string FmtFloat(double v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", v);
    char* dot = strchr(buf, '.');
    if (dot) {
        char* end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (*end == '.') *end = '\0';
    }
    return buf;
}

// 简易 BBox 跟踪器
struct BBox {
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    bool valid = false;

    void add(double x, double y) {
        if (x < minX) minX = x;
        if (y < minY) minY = y;
        if (x > maxX) maxX = x;
        if (y > maxY) maxY = y;
        valid = true;
    }

    double width() const { return valid ? (maxX - minX) : 0; }
    double height() const { return valid ? (maxY - minY) : 0; }
    double maxDim() const { return std::max(width(), height()); }
};

// 按坐标系比例计算线宽 (下限 0.05，确保可见且最细化)
static std::string CalcStrokeWidth(double maxDim) {
    if (maxDim <= 0) return "0.5";
    double sw = maxDim * 0.0003; // 0.03% of max dimension
    if (sw < 0.05) sw = 0.05;
    return FmtFloat(sw);
}

// XML 文本净化: 转义特殊字符 + 替换非 ASCII 字节为 '?'
// D2D 的 SVG 解析器要求 UTF-8，GBK 编码的中文等非 ASCII 字节会导致解析失败
static std::string SanitizeXmlText(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                    out += '?'; // 控制字符
                } else if (c > 0x7E) {
                    out += '?'; // 非 ASCII (GBK 中文等)
                } else {
                    out += (char)c;
                }
        }
    }
    return out;
}

// ============================================================================
// PLT (HPGL) 解析器
// ============================================================================
// 坐标单位: 0.025mm，Y轴朝上 → SVG Y朝下: y' = -y
// ============================================================================

static const char* kPLTPenColors[] = {
    "black", "black", "red", "red", "green",
    "blue", "magenta", "cyan", "black"
};
static constexpr int kPLTMaxPen = 8;

static void ParseHPGLCoords(std::string_view args, std::vector<double>& xs,
                            std::vector<double>& ys) {
    xs.clear();
    ys.clear();
    size_t i = 0;
    const size_t n = args.size();
    bool expectY = false;
    while (i < n) {
        while (i < n && (args[i] == ',' || args[i] == ' ' || args[i] == '\r' ||
                         args[i] == '\n' || args[i] == '\t'))
            i++;
        if (i >= n) break;
        std::string numStr;
        while (i < n && args[i] != ',' && args[i] != ' ' && args[i] != '\r' &&
               args[i] != '\n' && args[i] != '\t') {
            numStr += args[i++];
        }
        if (numStr.empty()) break;
        char* endp = nullptr;
        double val = strtod(numStr.c_str(), &endp);
        if (endp == numStr.c_str()) break;
        if (!expectY) { xs.push_back(val); expectY = true; }
        else          { ys.push_back(val); expectY = false; }
    }
    if (xs.size() > ys.size()) ys.push_back(0.0);
}

std::string LoadPLTtoSVG(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};

    std::string_view text(reinterpret_cast<const char*>(data), size);

    int currentPen = 1;
    bool penDown = false;
    double curX = 0, curY = 0; // 原始 HPGL 坐标
    BBox bbox;

    // 收集所有 path 元素（用原始坐标），最后统一翻转 Y
    // 两遍策略: 第一遍计算 bbox，第二遍生成 path（stroke-width 已知）
    // 但为简单起见，用占位 stroke-width，最终替换
    std::ostringstream paths;
    int activePen = -1;

    auto flushPath = [&]() {
        if (activePen >= 0) {
            const char* color = (activePen >= 0 && activePen <= kPLTMaxPen)
                                    ? kPLTPenColors[activePen] : "black";
            paths << "\" stroke=\"" << color
                  << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
            activePen = -1;
        }
    };

    auto startPath = [&](int pen) {
        flushPath();
        paths << "<path d=\"";
        activePen = pen;
    };

    // 更新包围盒 (原始坐标)
    auto updateBBox = [&](double x, double y) {
        bbox.add(x, y);
    };

    // 输出 M/L 命令时翻转 Y: y' = -y
    auto outM = [&](double x, double y) {
        paths << "M " << FmtFloat(x) << " " << FmtFloat(-y) << " ";
        updateBBox(x, y); // 起始点也纳入 bbox
    };
    auto outL = [&](double x, double y) {
        paths << "L " << FmtFloat(x) << " " << FmtFloat(-y) << " ";
        updateBBox(x, y);
    };

    size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() &&
               (text[pos] == ';' || text[pos] == ' ' || text[pos] == '\r' ||
                text[pos] == '\n' || text[pos] == '\t'))
            pos++;
        if (pos >= text.size()) break;

        if (pos + 1 >= text.size()) break;
        char cmdBuf[3] = {0};
        cmdBuf[0] = (char)toupper((unsigned char)text[pos]);
        cmdBuf[1] = (char)toupper((unsigned char)text[pos + 1]);
        cmdBuf[2] = '\0';
        pos += 2;

        // 找参数结束（分号或下一个两字母命令）
        size_t argStart = pos;
        while (pos < text.size() && text[pos] != ';') {
            if (isalpha((unsigned char)text[pos]) && pos > argStart) {
                if (pos + 1 < text.size() && isalpha((unsigned char)text[pos + 1]))
                    break;
            }
            pos++;
        }
        std::string_view args(text.data() + argStart, pos - argStart);

        if (strcmp(cmdBuf, "IN") == 0) {
            flushPath();
            penDown = false;
            curX = curY = 0;
            currentPen = 1;
        } else if (strcmp(cmdBuf, "SP") == 0) {
            flushPath();
            char* endp = nullptr;
            std::string penStr(args);
            int pen = (int)strtol(penStr.c_str(), &endp, 10);
            if (pen >= 0 && pen <= kPLTMaxPen) currentPen = pen;
            penDown = false;
        } else if (strcmp(cmdBuf, "PU") == 0) {
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);
            if (!xs.empty() && !ys.empty()) {
                flushPath();
                curX = xs[0];
                curY = ys[0];
            }
            penDown = false;
        } else if (strcmp(cmdBuf, "PD") == 0) {
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);
            if (xs.empty()) {
                penDown = true;
            } else {
                if (activePen != currentPen) {
                    startPath(currentPen);
                    outM(curX, curY);
                }
                for (size_t i = 0; i < xs.size() && i < ys.size(); i++) {
                    outL(xs[i], ys[i]);
                    curX = xs[i]; curY = ys[i];
                }
                penDown = true;
            }
        } else if (strcmp(cmdBuf, "PA") == 0) {
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);
            if (!xs.empty() && !ys.empty()) {
                if (penDown) {
                    if (activePen != currentPen) {
                        startPath(currentPen);
                        outM(curX, curY);
                    }
                    for (size_t i = 0; i < xs.size() && i < ys.size(); i++) {
                        outL(xs[i], ys[i]);
                        curX = xs[i]; curY = ys[i];
                    }
                } else {
                    curX = xs[0]; curY = ys[0];
                }
            }
        } else if (strcmp(cmdBuf, "CI") == 0) {
            char* endp = nullptr;
            std::string rStr(args);
            double r = strtod(rStr.c_str(), &endp);
            if (r > 0) {
                if (activePen != currentPen) startPath(currentPen);
                // y' = -y for all coordinates
                paths << "M " << FmtFloat(curX - r) << " " << FmtFloat(-curY)
                      << " A " << FmtFloat(r) << " " << FmtFloat(r)
                      << " 0 1 0 " << FmtFloat(curX + r) << " " << FmtFloat(-curY)
                      << " A " << FmtFloat(r) << " " << FmtFloat(r)
                      << " 0 1 0 " << FmtFloat(curX - r) << " " << FmtFloat(-curY)
                      << " ";
                updateBBox(curX - r, curY - r);
                updateBBox(curX + r, curY + r);
            }
        } else if (strcmp(cmdBuf, "AA") == 0) {
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);
            if (xs.size() >= 3 && ys.size() >= 1) {
                double cx = xs[0], cy = ys[0];
                double angle = xs[2];
                double r = std::hypot(curX - cx, curY - cy);
                if (r > 0 && std::abs(angle) > 0.01) {
                    if (activePen != currentPen) {
                        startPath(currentPen);
                        outM(curX, curY);
                    }
                    double endAngle = atan2(curY - cy, curX - cx) +
                                      angle * 3.14159265358979 / 180.0;
                    double ex = cx + r * cos(endAngle);
                    double ey = cy + r * sin(endAngle);
                    int largeArc = (std::abs(angle) > 180.0) ? 1 : 0;
                    int sweep = (angle > 0) ? 1 : 0;
                    // 圆弧也需要翻转 Y: 但 SVG arc 的 sweep 方向在翻转后需反转
                    paths << "A " << FmtFloat(r) << " " << FmtFloat(r)
                          << " 0 " << largeArc << " " << (1 - sweep)
                          << " " << FmtFloat(ex) << " " << FmtFloat(-ey) << " ";
                    curX = ex; curY = ey;
                    updateBBox(curX, curY);
                    updateBBox(cx - r, cy - r);
                    updateBBox(cx + r, cy + r);
                }
            }
        }
    }

    flushPath();
    if (!bbox.valid) return {};

    // 翻转后的 Y 范围: [-maxY, -minY]
    double margin = bbox.maxDim() * 0.02; // 2% margin
    if (margin < 1.0) margin = 1.0;
    double vbX = bbox.minX - margin;
    double vbY = -bbox.maxY - margin;       // 翻转后 Y 最小值
    double vbW = bbox.width() + margin * 2;
    double vbH = bbox.height() + margin * 2;
    if (vbW <= 0) vbW = 100;
    if (vbH <= 0) vbH = 100;

    // 替换占位 stroke-width
    std::string strokeW = CalcStrokeWidth(bbox.maxDim());
    std::string pathsStr = paths.str();
    {
        const std::string placeholder = "\" stroke-width=\"__SW__\"";
        const std::string replacement = "\" stroke-width=\"" + strokeW + "\"";
        size_t p = 0;
        while ((p = pathsStr.find(placeholder, p)) != std::string::npos) {
            pathsStr.replace(p, placeholder.size(), replacement);
            p += replacement.size();
        }
    }

    std::ostringstream svg;
    svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<svg xmlns=\"http://www.w3.org/2000/svg\""
        << " viewBox=\"" << FmtFloat(vbX) << " " << FmtFloat(vbY) << " "
        << FmtFloat(vbW) << " " << FmtFloat(vbH) << "\""
        << " width=\"" << FmtFloat(vbW) << "\" height=\"" << FmtFloat(vbH)
        << "\">\n"
        << pathsStr
        << "</svg>";

    std::string result = svg.str();
    return result;
}

// ============================================================================
// DXF/DWG (AutoCAD) 解析器 — via libdxfrw
// ============================================================================
// 使用 libdxfrw 库解析 DXF (ASCII/Binary) 和 DWG 文件。
// 通过继承 DRW_Interface 回调接口，在回调中生成 SVG XML 元素。
// 支持全部实体类型: LINE/CIRCLE/ARC/ELLIPSE/SPLINE/LWPOLYLINE/POLYLINE/
// INSERT/BLOCK/HATCH/DIMENSION/LEADER/TEXT/MTEXT/POINT/3DFACE/SOLID/TRACE
// 支持图层系统(颜色/可见性)、块引用(含嵌套)。
// Y轴翻转: y' = -y (DXF Y朝上 → SVG Y朝下)
// ============================================================================

// --- ACI 颜色表 ---
static const char* kACIStandard[] = {
    "#000000",  // 0 ByBlock
    "#FF0000",  // 1 Red
    "#FFFF00",  // 2 Yellow
    "#00FF00",  // 3 Green
    "#00FFFF",  // 4 Cyan
    "#0000FF",  // 5 Blue
    "#FF00FF",  // 6 Magenta
    "#FFFFFF",  // 7 White
    "#808080",  // 8 Gray
    "#C0C0C0"   // 9 Light Gray
};

static std::string ACIToHex(int aci) {
    if (aci < 0) aci = -aci;
    if (aci == 256 || aci == 0) return "#000000";
    if (aci >= 1 && aci <= 9) return kACIStandard[aci];
    if (aci >= 250 && aci <= 255) {
        int g = 60 + (aci - 250) * 30;
        char buf[8];
        snprintf(buf, sizeof(buf), "#%02X%02X%02X", g, g, g);
        return buf;
    }
    // 10-249: 简易 HSV→RGB 近似
    double h = ((aci - 10) % 24) * 15.0;
    double s = 0.8, v = 0.8;
    double c = v * s;
    double x = c * (1 - std::abs(std::fmod(h / 60.0, 2) - 1));
    double m = v - c;
    double r, g, b;
    if (h < 60)       { r=c; g=x; b=0; }
    else if (h < 120) { r=x; g=c; b=0; }
    else if (h < 180) { r=0; g=c; b=x; }
    else if (h < 240) { r=0; g=x; b=c; }
    else if (h < 300) { r=x; g=0; b=c; }
    else              { r=c; g=0; b=x; }
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             (int)((r+m)*255), (int)((g+m)*255), (int)((b+m)*255));
    return buf;
}

// --- De Boor 算法 (用于样条曲线采样) ---
static void DeBoorSpline(
    int degree,
    const std::vector<double>& knots,
    const std::vector<double>& ctrlX,
    const std::vector<double>& ctrlY,
    const std::vector<double>& weights,
    double u, double& outX, double& outY)
{
    int n = (int)ctrlX.size() - 1;
    int span = n + degree;
    for (int i = degree; i <= n + degree; i++) {
        if (u >= knots[i] && u < knots[i + 1]) { span = i; break; }
        if (i == n + degree) span = i;
    }
    std::vector<double> dx(degree + 1), dy(degree + 1), dw(degree + 1);
    bool hasWeights = !weights.empty();
    for (int j = 0; j <= degree; j++) {
        int idx = span - degree + j;
        if (idx < 0) idx = 0;
        if (idx >= (int)ctrlX.size()) idx = (int)ctrlX.size() - 1;
        dx[j] = ctrlX[idx]; dy[j] = ctrlY[idx];
        dw[j] = hasWeights ? weights[idx] : 1.0;
    }
    for (int r = 1; r <= degree; r++) {
        for (int j = degree; j >= r; j--) {
            int i = span - degree + j;
            double denom = knots[i + degree - r + 1] - knots[i];
            if (denom == 0.0) denom = 1.0;
            double alpha = (u - knots[i]) / denom;
            if (hasWeights) {
                double w0 = dw[j - 1], w1 = dw[j];
                double wn = (1.0 - alpha) * w0 + alpha * w1;
                dx[j] = ((1.0 - alpha) * dx[j - 1] * w0 + alpha * dx[j] * w1) /
                        (wn != 0.0 ? wn : 1.0);
                dy[j] = ((1.0 - alpha) * dy[j - 1] * w0 + alpha * dy[j] * w1) /
                        (wn != 0.0 ? wn : 1.0);
                dw[j] = wn;
            } else {
                dx[j] = (1.0 - alpha) * dx[j - 1] + alpha * dx[j];
                dy[j] = (1.0 - alpha) * dy[j - 1] + alpha * dy[j];
            }
        }
    }
    outX = dx[degree]; outY = dy[degree];
}

// --- MTEXT 格式码剥离 ---
static std::string StripMTextFormatting(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inBrace = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{') { inBrace = true; continue; }
        if (s[i] == '}') { inBrace = false; continue; }
        if (inBrace) {
            if (s[i] == '\\') {
                i++;
                while (i < s.size() && s[i] != ';' && s[i] != '\\') i++;
            }
            continue;
        }
        if (s[i] == '\\') {
            if (i + 1 < s.size()) {
                char c = s[i + 1];
                if (c == 'P' || c == 'p') { out += ' '; i++; continue; }
                if (c == 'S' || c == 's') {
                    i += 2;
                    while (i < s.size() && s[i] != ';') i++;
                    continue;
                }
                if (c == '\\' || c == '{' || c == '}') {
                    out += c; i++; continue;
                }
                i++;
                while (i < s.size() && s[i] != ';') i++;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// --- 临时文件写入 (dxfRW/dwgR 构造函数接受 const char* 文件名) ---
static std::string WriteTempFile(const uint8_t* data, size_t size) {
    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0) return "";
    char tempFile[MAX_PATH];
    if (GetTempFileNameA(tempPath, "qvdxf", 0, tempFile) == 0) return "";
    FILE* fp = nullptr;
    fopen_s(&fp, tempFile, "wb");
    if (!fp) return "";
    fwrite(data, 1, size, fp);
    fclose(fp);
    return tempFile;
}

// ============================================================================
// SVGDRWInterface — DRW_Interface 子类，在回调中生成 SVG XML
// ============================================================================
class SVGDRWInterface : public DRW_Interface {
public:
    SVGDRWInterface() = default;
    ~SVGDRWInterface() override = default;

    std::string getResult() const {
        if (!m_bbox.valid) return "";
        double margin = m_bbox.maxDim() * 0.02;
        if (margin < 1.0) margin = 1.0;
        double vbX = m_bbox.minX - margin;
        double vbY = -m_bbox.maxY - margin;
        double vbW = m_bbox.width() + margin * 2;
        double vbH = m_bbox.height() + margin * 2;
        if (vbW <= 0) vbW = 100;
        if (vbH <= 0) vbH = 100;

        std::string strokeW = CalcStrokeWidth(m_bbox.maxDim());
        std::string bodyStr = m_body.str();
        {
            const std::string placeholder = "\" stroke-width=\"__SW__\"";
            const std::string replacement = "\" stroke-width=\"" + strokeW + "\"";
            size_t p = 0;
            while ((p = bodyStr.find(placeholder, p)) != std::string::npos) {
                bodyStr.replace(p, placeholder.size(), replacement);
                p += replacement.size();
            }
        }

        std::ostringstream svg;
        svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            << "<svg xmlns=\"http://www.w3.org/2000/svg\""
            << " viewBox=\"" << FmtFloat(vbX) << " " << FmtFloat(vbY) << " "
            << FmtFloat(vbW) << " " << FmtFloat(vbH) << "\""
            << " width=\"" << FmtFloat(vbW) << "\" height=\"" << FmtFloat(vbH)
            << "\">\n"
            << bodyStr
            << "</svg>";
        return svg.str();
    }

    // --- Header / Tables (最小化实现) ---
    void addHeader(const DRW_Header*) override {}
    void addLType(const DRW_LType&) override {}
    void addLayer(const DRW_Layer& data) override {
        LayerInfo info;
        info.color = data.color;
        info.visible = (data.color >= 0);
        m_layers[data.name] = info;
    }
    void addDimStyle(const DRW_Dimstyle&) override {}
    void addVport(const DRW_Vport&) override {}
    void addTextStyle(const DRW_Textstyle&) override {}
    void addAppId(const DRW_AppId&) override {}

    // --- Block 系统 ---
    void addBlock(const DRW_Block& data) override {
        m_blockHandleToName[data.handle] = data.name;
        m_currentBlockName = data.name;
        if (data.name == "*Model_Space" || data.name == "*Paper_Space") {
            m_inBlockDef = false;
        } else {
            m_inBlockDef = true;
            m_blockBody.str("");
            m_blockBody.clear();
            m_blockBBox = BBox();
        }
    }
    void setBlock(const int handle) override {
        auto it = m_blockHandleToName.find(handle);
        if (it != m_blockHandleToName.end()) {
            m_currentBlockName = it->second;
            if (it->second == "*Model_Space" || it->second == "*Paper_Space") {
                m_inBlockDef = false;
            } else if (!m_inBlockDef) {
                m_inBlockDef = true;
                m_blockBody.str("");
                m_blockBody.clear();
                m_blockBBox = BBox();
            }
        }
    }
    void endBlock() override {
        if (m_inBlockDef && !m_currentBlockName.empty()) {
            BlockDef def;
            def.svg = m_blockBody.str();
            def.bbox = m_blockBBox;
            m_blocks[m_currentBlockName] = def;
        }
        m_inBlockDef = false;
    }

    // --- 实体回调 ---
    void addPoint(const DRW_Point& data) override {
        if (!isVisible(data)) return;
        double x = data.basePoint.x, y = data.basePoint.y;
        bboxAdd(x, y);
        double r = m_bbox.maxDim() * 0.002;
        if (r < 0.5) r = 0.5;
        emit() << "<circle cx=\"" << FmtFloat(x) << "\" cy=\"" << FmtFloat(-y)
               << "\" r=\"" << FmtFloat(r) << "\" fill=\"" << resolveColor(data)
               << "\"/>\n";
    }

    void addLine(const DRW_Line& data) override {
        if (!isVisible(data)) return;
        double x1 = data.basePoint.x, y1 = data.basePoint.y;
        double x2 = data.secPoint.x, y2 = data.secPoint.y;
        bboxAdd(x1, y1); bboxAdd(x2, y2);
        emit() << "<line x1=\"" << FmtFloat(x1) << "\" y1=\"" << FmtFloat(-y1)
               << "\" x2=\"" << FmtFloat(x2) << "\" y2=\"" << FmtFloat(-y2)
               << "\" stroke=\"" << resolveColor(data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void addRay(const DRW_Ray& data) override {
        if (!isVisible(data)) return;
        double x1 = data.basePoint.x, y1 = data.basePoint.y;
        double dx = data.secPoint.x - x1, dy = data.secPoint.y - y1;
        double len = std::hypot(dx, dy);
        if (len < 1e-6) return;
        double scale = 1e6 / len;
        double x2 = x1 + dx * scale, y2 = y1 + dy * scale;
        bboxAdd(x1, y1);
        emit() << "<line x1=\"" << FmtFloat(x1) << "\" y1=\"" << FmtFloat(-y1)
               << "\" x2=\"" << FmtFloat(x2) << "\" y2=\"" << FmtFloat(-y2)
               << "\" stroke=\"" << resolveColor(data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }
    void addXline(const DRW_Xline& data) override { addRay(data); }

    void addCircle(const DRW_Circle& data) override {
        if (!isVisible(data)) return;
        double cx = data.basePoint.x, cy = data.basePoint.y, r = data.radious;
        if (r <= 0) return;
        bboxAdd(cx - r, cy - r); bboxAdd(cx + r, cy + r);
        emit() << "<circle cx=\"" << FmtFloat(cx) << "\" cy=\""
               << FmtFloat(-cy) << "\" r=\"" << FmtFloat(r)
               << "\" stroke=\"" << resolveColor(data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void addArc(const DRW_Arc& data) override {
        if (!isVisible(data)) return;
        double cx = data.basePoint.x, cy = data.basePoint.y, r = data.radious;
        if (r <= 0) return;
        double sa = data.staangle, ea = data.endangle;
        double x1 = cx + r * cos(sa), y1 = cy + r * sin(sa);
        double x2 = cx + r * cos(ea), y2 = cy + r * sin(ea);
        double arcAngle = ea - sa;
        if (arcAngle < 0) arcAngle += 2.0 * 3.14159265358979;
        int largeArc = (arcAngle > 3.14159265358979) ? 1 : 0;
        // DXF ARC 始终逆时针(Y朝上) → Y翻转后变顺时针 → sweep=1
        bboxAdd(x1, y1); bboxAdd(x2, y2);
        bboxAdd(cx - r, cy - r); bboxAdd(cx + r, cy + r);
        emit() << "<path d=\"M " << FmtFloat(x1) << " " << FmtFloat(-y1)
               << " A " << FmtFloat(r) << " " << FmtFloat(r) << " 0 "
               << largeArc << " 1 " << FmtFloat(x2) << " " << FmtFloat(-y2)
               << "\" stroke=\"" << resolveColor(data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void addEllipse(const DRW_Ellipse& data) override {
        if (!isVisible(data)) return;
        double cx = data.basePoint.x, cy = data.basePoint.y;
        // secPoint 是主轴向量(从中心到端点)
        double mx = data.secPoint.x, my = data.secPoint.y;
        double majorLen = std::hypot(mx, my);
        if (majorLen < 1e-6) return;
        double rotAngle = atan2(my, mx);
        double minorLen = majorLen * data.ratio;
        double stParam = data.staparam;
        double enParam = data.endparam;
        bool full = (std::abs(stParam) < 1e-6 && std::abs(enParam - 2.0 * 3.14159265358979) < 1e-6);

        if (full && std::abs(rotAngle) < 1e-6) {
            // 无旋转的完整椭圆
            bboxAdd(cx - majorLen, cy - minorLen);
            bboxAdd(cx + majorLen, cy + minorLen);
            emit() << "<ellipse cx=\"" << FmtFloat(cx) << "\" cy=\""
                   << FmtFloat(-cy) << "\" rx=\"" << FmtFloat(majorLen)
                   << "\" ry=\"" << FmtFloat(minorLen)
                   << "\" stroke=\"" << resolveColor(data)
                   << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
        } else {
            // 采样路径(支持旋转和椭圆弧)
            int numSamples = full ? 128 : std::max(32, (int)(64 * (enParam - stParam) / (2.0 * 3.14159265358979)));
            double cosR = cos(rotAngle), sinR = sin(rotAngle);
            emit() << "<path d=\"";
            for (int i = 0; i <= numSamples; i++) {
                double t = stParam + (enParam - stParam) * i / numSamples;
                double ex = majorLen * cos(t);
                double ey = minorLen * sin(t);
                // 旋转
                double px = cx + ex * cosR - ey * sinR;
                double py = cy + ex * sinR + ey * cosR;
                bboxAdd(px, py);
                if (i == 0) emit() << "M " << FmtFloat(px) << " " << FmtFloat(-py);
                else        emit() << " L " << FmtFloat(px) << " " << FmtFloat(-py);
            }
            if (full) emit() << " Z";
            emit() << "\" stroke=\"" << resolveColor(data)
                   << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
        }
    }

    void addLWPolyline(const DRW_LWPolyline& data) override {
        if (!isVisible(data)) return;
        const auto& vl = data.vertlist;
        if (vl.empty()) return;
        bool closed = (data.flags & 1) != 0;
        emit() << "<path d=\"";
        for (size_t i = 0; i < vl.size(); i++) {
            double x = vl[i]->x, y = vl[i]->y;
            bboxAdd(x, y);
            if (i == 0) {
                emit() << "M " << FmtFloat(x) << " " << FmtFloat(-y);
            } else {
                double bulge = vl[i - 1]->bulge;
                if (std::abs(bulge) > 1e-6) {
                    // 弧段: bulge = tan(θ/4)
                    double px = vl[i - 1]->x, py = vl[i - 1]->y;
                    double dx = x - px, dy = y - py;
                    double dist = std::hypot(dx, dy);
                    if (dist > 1e-6) {
                        double theta = 4.0 * std::atan(std::abs(bulge));
                        double r = dist / (2.0 * std::sin(theta / 2.0));
                        int largeArc = (theta > 3.14159265358979) ? 1 : 0;
                        // 正bulge=逆时针(Y朝上) → Y翻转后顺时针 → sweep=1
                        int sweep = (bulge > 0) ? 1 : 0;
                        emit() << " A " << FmtFloat(r) << " " << FmtFloat(r)
                               << " 0 " << largeArc << " " << sweep
                               << " " << FmtFloat(x) << " " << FmtFloat(-y);
                    } else {
                        emit() << " L " << FmtFloat(x) << " " << FmtFloat(-y);
                    }
                } else {
                    emit() << " L " << FmtFloat(x) << " " << FmtFloat(-y);
                }
            }
        }
        if (closed) emit() << " Z";
        emit() << "\" stroke=\"" << resolveColor(data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void addPolyline(const DRW_Polyline& data) override {
        if (!isVisible(data)) return;
        const auto& vl = data.vertlist;
        if (vl.empty()) return;
        bool closed = (data.flags & 1) != 0;
        emit() << "<path d=\"";
        for (size_t i = 0; i < vl.size(); i++) {
            double x = vl[i]->basePoint.x, y = vl[i]->basePoint.y;
            bboxAdd(x, y);
            if (i == 0) {
                emit() << "M " << FmtFloat(x) << " " << FmtFloat(-y);
            } else {
                double bulge = vl[i - 1]->bulge;
                if (std::abs(bulge) > 1e-6) {
                    double px = vl[i - 1]->basePoint.x, py = vl[i - 1]->basePoint.y;
                    double dx = x - px, dy = y - py;
                    double dist = std::hypot(dx, dy);
                    if (dist > 1e-6) {
                        double theta = 4.0 * std::atan(std::abs(bulge));
                        double r = dist / (2.0 * std::sin(theta / 2.0));
                        int largeArc = (theta > 3.14159265358979) ? 1 : 0;
                        int sweep = (bulge > 0) ? 1 : 0;
                        emit() << " A " << FmtFloat(r) << " " << FmtFloat(r)
                               << " 0 " << largeArc << " " << sweep
                               << " " << FmtFloat(x) << " " << FmtFloat(-y);
                    } else {
                        emit() << " L " << FmtFloat(x) << " " << FmtFloat(-y);
                    }
                } else {
                    emit() << " L " << FmtFloat(x) << " " << FmtFloat(-y);
                }
            }
        }
        if (closed) emit() << " Z";
        emit() << "\" stroke=\"" << resolveColor(data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void addSpline(const DRW_Spline* data) override {
        if (!isVisible(*data)) return;
        int degree = data->degree;
        if (degree < 1) degree = 3;
        const auto& knots = data->knotslist;
        const auto& ctrl = data->controllist;
        int numCtrl = (int)ctrl.size();
        int numKnots = (int)knots.size();
        if (numCtrl < 2 || numKnots < 2) return;

        std::vector<double> cx, cy;
        for (const auto& c : ctrl) { cx.push_back(c->x); cy.push_back(c->y); }
        const auto& w = data->weightlist;
        bool rational = !w.empty();

        double uMin = knots[degree];
        double uMax = knots[numKnots - degree - 1];
        if (uMax <= uMin) uMax = uMin + 1.0;
        int numSamples = std::max(100, numCtrl * 20);
        double du = (uMax - uMin) / numSamples;

        emit() << "<path d=\"";
        for (int i = 0; i <= numSamples; i++) {
            double u = (i == numSamples) ? uMax : (uMin + du * i);
            double px, py;
            DeBoorSpline(degree, knots, cx, cy,
                         rational ? w : std::vector<double>(),
                         u, px, py);
            bboxAdd(px, py);
            if (i == 0) emit() << "M " << FmtFloat(px) << " " << FmtFloat(-py);
            else        emit() << " L " << FmtFloat(px) << " " << FmtFloat(-py);
        }
        emit() << "\" stroke=\"" << resolveColor(*data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void addKnot(const DRW_Entity&) override {}

    void addInsert(const DRW_Insert& data) override {
        if (!isVisible(data)) return;
        auto it = m_blocks.find(data.name);
        if (it == m_blocks.end()) return;
        const BlockDef& block = it->second;
        if (block.svg.empty()) return;

        double px = data.basePoint.x, py = data.basePoint.y;
        double sx = data.xscale, sy = data.yscale;
        double angle_deg = data.angle * 180.0 / 3.14159265358979;

        // 多重插入 (MINSERT)
        for (int col = 0; col < data.colcount; col++) {
            for (int row = 0; row < data.rowcount; row++) {
                double ox = px + col * data.colspace;
                double oy = py + row * data.rowspace;

                // 生成 <g transform>
                emit() << "<g transform=\"translate(" << FmtFloat(ox) << ","
                       << FmtFloat(-oy) << ")";
                if (sx != 1.0 || sy != 1.0)
                    emit() << " scale(" << FmtFloat(sx) << "," << FmtFloat(sy) << ")";
                if (std::abs(angle_deg) > 0.001)
                    emit() << " rotate(" << FmtFloat(-angle_deg) << ")";
                emit() << ">\n" << block.svg << "</g>\n";

                // 更新 BBox: 变换块的BBox角点
                double cosA = cos(data.angle), sinA = sin(data.angle);
                double corners[4][2] = {
                    {block.bbox.minX * sx, block.bbox.minY * sy},
                    {block.bbox.maxX * sx, block.bbox.minY * sy},
                    {block.bbox.minX * sx, block.bbox.maxY * sy},
                    {block.bbox.maxX * sx, block.bbox.maxY * sy}
                };
                for (auto& c : corners) {
                    double rx = c[0] * cosA + c[1] * sinA;
                    double ry = -c[0] * sinA + c[1] * cosA;
                    bboxAdd(ox + rx, oy + ry);
                }
            }
        }
    }

    void addTrace(const DRW_Trace& data) override { addQuadrilateral(data.basePoint, data.secPoint, data.thirdPoint, data.fourPoint, data); }
    void add3dFace(const DRW_3Dface& data) override { addQuadrilateral(data.basePoint, data.secPoint, data.thirdPoint, data.fourPoint, data); }
    void addSolid(const DRW_Solid& data) override { addQuadrilateral(data.basePoint, data.secPoint, data.thirdPoint, data.fourPoint, data); }

    void addMText(const DRW_MText& data) override {
        if (!isVisible(data)) return;
        double x = data.basePoint.x, y = data.basePoint.y;
        double h = data.height;
        if (h <= 0) h = 2.5;
        bboxAdd(x, y);
        std::string txt = StripMTextFormatting(data.text);
        std::string esc = SanitizeXmlText(txt);
        emit() << "<text x=\"" << FmtFloat(x) << "\" y=\""
               << FmtFloat(-y) << "\" font-size=\"" << FmtFloat(h)
               << "\" fill=\"" << resolveColor(data) << "\">"
               << esc << "</text>\n";
    }

    void addText(const DRW_Text& data) override {
        if (!isVisible(data)) return;
        double x = data.basePoint.x, y = data.basePoint.y;
        double h = data.height;
        if (h <= 0) h = 2.5;
        bboxAdd(x, y);
        std::string esc = SanitizeXmlText(data.text);
        emit() << "<text x=\"" << FmtFloat(x) << "\" y=\""
               << FmtFloat(-y) << "\" font-size=\"" << FmtFloat(h)
               << "\" fill=\"" << resolveColor(data) << "\">"
               << esc << "</text>\n";
    }

    // --- 尺寸标注 (简化: 渲染文字) ---
    void addDimAlign(const DRW_DimAligned* data) override { renderDimension(data); }
    void addDimLinear(const DRW_DimLinear* data) override { renderDimension(data); }
    void addDimRadial(const DRW_DimRadial* data) override { renderDimension(data); }
    void addDimDiametric(const DRW_DimDiametric* data) override { renderDimension(data); }
    void addDimAngular(const DRW_DimAngular* data) override { renderDimension(data); }
    void addDimAngular3P(const DRW_DimAngular3p* data) override { renderDimension(data); }
    void addDimOrdinate(const DRW_DimOrdinate* data) override { renderDimension(data); }

    void addLeader(const DRW_Leader* data) override {
        if (!isVisible(*data)) return;
        const auto& vl = data->vertexlist;
        if (vl.size() < 2) return;
        emit() << "<path d=\"";
        for (size_t i = 0; i < vl.size(); i++) {
            double x = vl[i]->x, y = vl[i]->y;
            bboxAdd(x, y);
            if (i == 0) emit() << "M " << FmtFloat(x) << " " << FmtFloat(-y);
            else        emit() << " L " << FmtFloat(x) << " " << FmtFloat(-y);
        }
        emit() << "\" stroke=\"" << resolveColor(*data)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }

    void addHatch(const DRW_Hatch* data) override {
        if (!isVisible(*data)) return;
        if (data->looplist.empty()) return;

        std::string fillColor = resolveColor(*data);
        std::ostringstream pathSS;
        constexpr double PI = 3.14159265358979;

        for (const auto& loop : data->looplist) {
            bool firstPoint = true;

            for (const auto& ent : loop->objlist) {
                switch (ent->eType) {
                    case DRW::LINE: {
                        auto* line = static_cast<DRW_Line*>(ent.get());
                        double x1 = line->basePoint.x, y1 = line->basePoint.y;
                        double x2 = line->secPoint.x, y2 = line->secPoint.y;
                        bboxAdd(x1, y1); bboxAdd(x2, y2);
                        if (firstPoint) {
                            pathSS << "M " << FmtFloat(x1) << " " << FmtFloat(-y1) << " ";
                            firstPoint = false;
                        }
                        pathSS << "L " << FmtFloat(x2) << " " << FmtFloat(-y2) << " ";
                        break;
                    }
                    case DRW::ARC: {
                        auto* arc = static_cast<DRW_Arc*>(ent.get());
                        double cx = arc->basePoint.x, cy = arc->basePoint.y, r = arc->radious;
                        if (r <= 0) break;
                        double sa = arc->staangle, ea = arc->endangle;
                        double x1 = cx + r * cos(sa), y1 = cy + r * sin(sa);
                        double x2 = cx + r * cos(ea), y2 = cy + r * sin(ea);
                        bboxAdd(x1, y1); bboxAdd(x2, y2);
                        bboxAdd(cx - r, cy - r); bboxAdd(cx + r, cy + r);
                        if (firstPoint) {
                            pathSS << "M " << FmtFloat(x1) << " " << FmtFloat(-y1) << " ";
                            firstPoint = false;
                        }
                        double arcAngle = ea - sa;
                        if (arcAngle < 0) arcAngle += 2.0 * PI;
                        int largeArc = (arcAngle > PI) ? 1 : 0;
                        pathSS << "A " << FmtFloat(r) << " " << FmtFloat(r) << " 0 "
                               << largeArc << " 1 " << FmtFloat(x2) << " " << FmtFloat(-y2) << " ";
                        break;
                    }
                    case DRW::ELLIPSE: {
                        auto* el = static_cast<DRW_Ellipse*>(ent.get());
                        double cx = el->basePoint.x, cy = el->basePoint.y;
                        double mx = el->secPoint.x, my = el->secPoint.y;
                        double majorLen = std::hypot(mx, my);
                        if (majorLen < 1e-6) break;
                        double rotAngle = atan2(my, mx);
                        double minorLen = majorLen * el->ratio;
                        double stParam = el->staparam;
                        double enParam = el->endparam;
                        double cosR = cos(rotAngle), sinR = sin(rotAngle);
                        int numSamples = 32;
                        for (int i = 0; i <= numSamples; i++) {
                            double t = stParam + (enParam - stParam) * i / numSamples;
                            double ex = majorLen * cos(t);
                            double ey = minorLen * sin(t);
                            double px = cx + ex * cosR - ey * sinR;
                            double py = cy + ex * sinR + ey * cosR;
                            bboxAdd(px, py);
                            if (firstPoint) {
                                pathSS << "M " << FmtFloat(px) << " " << FmtFloat(-py) << " ";
                                firstPoint = false;
                            } else {
                                pathSS << "L " << FmtFloat(px) << " " << FmtFloat(-py) << " ";
                            }
                        }
                        break;
                    }
                    case DRW::LWPOLYLINE: {
                        auto* pl = static_cast<DRW_LWPolyline*>(ent.get());
                        const auto& vl = pl->vertlist;
                        for (size_t i = 0; i < vl.size(); i++) {
                            double x = vl[i]->x, y = vl[i]->y;
                            bboxAdd(x, y);
                            if (firstPoint) {
                                pathSS << "M " << FmtFloat(x) << " " << FmtFloat(-y) << " ";
                                firstPoint = false;
                            } else {
                                double bulge = vl[i - 1]->bulge;
                                if (std::abs(bulge) > 1e-6) {
                                    double px = vl[i - 1]->x, py = vl[i - 1]->y;
                                    double dx = x - px, dy = y - py;
                                    double dist = std::hypot(dx, dy);
                                    if (dist > 1e-6) {
                                        double theta = 4.0 * std::atan(std::abs(bulge));
                                        double rr = dist / (2.0 * std::sin(theta / 2.0));
                                        int largeArc = (theta > PI) ? 1 : 0;
                                        int sweep = (bulge > 0) ? 1 : 0;
                                        pathSS << "A " << FmtFloat(rr) << " " << FmtFloat(rr)
                                               << " 0 " << largeArc << " " << sweep
                                               << " " << FmtFloat(x) << " " << FmtFloat(-y) << " ";
                                    } else {
                                        pathSS << "L " << FmtFloat(x) << " " << FmtFloat(-y) << " ";
                                    }
                                } else {
                                    pathSS << "L " << FmtFloat(x) << " " << FmtFloat(-y) << " ";
                                }
                            }
                        }
                        break;
                    }
                    case DRW::SPLINE: {
                        auto* sp = static_cast<DRW_Spline*>(ent.get());
                        int degree = sp->degree;
                        if (degree < 1) degree = 3;
                        const auto& knots = sp->knotslist;
                        const auto& ctrl = sp->controllist;
                        int numCtrl = (int)ctrl.size();
                        int numKnots = (int)knots.size();
                        if (numCtrl < 2 || numKnots < 2) break;
                        std::vector<double> cx, cy;
                        for (const auto& c : ctrl) { cx.push_back(c->x); cy.push_back(c->y); }
                        const auto& w = sp->weightlist;
                        bool rational = !w.empty();
                        double uMin = knots[degree];
                        double uMax = knots[numKnots - degree - 1];
                        if (uMax <= uMin) uMax = uMin + 1.0;
                        int numSamples = std::max(50, numCtrl * 10);
                        double du = (uMax - uMin) / numSamples;
                        for (int i = 0; i <= numSamples; i++) {
                            double u = (i == numSamples) ? uMax : (uMin + du * i);
                            double px, py;
                            DeBoorSpline(degree, knots, cx, cy,
                                         rational ? w : std::vector<double>(),
                                         u, px, py);
                            bboxAdd(px, py);
                            if (firstPoint) {
                                pathSS << "M " << FmtFloat(px) << " " << FmtFloat(-py) << " ";
                                firstPoint = false;
                            } else {
                                pathSS << "L " << FmtFloat(px) << " " << FmtFloat(-py) << " ";
                            }
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
            pathSS << "Z ";
        }

        std::string pathD = pathSS.str();
        if (pathD.length() < 3) return;

        emit() << "<path d=\"" << pathD << "\" fill=\"" << fillColor
               << "\" fill-rule=\"evenodd\" stroke=\"none\"/>\n";
    }

    void addViewport(const DRW_Viewport&) override {}
    void addImage(const DRW_Image*) override {}
    void linkImage(const DRW_ImageDef*) override {}
    void addComment(const char*) override {}
    void addPlotSettings(const DRW_PlotSettings*) override {}

    // --- Write 侧 (读取不需要) ---
    void writeHeader(DRW_Header&) override {}
    void writeBlocks() override {}
    void writeBlockRecords() override {}
    void writeEntities() override {}
    void writeLTypes() override {}
    void writeLayers() override {}
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeObjects() override {}
    void writeAppId() override {}

private:
    BBox m_bbox;
    std::ostringstream m_body;

    struct LayerInfo {
        int color = 7;
        bool visible = true;
    };
    std::unordered_map<std::string, LayerInfo> m_layers;

    struct BlockDef {
        std::string svg;
        BBox bbox;
    };
    std::unordered_map<std::string, BlockDef> m_blocks;
    std::unordered_map<int, std::string> m_blockHandleToName;
    std::string m_currentBlockName;
    bool m_inBlockDef = false;
    std::ostringstream m_blockBody;
    BBox m_blockBBox;

    std::ostringstream& emit() {
        return m_inBlockDef ? m_blockBody : m_body;
    }
    void bboxAdd(double x, double y) {
        m_bbox.add(x, y);
        if (m_inBlockDef) m_blockBBox.add(x, y);
    }
    bool isVisible(const DRW_Entity& ent) {
        if (!ent.visible) return false;
        auto it = m_layers.find(ent.layer);
        if (it != m_layers.end() && !it->second.visible) return false;
        return true;
    }
    std::string resolveColor(const DRW_Entity& ent) {
        int aci = ent.color;
        if (aci == 256) {
            auto it = m_layers.find(ent.layer);
            if (it != m_layers.end()) aci = it->second.color;
            else aci = 7;
        } else if (aci == 0) {
            aci = 7;
        }
        return ACIToHex(aci);
    }
    void addQuadrilateral(const DRW_Coord& p1, const DRW_Coord& p2,
                          const DRW_Coord& p3, const DRW_Coord& p4,
                          const DRW_Entity& ent) {
        if (!isVisible(ent)) return;
        bboxAdd(p1.x, p1.y); bboxAdd(p2.x, p2.y);
        bboxAdd(p3.x, p3.y); bboxAdd(p4.x, p4.y);
        emit() << "<polygon points=\""
               << FmtFloat(p1.x) << "," << FmtFloat(-p1.y) << " "
               << FmtFloat(p2.x) << "," << FmtFloat(-p2.y) << " "
               << FmtFloat(p3.x) << "," << FmtFloat(-p3.y) << " "
               << FmtFloat(p4.x) << "," << FmtFloat(-p4.y)
               << "\" stroke=\"" << resolveColor(ent)
               << "\" stroke-width=\"__SW__\" fill=\"none\"/>\n";
    }
    void renderDimension(const DRW_Dimension* data) {
        if (!data || !isVisible(*data)) return;
        DRW_Coord tp = data->getTextPoint();
        double x = tp.x, y = tp.y;
        if (std::abs(x) < 1e-6 && std::abs(y) < 1e-6) {
            DRW_Coord dp = data->getDefPoint();
            x = dp.x; y = dp.y;
        }
        // DRW_Dimension 的 height 是私有成员，使用默认值
        double h = 2.5;
        bboxAdd(x, y);
        std::string txt = data->getText();
        if (txt.empty()) txt = "<>";
        std::string esc = SanitizeXmlText(txt);
        emit() << "<text x=\"" << FmtFloat(x) << "\" y=\""
               << FmtFloat(-y) << "\" font-size=\"" << FmtFloat(h)
               << "\" fill=\"" << resolveColor(*data) << "\">"
               << esc << "</text>\n";
    }
};

// ============================================================================
// DXF → SVG (via libdxfrw)
// ============================================================================
std::string LoadDXFtoSVG(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};

    std::string tempPath = WriteTempFile(data, size);
    if (tempPath.empty()) return {};

    std::string result;
    {
        SVGDRWInterface iface;
        dxfRW dxf(tempPath.c_str());
        if (dxf.read(&iface, true))
            result = iface.getResult();
    }
    DeleteFileA(tempPath.c_str());

    return result;
}

// ============================================================================
// DWG → SVG (via libdxfrw)
// ============================================================================
std::string LoadDWGtoSVG(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};

    std::string tempPath = WriteTempFile(data, size);
    if (tempPath.empty()) return {};

    std::string result;
    {
        SVGDRWInterface iface;
        dwgR dwg(tempPath.c_str());
        bool ok = dwg.read(&iface, true);
        if (ok) {
            result = iface.getResult();
        } else {
            // DWG parse failed
        }
    }
    DeleteFileA(tempPath.c_str());

    return result;
}

} // namespace QuickView
