// ============================================================================
// QuickViewThumbnailProvider.cpp
// COM DLL: Windows Shell IThumbnailProvider for CDR/CMX files
// 数据流: IStream → libcdr::parse → SVG XML → D2D WARP → HBITMAP
// ============================================================================
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <shobjidl.h>       // IThumbnailProvider, IInitializeWithStream
#include <shlwapi.h>
#include <d2d1_3.h>       // D2D 1.0-1.3: ID2D1DeviceContext, ID2D1Bitmap1, ID2D1SvgDocument
#include <d3d11.h>
#include <dxgi.h>

#include <libcdr/libcdr.h>
#include <librevenge/librevenge.h>

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

// ============================================================================
// CLSID / IID definitions
// ============================================================================

// {4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}
DEFINE_GUID(CLSID_QuickViewThumbnailProvider,
    0x4F8C2A6E, 0x3B5D, 0x4E7F, 0x9A, 0x1C, 0x2D, 0x3E, 0x4F, 0x5A, 0x6B, 0x7C);

// IInitializeWithStream: {B824B65D-E5AC-4F6B-9A13-4AB6B37E5A82}
DEFINE_GUID(IID_IInitializeWithStream,
    0xB824B65D, 0xE5AC, 0x4F6B, 0x9A, 0x13, 0x4A, 0xB6, 0xB3, 0x7E, 0x5A, 0x82);

// IThumbnailProvider: {E357FCC4-A995-453C-BF9A-9B18E2BD4DCA}
DEFINE_GUID(IID_IThumbnailProvider,
    0xE357FCC4, 0xA995, 0x453C, 0xBF, 0x9A, 0x9B, 0x18, 0xE2, 0xBD, 0x4D, 0xCA);

// WTS_ALPHATYPE
typedef enum WTS_ALPHATYPE {
    WTSAT_UNKNOWN = 0,
    WTSAT_RGB = 1,
    WTSAT_ARGB = 2,
} WTS_ALPHATYPE;

// ============================================================================
// Reference counting
// ============================================================================
static LONG g_cRefModule = 0;     // Module ref count
static LONG g_cRefServer = 0;     // Lock count

// ============================================================================
// CThumbnailProvider — implements IInitializeWithStream + IThumbnailProvider
// ============================================================================
class CThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    CThumbnailProvider() : m_cRef(1), m_pStream(nullptr) {
        InterlockedIncrement(&g_cRefModule);
    }
    ~CThumbnailProvider() {
        if (m_pStream) m_pStream->Release();
        InterlockedDecrement(&g_cRefModule);
    }

    // --- IUnknown ---
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IInitializeWithStream)
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == IID_IThumbnailProvider)
            *ppv = static_cast<IThumbnailProvider*>(this);
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

    // --- IInitializeWithStream ---
    HRESULT STDMETHODCALLTYPE Initialize(IStream* pStream, DWORD grfMode) override {
        if (!pStream) return E_POINTER;
        if (m_pStream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_pStream = pStream;
        m_pStream->AddRef();
        return S_OK;
    }

    // --- IThumbnailProvider ---
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override {
        if (!phbmp || !pdwAlpha) return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_RGB;

        if (!m_pStream) return E_UNEXPECTED;

        // 1. Read stream into memory
        std::vector<uint8_t> fileData;
        BYTE buf[65536];
        ULONG cbRead;
        while (m_pStream->Read(buf, sizeof(buf), &cbRead) == S_OK && cbRead > 0)
            fileData.insert(fileData.end(), buf, buf + cbRead);
        if (fileData.empty()) return E_FAIL;

        // 2. Parse CDR/CMX via libcdr → SVG XML
        std::string svgXml;
        float svgW = 0, svgH = 0;
        if (!ParseToSvg(fileData, svgXml, svgW, svgH))
            return E_FAIL;

        // 3. Rasterize SVG via D2D WARP → HBITMAP
        HBITMAP hbmp = RasterizeSvgToHBitmap(svgXml, svgW, svgH, cx);
        if (!hbmp) return E_FAIL;

        *phbmp = hbmp;
        *pdwAlpha = WTSAT_ARGB;
        return S_OK;
    }

private:
    LONG m_cRef;
    IStream* m_pStream;

    // --- CDR/CMX → SVG XML (simplified from QuickView LoadCDR) ---
    static bool ParseToSvg(const std::vector<uint8_t>& fileData,
                           std::string& outSvg, float& outW, float& outH) {
        librevenge::RVNGStringStream input(fileData.data(),
                                           (unsigned)fileData.size());
        bool isCdr = libcdr::CDRDocument::isSupported(&input);
        bool isCmx = false;
        if (!isCdr) {
            isCmx = libcdr::CMXDocument::isSupported(&input);
            if (!isCmx) return false;
        }

        librevenge::RVNGStringVector svgPages;
        librevenge::RVNGSVGDrawingGenerator painter(svgPages, "");

        bool parsed = false;
        if (isCdr)
            parsed = libcdr::CDRDocument::parse(&input, &painter);
        else
            parsed = libcdr::CMXDocument::parse(&input, &painter);

        if (!parsed || svgPages.empty()) return false;

        outSvg = svgPages[0].cstr();
        // Try to extract width/height from SVG root element
        outW = ExtractSvgAttr(outSvg, "width");
        outH = ExtractSvgAttr(outSvg, "height");
        if (outW <= 0) outW = 512;
        if (outH <= 0) outH = 512;
        return true;
    }

    static float ExtractSvgAttr(const std::string& svg, const char* attr) {
        std::string needle = std::string(attr) + "=\"";
        size_t pos = svg.find(needle);
        if (pos == std::string::npos) return 0;
        pos += needle.size();
        size_t end = svg.find('"', pos);
        if (end == std::string::npos) return 0;
        std::string val = svg.substr(pos, end - pos);
        // Strip units (px, pt, mm, etc.)
        float f = 0;
        try { f = std::stof(val); } catch (...) { return 0; }
        return f;
    }

    // --- SVG XML → HBITMAP via D3D/D2D WARP ---
    static HBITMAP RasterizeSvgToHBitmap(const std::string& svgXml,
                                         float svgW, float svgH, UINT targetSize) {
        const float safeW = svgW > 0 ? svgW : 512.0f;
        const float safeH = svgH > 0 ? svgH : 512.0f;
        const float scale = (std::min)(1.0f, (float)targetSize / (std::max)(safeW, safeH));
        const UINT outW = (UINT)(std::max)(1u, (UINT)(safeW * scale + 0.5f));
        const UINT outH = (UINT)(std::max)(1u, (UINT)(safeH * scale + 0.5f));

        // Create D3D WARP device
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                       D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext);
        if (FAILED(hr)) return nullptr;

        // Create render target texture
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = outW;
        texDesc.Height = outH;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> renderTex;
        hr = d3dDevice->CreateTexture2D(&texDesc, nullptr, &renderTex);
        if (FAILED(hr)) return nullptr;

        // Get DXGI surface → create D2D bitmap
        ComPtr<IDXGISurface> dxgiSurface;
        renderTex.As(&dxgiSurface);
        ComPtr<ID2D1Factory1> d2dFactory;
        D2D1_FACTORY_OPTIONS opts = {};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                          __uuidof(ID2D1Factory1), &opts,
                          reinterpret_cast<void**>(d2dFactory.GetAddressOf()));
        if (FAILED(hr)) return nullptr;

        ComPtr<ID2D1Device> d2dDevice;
        ComPtr<IDXGIDevice> dxgiDevice;
        d3dDevice.As(&dxgiDevice);
        d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice);

        ComPtr<ID2D1DeviceContext> d2dCtx;
        d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx);

        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        ComPtr<ID2D1Bitmap1> targetBmp;
        d2dCtx->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bmpProps, &targetBmp);

        // Create SVG document from XML
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, svgXml.size());
        if (!hMem) return nullptr;
        void* mem = GlobalLock(hMem);
        memcpy(mem, svgXml.data(), svgXml.size());
        GlobalUnlock(hMem);
        ComPtr<IStream> stream;
        CreateStreamOnHGlobal(hMem, TRUE, &stream);

        ComPtr<ID2D1DeviceContext5> d2dCtx5;
        d2dCtx.As(&d2dCtx5);
        if (!d2dCtx5) { GlobalFree(hMem); return nullptr; }

        ComPtr<ID2D1SvgDocument> svgDoc;
        hr = d2dCtx5->CreateSvgDocument(stream.Get(),
                                         D2D1::SizeF(safeW, safeH), &svgDoc);
        if (FAILED(hr)) { GlobalFree(hMem); return nullptr; }

        // Render
        d2dCtx5->SetTarget(targetBmp.Get());
        d2dCtx5->BeginDraw();
        d2dCtx5->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        d2dCtx5->SetTransform(
            D2D1::Matrix3x2F::Scale((float)outW / safeW, (float)outH / safeH));
        d2dCtx5->DrawSvgDocument(svgDoc.Get());
        d2dCtx5->EndDraw();

        // Copy to staging texture
        D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> stagingTex;
        d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
        d3dContext->CopyResource(stagingTex.Get(), renderTex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d3dContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return nullptr;

        // Create HBITMAP from pixel data
        HBITMAP hbmp = CreateHBitmapFromPixels(outW, outH,
                                                 mapped.pData, mapped.RowPitch);
        d3dContext->Unmap(stagingTex.Get(), 0);
        return hbmp;
    }

    static HBITMAP CreateHBitmapFromPixels(UINT width, UINT height,
                                            const void* pixels, UINT srcStride) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = -(LONG)height; // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* pBits = nullptr;
        HDC hdc = GetDC(nullptr);
        HBITMAP hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        if (!hbmp || !pBits) return nullptr;
        // Copy row by row (stride may differ)
        UINT dstStride = width * 4;
        for (UINT y = 0; y < height; ++y)
            memcpy((BYTE*)pBits + y * dstStride,
                   (BYTE*)pixels + y * srcStride, dstStride);
        return hbmp;
    }

    // Simple ComPtr (to avoid ATL dependency)
    template<typename T>
    class ComPtr {
    public:
        ComPtr() = default;
        ~ComPtr() { if (ptr) ptr->Release(); }
        ComPtr(const ComPtr&) = delete;
        ComPtr& operator=(const ComPtr&) = delete;
        T* operator->() { return ptr; }
        operator T**() { return &ptr; }
        operator bool() const { return ptr != nullptr; }
        T** GetAddressOf() { return &ptr; }
        T* Get() { return ptr; }
        template<typename U>
        void As(ComPtr<U>* other) {
            if (ptr && other) {
                void* p = nullptr;
                if (SUCCEEDED(ptr->QueryInterface(__uuidof(U), &p))) {
                    other->Reset(static_cast<U*>(p));
                }
            }
        }
        void Reset(T* p = nullptr) {
            if (ptr) ptr->Release();
            ptr = p;
        }
        T* ptr = nullptr;
    };
};

// ============================================================================
// CClassFactory — creates CThumbnailProvider instances
// ============================================================================
class CClassFactory : public IClassFactory {
public:
    CClassFactory() : m_cRef(1) {}
    ~CClassFactory() {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
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

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        auto* p = new CThumbnailProvider();
        if (!p) return E_OUTOFMEMORY;
        HRESULT hr = p->QueryInterface(riid, ppv);
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
// DllMain — save module handle for path resolution
// ============================================================================
static HMODULE g_hModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

// ============================================================================
// DLL Exports
// ============================================================================
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_QuickViewThumbnailProvider)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* pFactory = new CClassFactory();
    if (!pFactory) return E_OUTOFMEMORY;
    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return (g_cRefModule == 0 && g_cRefServer == 0) ? S_OK : S_FALSE;
}

#include <strsafe.h>
extern "C" HRESULT __stdcall DllRegisterServer() {
    wchar_t dllPath[MAX_PATH];
    GetModuleFileNameW(g_hModule, dllPath, MAX_PATH);

    wchar_t clsidPath[] = L"Software\\Classes\\CLSID\\{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}";
    HKEY hKey;
    // CLSID registration
    if (RegCreateKeyExW(HKEY_CURRENT_USER, clsidPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)L"QuickView Thumbnail Provider",
                       sizeof(L"QuickView Thumbnail Provider"));
        RegCloseKey(hKey);
    }
    // InprocServer32
    wchar_t inprocPath[MAX_PATH + 64];
    StringCchPrintfW(inprocPath, _countof(inprocPath), L"%s\\InprocServer32", clsidPath);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, inprocPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)dllPath,
                       (DWORD)((wcslen(dllPath) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hKey, L"ThreadingModel", 0, REG_SZ,
                       (const BYTE*)L"Apartment", sizeof(L"Apartment"));
        RegCloseKey(hKey);
    }
    // Register thumbnail provider for .cdr and .cmx
    const wchar_t* exts[] = { L".cdr", L".cmx" };
    const wchar_t* thumbnailIID = L"{E357FCC4-A995-453C-BF9A-9B18E2BD4DCA}";
    const wchar_t* providerCLSID = L"{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}";
    for (const wchar_t* ext : exts) {
        wchar_t shellexPath[MAX_PATH + 64];
        StringCchPrintfW(shellexPath, _countof(shellexPath),
                         L"Software\\Classes\\%s\\ShellEx\\%s", ext, thumbnailIID);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, shellexPath, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                           (const BYTE*)providerCLSID,
                           (DWORD)((wcslen(providerCLSID) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    // Delete CLSID
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\CLSID\\{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}");
    // Delete ShellEx thumbnail provider for .cdr and .cmx
    const wchar_t* exts[] = { L".cdr", L".cmx" };
    const wchar_t* thumbnailIID = L"{E357FCC4-A995-453C-BF9A-9B18E2BD4DCA}";
    for (const wchar_t* ext : exts) {
        wchar_t shellexPath[MAX_PATH + 64];
        StringCchPrintfW(shellexPath, _countof(shellexPath),
                         L"Software\\Classes\\%s\\ShellEx\\%s", ext, thumbnailIID);
        RegDeleteTreeW(HKEY_CURRENT_USER, shellexPath);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
