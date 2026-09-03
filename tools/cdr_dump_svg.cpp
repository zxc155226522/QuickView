// Diagnostic tool: parse CDR/CMX via vendored libcdr -> dump per-page SVG.
// Usage: cdr_dump_svg.exe <in.cdr> [outdir]
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include "librevenge/librevenge.h"
#include "librevenge-stream/librevenge-stream.h"
#include "libcdr/libcdr.h"

int main(int argc, char **argv)
{
  if (argc < 2)
  {
    fprintf(stderr, "usage: %s <in.cdr> [outdir]\n", argv[0]);
    return 1;
  }
  const char *inPath = argv[1];
  std::string outDir = argc >= 3 ? argv[2] : ".";

  std::ifstream f(inPath, std::ios::binary);
  if (!f)
  {
    fprintf(stderr, "cannot open %s\n", inPath);
    return 1;
  }
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  fprintf(stderr, "file: %s size=%zu head=%02X%02X%02X%02X\n", inPath, data.size(),
          (unsigned char)data[0], (unsigned char)data[1], (unsigned char)data[2], (unsigned char)data[3]);

  librevenge::RVNGStringStream input((const unsigned char *)data.data(), (unsigned)data.size());

  // Version detection (mirror CDRDocument::getCDRVersion)
  {
    const unsigned char *d = (const unsigned char *)data.data();
    if (data.size() >= 9 && !memcmp(d, "RIFF", 4))
      fprintf(stderr, "cdr magic: %.4s version-tag=%c -> ~CDR %d00\n", (const char *)d + 8, d[8],
              (d[8] >= '0' && d[8] <= '9') ? (d[8] - '0') : -1);
  }

  bool isCdr = libcdr::CDRDocument::isSupported(&input);
  input.seek(0, librevenge::RVNG_SEEK_SET);
  bool isCmx = false;
  if (!isCdr)
  {
    isCmx = libcdr::CMXDocument::isSupported(&input);
    input.seek(0, librevenge::RVNG_SEEK_SET);
  }
  fprintf(stderr, "supported: cdr=%d cmx=%d\n", isCdr ? 1 : 0, isCmx ? 1 : 0);
  if (!isCdr && !isCmx)
    return 2;

  librevenge::RVNGStringVector pages;
  librevenge::RVNGSVGDrawingGenerator painter(pages, "");
  bool ok = isCdr ? libcdr::CDRDocument::parse(&input, &painter)
                  : libcdr::CMXDocument::parse(&input, &painter);
  fprintf(stderr, "parse=%d pages=%zu\n", ok ? 1 : 0, pages.size());
  if (!ok || pages.empty())
    return 3;

  std::string base = inPath;
  size_t slash = base.find_last_of("/\\");
  if (slash != std::string::npos)
    base = base.substr(slash + 1);
  size_t dot = base.find_last_of('.');
  if (dot != std::string::npos)
    base = base.substr(0, dot);

  for (size_t i = 0; i < pages.size(); ++i)
  {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_p%02d.svg", outDir.c_str(), base.c_str(), (int)i);
    std::ofstream o(path, std::ios::binary);
    o.write(pages[i].cstr(), (std::streamsize)strlen(pages[i].cstr()));
    fprintf(stderr, "wrote %s size=%zu\n", path, strlen(pages[i].cstr()));
  }
  return 0;
}
