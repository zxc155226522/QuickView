// Test: how does WIC handle 32bpp BI_RGB BMP with alpha byte = 0?
// Mirrors the app's ConvertBmpDataUrisToPng pipeline (decode -> 32bppBGRA -> PNG).
#include <cstdio>
#include <vector>
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")

int main(int argc, char **argv)
{
  if (argc < 2) { fprintf(stderr, "usage: %s <file.bmp>\n", argv[0]); return 1; }
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IWICImagingFactory *fac = nullptr;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));

  IWICBitmapDecoder *dec = nullptr;
  HANDLE h = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "open fail\n"); return 1; }
  DWORD size = GetFileSize(h, nullptr);
  std::vector<uint8_t> buf(size);
  DWORD rd; ReadFile(h, buf.data(), size, &rd, nullptr); CloseHandle(h);

  IWICStream *st = nullptr;
  fac->CreateStream(&st);
  st->InitializeFromMemory(buf.data(), (DWORD)buf.size());
  HRESULT hr = fac->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
  printf("CreateDecoderFromStream hr=0x%08lX\n", hr);
  IWICBitmapFrameDecode *frame = nullptr;
  dec->GetFrame(0, &frame);
  WICPixelFormatGUID pf;
  frame->GetPixelFormat(&pf);
  WCHAR pfs[64] = L"?";
  StringFromGUID2(pf, pfs, 64);
  wprintf(L"native pixel format: %s\n", pfs);
  UINT w, hh; frame->GetSize(&w, &hh);
  printf("size: %ux%u\n", w, hh);

  IWICFormatConverter *conv = nullptr;
  fac->CreateFormatConverter(&conv);
  hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
  printf("convert to 32bppBGRA hr=0x%08lX\n", hr);
  std::vector<uint8_t> px((size_t)w * hh * 4);
  conv->CopyPixels(nullptr, w * 4, (DWORD)px.size(), px.data());
  size_t n = (size_t)w * hh, a0 = 0, a255 = 0;
  for (size_t i = 0; i < n; ++i)
  {
    uint8_t a = px[i * 4 + 3];
    if (a == 0) ++a0; else if (a == 255) ++a255;
  }
  printf("after conversion to BGRA: alpha=0: %.1f%%  alpha=255: %.1f%%\n", 100.0 * a0 / n, 100.0 * a255 / n);
  return 0;
}
