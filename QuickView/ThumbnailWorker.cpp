// ============================================================================
// ThumbnailWorker.cpp
// Headless thumbnail generator: reuses CImageLoader::LoadThumbnail (the same
// pipeline as the in-app gallery) and writes a 32bpp BGRA BMP for the shell
// thumbnail provider DLL (QuickViewThumbnailProvider.dll).
//
//   QuickView.exe --thumbnail --input <file> --out <bmp> --size <px>
//
// Vector formats (SVG/CDR/CMX/PLT/DXF/DWG) render on a TRANSPARENT background
// (Explorer draws its own plate). PDF/AI stay opaque white (paper semantics).
// ============================================================================
#include "ThumbnailWorker.h"
#include "ImageLoader.h"
#include "SupportedExtensions.h"

#include <string>
#include <vector>

namespace {

bool TryReadArgValue(int argc, LPWSTR* argv, const wchar_t* name,
                     std::wstring* out) {
  if (!out) return false;
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] && _wcsicmp(argv[i], name) == 0 && argv[i + 1]) {
      *out = argv[i + 1];
      return true;
    }
  }
  return false;
}

bool TryReadPositiveIntArg(int argc, LPWSTR* argv, const wchar_t* name,
                           int* out) {
  std::wstring val;
  if (!TryReadArgValue(argc, argv, name, &val)) return false;
  wchar_t* end = nullptr;
  long v = wcstol(val.c_str(), &end, 10);
  if (!end || *end != L'\0' || v <= 0 || v > 4096) return false;
  *out = static_cast<int>(v);
  return true;
}

// Vector formats: transparent background. Documents (PDF/AI): opaque white.
bool WantsTransparentBackground(const std::wstring& path) {
  std::wstring_view ext = QuickView::ExtensionOf(path);
  return QuickView::ExtEqualsIgnoreCase(ext, L".svg") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".cdr") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".cmx") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".plt") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".dxf") ||
         QuickView::ExtEqualsIgnoreCase(ext, L".dwg");
}

// Write 32bpp BGRA top-down BMP. Alpha bytes are preserved verbatim (some
// viewers ignore the alpha channel, but our provider DLL reads it back raw).
bool WriteBmp32(const std::wstring& outPath, const uint8_t* pixels, int width,
                int height, int stride) {
  if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return false;

  BITMAPFILEHEADER bfh = {};
  BITMAPINFOHEADER bih = {};
  bih.biSize = sizeof(BITMAPINFOHEADER);
  bih.biWidth = width;
  bih.biHeight = -height; // top-down
  bih.biPlanes = 1;
  bih.biBitCount = 32;
  bih.biCompression = BI_RGB;

  const DWORD rowBytes = static_cast<DWORD>(width) * 4;
  const DWORD imageBytes = rowBytes * static_cast<DWORD>(height);
  bfh.bfType = 0x4D42; // 'BM'
  bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
  bfh.bfSize = bfh.bfOffBits + imageBytes;

  HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (hFile == INVALID_HANDLE_VALUE) return false;

  bool ok = false;
  DWORD written = 0;
  if (WriteFile(hFile, &bfh, sizeof(bfh), &written, nullptr) &&
      WriteFile(hFile, &bih, sizeof(bih), &written, nullptr)) {
    ok = true;
    for (int y = 0; y < height && ok; ++y) {
      ok = WriteFile(hFile, pixels + static_cast<size_t>(y) * stride, rowBytes,
                     &written, nullptr) != FALSE;
    }
  }
  CloseHandle(hFile);
  if (!ok) DeleteFileW(outPath.c_str());
  return ok;
}

} // namespace

int QuickView::RunThumbnailWorker(int argc, LPWSTR* argv) {
  std::wstring inputPath;
  std::wstring outPath;
  int size = 0;
  if (!TryReadArgValue(argc, argv, L"--input", &inputPath) ||
      !TryReadArgValue(argc, argv, L"--out", &outPath) ||
      !TryReadPositiveIntArg(argc, argv, L"--size", &size)) {
    return 2;
  }

  // WIC is used by some thumbnail fallbacks inside LoadThumbnail.
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  CImageLoader loader;
  CImageLoader::ThumbData thumb;
  const bool transparentBg = WantsTransparentBackground(inputPath);
  HRESULT hr =
      loader.LoadThumbnail(inputPath.c_str(), size, &thumb, true, transparentBg);

  bool ok = SUCCEEDED(hr) && thumb.isValid && !thumb.pixels.empty() &&
            WriteBmp32(outPath, thumb.pixels.data(), thumb.width, thumb.height,
                       thumb.stride);

  if (SUCCEEDED(coHr)) CoUninitialize();
  return ok ? 0 : 2;
}
