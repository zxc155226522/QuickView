// 临时测试 harness: 直接调用 LoadDWGtoSVG/LoadDXFtoSVG 验证渲染输出
#include "pch.h"
#include "VectorLoader.h"
#include <cstdio>
#include <fstream>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: _test_dwg <file> [out.svg]\n"); return 1; }
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { printf("cannot open file\n"); return 1; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    printf("file size: %zu\n", data.size());
    fflush(stdout);

    const char* name = argv[1];
    size_t n = strlen(name);
    bool isDxf = (n > 4 && _stricmp(name + n - 4, ".dxf") == 0);

    printf("calling %s...\n", isDxf ? "LoadDXFtoSVG" : "LoadDWGtoSVG");
    fflush(stdout);
    std::string svg = isDxf ? QuickView::LoadDXFtoSVG(data.data(), data.size())
                            : QuickView::LoadDWGtoSVG(data.data(), data.size());
    printf("SVG length: %zu\n", svg.size());
    fflush(stdout);

    const char* out = (argc >= 3) ? argv[2] : "_test_dwg_out.svg";
    FILE* fp = fopen(out, "wb");
    if (fp) {
        fwrite(svg.data(), 1, svg.size(), fp);
        fclose(fp);
        printf("written to %s\n", out);
    }
    return 0;
}
