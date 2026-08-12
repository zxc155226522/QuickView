// ============================================================================
// QuickViewThumbnailProvider.cpp
// COM DLL: Windows Shell IThumbnailProvider for QuickView vector/document
// formats (.cdr .cmx .plt .dxf .dwg .pdf .ai).
//
// Architecture: thin out-of-process shim. GetThumbnail() launches
//   QuickView.exe --thumbnail --input <file> --out <tmp.bmp> --size <px>
// which reuses the FULL application rendering pipeline (libcdr / VectorLoader
// / MuPDF via CImageLoader::LoadThumbnail), then converts the resulting
// 32bpp BGRA BMP into an HBITMAP. This keeps the shell extension tiny and
// guarantees Explorer thumbnails always match what the app itself renders.
// ============================================================================
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>          // SHChangeNotify
#include <shobjidl.h>        // IShellItem / SIGDN_FILESYSPATH
#include <shlwapi.h>
#include <strsafe.h>

#include <string>
#include <vector>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <memory>
#include <gdiplus.h>
#include <objidl.h>          // STATSTG, STATFLAG_NONAME
#include <atomic>            // bounded one-shot worker fallback

// Bound concurrent one-shot worker spawns so extreme server overflow (e.g.
// >kSlowCap CDR requests at once) cannot re-create the old per-thumbnail
// process storm.
static const int kOneShotMax = 4;
static std::atomic<int> g_oneShotActive{0};

// ============================================================================
// Manual interface declarations (kept minimal; the project's adaptive SDK
// setup does not always expose these via the standard headers)
// ============================================================================

// IInitializeWithFile (propsys.h): lets us recover the on-disk file path,
// which the out-of-process worker needs (IInitializeWithStream cannot).
#ifndef __IInitializeWithFile_INTERFACE_DEFINED__
#define __IInitializeWithFile_INTERFACE_DEFINED__
MIDL_INTERFACE("b7d14566-0509-4cce-a71f-0a554233bd9b")
IInitializeWithFile : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath, DWORD grfMode) = 0;
};
#endif

// IInitializeWithStream (propsys.h): Windows 10/11 Explorer PREFERS this over
// IInitializeWithFile and hands us only a byte stream (no path). Because we need
// the real on-disk path for the badge extension, we deliberately do NOT implement
// this interface — so the shell falls back to IInitializeWithFile and gives us the
// path directly. (The interface definition is kept below only for completeness.)
#ifndef __IInitializeWithStream_INTERFACE_DEFINED__
#define __IInitializeWithStream_INTERFACE_DEFINED__
MIDL_INTERFACE("b824b49d-22ac-4161-ac8a-9916e8fa3f7f")
IInitializeWithStream : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE Initialize(IStream* pstream, DWORD grfMode) = 0;
};
#endif

// WTS_ALPHATYPE
typedef enum __MIDL___MIDL_itf_shobjidl_0000_0000_0002 {
    WTSAT_UNKNOWN = 0,
    WTSAT_RGB = 1,
    WTSAT_ARGB = 2,
} WTS_ALPHATYPE;

// IObjectWithSite (oleidl.h) — kept for interface completeness. In practice
// Explorer does not call SetSite for our provider (verified via debug log), so
// the real path is obtained via IInitializeWithFile instead. SetSite is a no-op.
#ifndef __IObjectWithSite_INTERFACE_DEFINED__
#define __IObjectWithSite_INTERFACE_DEFINED__
MIDL_INTERFACE("FC4801A3-2BA9-11CF-A229-00AA003D7352")
IObjectWithSite : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) = 0;
};
#endif

// IThumbnailProvider
#ifndef __IThumbnailProvider_INTERFACE_DEFINED__
#define __IThumbnailProvider_INTERFACE_DEFINED__
MIDL_INTERFACE("e357fccd-a995-4576-b01f-234630154e96")
IThumbnailProvider : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha) = 0;
};
#endif

// ============================================================================
// CLSID
// {4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}
// ============================================================================
DEFINE_GUID(CLSID_QuickViewThumbnailProvider,
    0x4F8C2A6E, 0x3B5D, 0x4E7F, 0x9A, 0x1C, 0x2D, 0x3E, 0x4F, 0x5A, 0x6B, 0x7C);

// All extensions handled by this provider (registered in DllRegisterServer
// and mirrored by SettingsOverlay::RegisterAssociations).
static const wchar_t* kThumbnailExts[] = {
    L".cdr", L".cmx", L".plt", L".dxf", L".dwg", L".pdf", L".ai", L".svg", L".svgz",
    L".tif", L".tiff"
};

// Hard cap for a single thumbnail render. Explorer calls us on a thread pool;
// a hung worker must not block extraction forever.
static constexpr DWORD kPipeTimeoutMs = 60000; // raised from 15s: large/CDR thumbs need more time

static LONG g_cRefModule = 0;
static LONG g_cRefServer = 0;
static HMODULE g_hModule = nullptr;
static LONG g_tempCounter = 0;

// [Debug] Log provider activity to diagnose shell invocation issues.
static void DbgLog(const wchar_t* msg) {
    HANDLE h = CreateFileW(L"C:\\Windows\\Temp\\qvthumb_provider.log",
                           FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    wchar_t line[512];
    StringCchPrintfW(line, 512, L"[%lu tid=%lu] %s\r\n", GetCurrentProcessId(),
                     GetCurrentThreadId(), msg);
    DWORD written;
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
}

// ============================================================================
// Helpers
// ============================================================================

// QuickView.exe lives next to this DLL (same install/build directory).
static bool GetHostExePath(std::wstring& outExe) {
    wchar_t dllPath[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, dllPath, MAX_PATH)) return false;
    std::wstring dir(dllPath);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    outExe = dir.substr(0, slash) + L"\\QuickView.exe";
    return GetFileAttributesW(outExe.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::wstring MakeTempBmpPath() {
    wchar_t tempDir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDir)) return {};
    wchar_t name[128];
    StringCchPrintfW(name, 128, L"qvthumb_%lu_%lu_%ld.bmp",
                     GetCurrentProcessId(), GetCurrentThreadId(),
                     InterlockedIncrement(&g_tempCounter));
    return std::wstring(tempDir) + name;
}

// Run "QuickView.exe --thumbnail" synchronously. Returns true when the worker
// exited with code 0 within the timeout.
static bool RunThumbnailWorker(const std::wstring& exePath,
                               const std::wstring& inputPath,
                               const std::wstring& outBmp, UINT size) {
    std::wstring cmd = L"\"" + exePath + L"\" --thumbnail --input \"" + inputPath +
                       L"\" --out \"" + outBmp + L"\" --size " + std::to_wstring(size);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd; // CreateProcessW may write to this buffer
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }

    DWORD wait = WaitForSingleObject(pi.hProcess, kPipeTimeoutMs);
    bool ok = false;
    if (wait == WAIT_OBJECT_0) {
        DWORD exitCode = 1;
        if (GetExitCodeProcess(pi.hProcess, &exitCode) && exitCode == 0) ok = true;
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok;
}

// Read the 32bpp top-down BGRA BMP written by the worker into an HBITMAP
// suitable for IThumbnailProvider (keeps the alpha channel verbatim).
static HBITMAP LoadWorkerBmp(const std::wstring& bmpPath) {
    HANDLE hFile = CreateFileW(bmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return nullptr;

    HBITMAP result = nullptr;
    do {
        BITMAPFILEHEADER bfh;
        BITMAPINFOHEADER bih;
        DWORD read = 0;
        if (!ReadFile(hFile, &bfh, sizeof(bfh), &read, nullptr) || read != sizeof(bfh)) break;
        if (!ReadFile(hFile, &bih, sizeof(bih), &read, nullptr) || read != sizeof(bih)) break;
        if (bfh.bfType != 0x4D42) break;
        if (bih.biSize != sizeof(BITMAPINFOHEADER) || bih.biBitCount != 32 ||
            bih.biCompression != BI_RGB || bih.biWidth <= 0 || bih.biHeight == 0) break;

        const LONG w = bih.biWidth;
        const LONG h = bih.biHeight < 0 ? -bih.biHeight : bih.biHeight;
        const bool topDown = bih.biHeight < 0;
        const DWORD rowBytes = (DWORD)w * 4;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h; // top-down DIB
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC hdc = GetDC(nullptr);
        HBITMAP hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        if (!hbmp || !bits) break;

        if (SetFilePointer(hFile, bfh.bfOffBits, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
            DeleteObject(hbmp);
            break;
        }

        bool ok = true;
        for (LONG y = 0; y < h && ok; ++y) {
            LONG dstY = topDown ? y : (h - 1 - y);
            ok = ReadFile(hFile, (BYTE*)bits + (size_t)dstY * rowBytes, rowBytes,
                          &read, nullptr) && read == rowBytes;
        }
        if (!ok) {
            DeleteObject(hbmp);
            break;
        }
        result = hbmp;
    } while (false);

    CloseHandle(hFile);
    return result;
}

// ---------------------------------------------------------------------------
// Persistent server IPC client (named pipe). Replaces the per-thumbnail
// CreateProcess storm with one long-lived worker process.
// ---------------------------------------------------------------------------
static const wchar_t kPipeName[] = L"\\\\.\\pipe\\QuickViewThumb";

static bool PipeIoExact(HANDLE hPipe, HANDLE hEvent, void* buf, DWORD len,
                        DWORD timeoutMs, bool isRead) {
    BYTE* p = static_cast<BYTE*>(buf);
    DWORD total = 0;
    while (total < len) {
        ResetEvent(hEvent);
        OVERLAPPED ol = {};
        ol.hEvent = hEvent;
        DWORD done = 0;
        BOOL ok = isRead ? ReadFile(hPipe, p + total, len - total, &done, &ol)
                         : WriteFile(hPipe, p + total, len - total, &done, &ol);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (WaitForSingleObject(hEvent, timeoutMs) != WAIT_OBJECT_0) {
                    CancelIo(hPipe);
                    GetOverlappedResult(hPipe, &ol, &done, TRUE); // reap aborted op
                    return false;
                }
                if (!GetOverlappedResult(hPipe, &ol, &done, FALSE)) return false;
            } else {
                return false;
            }
        }
        if (done == 0) return false; // peer closed early
        total += done;
    }
    return true;
}
static bool PipeReadExact(HANDLE hPipe, HANDLE hEvent, void* buf, DWORD len, DWORD ms) {
    return PipeIoExact(hPipe, hEvent, buf, len, ms, true);
}
static bool PipeWriteAll(HANDLE hPipe, HANDLE hEvent, const void* buf, DWORD len, DWORD ms) {
    return PipeIoExact(hPipe, hEvent, const_cast<void*>(buf), len, ms, false);
}

// Convert an in-memory 32bpp BGRA BMP (same layout WriteBmp32 produces) to HBITMAP.
static HBITMAP BmpBytesToHBITMAP(const BYTE* data, size_t len) {
    if (!data || len < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) return nullptr;
    const BITMAPFILEHEADER* bfh = reinterpret_cast<const BITMAPFILEHEADER*>(data);
    const BITMAPINFOHEADER* bih = reinterpret_cast<const BITMAPINFOHEADER*>(data + sizeof(BITMAPFILEHEADER));
    if (bfh->bfType != 0x4D42) return nullptr;
    if (bih->biSize != sizeof(BITMAPINFOHEADER) || bih->biBitCount != 32 ||
        bih->biCompression != BI_RGB || bih->biWidth <= 0 || bih->biHeight == 0) return nullptr;
    const LONG w = bih->biWidth;
    const LONG h = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
    const bool topDown = bih->biHeight < 0;
    const DWORD rowBytes = static_cast<DWORD>(w) * 4;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hbmp || !bits) return nullptr;
    const BYTE* pix = data + bfh->bfOffBits;
    for (LONG y = 0; y < h; ++y) {
        LONG dstY = topDown ? y : (h - 1 - y);
        memcpy(static_cast<BYTE*>(bits) + static_cast<size_t>(dstY) * rowBytes,
               pix + static_cast<size_t>(y) * rowBytes, rowBytes);
    }
    return hbmp;
}

static HANDLE OpenServerPipe() {
    return CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
}

static void LaunchServer(const std::wstring& exePath) {
    std::wstring cmd = L"\"" + exePath + L"\" --thumbnail-server --idle 60";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    if (CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Result of one pipe transaction. Stale means the server dropped this request
// (its queue was overwhelmed by newer requests, e.g. the user switched
// folders); the provider must NOT fall back to a one-shot worker then, or we
// would re-introduce the per-thumbnail process storm we removed.
enum class PipeResult { Ok, Stale, Error };

// One full request/response over the pipe, bounded by a 15s timeout so a
// wedged server can never block Explorer forever.
static PipeResult PipeTransaction(HANDLE hPipe, const std::wstring& inputPath, UINT size,
                                  std::vector<BYTE>& outBmp) {
    HANDLE hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!hEvent) return PipeResult::Error;

    uint32_t pathByteLen = static_cast<uint32_t>(inputPath.size() * sizeof(wchar_t));
    bool ok = PipeWriteAll(hPipe, hEvent, &pathByteLen, sizeof(pathByteLen), kPipeTimeoutMs) &&
              PipeWriteAll(hPipe, hEvent, inputPath.c_str(), pathByteLen, kPipeTimeoutMs) &&
              PipeWriteAll(hPipe, hEvent, &size, sizeof(uint32_t), kPipeTimeoutMs);
    PipeResult result = PipeResult::Error;
    if (ok) {
        uint32_t status = 1;
        if (PipeReadExact(hPipe, hEvent, &status, sizeof(status), kPipeTimeoutMs)) {
            if (status == 0) {
                uint32_t bmpLen = 0;
                if (PipeReadExact(hPipe, hEvent, &bmpLen, sizeof(bmpLen), kPipeTimeoutMs) &&
                    bmpLen > 0 && bmpLen <= 20 * 1024 * 1024) {
                    outBmp.resize(bmpLen);
                    ok = PipeReadExact(hPipe, hEvent, outBmp.data(), bmpLen, kPipeTimeoutMs);
                    result = ok ? PipeResult::Ok : PipeResult::Error;
                } else {
                    result = PipeResult::Error;
                }
            } else if (status == 3) {
                result = PipeResult::Stale; // server dropped this as stale
            } else {
                result = PipeResult::Error;
            }
        }
    }
    CloseHandle(hEvent);
    return result;
}

// Try the persistent server; launch it on first use; relaunch once on error.
// A Stale result is returned as-is (no relaunch, no fallback).
static PipeResult RequestThumbnailViaPipe(const std::wstring& exePath,
                                          const std::wstring& inputPath, UINT size,
                                          std::vector<BYTE>& outBmp) {
    bool launched = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE hPipe = INVALID_HANDLE_VALUE;
        for (int k = 0; k < 60 && hPipe == INVALID_HANDLE_VALUE; ++k) {
            hPipe = OpenServerPipe();
            if (hPipe != INVALID_HANDLE_VALUE) break;
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND && !launched) {
                LaunchServer(exePath);
                launched = true;
            }
            Sleep(100);
        }
        if (hPipe == INVALID_HANDLE_VALUE) break; // unreachable -> one-shot fallback
        PipeResult r = PipeTransaction(hPipe, inputPath, size, outBmp);
        CloseHandle(hPipe);
        if (r == PipeResult::Ok) return PipeResult::Ok;
        if (r == PipeResult::Stale) return PipeResult::Stale; // do not relaunch
        launched = false; // error -> relaunch on next attempt
    }
    return PipeResult::Error;
}

// ============================================================================
// Type badge (capsule, top-right, category color) composited into the
// returned thumbnail bitmap via GDI+. Explorer owns the render loop, so the
// badge can only travel WITH the thumbnail (baked in), never separately.
// ============================================================================
namespace {
    // 扩展名 -> 基础色（GDI+ Color，alpha=255），绘制时再乘 alpha
    Gdiplus::Color BadgeColorForGdi(const std::wstring& ext) {
        auto in = [&](const wchar_t* s) -> bool {
            return ext.size() == wcslen(s) && _wcsicmp(ext.c_str(), s) == 0;
        };
        if (in(L"PNG")||in(L"JPG")||in(L"JPEG")||in(L"BMP")||in(L"GIF")||in(L"WEBP")||in(L"HEIC")||in(L"TIF")||in(L"TIFF")) return Gdiplus::Color(16,185,129);
        if (in(L"CR2")||in(L"CR3")||in(L"ARW")||in(L"NEF")||in(L"DNG")||in(L"RAF")||in(L"RW2")||in(L"ORF")) return Gdiplus::Color(139,92,246);
        if (in(L"CDR")||in(L"CMX")||in(L"AI")||in(L"SVG")||in(L"SVGZ")||in(L"EPS")) return Gdiplus::Color(59,130,246);
        if (in(L"PDF")||in(L"TXT")||in(L"DOC")||in(L"DOCX")||in(L"XLS")||in(L"XLSX")||in(L"PPT")||in(L"PPTX")) return Gdiplus::Color(239,68,68);
        if (in(L"PLT")||in(L"DXF")||in(L"DWG")) return Gdiplus::Color(6,182,212);
        if (in(L"MP4")||in(L"MOV")||in(L"AVI")||in(L"MKV")||in(L"WEBM")) return Gdiplus::Color(244,63,94);
        if (in(L"MP3")||in(L"WAV")||in(L"FLAC")||in(L"OGG")||in(L"M4A")) return Gdiplus::Color(34,197,94);
        if (in(L"ZIP")||in(L"RAR")||in(L"7Z")||in(L"TAR")) return Gdiplus::Color(245,158,11);
        if (in(L"CPP")||in(L"H")||in(L"PY")||in(L"JS")||in(L"TS")||in(L"JSON")||in(L"XML")||in(L"HTML")) return Gdiplus::Color(99,102,241);
        return Gdiplus::Color(100,116,139);
    }

    std::wstring GetExtUpper(const std::wstring& p) {
        size_t dot = p.find_last_of(L'.');
        if (dot == std::wstring::npos || dot + 1 >= p.size()) return L"";
        std::wstring e = p.substr(dot + 1);
        for (auto& c : e) c = (wchar_t)std::towupper((wint_t)c);
        return e;
    }

    // 圆角矩形路径。radius 直接表示圆角半径（弧外接矩形边长 = 2*radius），
    // 故传 radius = 半高 即得两端半圆胶囊。
    void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
        radius = (std::min)(radius, (std::min)(w, h) * 0.5f);
        const float d = radius * 2.0f;
        path.StartFigure();
        path.AddLine(x + radius, y, x + w - radius, y);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddLine(x + w, y + radius, x + w, y + h - radius);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddLine(x + w - radius, y + h, x + radius, y + h);
        path.AddArc(x + radius, y + h - d, d, d, 90.0f, 90.0f);
        path.AddLine(x, y + h - radius, x, y + radius);
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.CloseFigure();
    }

    // 把类型角标烤进缩略图位图（直写 DIB 像素，top-down 32bpp ARGB）
    void CompositeTypeBadge(HBITMAP hbmp, const std::wstring& extUpper) {
        if (!hbmp || extUpper.empty()) return;
        BITMAP bm;
        if (GetObject(hbmp, sizeof(bm), &bm) != sizeof(bm)) return;
        if (bm.bmBitsPixel != 32 || !bm.bmBits) return;
        const int w = bm.bmWidth, h = bm.bmHeight;
        if (w <= 0 || h <= 0) return;

        static std::once_flag s_init;
        static ULONG_PTR s_token = 0;
        std::call_once(s_init, []() {
            Gdiplus::GdiplusStartupInput gsi;
            Gdiplus::GdiplusStartup(&s_token, &gsi, nullptr);
        });

        Gdiplus::Bitmap gbm(w, h, w * 4, PixelFormat32bppARGB, (BYTE*)bm.bmBits);
        Gdiplus::Graphics g(&gbm);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

        // 字号随缩略图大小缩放，但严格限幅，避免小图上过大。
        float fontSize = (float)h * 0.05f;
        if (fontSize < 9.0f)  fontSize = 9.0f;
        if (fontSize > 16.0f) fontSize = 16.0f;

        // 右上角与图片之间的间隔（空隙，不贴边，避免与图内图标重叠）。
        const float margin = (std::max)(6.0f, (float)h * 0.06f);
        const float availW = (float)w - 2.0f * margin;
        const float availH = (float)h - 2.0f * margin;

        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        Gdiplus::Font font(L"Segoe UI", fontSize, Gdiplus::FontStyleBold);
        Gdiplus::RectF layout(0, 0, (Gdiplus::REAL)w, (Gdiplus::REAL)h);
        Gdiplus::RectF bound;
        g.MeasureString(extUpper.c_str(), (INT)extUpper.length(), &font, layout, &sf, &bound);

        float bh = fontSize * 1.6f;
        float bw = bound.Width + fontSize * 1.0f;
        // 边界保护：角标（含圆角）必须完整落在图内，越界则字号/尺寸等比缩小。
        const bool needScale = (bw > availW || bh > availH);
        std::unique_ptr<Gdiplus::Font> scaledFont;
        const Gdiplus::Font* pFont = &font;
        if (needScale) {
            float s = (std::min)(availW / bw, availH / bh);
            fontSize *= s; bh *= s; bw *= s;
            scaledFont.reset(new Gdiplus::Font(L"Segoe UI", fontSize, Gdiplus::FontStyleBold));
            pFont = scaledFont.get();
        }
        const float x = (float)w - margin - bw;
        const float y = margin;
        const float r = bh * 0.5f; // 胶囊

        Gdiplus::Color base = BadgeColorForGdi(extUpper);
        Gdiplus::SolidBrush brushPill(Gdiplus::Color(170, base.GetR(), base.GetG(), base.GetB()));
        Gdiplus::GraphicsPath path;
        AddRoundedRect(path, x, y, bw, bh, r);
        g.FillPath(&brushPill, &path);

        Gdiplus::SolidBrush brushText(Gdiplus::Color(255, 255, 255, 255));
        g.DrawString(extUpper.c_str(), (INT)extUpper.length(), pFont,
                     Gdiplus::RectF(x, y, bw, bh), &sf, &brushText);
    }
}

// ============================================================================
// CThumbnailProvider
// ============================================================================
class CThumbnailProvider : public IInitializeWithStream,
                            public IInitializeWithFile,
                            public IThumbnailProvider, public IObjectWithSite {
public:
    CThumbnailProvider() : m_cRef(1) {
        InterlockedIncrement(&g_cRefModule);
    }
    virtual ~CThumbnailProvider() {
        if (m_spSite) m_spSite->Release();
        InterlockedDecrement(&g_cRefModule);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        wchar_t iidStr[64] = {};
        StringFromGUID2(riid, iidStr, 64);
        const wchar_t* hit = L"";
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInitializeWithFile))
            { *ppv = static_cast<IInitializeWithFile*>(this); hit = L" ->IInitFile"; }
        else if (riid == __uuidof(IInitializeWithStream))
            { *ppv = static_cast<IInitializeWithStream*>(this); hit = L" ->IInitStream"; }
        else if (riid == __uuidof(IThumbnailProvider))
            { *ppv = static_cast<IThumbnailProvider*>(this); hit = L" ->IThumb"; }
        else if (riid == __uuidof(IObjectWithSite))
            { *ppv = static_cast<IObjectWithSite*>(this); hit = L" ->IObjSite"; }
        else {
            DbgLog((std::wstring(L"Provider QI: ") + iidStr + L" ->NOINTERFACE").c_str());
            return E_NOINTERFACE;
        }
        AddRef();
        DbgLog((std::wstring(L"Provider QI: ") + iidStr + hit + L" OK").c_str());
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_cRef); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    // IInitializeWithFile
    HRESULT STDMETHODCALLTYPE Initialize(LPCWSTR pszFilePath, DWORD) override {
        DbgLog(pszFilePath ? (std::wstring(L"Initialize: ") + pszFilePath).c_str()
                           : L"Initialize: (null)");
        if (!pszFilePath) return E_POINTER;
        if (!m_path.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_path = pszFilePath;
        m_realPath = pszFilePath;
        return S_OK;
    }

    // IInitializeWithStream — Explorer 优先用此接口，只给字节流（无路径）。
    // 把流落盘为临时文件交 worker 解码（恢复提取），同时用 IStream::Stat 取真实路径用于角标。
    HRESULT STDMETHODCALLTYPE Initialize(IStream* pstream, DWORD) override {
        DbgLog(L"IInitStream::Initialize");
        if (!pstream) return E_POINTER;
        if (!m_path.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

        // 真实路径：Explorer 提供的文件流，Stat 名称通常为真实全路径（用于角标扩展名）。
        STATSTG stg = {};
        if (SUCCEEDED(pstream->Stat(&stg, STATFLAG_DEFAULT)) && stg.pwcsName) {
            m_realPath = stg.pwcsName;
            CoTaskMemFree(stg.pwcsName);
            DbgLog((std::wstring(L"  realPath(Stat)=") + m_realPath).c_str());
        }

        // 流式 spill：先读前缀用于 magic 判定，后续边读流边写临时文件（内存恒定 64KB 缓冲）。
        std::vector<uint8_t> head(64);
        ULONG got = 0;
        HRESULT shr = pstream->Read(head.data(), (ULONG)head.size(), &got);
        if (FAILED(shr) || got == 0) { DbgLog(L"  stream read head fail"); return E_FAIL; }
        head.resize(got);

        const unsigned char* p = head.data();
        size_t n = head.size();
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; }

        const wchar_t* ext = L"bin";
        if (n >= 4 && memcmp(p, "%PDF", 4) == 0)           ext = L"pdf";
        else if (n >= 4 && p[0] == 'P' && p[1] == 'K' &&
                 ((p[2] == 0x03 && p[3] == 0x04) ||
                  (p[2] == 0x05 && p[3] == 0x06)))         ext = L"cdr";
        else if (n >= 4 && memcmp(p, "RIFF", 4) == 0)      ext = L"cdr";
        else if (n >= 4 &&
                 (p[0] == 'A' || p[0] == 'a') && p[1] == 'C' &&
                 p[2] >= '0' && p[2] <= '9')                              ext = L"dwg";
        else if (n >= 4 && p[0] == ' ' && p[1] == ' ' &&
                 p[2] == '0' && (p[3] == '\r' || p[3] == '\n'))          ext = L"dxf";
        else if (n >= 4 &&
                 ((p[0] == 0x49 && p[1] == 0x49 && p[2] == 0x2A && p[3] == 0x00) ||
                  (p[0] == 0x4D && p[1] == 0x4D && p[2] == 0x00 && p[3] == 0x2A)))
            ext = L"tif";
        else if (n >= 5 && memcmp(p, "<?xml", 5) == 0)     ext = L"svg";
        else if (n >= 4 && p[0] == '<' && p[1] == 's' &&
                 p[2] == 'v' && p[3] == 'g')                              ext = L"svg";
        else if (n >= 11 && memcmp(p, "%!PS-Adobe", 11) == 0) ext = L"ai";
        else {
            bool asc = true;
            size_t probe = n < 1024 ? n : 1024;
            for (size_t i = 0; i < probe; ++i) {
                if (p[i] == 0) { asc = false; break; }
            }
            if (asc) ext = L"plt";
        }
        DbgLog((std::wstring(L"  ext=") + ext).c_str());

        wchar_t tempDir[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, tempDir)) return E_FAIL;
        wchar_t path[MAX_PATH];
        StringCchPrintfW(path, MAX_PATH, L"%sqvstream_%lu_%lu_%ld.%s",
                         tempDir, GetCurrentProcessId(), GetCurrentThreadId(),
                         InterlockedIncrement(&g_tempCounter), ext);
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return E_FAIL;
        DWORD written = 0;
        if (!WriteFile(h, head.data(), (DWORD)head.size(), &written, nullptr)
            || written != head.size()) {
            CloseHandle(h); DeleteFileW(path); return E_FAIL;
        }
        const ULONGLONG kMaxStream = 2ULL * 1024 * 1024 * 1024;
        BYTE chunk[65536];
        ULONGLONG total = head.size();
        for (;;) {
            ULONG r = 0;
            HRESULT sr = pstream->Read(chunk, sizeof(chunk), &r);
            if (r == 0) break;
            if (FAILED(sr)) break;
            DWORD w = 0;
            if (!WriteFile(h, chunk, r, &w, nullptr) || w != r) {
                CloseHandle(h); DeleteFileW(path); return E_FAIL;
            }
            total += r;
            if (total > kMaxStream) {
                DbgLog(L"  stream too large, abort");
                CloseHandle(h); DeleteFileW(path); return E_OUTOFMEMORY;
            }
        }
        CloseHandle(h);

        m_path = path;
        DbgLog((std::wstring(L"  temp=") + m_path).c_str());
        return S_OK;
    }

    // IThumbnailProvider
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override {
        DbgLog((L"GetThumbnail cx=" + std::to_wstring(cx)).c_str());
        if (!phbmp || !pdwAlpha) return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_RGB;
        if (m_path.empty()) { DbgLog(L"  no path"); return E_UNEXPECTED; }
        if (cx == 0) cx = 256;
        if (cx > 1024) cx = 1024; // sanity cap; Explorer asks for <= 256 typically

        std::wstring exePath;
        if (!GetHostExePath(exePath)) { DbgLog(L"  host exe not found"); return E_FAIL; }

        // 解码输入：m_path 即 IInitializeWithFile 传入的真实文件路径，worker 直接解码真文件。
        std::wstring inputPath = m_path;

        auto t0 = GetTickCount64();
        HBITMAP hbmp = nullptr;

        // Fast path: persistent server over named pipe (no per-call process
        // spawn). Falls back to the one-shot worker if the pipe path is
        // unavailable, so we never regress to "no thumbnail".
        std::vector<BYTE> bmp;
        PipeResult r = RequestThumbnailViaPipe(exePath, inputPath, cx, bmp);
        if (r == PipeResult::Ok)
            hbmp = BmpBytesToHBITMAP(bmp.data(), bmp.size());
        else
            DbgLog(L"  pipe not Ok (stale/error) -> fallback one-shot worker");

        // Any non-Ok result (server stale-drop or pipe error) falls back to the
        // one-shot worker. The slow channels now queue (kSlowCap=16) so stale
        // drops are rare; the fallback only covers overflow, so it cannot
        // re-create the old per-thumbnail process storm.
        if (!hbmp) {
            int cur = g_oneShotActive.load(std::memory_order_relaxed);
            if (cur < kOneShotMax &&
                g_oneShotActive.compare_exchange_weak(cur, cur + 1,
                                                      std::memory_order_acq_rel)) {
                DbgLog(L"  fallback one-shot worker");
                std::wstring tmpBmp = MakeTempBmpPath();
                if (!tmpBmp.empty()) {
                    if (RunThumbnailWorker(exePath, inputPath, tmpBmp, cx))
                        hbmp = LoadWorkerBmp(tmpBmp);
                    DeleteFileW(tmpBmp.c_str());
                }
                --g_oneShotActive;
            } else {
                DbgLog(L"  one-shot fallback skipped (concurrency cap reached)");
            }
        }

        auto t1 = GetTickCount64();
        if (!hbmp) { DbgLog(L"  worker failed/timeout"); return E_FAIL; }
        // 烤入右上角胶囊型分类型角标。扩展名优先取真实文件名(m_realPath，来自 Stat/Site/
        // IInitializeWithFile)，取不到再退回临时文件名的 magic 扩展名。
        CompositeTypeBadge(hbmp, GetExtUpper(m_realPath.empty() ? m_path : m_realPath));
        *phbmp = hbmp;
        *pdwAlpha = WTSAT_ARGB;
        DbgLog((L"  OK (" + std::to_wstring(t1 - t0) + L"ms)").c_str());
        return S_OK;
    }

    // IObjectWithSite：尝试从 site 取 IShellItem 真实路径（角标扩展名来源之一，多重保险）。
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override {
        if (m_spSite) { m_spSite->Release(); m_spSite = nullptr; }
        if (pUnkSite) {
            m_spSite = pUnkSite; m_spSite->AddRef();
            IShellItem* psi = nullptr;
            if (SUCCEEDED(pUnkSite->QueryInterface(__uuidof(IShellItem), (void**)&psi))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    m_realPath = p; CoTaskMemFree(p);
                    DbgLog((std::wstring(L"  realPath(Site)=") + m_realPath).c_str());
                }
                psi->Release();
            }
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID, void** ppvSite) override {
        if (ppvSite) *ppvSite = nullptr;
        return E_NOINTERFACE; // we don't retain the site
    }

private:
    LONG m_cRef;
    std::wstring m_path;      // worker 解码输入（stream 落盘临时文件，或 IInitializeWithFile 真实路径）
    std::wstring m_realPath;  // 真实文件路径（角标扩展名来源：Stat / Site / IInitializeWithFile）
    IUnknown* m_spSite = nullptr;
};

// ============================================================================
// ClassFactory
// ============================================================================
class CClassFactory : public IClassFactory {
public:
    CClassFactory() : m_cRef(1) {}
    virtual ~CClassFactory() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
            *ppv = this;
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_cRef); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnk, REFIID riid, void** ppv) override {
        wchar_t riidStr[64] = {};
        StringFromGUID2(riid, riidStr, 64);
        DbgLog((std::wstring(L"CF::CreateInstance riid=") + riidStr).c_str());
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnk) { DbgLog(L"  -> CLASS_E_NOAGGREGATION"); return CLASS_E_NOAGGREGATION; }
        auto* p = new CThumbnailProvider();
        HRESULT hr = p->QueryInterface(riid, ppv);
        DbgLog((std::wstring(L"  TP QI -> 0x") +
                std::to_wstring((unsigned)(hr & 0xFFFFFFFF))).c_str());
        p->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_cRefServer);
        else InterlockedDecrement(&g_cRefServer);
        return S_OK;
    }
private:
    LONG m_cRef;
};

// ============================================================================
// DllMain
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        DbgLog(L"=== DLL PROCESS_ATTACH (new build) ===");
    }
    return TRUE;
}

// ============================================================================
// DLL Exports
// ============================================================================
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    wchar_t clsidStr[64] = {}, riidStr[64] = {};
    StringFromGUID2(rclsid, clsidStr, 64);
    StringFromGUID2(riid, riidStr, 64);
    DbgLog((std::wstring(L"DllGetClassObject clsid=") + clsidStr +
            L" riid=" + riidStr).c_str());
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_QuickViewThumbnailProvider) {
        DbgLog(L"  -> CLASS_E_CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* p = new CClassFactory();
    HRESULT hr = p->QueryInterface(riid, ppv);
    wchar_t hrbuf[32];
    wsprintfW(hrbuf, L"  CF QI -> 0x%08X", (unsigned)(hr & 0xFFFFFFFF));
    DbgLog(hrbuf);
    p->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return (g_cRefModule == 0 && g_cRefServer == 0) ? S_OK : S_FALSE;
}

extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);
    const wchar_t* clsid = L"{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}";
    const wchar_t* thumbIID = L"{E357FCCD-A995-4576-B01F-234630154E96}";
    HKEY hKey;
    // CLSID
    std::wstring clsidKey = L"Software\\Classes\\CLSID\\" + std::wstring(clsid);
    RegCreateKeyExW(HKEY_CURRENT_USER, clsidKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (hKey) { RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)L"QuickView Thumb", sizeof(L"QuickView Thumb")); RegCloseKey(hKey); }
    // InprocServer32
    std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    RegCreateKeyExW(HKEY_CURRENT_USER, inprocKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (hKey) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)dllPath, (DWORD)((wcslen(dllPath)+1)*sizeof(wchar_t)));
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Apartment", sizeof(L"Apartment"));
        RegCloseKey(hKey);
    }
    // ShellEx thumbnail handler for every vector/document format QuickView renders
    for (auto ext : kThumbnailExts) {
        std::wstring key = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + thumbIID;
        RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
        if (hKey) { RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)clsid, (DWORD)((wcslen(clsid)+1)*sizeof(wchar_t))); RegCloseKey(hKey); }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}");
    const wchar_t* thumbIID = L"{E357FCCD-A995-4576-B01F-234630154E96}";
    for (auto ext : kThumbnailExts) {
        std::wstring key = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + thumbIID;
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
