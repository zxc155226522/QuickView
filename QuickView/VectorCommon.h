#pragma once
// ============================================================================
// VectorCommon.h - 矢量格式加载器共享工具 (PLT / DwgLoader 共用)
// ============================================================================
// 从 VectorLoader.cpp 抽出的通用工具: 坐标格式化、BBox 跟踪、线宽计算、
// XML 文本净化、ACI 颜色表、De Boor 样条采样、临时文件写入。
// ============================================================================
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace QuickView {

// 浮点数格式化 (最多 3 位小数, 去尾零)
inline std::string FmtFloat(double v) {
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
inline std::string CalcStrokeWidth(double maxDim) {
    if (maxDim <= 0) return "0.5";
    double sw = maxDim * 0.0003; // 0.03% of max dimension
    if (sw < 0.05) sw = 0.05;
    return FmtFloat(sw);
}

// XML 文本净化: 转义特殊字符 + 替换非 ASCII 字节为 '?'
// D2D 的 SVG 解析器要求 UTF-8，GBK 编码的中文等非 ASCII 字节会导致解析失败
inline std::string SanitizeXmlText(const std::string& s) {
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

// --- ACI (AutoCAD Color Index) → #RRGGBB ---
inline const char* ACIStandard(int aci) {
    static const char* kTable[] = {
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
    return kTable[aci];
}

inline std::string ACIToHex(int aci) {
    if (aci < 0) aci = -aci;
    if (aci == 256 || aci == 0) return "#000000";
    if (aci >= 1 && aci <= 9) return ACIStandard(aci);
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
inline void DeBoorSpline(
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
inline std::string StripMTextFormatting(const std::string& s) {
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

// --- 临时文件写入 (dwg_read_file 接受 const char* 文件名) ---
inline std::string WriteTempFile(const uint8_t* data, size_t size, const char* prefix) {
    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath) == 0) return "";
    char tempFile[MAX_PATH];
    if (GetTempFileNameA(tempPath, prefix, 0, tempFile) == 0) return "";
    FILE* fp = nullptr;
    fopen_s(&fp, tempFile, "wb");
    if (!fp) return "";
    fwrite(data, 1, size, fp);
    fclose(fp);
    return tempFile;
}

// SVG 文档外壳: 由 viewBox 与 body 组装完整 SVG XML
inline std::string AssembleSvg(const BBox& bbox, std::string bodyStr) {
    if (!bbox.valid) return "";
    double margin = bbox.maxDim() * 0.02;
    if (margin < 1.0) margin = 1.0;
    double vbX = bbox.minX - margin;
    double vbY = -bbox.maxY - margin; // Y 翻转后的最小值
    double vbW = bbox.width() + margin * 2;
    double vbH = bbox.height() + margin * 2;
    if (vbW <= 0) vbW = 100;
    if (vbH <= 0) vbH = 100;

    // 替换占位 stroke-width
    std::string strokeW = CalcStrokeWidth(bbox.maxDim());
    {
        const std::string placeholder = "\" stroke-width=\"__SW__\"";
        const std::string replacement = "\" stroke-width=\"" + strokeW + "\"";
        size_t p = 0;
        while ((p = bodyStr.find(placeholder, p)) != std::string::npos) {
            bodyStr.replace(p, placeholder.size(), replacement);
            p += replacement.size();
        }
    }

    std::string svg;
    svg.reserve(bodyStr.size() + 256);
    svg += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\"";
    svg += " viewBox=\"" + FmtFloat(vbX) + " " + FmtFloat(vbY) + " " +
           FmtFloat(vbW) + " " + FmtFloat(vbH) + "\"";
    svg += " width=\"" + FmtFloat(vbW) + "\" height=\"" + FmtFloat(vbH) + "\">\n";
    svg += bodyStr;
    svg += "</svg>";
    return svg;
}

} // namespace QuickView
