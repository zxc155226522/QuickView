// 直接调用 QuickView ThumbnailProvider COM，验证指定文件缩略图是否生成
#include <windows.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <shlwapi.h>
#include <string>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")

// CLSID_QuickViewThumbnailProvider
static const CLSID CLSID_QVThumb =
{ 0x4F8C2A6E, 0x3B5D, 0x4E7F, { 0x9A, 0x1C, 0x2D, 0x3E, 0x4F, 0x5A, 0x6B, 0x7C } };

static bool SaveArgbHbmp(HBITMAP hbmp, const wchar_t* path) {
    BITMAP bm = {};
    if (!GetObject(hbmp, sizeof(bm), &bm)) return false;
    if (bm.bmBitsPixel != 32) return false;

    int w = bm.bmWidth, h = bm.bmHeight;
    DWORD row = ((w * 4 + 3) & ~3);
    DWORD dibSize = row * abs(h);
    std::vector<BYTE> dib(dibSize);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = h; // top-down if negative
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC hdc = GetDC(nullptr);
    if (GetDIBits(hdc, hbmp, 0, abs(h), dib.data(), &bmi, DIB_RGB_COLORS) != abs(h)) {
        ReleaseDC(nullptr, hdc); return false;
    }
    ReleaseDC(nullptr, hdc);

    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + dibSize;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bmi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
    fwrite(dib.data(), dibSize, 1, f);
    fclose(f);
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        wprintf(L"usage: com_test_one.exe <tif-path>\n");
        return 2;
    }
    const wchar_t* filePath = argv[1];

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { wprintf(L"CoInit failed 0x%08X\n", hr); return 1; }

    IThumbnailProvider* thumb = nullptr;
    hr = CoCreateInstance(CLSID_QVThumb, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IThumbnailProvider, (void**)&thumb);
    if (FAILED(hr)) {
        wprintf(L"CoCreateInstance failed 0x%08X\n", hr);
        CoUninitialize();
        return 1;
    }

    IInitializeWithFile* initFile = nullptr;
    hr = thumb->QueryInterface(IID_IInitializeWithFile, (void**)&initFile);
    if (FAILED(hr)) {
        wprintf(L"QI IInitializeWithFile failed 0x%08X\n", hr);
        thumb->Release();
        CoUninitialize();
        return 1;
    }

    hr = initFile->Initialize(filePath, STGM_READ);
    initFile->Release();
    if (FAILED(hr)) {
        wprintf(L"Initialize failed 0x%08X\n", hr);
        thumb->Release();
        CoUninitialize();
        return 1;
    }

    HBITMAP hbmp = nullptr;
    WTS_ALPHATYPE alpha = WTSAT_UNKNOWN;
    hr = thumb->GetThumbnail(256, &hbmp, &alpha);
    wprintf(L"GetThumbnail hr=0x%08X hbmp=%p alpha=%d\n", hr, (void*)hbmp, (int)alpha);

    if (SUCCEEDED(hr) && hbmp) {
        BITMAP bm = {};
        GetObject(hbmp, sizeof(bm), &bm);
        wprintf(L"bitmap %dx%d %dbpp\n", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
        wchar_t outPath[MAX_PATH];
        GetTempPathW(MAX_PATH, outPath);
        wcscat_s(outPath, L"qv_com_test_one.bmp");
        if (SaveArgbHbmp(hbmp, outPath)) {
            wprintf(L"saved %s\n", outPath);
        }
        DeleteObject(hbmp);
    }

    thumb->Release();
    CoUninitialize();
    return FAILED(hr) ? 1 : 0;
}
