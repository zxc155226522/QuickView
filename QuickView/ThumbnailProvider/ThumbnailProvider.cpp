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
#include <shlwapi.h>
#include <strsafe.h>

#include <string>
#include <vector>
#include <cctype>

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

// IInitializeWithStream (propsys.h): Windows 10/11 Explorer calls this
// INSTEAD of IInitializeWithFile. The provider receives an IStream backed
// by the file and must read the content itself; the shell never tells us the
// path. We spill the stream to a temp file (extension inferred from the
// magic bytes) so the existing worker can reuse the normal render pipeline.
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

// IObjectWithSite (oleidl.h) — Explorer passes the IShellItem site here so
// the provider can resolve the file path (IThumbnailProvider has no path
// param; Win10/11 Explorer bypasses IInitializeWithFile in favor of SetSite).
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
    L".cdr", L".cmx", L".plt", L".dxf", L".dwg", L".pdf", L".ai", L".svg", L".svgz"
};

// Hard cap for a single thumbnail render. Explorer calls us on a thread pool;
// a hung worker must not block extraction forever.
static constexpr DWORD kWorkerTimeoutMs = 15000;

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

    DWORD wait = WaitForSingleObject(pi.hProcess, kWorkerTimeoutMs);
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

// ============================================================================
// CThumbnailProvider
// ============================================================================
class CThumbnailProvider : public IInitializeWithFile, public IInitializeWithStream,
                            public IThumbnailProvider, public IObjectWithSite {
public:
    CThumbnailProvider() : m_cRef(1) {
        InterlockedIncrement(&g_cRefModule);
    }
    virtual ~CThumbnailProvider() {
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
        else if (riid == __uuidof(IThumbnailProvider))
            { *ppv = static_cast<IThumbnailProvider*>(this); hit = L" ->IThumb"; }
        else if (riid == __uuidof(IInitializeWithStream))
            { *ppv = static_cast<IInitializeWithStream*>(this); hit = L" ->IInitStream"; }
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
        return S_OK;
    }

    // IInitializeWithStream — Win10/11 Explorer hands us the file as a
    // stream (no path). Spill to a temp file so the worker can reuse the
    // normal render pipeline. The extension is inferred from magic bytes.
    HRESULT STDMETHODCALLTYPE Initialize(IStream* pstream, DWORD) override {
        DbgLog(L"IInitStream::Initialize");
        if (!pstream) return E_POINTER;
        if (!m_path.empty()) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

        std::vector<uint8_t> buf;
        buf.reserve(64 * 1024);
        BYTE chunk[4096];
        ULONG got = 0;
        for (;;) {
            HRESULT hr = pstream->Read(chunk, sizeof(chunk), &got);
            if (FAILED(hr) || got == 0) break;
            buf.insert(buf.end(), chunk, chunk + got);
            if (buf.size() > 200u * 1024u * 1024u) {
                DbgLog(L"  stream > 200MB, abort"); return E_OUTOFMEMORY;
            }
            if (got < sizeof(chunk)) break;
        }
        if (buf.empty()) { DbgLog(L"  stream empty/read fail"); return E_FAIL; }

        // Magic-byte detection. Order matters: PDF/CDR/DWG headers are
        // unambiguous; DXF is a known ASCII prologue; PLT/HPGL is anything
        // else that looks like plain ASCII text.
        const wchar_t* ext = L"bin";
        if (buf.size() >= 4 && memcmp(buf.data(), "%PDF", 4) == 0)           ext = L"pdf";
        else if (buf.size() >= 4 && memcmp(buf.data(), "RIFF", 4) == 0)      ext = L"cdr";
        else if (buf.size() >= 4 &&
                 (buf[0] == 'A' || buf[0] == 'a') && buf[1] == 'C' &&
                 buf[2] >= '0' && buf[2] <= '9')                              ext = L"dwg";
        else if (buf.size() >= 4 && buf[0] == ' ' && buf[1] == ' ' &&
                 buf[2] == '0' && (buf[3] == '\r' || buf[3] == '\n'))      ext = L"dxf";
        else {
            bool asc = true;
            size_t probe = buf.size() < 1024 ? buf.size() : 1024;
            for (size_t i = 0; i < probe; ++i) {
                if (buf[i] == 0) { asc = false; break; }
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
        BOOL ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &written, nullptr) != 0
                  && written == buf.size();
        CloseHandle(h);
        if (!ok) { DeleteFileW(path); return E_FAIL; }

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

        std::wstring tmpBmp = MakeTempBmpPath();
        if (tmpBmp.empty()) return E_FAIL;

        HBITMAP hbmp = nullptr;
        if (RunThumbnailWorker(exePath, m_path, tmpBmp, cx))
            hbmp = LoadWorkerBmp(tmpBmp);
        else
            DbgLog(L"  worker failed/timeout");
        DeleteFileW(tmpBmp.c_str());

        if (!hbmp) return E_FAIL;
        *phbmp = hbmp;
        *pdwAlpha = WTSAT_ARGB;
        DbgLog(L"  OK");
        return S_OK;
    }

    // IObjectWithSite — Explorer hands us the IShellItem for the file via
    // SetSite. Extract the on-disk path so GetThumbnail can launch the
    // worker. Win10/11 Explorer calls SetSite instead of IInitializeWithFile.
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) override {
        DbgLog(L"SetSite called");
        m_path.clear();
        if (!pUnkSite) return S_OK;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetIDListFromObject(pUnkSite, &pidl))) {
            PWSTR name = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &name))) {
                m_path = name;
                DbgLog((std::wstring(L"  site path=") + m_path).c_str());
                CoTaskMemFree(name);
            } else {
                DbgLog(L"  SHGetNameFromIDList failed");
            }
            ILFree(pidl);
        } else {
            DbgLog(L"  SHGetIDListFromObject failed");
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSite(REFIID, void** ppvSite) override {
        if (ppvSite) *ppvSite = nullptr;
        return E_NOINTERFACE; // we don't retain the site
    }

private:
    LONG m_cRef;
    std::wstring m_path;
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
