// ============================================================================
// VectorLoader.cpp - PLT(HPGL) / DXF(AutoCAD) → SVG XML 转换器
// ============================================================================
// Y 轴翻转策略: 直接在坐标中取负 (y' = -y)，不使用 <g transform>，
// 因为 D2D 的 SVG 渲染器对 transform 支持有限。
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

// 按坐标系比例计算线宽 (至少 0.1，确保可见)
static std::string CalcStrokeWidth(double maxDim) {
    if (maxDim <= 0) return "0.5";
    double sw = maxDim * 0.0008; // 0.08% of max dimension
    if (sw < 0.1) sw = 0.1;
    return FmtFloat(sw);
}

// 调试: 将生成的 SVG 保存到桌面 (临时诊断用)
static void DebugDumpSvg(const std::string& svg, const char* suffix) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path),
             "C:\\Users\\Administrator\\Desktop\\debug_%s.svg", suffix);
    FILE* fp = nullptr;
    fopen_s(&fp, path, "wb");
    if (fp) {
        fwrite(svg.data(), 1, svg.size(), fp);
        fclose(fp);
    }
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
    "black", "black", "red", "green", "yellow",
    "blue", "magenta", "cyan", "white"
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
    DebugDumpSvg(result, "plt");
    return result;
}

// ============================================================================
// DXF (AutoCAD) 解析器
// ============================================================================
// Y轴朝上 → SVG Y朝下: y' = -y
// viewBox 从实际实体坐标计算，不依赖 HEADER 段的 $EXTMIN/$EXTMAX
// ============================================================================

static const char* kDXFColors[] = {
    "#000000", "#FF0000", "#FFFF00", "#00FF00", "#00FFFF",
    "#0000FF", "#FF00FF", "#FFFFFF", "#808080", "#C0C0C0"
};

static const char* DXFColorToHex(int aci) {
    if (aci >= 0 && aci <= 9) return kDXFColors[aci];
    if (aci == 256) return "#000000";
    static char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             (aci * 37) % 256, (aci * 73) % 256, (aci * 109) % 256);
    return buf;
}

class DXFReader {
public:
    DXFReader(const char* data, size_t size)
        : m_data(data), m_size(size), m_pos(0) {}

    bool NextPair(int& code, std::string& value) {
        if (m_pos >= m_size) return false;
        int startCode = m_pos;
        while (m_pos < m_size && m_data[m_pos] != '\n' && m_data[m_pos] != '\r')
            m_pos++;
        int codeLen = m_pos - startCode;
        while (m_pos < m_size && (m_data[m_pos] == '\n' || m_data[m_pos] == '\r'))
            m_pos++;
        if (codeLen <= 0) return false;

        const char* codeStr = m_data + startCode;
        while (codeLen > 0 && (*codeStr == ' ' || *codeStr == '\t')) {
            codeStr++; codeLen--;
        }
        char* endp = nullptr;
        std::string codeStrTmp(codeStr, codeLen);
        code = (int)strtol(codeStrTmp.c_str(), &endp, 10);
        if (endp == codeStrTmp.c_str()) return false;

        if (m_pos >= m_size) return false;
        int startVal = m_pos;
        while (m_pos < m_size && m_data[m_pos] != '\n' && m_data[m_pos] != '\r')
            m_pos++;
        int valLen = m_pos - startVal;
        while (m_pos < m_size && (m_data[m_pos] == '\n' || m_data[m_pos] == '\r'))
            m_pos++;

        const char* valStart = m_data + startVal;
        while (valLen > 0 && (*valStart == ' ' || *valStart == '\t')) {
            valStart++; valLen--;
        }
        while (valLen > 0 && (valStart[valLen - 1] == ' ' ||
                              valStart[valLen - 1] == '\t' ||
                              valStart[valLen - 1] == '\r'))
            valLen--;
        value.assign(valStart, valLen);
        return true;
    }

    void Reset() { m_pos = 0; }

private:
    const char* m_data;
    size_t m_size;
    size_t m_pos;
};

static double ParseDouble(const std::string& s) {
    char* endp = nullptr;
    double v = strtod(s.c_str(), &endp);
    if (endp == s.c_str()) return 0.0;
    return v;
}

// De Boor 算法
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

std::string LoadDXFtoSVG(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};

    DXFReader reader(reinterpret_cast<const char*>(data), size);

    // 跳到 ENTITIES 段
    bool inEntities = false;
    {
        int code;
        std::string value;
        while (reader.NextPair(code, value)) {
            if (code == 0 && value == "SECTION") {
                int code2; std::string val2;
                if (reader.NextPair(code2, val2) && code2 == 2) {
                    if (val2 == "ENTITIES") { inEntities = true; break; }
                }
                continue;
            }
        }
    }
    if (!inEntities) return {};

    // 辅助: Y 翻转
    auto FY = [](double y) { return -y; };

    BBox bbox;
    std::ostringstream svgBody;
    int entityColor = 7;
    std::string currentEntityType;
    // 占位 stroke-width，最终替换
    const char* SW_PLACEHOLDER = "__SW__";

    auto resetEntity = [&]() { entityColor = 7; };

    int code;
    std::string value;

    while (reader.NextPair(code, value)) {
        if (code == 0 && (value == "ENDSEC" || value == "EOF")) break;
        if (code == 0) { resetEntity(); currentEntityType = value; continue; }

        // --- LINE ---
        if (currentEntityType == "LINE") {
            double x1=0,y1=0,x2=0,y2=0; bool hasP1=false,hasP2=false;
            do {
                if (code == 10) { x1 = ParseDouble(value); }
                else if (code == 20) { y1 = ParseDouble(value); hasP1 = true; }
                else if (code == 11) { x2 = ParseDouble(value); }
                else if (code == 21) { y2 = ParseDouble(value); hasP2 = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);
            if (hasP1 && hasP2) {
                bbox.add(x1, y1);
                bbox.add(x2, y2);
                svgBody << "<line x1=\"" << FmtFloat(x1) << "\" y1=\"" << FmtFloat(FY(y1))
                        << "\" x2=\"" << FmtFloat(x2) << "\" y2=\"" << FmtFloat(FY(y2))
                        << "\" stroke=\"" << DXFColorToHex(entityColor)
                        << "\" stroke-width=\"" << SW_PLACEHOLDER
                        << "\" fill=\"none\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- CIRCLE ---
        if (currentEntityType == "CIRCLE") {
            double cx=0,cy=0,r=0; bool hasC=false,hasR=false;
            do {
                if (code == 10) { cx = ParseDouble(value); }
                else if (code == 20) { cy = ParseDouble(value); hasC = true; }
                else if (code == 40) { r = ParseDouble(value); hasR = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);
            if (hasC && hasR && r > 0) {
                bbox.add(cx - r, cy - r);
                bbox.add(cx + r, cy + r);
                svgBody << "<circle cx=\"" << FmtFloat(cx) << "\" cy=\""
                        << FmtFloat(FY(cy)) << "\" r=\"" << FmtFloat(r)
                        << "\" stroke=\"" << DXFColorToHex(entityColor)
                        << "\" stroke-width=\"" << SW_PLACEHOLDER
                        << "\" fill=\"none\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- ARC ---
        if (currentEntityType == "ARC") {
            double cx=0,cy=0,r=0,sa_deg=0,ea_deg=0;
            bool hasC=false,hasR=false,hasS=false,hasE=false;
            do {
                if (code == 10) { cx = ParseDouble(value); }
                else if (code == 20) { cy = ParseDouble(value); hasC = true; }
                else if (code == 40) { r = ParseDouble(value); hasR = true; }
                else if (code == 50) { sa_deg = ParseDouble(value); hasS = true; }
                else if (code == 51) { ea_deg = ParseDouble(value); hasE = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);
            if (hasC && hasR && r > 0 && hasS && hasE) {
                double sa = sa_deg * 3.14159265358979 / 180.0;
                double ea = ea_deg * 3.14159265358979 / 180.0;
                double x1 = cx + r * cos(sa);
                double y1 = cy + r * sin(sa);
                double x2 = cx + r * cos(ea);
                double y2 = cy + r * sin(ea);
                double arcAngle = ea_deg - sa_deg;
                if (arcAngle < 0) arcAngle += 360.0;
                int largeArc = (arcAngle > 180.0) ? 1 : 0;
                // Y 翻转后 sweep 方向反转
                bbox.add(x1, y1);
                bbox.add(x2, y2);
                bbox.add(cx - r, cy - r);
                bbox.add(cx + r, cy + r);
                svgBody << "<path d=\"M " << FmtFloat(x1) << " " << FmtFloat(FY(y1))
                        << " A " << FmtFloat(r) << " " << FmtFloat(r) << " 0 "
                        << largeArc << " 0 " << FmtFloat(x2) << " " << FmtFloat(FY(y2))
                        << "\" stroke=\"" << DXFColorToHex(entityColor)
                        << "\" stroke-width=\"" << SW_PLACEHOLDER
                        << "\" fill=\"none\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- LWPOLYLINE ---
        if (currentEntityType == "LWPOLYLINE") {
            std::vector<double> px, py; bool closed = false;
            do {
                if (code == 10) { px.push_back(ParseDouble(value)); py.push_back(0.0); }
                else if (code == 20) { if (!py.empty()) py.back() = ParseDouble(value); }
                else if (code == 70) { closed = (atoi(value.c_str()) & 1) != 0; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);
            if (!px.empty()) {
                for (size_t i = 0; i < px.size() && i < py.size(); i++)
                    bbox.add(px[i], py[i]);
                svgBody << "<path d=\"M " << FmtFloat(px[0]) << " " << FmtFloat(FY(py[0]));
                for (size_t i = 1; i < px.size(); i++)
                    svgBody << " L " << FmtFloat(px[i]) << " " << FmtFloat(FY(py[i]));
                if (closed) svgBody << " Z";
                svgBody << "\" stroke=\"" << DXFColorToHex(entityColor)
                        << "\" stroke-width=\"" << SW_PLACEHOLDER
                        << "\" fill=\"none\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- POLYLINE / VERTEX ---
        if (currentEntityType == "POLYLINE") {
            std::vector<double> px, py; bool closed = false;
            do {
                if (code == 70) { closed = (atoi(value.c_str()) & 1) != 0; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            while (code == 0 && value == "VERTEX") {
                double vx=0, vy=0; bool hasV=false;
                do {
                    if (code == 10) { vx = ParseDouble(value); }
                    else if (code == 20) { vy = ParseDouble(value); hasV = true; }
                } while (reader.NextPair(code, value) && code != 0);
                if (hasV) {
                    px.push_back(vx); py.push_back(vy);
                    bbox.add(vx, vy);
                }
            }
            if (!px.empty()) {
                svgBody << "<path d=\"M " << FmtFloat(px[0]) << " " << FmtFloat(FY(py[0]));
                for (size_t i = 1; i < px.size(); i++)
                    svgBody << " L " << FmtFloat(px[i]) << " " << FmtFloat(FY(py[i]));
                if (closed) svgBody << " Z";
                svgBody << "\" stroke=\"" << DXFColorToHex(entityColor)
                        << "\" stroke-width=\"" << SW_PLACEHOLDER
                        << "\" fill=\"none\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- SPLINE ---
        if (currentEntityType == "SPLINE") {
            int degree=3; std::vector<double> knots,ctrlX,ctrlY,weights;
            int numKnots=0, numCtrl=0; bool rational=false;
            do {
                if (code == 70) { rational = (atoi(value.c_str()) & 4) != 0; }
                else if (code == 71) { degree = atoi(value.c_str()); if (degree < 1) degree = 3; }
                else if (code == 72) { numKnots = atoi(value.c_str()); }
                else if (code == 73) { numCtrl = atoi(value.c_str()); }
                else if (code == 40) { knots.push_back(ParseDouble(value)); }
                else if (code == 10) { ctrlX.push_back(ParseDouble(value)); ctrlY.push_back(0.0); }
                else if (code == 20) { if (!ctrlY.empty()) ctrlY.back() = ParseDouble(value); }
                else if (code == 41) { weights.push_back(ParseDouble(value)); }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (numCtrl > 1 && (int)ctrlX.size() >= numCtrl &&
                (int)knots.size() >= numKnots && numKnots > 0 && degree >= 1) {
                double uMin = knots[degree];
                double uMax = knots[numKnots - degree - 1];
                if (uMax <= uMin) uMax = uMin + 1.0;
                int numSamples = std::max(100, numCtrl * 20);
                double du = (uMax - uMin) / numSamples;
                svgBody << "<path d=\"";
                for (int i = 0; i <= numSamples; i++) {
                    double u = (i == numSamples) ? uMax : (uMin + du * i);
                    double px, py;
                    DeBoorSpline(degree, knots, ctrlX, ctrlY,
                                 rational ? weights : std::vector<double>(),
                                 u, px, py);
                    bbox.add(px, py);
                    if (i == 0) svgBody << "M " << FmtFloat(px) << " " << FmtFloat(FY(py));
                    else        svgBody << " L " << FmtFloat(px) << " " << FmtFloat(FY(py));
                }
                svgBody << "\" stroke=\"" << DXFColorToHex(entityColor)
                        << "\" stroke-width=\"" << SW_PLACEHOLDER
                        << "\" fill=\"none\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- ELLIPSE ---
        if (currentEntityType == "ELLIPSE") {
            double cx=0,cy=0,rx=0,ry=0; bool hasC=false,hasR=false;
            do {
                if (code == 10) { cx = ParseDouble(value); }
                else if (code == 20) { cy = ParseDouble(value); hasC = true; }
                else if (code == 11) { rx = ParseDouble(value); }
                else if (code == 21) { ry = ParseDouble(value); hasR = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);
            if (hasC && hasR) {
                double majorLen = std::hypot(rx, ry);
                bbox.add(cx - majorLen, cy - majorLen * 0.5);
                bbox.add(cx + majorLen, cy + majorLen * 0.5);
                svgBody << "<ellipse cx=\"" << FmtFloat(cx) << "\" cy=\""
                        << FmtFloat(FY(cy)) << "\" rx=\"" << FmtFloat(majorLen)
                        << "\" ry=\"" << FmtFloat(majorLen * 0.5)
                        << "\" stroke=\"" << DXFColorToHex(entityColor)
                        << "\" stroke-width=\"" << SW_PLACEHOLDER
                        << "\" fill=\"none\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- TEXT / MTEXT ---
        if (currentEntityType == "TEXT" || currentEntityType == "MTEXT") {
            double tx=0,ty=0,height=2.5; std::string txt; bool hasPos=false;
            do {
                if (code == 10) { tx = ParseDouble(value); }
                else if (code == 20) { ty = ParseDouble(value); hasPos = true; }
                else if (code == 40) { height = ParseDouble(value); }
                else if (code == 1) { txt = value; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);
            if (hasPos && !txt.empty()) {
                bbox.add(tx, ty);
                // SanitizeXmlText 转义 XML 特殊字符 + 替换非 ASCII 字节
                std::string esc = SanitizeXmlText(txt);
                svgBody << "<text x=\"" << FmtFloat(tx) << "\" y=\""
                        << FmtFloat(FY(ty)) << "\" font-size=\"" << FmtFloat(height)
                        << "\" fill=\"" << DXFColorToHex(entityColor) << "\">"
                        << esc << "</text>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }

        // --- POINT ---
        if (currentEntityType == "POINT") {
            double px=0,py=0; bool hasPos=false;
            do {
                if (code == 10) { px = ParseDouble(value); }
                else if (code == 20) { py = ParseDouble(value); hasPos = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);
            if (hasPos) {
                bbox.add(px, py);
                svgBody << "<circle cx=\"" << FmtFloat(px) << "\" cy=\""
                        << FmtFloat(FY(py)) << "\" r=\"" << FmtFloat(bbox.maxDim() * 0.002)
                        << "\" fill=\"" << DXFColorToHex(entityColor) << "\"/>\n";
            }
            if (code == 0) { resetEntity(); currentEntityType = value; }
            continue;
        }
        // 未知实体: 外层循环继续读取
    }

    if (!bbox.valid) return {};

    // viewBox: Y 翻转后范围 [-maxY, -minY]
    double margin = bbox.maxDim() * 0.02; // 2% margin
    if (margin < 1.0) margin = 1.0;
    double vbX = bbox.minX - margin;
    double vbY = -bbox.maxY - margin;
    double vbW = bbox.width() + margin * 2;
    double vbH = bbox.height() + margin * 2;
    if (vbW <= 0) vbW = 100;
    if (vbH <= 0) vbH = 100;

    // 替换占位 stroke-width
    std::string strokeW = CalcStrokeWidth(bbox.maxDim());
    std::string bodyStr = svgBody.str();
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

    std::string result = svg.str();
    DebugDumpSvg(result, "dxf");
    return result;
}

} // namespace QuickView
