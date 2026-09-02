// ============================================================================
// QuickViewThumbnailProvider.cpp
// COM DLL: Windows Shell IThumbnailProvider for QuickView vector/document
// formats (.cdr .cmx .plt .dxf .dwg .pdf .ai).
//
// Architecture: thin out-of-process shim. GetThumbnail() communicates with
//   QuickView.exe via named pipe (persistent server) or stdout pipe (one-shot
//   fallback), receiving 32bpp BGRA BMP data in memory — no temp files.
//   This reuses the FULL application rendering pipeline (libcdr / VectorLoader
//   / MuPDF via CImageLoader::LoadThumbnail), and keeps the shell extension
//   tiny while guaranteeing Explorer thumbnails match the app's rendering.
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
#include <map>
#include <cmath>
#include <gdiplus.h>
#include <objidl.h>          // STATSTG, STATFLAG_NONAME
#include <atomic>            // bounded one-shot worker fallback

// Thumbnail extension coverage (single source of truth, shared with SettingsOverlay).
#include "ThumbnailExts.h"
// Persistent on-disk thumbnail cache (key = path + mtime + size + cx).
#include "ThumbDiskCache.h"

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

// ============================================================================
// CLSID — 文件类型覆盖图标（IconHandler, IExtractIconW）
// {DAA561A2-0EEA-478F-9C2E-DBC41B59056B}
// ============================================================================
DEFINE_GUID(CLSID_QuickViewIconProvider,
    0xDAA561A2, 0x0EEA, 0x478F, 0x9C, 0x2E, 0xDB, 0xC4, 0x1B, 0x59, 0x05, 0x6B);

// IconHandler 的 ShellEx 类别 GUID（IExtractIconW）
static const wchar_t kIconHandlerIID[] = L"{00021401-0000-0000-C000-000000000046}";

// All extensions handled by this provider are defined once in ThumbnailExts.h
// (derived from SupportedExtensions) and shared with SettingsOverlay, so the DLL
// self-register and the app's RegisterAssociations can never drift apart.

// Hard cap for a single thumbnail render. Explorer calls us on a thread pool;
// a hung worker must not block extraction forever.
static constexpr DWORD kPipeTimeoutMs = 10000; // hard cap: a slow request is abandoned after 10s
                                           // (closes its pipe + worker thread) so it can never
                                           // wedge Explorer; the soft 4s watchdog still returns a
                                           // placeholder earlier to stay responsive.

// [Watchdog] Soft timeout for a single thumbnail render. If the persistent
// server hasn't returned within this window, GetThumbnail returns a placeholder
// immediately so Explorer never blocks on a slow document (e.g. complex .ai/.cdr).
// The background request keeps running; once the server caches the result (its
// own in-memory L1/L2), subsequent requests return instantly. Tune freely.
static constexpr DWORD kWatchdogMs = 4000;

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

// [Watchdog] Shared state for a background thumbnail request. The main thread
// waits up to kWatchdogMs for a result; on timeout it returns a placeholder and
// marks `detached`, so the worker keeps rendering and self-cleans on completion
// (the persistent server caches the result for next time). The CS serializes the
// `detached` flag so there is no data race between caller and worker.
struct WatchdogState {
    std::wstring exePath;
    std::wstring inputPath;
    UINT cx = 0;
    HBITMAP hbmp = nullptr;
    bool done = false;
    bool detached = false;
    HANDLE hEvent = nullptr;
    CRITICAL_SECTION cs;
    WatchdogState() { InitializeCriticalSection(&cs); }
    ~WatchdogState() { DeleteCriticalSection(&cs); }
};

// [Watchdog] Forward declarations so WatchdogWorker (above) can call the pipe
// helpers defined later in this file. The enum is hoisted here from its
// original location so PipeResult::Ok is visible at the call site.
enum class PipeResult { Ok, Stale, Error };
static HBITMAP BmpBytesToHBITMAP(const BYTE* data, size_t len);
static PipeResult RequestThumbnailViaPipe(const std::wstring& exePath,
                                          const std::wstring& inputPath, UINT size,
                                          std::vector<BYTE>& outBmp);

static DWORD WINAPI WatchdogWorker(LPVOID lp) {
    auto* st = static_cast<WatchdogState*>(lp);
    std::vector<BYTE> bmp;
    PipeResult r = RequestThumbnailViaPipe(st->exePath, st->inputPath, st->cx, bmp);
    HBITMAP h = (r == PipeResult::Ok) ? BmpBytesToHBITMAP(bmp.data(), bmp.size()) : nullptr;
    // [Disk cache] Persist the raw server bitmap so a later view (after a
    // server restart / cleared Explorer cache) is instant. This also fixes the
    // old behavior where a timed-out render discarded its result, forcing a
    // re-scroll to see the real thumbnail.
    if (r == PipeResult::Ok)
        QuickView::ThumbDiskCache::Instance().Put(st->inputPath, st->cx, bmp.data(), bmp.size());
    bool detached = false;
    {
        EnterCriticalSection(&st->cs);
        st->hbmp = h;
        st->done = true;
        detached = st->detached;
        LeaveCriticalSection(&st->cs);
    }
    SetEvent(st->hEvent);
    // Caller timed out and detached: it will never read hbmp, so free it and the
    // state ourselves to avoid leaks. Otherwise the caller owns st and hbmp.
    if (detached) {
        if (st->hbmp) DeleteObject(st->hbmp);
        CloseHandle(st->hEvent);
        delete st;
    }
    return 0;
}

// Build a solid-color square DIB used as the placeholder thumbnail source.
static HBITMAP MakeSolidDib(int w, int h, COLORREF c) {
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
    if (hbmp && bits) {
        BYTE* p = static_cast<BYTE*>(bits);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                *p++ = GetBValue(c); *p++ = GetGValue(c);
                *p++ = GetRValue(c); *p++ = 0xFF;
            }
        }
    }
    return hbmp;
}

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

// Run "QuickView.exe --thumbnail" synchronously, reading the BMP result via
// the child's stdout pipe (no temp file). Returns the HBITMAP on success.
// Protocol: 4-byte status (0=OK, 1=FAIL) + 4-byte bmpLen + bmp data.
static HBITMAP RunOneShotWorker(const std::wstring& exePath,
                                 const std::wstring& inputPath, UINT size) {
    // Create an anonymous pipe for the child's stdout.
    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; // child inherits the write end
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return nullptr;
    // Ensure the read end is NOT inherited by the child.
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = L"\"" + exePath + L"\" --thumbnail --input \"" + inputPath +
                       L"\" --size " + std::to_wstring(size);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    BOOL launched = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    // Close the write end in the parent so the child's writes reach EOF.
    CloseHandle(hWritePipe);
    if (!launched) {
        CloseHandle(hReadPipe);
        return nullptr;
    }

    // Read all stdout output into a buffer (status + bmpLen + bmp data).
    std::vector<BYTE> buf;
    BYTE chunk[65536];
    for (;;) {
        DWORD got = 0;
        BOOL ok = ReadFile(hReadPipe, chunk, sizeof(chunk), &got, nullptr);
        if (!ok || got == 0) break;
        buf.insert(buf.end(), chunk, chunk + got);
        if (buf.size() > 64 * 1024 * 1024) break; // sanity cap (64 MB)
    }
    CloseHandle(hReadPipe);

    // Wait for the child to exit (bounded by kPipeTimeoutMs).
    DWORD wait = WaitForSingleObject(pi.hProcess, kPipeTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // Parse the pipe output: 4-byte status + 4-byte bmpLen + bmp data.
    if (buf.size() < 8) return nullptr;
    uint32_t status = *reinterpret_cast<const uint32_t*>(buf.data());
    if (status != 0) return nullptr; // FAIL
    uint32_t bmpLen = *reinterpret_cast<const uint32_t*>(buf.data() + 4);
    if (bmpLen == 0 || buf.size() < 8 + bmpLen) return nullptr;
    return BmpBytesToHBITMAP(buf.data() + 8, bmpLen);
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
// (PipeResult is defined earlier, near WatchdogState.)

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
        for (int k = 0; k < 100 && hPipe == INVALID_HANDLE_VALUE; ++k) {
            hPipe = OpenServerPipe();
            if (hPipe != INVALID_HANDLE_VALUE) break;
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND && !launched) {
                LaunchServer(exePath);
                launched = true;
                Sleep(50);
            } else if (err == ERROR_PIPE_BUSY) {
                // Wait until an instance becomes available
                WaitNamedPipeW(kPipeName, 2000);
            } else {
                Sleep(20);
            }
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
    // 方块画框缓存：key=扩展名@cx，value=cx×cx 透明 Bitmap，右上角已烤好按比例角标。
    // 多尺寸各存一份、角标严格按画布高度比例(fontSize=cx*0.05)渲染，任意视图尺寸下视觉一致；预渲染复用避免重复 MeasureString/FillPath/DrawString。
    static std::mutex s_frameMtx;
    static std::map<std::wstring, std::unique_ptr<Gdiplus::Bitmap>> s_frameCache;

    static std::once_flag s_gdiInit;
    static ULONG_PTR s_gdiToken = 0;
    static void EnsureGdiplus() {
        std::call_once(s_gdiInit, []() {
            Gdiplus::GdiplusStartupInput gsi;
            Gdiplus::GdiplusStartup(&s_gdiToken, &gsi, nullptr);
        });
    }

    // 扩展名 -> 基础色（GDI+ Color，胶囊实色用）
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

    // 预渲染单枚圆形徽章位图：背景透明，实心圆（类型色）+ 白字完整扩展名。
    // 始终完整绘制扩展名；若圆内放不下，则按比例自动缩小字号（不截断、不 fallback 首字母）。
    Gdiplus::Bitmap* CreateBadgeBmp(const std::wstring& extUpper, float d) {
        const int n = (int)extUpper.length();
        const float textScale = (n <= 2) ? 0.92f : 0.60f; // 两字母饱满，三字母留更多白
        float idealSize = d * textScale;

        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        // 量完整扩展名在理想字号下的字宽，决定是否需要缩小。
        Gdiplus::Bitmap dummy(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics gdummy(&dummy);
        gdummy.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        Gdiplus::Font idealFont(L"Segoe UI", idealSize, Gdiplus::FontStyleBold);
        Gdiplus::RectF layout(0, 0, 10000, 10000);
        Gdiplus::RectF bound;
        gdummy.MeasureString(extUpper.c_str(), (INT)extUpper.length(), &idealFont, layout, &sf, &bound);

        const float avail = d * 0.78f; // 圆内两侧边距共 22%
        float finalSize = idealSize;
        if (bound.Width > avail)
            finalSize = idealSize * (avail / bound.Width); // 放不下则等比缩小，仍完整显示

        Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap((INT)std::ceil(d), (INT)std::ceil(d), PixelFormat32bppARGB);
        Gdiplus::Graphics g(bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));   // 背景透明

        Gdiplus::Color base = BadgeColorForGdi(extUpper);
        Gdiplus::SolidBrush brushCircle(Gdiplus::Color(255, base.GetR(), base.GetG(), base.GetB()));
        g.FillEllipse(&brushCircle, 0.0f, 0.0f, d, d);

        Gdiplus::SolidBrush brushText(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::Font font(L"Segoe UI", finalSize, Gdiplus::FontStyleBold);
        g.DrawString(extUpper.c_str(), (INT)extUpper.length(), &font, Gdiplus::RectF(0, 0, d, d), &sf, &brushText);
        return bmp;
    }

    // 创建 cx×cx 透明 DIB-section HBITMAP（top-down 32bpp ARGB，像素未清零）。
    HBITMAP CreateSquareDib(int cx) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cx;
        bi.bmiHeader.biHeight = -cx; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC hdc = GetDC(nullptr);
        HBITMAP h = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        return h;
    }

    // 预渲染一枚方块画框：透明底 + 右上角完整圆形类型徽章。缓存复用。
    Gdiplus::Bitmap* CreateSquareFrameBmp(const std::wstring& extUpper, int cx) {
        Gdiplus::Bitmap* frame = new Gdiplus::Bitmap(cx, cx, PixelFormat32bppARGB);
        Gdiplus::Graphics g(frame);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0)); // 透明底

        // 圆形直径占画布 38%（加大以保证小图下也能辨认格式），圆心内移 pad=cx*4%
        // 保证圆完整不裁切；扩展名始终完整显示（放不下自动缩字号）。
        const float kRadius = (float)cx * 0.19f;
        const float kPad = (float)cx * 0.04f;
        const float d = kRadius * 2.0f; // 圆直径
        Gdiplus::Bitmap* badge = CreateBadgeBmp(extUpper, d);
        if (badge) {
            // 圆心 = (cx - R - pad, R + pad)；左上角 = (cx - d - pad, pad)
            const float x = (float)cx - d - kPad;
            const float y = kPad;
            g.DrawImage(badge, x, y, d, d);
            delete badge;
        }
        return frame;
    }

    // 取缓存方块画框（key=扩展名@cx，按比例渲染），未命中才生成并缓存（double-check 防重复）。
    Gdiplus::Bitmap* GetSquareFrame(const std::wstring& extUpper, int cx) {
        const std::wstring key = extUpper + L"@" + std::to_wstring(cx);
        Gdiplus::Bitmap* frame = nullptr;
        {
            std::lock_guard<std::mutex> lk(s_frameMtx);
            auto it = s_frameCache.find(key);
            if (it != s_frameCache.end()) frame = it->second.get();
        }
        if (!frame) {
            Gdiplus::Bitmap* created = CreateSquareFrameBmp(extUpper, cx);
            if (created) {
                std::lock_guard<std::mutex> lk(s_frameMtx);
                auto it = s_frameCache.find(key);
                if (it != s_frameCache.end()) { delete created; frame = it->second.get(); }
                else { s_frameCache[key].reset(created); frame = created; }
            }
        }
        return frame;
    }

    // 把解码出的原图（可能非正方形）等比重心缩放进 cx×cx 透明方块，再叠加缓存画框
    // （角标盖顶，始终可见），返回统一方块 HBITMAP。缓存命中时仅做一次缩放绘制。
    HBITMAP ComposeSquareThumbnail(HBITMAP hSrc, const std::wstring& extUpper, UINT cx) {
        if (!hSrc) return nullptr;
        EnsureGdiplus();
        BITMAP bms;
        if (GetObject(hSrc, sizeof(bms), &bms) != sizeof(bms) ||
            bms.bmBitsPixel != 32 || !bms.bmBits ||
            bms.bmWidth <= 0 || bms.bmHeight <= 0)
            return hSrc; // 防御：原图异常则退回原图，避免崩溃

        const int W = (int)cx;
        HBITMAP hCanvas = CreateSquareDib(W);
        if (!hCanvas) return hSrc;

        {
            BITMAP bmC;
            GetObject(hCanvas, sizeof(bmC), &bmC);
            Gdiplus::Bitmap gCanvas(bmC.bmWidth, bmC.bmHeight, bmC.bmWidthBytes,
                                    PixelFormat32bppARGB, (BYTE*)bmC.bmBits);
            Gdiplus::Graphics g(&gCanvas);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));

            const int sw = bms.bmWidth, sh = bms.bmHeight;
            const float scale = (std::min)((float)W / (float)sw, (float)W / (float)sh);
            const float dw = sw * scale, dh = sh * scale;
            const float dx = (W - dw) * 0.5f, dy = (W - dh) * 0.5f;

            Gdiplus::Bitmap gSrc(sw, sh, bms.bmWidthBytes, PixelFormat32bppARGB, (BYTE*)bms.bmBits);
            g.DrawImage(&gSrc, dx, dy, dw, dh);

            // 画框按请求尺寸 cx 各存一份、按比例渲染，1:1 叠加无缩放模糊。
            if (Gdiplus::Bitmap* frame = GetSquareFrame(extUpper, W))
                g.DrawImage(frame, 0.0f, 0.0f, (float)W, (float)W);
        }
        DeleteObject(hSrc);
        return hCanvas;
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
        // Clean up the IStream spill temp file (if any) to avoid accumulation.
        if (!m_streamTempFile.empty()) {
            DeleteFileW(m_streamTempFile.c_str());
        }
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
        // Deliberately refuse IInitializeWithStream so Explorer falls back to IInitializeWithFile.
        // This avoids copying entire 300MB+ network files from SMB into local %TEMP% on every thumbnail request!
        else if (riid == __uuidof(IInitializeWithStream))
            { return E_NOINTERFACE; }
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
        else if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) ext = L"jpg";
        else if (n >= 4 && p[0] == 0x89 && p[1] == 0x50 && p[2] == 0x4E && p[3] == 0x47) ext = L"png";
        else if (n >= 12 && p[0] == 'R' && p[1] == 'I' && p[2] == 'F' && p[3] == 'F' &&
                 p[8] == 'W' && p[9] == 'E' && p[10] == 'B' && p[11] == 'P') ext = L"webp";
        else if (n >= 2 && p[0] == 'B' && p[1] == 'M')     ext = L"bmp";
        else if (n >= 3 && p[0] == 'G' && p[1] == 'I' && p[2] == 'F') ext = L"gif";
        else if (n >= 4 && p[0] == '8' && p[1] == 'B' && p[2] == 'P' && p[3] == 'S') ext = L"psd";
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
                 ((p[0] == 0x49 && p[1] == 0x49 && (p[2] == 0x2A || p[2] == 0x2B)) ||
                  (p[0] == 0x4D && p[1] == 0x4D && (p[2] == 0x00 || p[3] == 0x2A || p[3] == 0x2B))))
            ext = L"tif";
        else if (n >= 12 && p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p') {
            if (memcmp(p + 8, "avif", 4) == 0 || memcmp(p + 8, "avis", 4) == 0) ext = L"avif";
            else ext = L"heic";
        }
        else if ((n >= 2 && p[0] == 0xFF && p[1] == 0x0A) ||
                 (n >= 12 && p[4] == 'J' && p[5] == 'X' && p[6] == 'L' && p[7] == ' ')) ext = L"jxl";
        else if (n >= 4 && p[0] == 0x76 && p[1] == 0x2F && p[2] == 0x31 && p[3] == 0x01) ext = L"exr";
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
        m_streamTempFile = path; // track for cleanup on destruction
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

        // [Disk cache] Hit first: a previously rendered thumbnail is returned
        // instantly without touching the pipe server at all. Key includes mtime
        // + size, so an edited file automatically misses and re-renders.
        {
            std::vector<BYTE> cached;
            if (QuickView::ThumbDiskCache::Instance().Get(inputPath, cx, cached)) {
                HBITMAP raw = BmpBytesToHBITMAP(cached.data(), cached.size());
                if (raw) {
                    std::wstring ext = GetExtUpper(m_realPath.empty() ? m_path : m_realPath);
                    HBITMAP final = ComposeSquareThumbnail(raw, ext, cx);
                    DeleteObject(raw);
                    if (final) {
                        *phbmp = final;
                        *pdwAlpha = WTSAT_RGB;
                        DbgLog((L"  disk cache hit cx=" + std::to_wstring(cx)).c_str());
                        return S_OK;
                    }
                }
            }
        }

        auto t0 = GetTickCount64();
        HBITMAP hbmp = nullptr;

        // Fast path: persistent server over named pipe (no per-call process
        // spawn). Falls back to the one-shot worker if the pipe path is
        // unavailable, so we never regress to "no thumbnail".
        // [Watchdog] Run the pipe request on a background thread and wait up to
        // kWatchdogMs. A slow document never blocks Explorer beyond that window;
        // we return a placeholder and the worker keeps running so the persistent
        // server caches the result for subsequent requests.
        auto* watch = new WatchdogState();
        watch->exePath = exePath;
        watch->inputPath = inputPath;
        watch->cx = cx;
        watch->hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE hThread = watch->hEvent ? CreateThread(nullptr, 0, WatchdogWorker, watch, 0, nullptr) : nullptr;

        if (!hThread) {
            // Thread/event creation failed: degrade to the original synchronous path.
            if (watch->hEvent) CloseHandle(watch->hEvent);
            delete watch;
            watch = nullptr;
            std::vector<BYTE> bmp;
            PipeResult r = RequestThumbnailViaPipe(exePath, inputPath, cx, bmp);
            if (r == PipeResult::Ok) {
                hbmp = BmpBytesToHBITMAP(bmp.data(), bmp.size());
                QuickView::ThumbDiskCache::Instance().Put(inputPath, cx, bmp.data(), bmp.size());
            } else
                DbgLog(L"  pipe not Ok (stale/error) -> fallback one-shot worker");
        } else {
            DWORD wait = WaitForSingleObject(watch->hEvent, kWatchdogMs);
            if (wait == WAIT_OBJECT_0) {
                hbmp = watch->hbmp; // worker finished; we own st and hbmp now
                CloseHandle(hThread);
                CloseHandle(watch->hEvent);
                delete watch;
                watch = nullptr;
            } else {
                // Soft timeout: detach the worker (it keeps rendering; the server
                // caches the result) and return a placeholder immediately.
                EnterCriticalSection(&watch->cs);
                watch->detached = true;
                LeaveCriticalSection(&watch->cs);
                CloseHandle(hThread);
                DbgLog(L"  watchdog timeout -> placeholder");
                hbmp = ComposeSquareThumbnail(
                    MakeSolidDib(static_cast<int>(cx), static_cast<int>(cx), RGB(232, 232, 232)),
                    GetExtUpper(m_realPath.empty() ? m_path : m_realPath), cx);
            }
        }

        // Any non-Ok result (server stale-drop, pipe error, or sync fallback) takes
        // the one-shot worker. The slow channels queue (kSlowCap=16) so stale drops
        // are rare; the fallback only covers overflow and cannot re-create the old
        // per-thumbnail process storm.
        if (!hbmp) {
            int cur = g_oneShotActive.load(std::memory_order_relaxed);
            if (cur < kOneShotMax &&
                g_oneShotActive.compare_exchange_weak(cur, cur + 1,
                                                      std::memory_order_acq_rel)) {
                DbgLog(L"  fallback one-shot worker (stdout pipe)");
                hbmp = RunOneShotWorker(exePath, inputPath, cx);
                --g_oneShotActive;
            } else {
                DbgLog(L"  one-shot fallback skipped (concurrency cap reached)");
            }
        }

        auto t1 = GetTickCount64();
        if (!hbmp) { DbgLog(L"  worker failed/timeout"); return E_FAIL; }
        // 把原图等比缩放进统一方块画布（透明底 + 右上角类型胶囊角标，画框已缓存）。
        // 扩展名优先取真实文件名(m_realPath)，取不到再退回临时文件名的 magic 扩展名。
        hbmp = ComposeSquareThumbnail(hbmp, GetExtUpper(m_realPath.empty() ? m_path : m_realPath), cx);
        *phbmp = hbmp;
        *pdwAlpha = WTSAT_ARGB;
        // Clean up the IStream spill temp file immediately after use.
        if (!m_streamTempFile.empty()) {
            DeleteFileW(m_streamTempFile.c_str());
            m_streamTempFile.clear();
        }
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
    std::wstring m_streamTempFile; // IInitializeWithStream 创建的临时文件路径（析构时删除）
    IUnknown* m_spSite = nullptr;
};

// ============================================================================
// CIconProvider — 文件类型覆盖图标（IconHandler: IPersistFile + IExtractIconW）
// Explorer 显示"图标"（列表/详细信息/小图标/无缩略图场景）时经 ShellEx
// {00021401-0000-0000-C000-000000000046} 调用本类，按文件扩展名实时合成
// 类别色圆形字母章（与静态文件图标/缩略图右上角胶囊同一套类别色语义）。
// 不影响缩略图（IThumbnailProvider 优先）。
// ============================================================================
class CIconProvider : public IPersistFile, public IExtractIconW {
public:
    CIconProvider() : m_cRef(1) { InterlockedIncrement(&g_cRefModule); }
    virtual ~CIconProvider() { InterlockedDecrement(&g_cRefModule); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IPersistFile))
            *ppv = static_cast<IPersistFile*>(this);
        else if (riid == __uuidof(IExtractIconW))
            *ppv = static_cast<IExtractIconW*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_cRef); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = InterlockedDecrement(&m_cRef);
        if (c == 0) delete this;
        return c;
    }

    // IPersist（IPersistFile 的基接口）
    HRESULT STDMETHODCALLTYPE GetClassID(CLSID* pClassID) override {
        if (!pClassID) return E_POINTER;
        *pClassID = CLSID_QuickViewIconProvider;
        return S_OK;
    }

    // IPersistFile
    HRESULT STDMETHODCALLTYPE IsDirty() override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE Load(LPCWSTR pszFileName, DWORD) override {
        m_path = pszFileName ? pszFileName : L"";
        DbgLog((L"IconProvider::Load " + m_path).c_str());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Save(LPCWSTR, BOOL) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SaveCompleted(LPCWSTR) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurFile(LPWSTR*) override { return E_NOTIMPL; }

    // IExtractIconW：返回 GIL_NOTFILENAME，让 Shell 调 Extract() 动态合成
    HRESULT STDMETHODCALLTYPE GetIconLocation(UINT, LPWSTR szIconFile, UINT cch,
                                              int* piIndex, UINT* pwFlags) override {
        if (!piIndex || !pwFlags || !szIconFile || cch == 0) return E_POINTER;
        lstrcpynW(szIconFile, L"QV", cch);
        *piIndex = 0;
        *pwFlags = GIL_NOTFILENAME;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Extract(LPCWSTR, UINT, HICON* phiconLarge, HICON* phiconSmall,
                                      UINT nIconSize) override {
        if (!phiconLarge && !phiconSmall) return E_POINTER;
        if (phiconLarge) *phiconLarge = nullptr;
        if (phiconSmall) *phiconSmall = nullptr;
        EnsureGdiplus();
        const std::wstring ext = ExtUpperOf(m_path);
        if (phiconLarge) {
            const UINT sz = LOWORD(nIconSize);
            *phiconLarge = CreateCapsuleIcon(ext, sz ? sz : 32);
        }
        if (phiconSmall) {
            const UINT sz = HIWORD(nIconSize);
            *phiconSmall = CreateCapsuleIcon(ext, sz ? sz : 16);
        }
        if (phiconLarge && !*phiconLarge) *phiconLarge = nullptr;
        return (*phiconLarge || *phiconSmall) ? S_OK : E_FAIL;
    }

private:
    static std::wstring ExtUpperOf(const std::wstring& p) {
        size_t dot = p.find_last_of(L'.');
        if (dot == std::wstring::npos || dot + 1 >= p.size()) return L"";
        std::wstring e = p.substr(dot + 1);
        for (auto& c : e) c = (wchar_t)std::towupper((wint_t)c);
        return e;
    }

    // 合成类别色圆形字母章 HICON（复用缩略图胶囊的绘制：BadgeColorForGdi + CreateBadgeBmp）
    static HICON CreateCapsuleIcon(const std::wstring& extUpper, UINT size) {
        if (size == 0 || size > 1024) size = 32;
        Gdiplus::Bitmap* badge = CreateBadgeBmp(extUpper, (float)size);
        if (!badge) return nullptr;
        HBITMAP hColor = nullptr;
        Gdiplus::Status st = badge->GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hColor);
        delete badge;
        if (st != Gdiplus::Ok || !hColor) return nullptr;

        // 全 0 单色掩码 = 整体不透明，颜色位图的 alpha 通道生效
        HBITMAP hMask = CreateBitmap(size, size, 1, 1, nullptr);
        if (hMask) {
            HDC hdcMem = CreateCompatibleDC(nullptr);
            if (hdcMem) {
                HGDIOBJ old = SelectObject(hdcMem, hMask);
                PatBlt(hdcMem, 0, 0, size, size, BLACKNESS);
                SelectObject(hdcMem, old);
                DeleteDC(hdcMem);
            }
        }
        ICONINFO ii = {};
        ii.fIcon = TRUE;
        ii.hbmMask = hMask;
        ii.hbmColor = hColor;
        HICON hIcon = CreateIconIndirect(&ii);
        if (hMask) DeleteObject(hMask);
        DeleteObject(hColor);
        return hIcon;
    }

    LONG m_cRef;
    std::wstring m_path;
};

// ============================================================================
// ClassFactory
// ============================================================================
class CClassFactory : public IClassFactory {
public:
    explicit CClassFactory(bool iconProvider) : m_cRef(1), m_iconProvider(iconProvider) {}
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
        // 多继承下直接 static_cast<IUnknown*> 有二义性，经具体接口指针调 QueryInterface
        HRESULT hr = E_FAIL;
        if (m_iconProvider) {
            IPersistFile* p = new CIconProvider();
            hr = p->QueryInterface(riid, ppv);
            DbgLog((std::wstring(L"  Icon QI -> 0x") +
                    std::to_wstring((unsigned)(hr & 0xFFFFFFFF))).c_str());
            p->Release();
        } else {
            IInitializeWithFile* p = new CThumbnailProvider();
            hr = p->QueryInterface(riid, ppv);
            DbgLog((std::wstring(L"  TP QI -> 0x") +
                    std::to_wstring((unsigned)(hr & 0xFFFFFFFF))).c_str());
            p->Release();
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_cRefServer);
        else InterlockedDecrement(&g_cRefServer);
        return S_OK;
    }
private:
    LONG m_cRef;
    bool m_iconProvider;
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
    if (rclsid == CLSID_QuickViewIconProvider) {
        auto* p = new CClassFactory(/*iconProvider=*/true);
        HRESULT hr = p->QueryInterface(riid, ppv);
        p->Release();
        return hr;
    }
    if (rclsid != CLSID_QuickViewThumbnailProvider) {
        DbgLog(L"  -> CLASS_E_CLASSNOTAVAILABLE");
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* p = new CClassFactory(false);
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
    // ShellEx thumbnail handler for every image format QuickView renders (badge overlay)
    for (auto ext : QuickView::kThumbnailExts) {
        std::wstring key = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + thumbIID;
        RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
        if (hKey) { RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)clsid, (DWORD)((wcslen(clsid)+1)*sizeof(wchar_t))); RegCloseKey(hKey); }
    }

    // 文件类型覆盖图标（IconHandler）：每个扩展名 .ext 级 ShellEx 最高优先级，
    // Explorer 显示图标时按扩展名动态合成类别色圆形字母章。
    const wchar_t* iconClsid = L"{DAA561A2-0EEA-478F-9C2E-DBC41B59056B}";
    std::wstring iconClsidKey = L"Software\\Classes\\CLSID\\" + std::wstring(iconClsid);
    RegCreateKeyExW(HKEY_CURRENT_USER, iconClsidKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (hKey) { RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)L"QuickView Icon", sizeof(L"QuickView Icon")); RegCloseKey(hKey); }
    std::wstring iconInprocKey = iconClsidKey + L"\\InprocServer32";
    RegCreateKeyExW(HKEY_CURRENT_USER, iconInprocKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
    if (hKey) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)dllPath, (DWORD)((wcslen(dllPath)+1)*sizeof(wchar_t)));
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Apartment", sizeof(L"Apartment"));
        RegCloseKey(hKey);
    }
    for (auto ext : QuickView::kThumbnailExts) {
        std::wstring key = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + kIconHandlerIID;
        RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
        if (hKey) { RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)iconClsid, (DWORD)((wcslen(iconClsid)+1)*sizeof(wchar_t))); RegCloseKey(hKey); }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{DAA561A2-0EEA-478F-9C2E-DBC41B59056B}");
    const wchar_t* thumbIID = L"{E357FCCD-A995-4576-B01F-234630154E96}";
    for (auto ext : QuickView::kThumbnailExts) {
        std::wstring key = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + thumbIID;
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
        std::wstring iconKey = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + kIconHandlerIID;
        RegDeleteTreeW(HKEY_CURRENT_USER, iconKey.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
