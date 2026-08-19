// ============================================================================
// VectorLoader.cpp - PLT(HPGL) → SVG XML 转换器
// ============================================================================
// PLT: 手写 HPGL 解析器 (IN/SP/PU/PD/PA/CI/AA 命令)
// DXF/DWG 解析已迁移至 DwgLoader.cpp (via GNU LibreDWG)。
// 共享工具 (FmtFloat/BBox/AssembleSvg 等) 见 VectorCommon.h。
// Y 轴翻转策略: 直接在坐标中取负 (y' = -y)
// viewBox 从实际绘制坐标计算，不依赖文件头元数据。
// stroke-width 按坐标系比例设置，避免大坐标系下线宽不可见。
// ============================================================================
#include "pch.h"
#include "VectorLoader.h"
#include "VectorCommon.h"
#include <cmath>
#include <cstring>
#include <sstream>
#include <string_view>

namespace QuickView {

// ============================================================================
// PLT(HPGL) 解析器
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
}

std::string LoadPLTtoSVG(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};

    std::string_view text(reinterpret_cast<const char*>(data), size);

    int currentPen = 1;
    bool penDown = false;
    double curX = 0, curY = 0; // 原始 HPGL 坐标
    BBox bbox;

    // 收集所有 path 元素（用原始坐标），最后统一翻转 Y
    // 用占位 stroke-width，AssembleSvg 最终替换
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

    return AssembleSvg(bbox, paths.str());
}

} // namespace QuickView
