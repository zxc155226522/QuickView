// ============================================================================
// VectorLoader.cpp - PLT(HPGL) / DXF(AutoCAD) → SVG XML 转换器
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
    // 紧凑浮点格式化（去掉多余零）
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", v);
    // 去除尾零和小数点
    char* dot = strchr(buf, '.');
    if (dot) {
        char* end = buf + strlen(buf) - 1;
        while (end > dot && *end == '0') *end-- = '\0';
        if (*end == '.') *end = '\0';
    }
    return buf;
}

// ============================================================================
// PLT (HPGL) 解析器
// ============================================================================
// HPGL 命令参考:
//   IN  - 初始化
//   SPn - 选笔 (n=1-8, 颜色)
//   PU  - 抬笔移动 (Pen Up)
//   PD  - 落笔画线 (Pen Down)
//   PA  - 绝对坐标移动 (同 PU/PD 的坐标参数)
//   LT  - 线型
//   PW  - 笔宽
//   AA  - 圆弧
//   CI  - 圆
//   LB  - 文字标签
//   SC  - 比例设置
//
// 坐标格式: "PU x,y,x,y;" 或 "PU x y x y;"
// 单位: 0.025mm (1/40mm)，Y轴朝上
// ============================================================================

static const char* kPLTPenColors[] = {
    "black",   // 0 (SP0 = 停笔/黑)
    "black",   // 1
    "red",     // 2
    "green",   // 3
    "yellow",  // 4
    "blue",    // 5
    "magenta", // 6
    "cyan",    // 7
    "white"    // 8
};

static constexpr int kPLTMaxPen = 8;

// 解析 HPGL 坐标参数列表
static void ParseHPGLCoords(std::string_view args, std::vector<double>& xs,
                            std::vector<double>& ys) {
    xs.clear();
    ys.clear();
    size_t i = 0;
    const size_t n = args.size();
    bool expectY = false;
    while (i < n) {
        // 跳过分隔符
        while (i < n && (args[i] == ',' || args[i] == ' ' || args[i] == '\r' ||
                         args[i] == '\n' || args[i] == '\t'))
            i++;
        if (i >= n) break;
        // 解析数字
        char* endp = nullptr;
        // 需要确保 null 终止，使用 strtod on the substring
        // 但 args 不一定 null 终止，用临时 string
        std::string numStr;
        while (i < n && args[i] != ',' && args[i] != ' ' && args[i] != '\r' &&
               args[i] != '\n' && args[i] != '\t') {
            numStr += args[i++];
        }
        if (numStr.empty()) break;
        double val = strtod(numStr.c_str(), &endp);
        if (endp == numStr.c_str()) break; // 解析失败
        if (!expectY) {
            xs.push_back(val);
            expectY = true;
        } else {
            ys.push_back(val);
            expectY = false;
        }
    }
    // 确保 xs 和 ys 长度一致
    if (xs.size() > ys.size()) {
        // 最后一个 X 没有对应的 Y，补零
        ys.push_back(0.0);
    }
}

std::string LoadPLTtoSVG(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};

    // 转为字符串视图（HPGL 是 ASCII 文本）
    std::string_view text(reinterpret_cast<const char*>(data), size);

    // 状态
    int currentPen = 1;
    bool penDown = false;
    double curX = 0, curY = 0;
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    bool hasPoints = false;

    // SVG path 数据
    std::ostringstream svgPaths;
    int activePen = -1; // 当前正在输出的笔色

    // 辅助：带颜色 flushPath
    auto flushPathWithColor = [&]() {
        if (activePen >= 0) {
            const char* color = (activePen >= 0 && activePen <= kPLTMaxPen)
                                    ? kPLTPenColors[activePen]
                                    : "black";
            svgPaths << "\" stroke=\"" << color
                     << "\" stroke-width=\"0.5\" fill=\"none\"/>\n";
            activePen = -1;
        }
    };

    // 重新定义 startPath 和 flushPath
    auto startPathColored = [&](int pen) {
        flushPathWithColor();
        svgPaths << "<path d=\"";
        activePen = pen;
    };

    // 解析命令
    size_t pos = 0;
    while (pos < text.size()) {
        // 跳过空白和分号
        while (pos < text.size() &&
               (text[pos] == ';' || text[pos] == ' ' || text[pos] == '\r' ||
                text[pos] == '\n' || text[pos] == '\t'))
            pos++;
        if (pos >= text.size()) break;

        // 提取命令（2个字母，不区分大小写）
        if (pos + 1 >= text.size()) break;
        char cmdBuf[3] = {0};
        cmdBuf[0] = (char)toupper((unsigned char)text[pos]);
        cmdBuf[1] = (char)toupper((unsigned char)text[pos + 1]);
        cmdBuf[2] = '\0';
        pos += 2;

        // 找到命令参数结束（分号或下一个命令）
        size_t argStart = pos;
        while (pos < text.size() && text[pos] != ';') {
            // 如果遇到字母，可能是下一个命令（无分号分隔的情况）
            if (isalpha((unsigned char)text[pos]) && pos > argStart) {
                // 检查是否是命令开始（两个字母）
                if (pos + 1 < text.size() && isalpha((unsigned char)text[pos + 1]))
                    break;
            }
            pos++;
        }
        std::string_view args(text.data() + argStart, pos - argStart);

        // 处理命令
        if (strcmp(cmdBuf, "IN") == 0) {
            // 初始化
            flushPathWithColor();
            penDown = false;
            curX = curY = 0;
            currentPen = 1;
        } else if (strcmp(cmdBuf, "SP") == 0) {
            // 选笔
            flushPathWithColor();
            char* endp = nullptr;
            std::string penStr(args);
            int pen = (int)strtol(penStr.c_str(), &endp, 10);
            if (pen >= 0 && pen <= kPLTMaxPen)
                currentPen = pen;
            penDown = false;
        } else if (strcmp(cmdBuf, "PU") == 0) {
            // 抬笔移动
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);
            if (!xs.empty() && !ys.empty()) {
                flushPathWithColor();
                curX = xs[0];
                curY = ys[0];
            }
            penDown = false;
        } else if (strcmp(cmdBuf, "PD") == 0) {
            // 落笔画线
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);

            if (xs.empty()) {
                penDown = true;
            } else {
                // 如果当前 path 的笔色不匹配，重新开始
                if (activePen != currentPen) {
                    startPathColored(currentPen);
                    // M 到当前位置（起点）
                    svgPaths << "M " << FmtFloat(curX) << " " << FmtFloat(curY) << " ";
                }

                // 绘制到各点
                for (size_t i = 0; i < xs.size() && i < ys.size(); i++) {
                    svgPaths << "L " << FmtFloat(xs[i]) << " " << FmtFloat(ys[i]) << " ";
                    curX = xs[i];
                    curY = ys[i];
                    if (curX < minX) minX = curX;
                    if (curY < minY) minY = curY;
                    if (curX > maxX) maxX = curX;
                    if (curY > maxY) maxY = curY;
                    hasPoints = true;
                }
                penDown = true;
            }
        } else if (strcmp(cmdBuf, "PA") == 0) {
            // 绝对坐标移动（行为类似 PU/PD，取决于笔状态）
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);
            if (!xs.empty() && !ys.empty()) {
                if (penDown) {
                    if (activePen != currentPen) {
                        startPathColored(currentPen);
                        svgPaths << "M " << FmtFloat(curX) << " " << FmtFloat(curY) << " ";
                    }
                    for (size_t i = 0; i < xs.size() && i < ys.size(); i++) {
                        svgPaths << "L " << FmtFloat(xs[i]) << " " << FmtFloat(ys[i]) << " ";
                        curX = xs[i];
                        curY = ys[i];
                        if (curX < minX) minX = curX;
                        if (curY < minY) minY = curY;
                        if (curX > maxX) maxX = curX;
                        if (curY > maxY) maxY = curY;
                        hasPoints = true;
                    }
                } else {
                    curX = xs[0];
                    curY = ys[0];
                }
            }
        } else if (strcmp(cmdBuf, "CI") == 0) {
            // 圆: CI radius
            char* endp = nullptr;
            std::string rStr(args);
            double r = strtod(rStr.c_str(), &endp);
            if (r > 0) {
                if (activePen != currentPen) {
                    startPathColored(currentPen);
                }
                // 用两段弧画圆
                svgPaths << "M " << FmtFloat(curX - r) << " " << FmtFloat(curY)
                         << " A " << FmtFloat(r) << " " << FmtFloat(r)
                         << " 0 1 0 " << FmtFloat(curX + r) << " " << FmtFloat(curY)
                         << " A " << FmtFloat(r) << " " << FmtFloat(r)
                         << " 0 1 0 " << FmtFloat(curX - r) << " " << FmtFloat(curY)
                         << " ";
                if (curX - r < minX) minX = curX - r;
                if (curY - r < minY) minY = curY - r;
                if (curX + r > maxX) maxX = curX + r;
                if (curY + r > maxY) maxY = curY + r;
                hasPoints = true;
            }
        } else if (strcmp(cmdBuf, "AA") == 0) {
            // 圆弧: AA x,y,angle(,chord)
            std::vector<double> xs, ys;
            ParseHPGLCoords(args, xs, ys);
            if (xs.size() >= 3 && ys.size() >= 1) {
                // AA 的参数是: x, y, angle (度), [chord_angle]
                double cx = xs[0], cy = ys[0];
                double angle = xs[2]; // 第三个数是角度
                double r = std::hypot(curX - cx, curY - cy);
                if (r > 0 && std::abs(angle) > 0.01) {
                    if (activePen != currentPen) {
                        startPathColored(currentPen);
                        svgPaths << "M " << FmtFloat(curX) << " " << FmtFloat(curY) << " ";
                    }
                    double endAngle = atan2(curY - cy, curX - cx) + angle * 3.14159265358979 / 180.0;
                    double ex = cx + r * cos(endAngle);
                    double ey = cy + r * sin(endAngle);
                    int largeArc = (std::abs(angle) > 180.0) ? 1 : 0;
                    int sweep = (angle > 0) ? 1 : 0;
                    svgPaths << "A " << FmtFloat(r) << " " << FmtFloat(r)
                             << " 0 " << largeArc << " " << sweep
                             << " " << FmtFloat(ex) << " " << FmtFloat(ey) << " ";
                    curX = ex;
                    curY = ey;
                    if (curX < minX) minX = curX;
                    if (curY < minY) minY = curY;
                    if (curX > maxX) maxX = curX;
                    if (curY > maxY) maxY = curY;
                    hasPoints = true;
                }
            }
        }
        // 其他命令 (LT, PW, VS, WU, etc.) 忽略
    }

    flushPathWithColor();

    if (!hasPoints) return {};

    // Y 轴翻转（HPGL Y朝上 → SVG Y朝下）
    // 使用 <g transform="translate(0,maxY+minY) scale(1,-1)"> 翻转

    double margin = 2.0; // 边距
    double vbX = minX - margin;
    double vbY = minY - margin;
    double vbW = (maxX - minX) + margin * 2;
    double vbH = (maxY - minY) + margin * 2;

    if (vbW <= 0) vbW = 100;
    if (vbH <= 0) vbH = 100;

    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\""
        << " viewBox=\"" << FmtFloat(vbX) << " " << FmtFloat(vbY) << " "
        << FmtFloat(vbW) << " " << FmtFloat(vbH) << "\""
        << " width=\"" << FmtFloat(vbW) << "\" height=\"" << FmtFloat(vbH) << "\""
        << " transform=\"translate(0," << FmtFloat(maxY + minY) << ") scale(1,-1)\">\n";
    // 用 transform 翻转 Y 轴
    // 注意：transform 应用在 svg 元素上时，需要重新调整 viewBox
    // 更简单的方法：直接在每个 path 的坐标中翻转 Y

    // 重新生成不带 transform 的 SVG，直接翻转坐标
    // 实际上上面的 paths 已经生成了原始坐标，我们用 transform 翻转
    // 但 D2D 可能不支持 svg 根上的 transform，所以改用 viewBox + 内部翻转

    // 重新用正确的坐标生成 SVG
    // 放弃 transform 方案，直接重新解析并翻转 Y 坐标
    // 但已经生成了 paths，不如用 transform

    // D2D 的 SVG 渲染器支持 <g transform>，用这个更安全
    svg.str("");
    svg.clear();
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\""
        << " viewBox=\"" << FmtFloat(vbX) << " " << FmtFloat(-maxY - margin) << " "
        << FmtFloat(vbW) << " " << FmtFloat(vbH) << "\""
        << " width=\"" << FmtFloat(vbW) << "\" height=\"" << FmtFloat(vbH) << "\">\n";
    svg << "<g transform=\"translate(0," << FmtFloat(maxY + minY) << ") scale(1,-1)\">\n";
    svg << svgPaths.str();
    svg << "</g>\n</svg>";

    return svg.str();
}

// ============================================================================
// DXF (AutoCAD) 解析器
// ============================================================================
// DXF 文件结构: 组码(整数) + 值 成对出现
// 主要段:
//   HEADER  - 系统变量 ($EXTMIN/$EXTMAX = 图形边界)
//   ENTITIES - 实体段 (LINE, CIRCLE, ARC, SPLINE, POLYLINE, LWPOLYLINE, etc.)
//
// 常用组码:
//   0  - 实体类型
//   8  - 图层名
//   10 - X坐标
//   20 - Y坐标
//   40 - 半径/节点值
//   62 - 颜色号 (ACI)
//   70 - 标志位
// ============================================================================

// ACI 颜色 → RGB (仅标准色 1-9, 其他用灰度近似)
static const char* kDXFColors[] = {
    "#000000", // 0 - BYBLOCK
    "#FF0000", // 1 - 红
    "#FFFF00", // 2 - 黄
    "#00FF00", // 3 - 绿
    "#00FFFF", // 4 - 青
    "#0000FF", // 5 - 蓝
    "#FF00FF", // 6 - 品红
    "#FFFFFF", // 7 - 白/黑(取决于背景)
    "#808080", // 8 - 灰
    "#C0C0C0"  // 9 - 浅灰
};

static const char* DXFColorToHex(int aci) {
    if (aci >= 0 && aci <= 9) return kDXFColors[aci];
    if (aci == 256) return "#000000"; // BYLAYER
    // 10-255: 近似映射
    static char buf[8];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X",
             (aci * 37) % 256, (aci * 73) % 256, (aci * 109) % 256);
    return buf;
}

// DXF 读取器：简化组码-值对解析
class DXFReader {
public:
    DXFReader(const char* data, size_t size) : m_data(data), m_size(size), m_pos(0) {}

    // 读取下一对组码-值
    bool NextPair(int& code, std::string& value) {
        if (m_pos >= m_size) return false;

        // 读取组码行
        int startCode = m_pos;
        while (m_pos < m_size && m_data[m_pos] != '\n' && m_data[m_pos] != '\r')
            m_pos++;
        int codeLen = m_pos - startCode;
        // 跳过换行
        while (m_pos < m_size && (m_data[m_pos] == '\n' || m_data[m_pos] == '\r'))
            m_pos++;
        if (codeLen <= 0) return false;

        // 解析组码（跳过前导空格）
        const char* codeStr = m_data + startCode;
        while (codeLen > 0 && (*codeStr == ' ' || *codeStr == '\t')) {
            codeStr++;
            codeLen--;
        }
        char* endp = nullptr;
        std::string codeStrTmp(codeStr, codeLen);
        code = (int)strtol(codeStrTmp.c_str(), &endp, 10);
        if (endp == codeStrTmp.c_str()) return false;

        // 读取值行
        if (m_pos >= m_size) return false;
        int startVal = m_pos;
        while (m_pos < m_size && m_data[m_pos] != '\n' && m_data[m_pos] != '\r')
            m_pos++;
        int valLen = m_pos - startVal;
        // 跳过换行
        while (m_pos < m_size && (m_data[m_pos] == '\n' || m_data[m_pos] == '\r'))
            m_pos++;

        // 去除值的前后空格
        const char* valStart = m_data + startVal;
        while (valLen > 0 && (*valStart == ' ' || *valStart == '\t')) {
            valStart++;
            valLen--;
        }
        while (valLen > 0 && (valStart[valLen - 1] == ' ' || valStart[valLen - 1] == '\t' ||
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

// 双精度解析
static double ParseDouble(const std::string& s) {
    char* endp = nullptr;
    double v = strtod(s.c_str(), &endp);
    if (endp == s.c_str()) return 0.0;
    return v;
}

// De Boor 算法：NURBS 曲线点计算
static void DeBoorSpline(
    int degree,
    const std::vector<double>& knots,
    const std::vector<double>& ctrlX,
    const std::vector<double>& ctrlY,
    const std::vector<double>& weights, // 可为空（无权重）
    double u,
    double& outX, double& outY)
{
    // 找到 u 所在的节点区间 [knots[i], knots[i+1])
    int n = (int)ctrlX.size() - 1;
    int span = n + degree; // 默认
    for (int i = degree; i <= n + degree; i++) {
        if (u >= knots[i] && u < knots[i + 1]) {
            span = i;
            break;
        }
        if (i == n + degree) span = i; // 边界
    }

    // De Boor 递归
    std::vector<double> dx(degree + 1), dy(degree + 1), dw(degree + 1);
    bool hasWeights = !weights.empty();

    for (int j = 0; j <= degree; j++) {
        int idx = span - degree + j;
        if (idx < 0) idx = 0;
        if (idx >= (int)ctrlX.size()) idx = (int)ctrlX.size() - 1;
        dx[j] = ctrlX[idx];
        dy[j] = ctrlY[idx];
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

    outX = dx[degree];
    outY = dy[degree];
}

std::string LoadDXFtoSVG(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};

    DXFReader reader(reinterpret_cast<const char*>(data), size);

    // 第一遍：解析 HEADER 获取边界
    double extMinX = 0, extMinY = 0, extMaxX = 0, extMaxY = 0;
    bool hasExtMin = false, hasExtMax = false;

    int code;
    std::string value;

    // 跟踪当前段和变量名
    std::string currentSection;
    std::string currentVar;

    while (reader.NextPair(code, value)) {
        if (code == 0 && value == "SECTION") {
            // 下一个 2 码是段名
            int code2;
            std::string val2;
            if (reader.NextPair(code2, val2) && code2 == 2) {
                currentSection = val2;
            }
            continue;
        }
        if (code == 0 && value == "ENDSEC") {
            currentSection.clear();
            continue;
        }

        if (currentSection == "HEADER") {
            if (code == 9) {
                currentVar = value;
            } else if (currentVar == "$EXTMIN") {
                if (code == 10) { extMinX = ParseDouble(value); }
                else if (code == 20) { extMinY = ParseDouble(value); hasExtMin = true; }
            } else if (currentVar == "$EXTMAX") {
                if (code == 10) { extMaxX = ParseDouble(value); }
                else if (code == 20) { extMaxY = ParseDouble(value); hasExtMax = true; }
            }
        }

        // ENTITIES 段开始后停止 HEADER 解析
        if (currentSection == "ENTITIES") break;
    }

    // 如果没有 EXTMIN/EXTMAX，使用默认值
    if (!hasExtMin || !hasExtMax) {
        extMinX = 0; extMinY = 0; extMaxX = 100; extMaxY = 100;
    }

    // 计算边界
    double minX = std::min(extMinX, extMaxX);
    double maxX = std::max(extMinX, extMaxX);
    double minY = std::min(extMinY, extMaxY);
    double maxY = std::max(extMinY, extMaxY);

    double w = maxX - minX;
    double h = maxY - minY;
    if (w <= 0) w = 100;
    if (h <= 0) h = 100;

    // 第二遍：解析 ENTITIES，生成 SVG
    reader.Reset();

    // 跳到 ENTITIES 段
    bool inEntities = false;
    while (reader.NextPair(code, value)) {
        if (code == 0 && value == "SECTION") {
            int code2;
            std::string val2;
            if (reader.NextPair(code2, val2) && code2 == 2) {
                if (val2 == "ENTITIES") {
                    inEntities = true;
                    break;
                }
            }
            continue;
        }
    }

    if (!inEntities) return {};

    std::ostringstream svg;
    double margin = std::max(w, h) * 0.02; // 2% 边距
    double vbX = minX - margin;
    double vbY = -maxY - margin; // Y轴翻转
    double vbW = w + margin * 2;
    double vbH = h + margin * 2;

    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\""
        << " viewBox=\"" << FmtFloat(vbX) << " " << FmtFloat(vbY) << " "
        << FmtFloat(vbW) << " " << FmtFloat(vbH) << "\""
        << " width=\"" << FmtFloat(vbW) << "\" height=\"" << FmtFloat(vbH) << "\">\n";
    // Y轴翻转
    svg << "<g transform=\"translate(0," << FmtFloat(maxY + minY) << ") scale(1,-1)\">\n";

    int entityColor = 7; // 默认白色
    std::string currentEntityType;

    // 实体属性缓冲
    auto resetEntity = [&]() {
        entityColor = 7;
    };

    // 解析实体
    while (reader.NextPair(code, value)) {
        if (code == 0 && value == "ENDSEC") break;
        if (code == 0 && value == "EOF") break;

        if (code == 0) {
            // 新实体
            resetEntity();
            currentEntityType = value;
            continue;
        }

        // 在实体内部，按类型处理
        if (currentEntityType == "LINE") {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            bool hasP1 = false, hasP2 = false;

            // 继续读取此实体的组码
            do {
                if (code == 10) { x1 = ParseDouble(value); }
                else if (code == 20) { y1 = ParseDouble(value); hasP1 = true; }
                else if (code == 11) { x2 = ParseDouble(value); }
                else if (code == 21) { y2 = ParseDouble(value); hasP2 = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (hasP1 && hasP2) {
                svg << "<line x1=\"" << FmtFloat(x1) << "\" y1=\"" << FmtFloat(y1)
                    << "\" x2=\"" << FmtFloat(x2) << "\" y2=\"" << FmtFloat(y2)
                    << "\" stroke=\"" << DXFColorToHex(entityColor)
                    << "\" stroke-width=\"0.5\"/>\n";
            }

            // code==0 已读入，处理下一个实体
            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "CIRCLE") {
            double cx = 0, cy = 0, r = 0;
            bool hasCenter = false, hasRadius = false;

            do {
                if (code == 10) { cx = ParseDouble(value); }
                else if (code == 20) { cy = ParseDouble(value); hasCenter = true; }
                else if (code == 40) { r = ParseDouble(value); hasRadius = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (hasCenter && hasRadius && r > 0) {
                svg << "<circle cx=\"" << FmtFloat(cx) << "\" cy=\"" << FmtFloat(cy)
                    << "\" r=\"" << FmtFloat(r) << "\" stroke=\""
                    << DXFColorToHex(entityColor)
                    << "\" stroke-width=\"0.5\" fill=\"none\"/>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "ARC") {
            double cx = 0, cy = 0, r = 0, startAngle = 0, endAngle = 0;
            bool hasCenter = false, hasRadius = false, hasStart = false, hasEnd = false;

            do {
                if (code == 10) { cx = ParseDouble(value); }
                else if (code == 20) { cy = ParseDouble(value); hasCenter = true; }
                else if (code == 40) { r = ParseDouble(value); hasRadius = true; }
                else if (code == 50) { startAngle = ParseDouble(value); hasStart = true; }
                else if (code == 51) { endAngle = ParseDouble(value); hasEnd = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (hasCenter && hasRadius && r > 0 && hasStart && hasEnd) {
                double sa = startAngle * 3.14159265358979 / 180.0;
                double ea = endAngle * 3.14159265358979 / 180.0;
                double x1 = cx + r * cos(sa);
                double y1 = cy + r * sin(sa);
                double x2 = cx + r * cos(ea);
                double y2 = cy + r * sin(ea);
                double arcAngle = endAngle - startAngle;
                if (arcAngle < 0) arcAngle += 360.0;
                int largeArc = (arcAngle > 180.0) ? 1 : 0;
                svg << "<path d=\"M " << FmtFloat(x1) << " " << FmtFloat(y1)
                    << " A " << FmtFloat(r) << " " << FmtFloat(r) << " 0 "
                    << largeArc << " 1 " << FmtFloat(x2) << " " << FmtFloat(y2)
                    << "\" stroke=\"" << DXFColorToHex(entityColor)
                    << "\" stroke-width=\"0.5\" fill=\"none\"/>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "LWPOLYLINE") {
            std::vector<double> px, py;
            bool closed = false;

            do {
                if (code == 10) {
                    px.push_back(ParseDouble(value));
                    py.push_back(0.0); // 占位
                }
                else if (code == 20) {
                    if (!py.empty()) py.back() = ParseDouble(value);
                }
                else if (code == 70) {
                    int flags = atoi(value.c_str());
                    closed = (flags & 1) != 0;
                }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (!px.empty()) {
                svg << "<path d=\"M " << FmtFloat(px[0]) << " " << FmtFloat(py[0]);
                for (size_t i = 1; i < px.size(); i++) {
                    svg << " L " << FmtFloat(px[i]) << " " << FmtFloat(py[i]);
                }
                if (closed) svg << " Z";
                svg << "\" stroke=\"" << DXFColorToHex(entityColor)
                    << "\" stroke-width=\"0.5\" fill=\"none\"/>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "POLYLINE") {
            std::vector<double> px, py;
            bool closed = false;

            // POLYLINE 头
            do {
                if (code == 70) {
                    int flags = atoi(value.c_str());
                    closed = (flags & 1) != 0;
                }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            // VERTEX 实体
            while (code == 0 && value == "VERTEX") {
                double vx = 0, vy = 0;
                bool hasV = false;
                do {
                    if (code == 10) { vx = ParseDouble(value); }
                    else if (code == 20) { vy = ParseDouble(value); hasV = true; }
                } while (reader.NextPair(code, value) && code != 0);
                if (hasV) {
                    px.push_back(vx);
                    py.push_back(vy);
                }
            }

            // SEQEND
            if (!px.empty()) {
                svg << "<path d=\"M " << FmtFloat(px[0]) << " " << FmtFloat(py[0]);
                for (size_t i = 1; i < px.size(); i++) {
                    svg << " L " << FmtFloat(px[i]) << " " << FmtFloat(py[i]);
                }
                if (closed) svg << " Z";
                svg << "\" stroke=\"" << DXFColorToHex(entityColor)
                    << "\" stroke-width=\"0.5\" fill=\"none\"/>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "SPLINE") {
            int degree = 3;
            std::vector<double> knots, ctrlX, ctrlY, weights;
            int numKnots = 0, numCtrl = 0;
            bool rational = false;

            do {
                if (code == 70) {
                    int flags = atoi(value.c_str());
                    rational = (flags & 4) != 0;
                }
                else if (code == 71) { degree = atoi(value.c_str()); if (degree < 1) degree = 3; }
                else if (code == 72) { numKnots = atoi(value.c_str()); }
                else if (code == 73) { numCtrl = atoi(value.c_str()); }
                else if (code == 40) { knots.push_back(ParseDouble(value)); }
                else if (code == 10) { ctrlX.push_back(ParseDouble(value)); ctrlY.push_back(0.0); }
                else if (code == 20) { if (!ctrlY.empty()) ctrlY.back() = ParseDouble(value); }
                else if (code == 41) { weights.push_back(ParseDouble(value)); }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            // De Boor 细分采样
            if (numCtrl > 1 && (int)ctrlX.size() >= numCtrl &&
                (int)knots.size() >= numKnots && numKnots > 0 && degree >= 1) {
                double uMin = knots[degree];
                double uMax = knots[numKnots - degree - 1];
                if (uMax <= uMin) uMax = uMin + 1.0;

                int numSamples = std::max(100, numCtrl * 20);
                double du = (uMax - uMin) / numSamples;

                svg << "<path d=\"";
                for (int i = 0; i <= numSamples; i++) {
                    double u = uMin + du * i;
                    if (i == numSamples) u = uMax; // 避免浮点误差
                    double px, py;
                    DeBoorSpline(degree, knots, ctrlX, ctrlY,
                                 rational ? weights : std::vector<double>(),
                                 u, px, py);
                    if (i == 0)
                        svg << "M " << FmtFloat(px) << " " << FmtFloat(py);
                    else
                        svg << " L " << FmtFloat(px) << " " << FmtFloat(py);
                }
                svg << "\" stroke=\"" << DXFColorToHex(entityColor)
                    << "\" stroke-width=\"0.5\" fill=\"none\"/>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "ELLIPSE") {
            double cx = 0, cy = 0, rx = 0, ry = 0;
            bool hasCenter = false, hasRadius = false;

            do {
                if (code == 10) { cx = ParseDouble(value); }
                else if (code == 20) { cy = ParseDouble(value); hasCenter = true; }
                else if (code == 11) { rx = ParseDouble(value); }
                else if (code == 21) { ry = ParseDouble(value); hasRadius = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (hasCenter && hasRadius) {
                // 简化：完整椭圆
                double majorLen = std::hypot(rx, ry);
                svg << "<ellipse cx=\"" << FmtFloat(cx) << "\" cy=\"" << FmtFloat(cy)
                    << "\" rx=\"" << FmtFloat(majorLen) << "\" ry=\""
                    << FmtFloat(majorLen * 0.5)
                    << "\" stroke=\"" << DXFColorToHex(entityColor)
                    << "\" stroke-width=\"0.5\" fill=\"none\"/>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "TEXT" || currentEntityType == "MTEXT") {
            double tx = 0, ty = 0;
            double height = 2.5;
            std::string textContent;
            bool hasPos = false;

            do {
                if (code == 10) { tx = ParseDouble(value); }
                else if (code == 20) { ty = ParseDouble(value); hasPos = true; }
                else if (code == 40) { height = ParseDouble(value); }
                else if (code == 1) { textContent = value; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (hasPos && !textContent.empty()) {
                // 转义 XML 特殊字符
                std::string escaped;
                for (char c : textContent) {
                    switch (c) {
                        case '&': escaped += "&amp;"; break;
                        case '<': escaped += "&lt;"; break;
                        case '>': escaped += "&gt;"; break;
                        case '"': escaped += "&quot;"; break;
                        default: escaped += c;
                    }
                }
                svg << "<text x=\"" << FmtFloat(tx) << "\" y=\"" << FmtFloat(ty)
                    << "\" font-size=\"" << FmtFloat(height)
                    << "\" fill=\"" << DXFColorToHex(entityColor) << "\">"
                    << escaped << "</text>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        if (currentEntityType == "POINT") {
            double px = 0, py = 0;
            bool hasPos = false;

            do {
                if (code == 10) { px = ParseDouble(value); }
                else if (code == 20) { py = ParseDouble(value); hasPos = true; }
                else if (code == 62) { entityColor = atoi(value.c_str()); }
            } while (reader.NextPair(code, value) && code != 0);

            if (hasPos) {
                svg << "<circle cx=\"" << FmtFloat(px) << "\" cy=\"" << FmtFloat(py)
                    << "\" r=\"0.5\" fill=\"" << DXFColorToHex(entityColor) << "\"/>\n";
            }

            if (code == 0) {
                resetEntity();
                currentEntityType = value;
            }
            continue;
        }

        // 跳过未知实体类型（读取到下一个 code==0）
        // 不需要做任何事，外层循环会继续
    }

    svg << "</g>\n</svg>";
    return svg.str();
}

} // namespace QuickView
