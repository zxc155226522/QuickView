// ============================================================================
// QuickViewThumbnailProvider.cpp
// COM DLL: Windows Shell IThumbnailProvider for CDR/CMX files
// 数据流: IStream → libcdr::parse → SVG XML → D2D WARP → HBITMAP
// ============================================================================
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>          // IThumbnailProvider, IInitializeWithStream, SHChangeNotify
#include <shlwapi.h>
#include <strsafe.h>
#include <d2d1_3.h>          // D2D 1.0-1.3: ID2D1DeviceContext5, ID2D1SvgDocument
#include <d3d11.h>
#include <dxgi.h>

#include <libcdr/libcdr.h>
#include <librevenge/librevenge.h>

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

// ============================================================================
// CLSID
// {4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}
DEFINE_GUID(CLSID_QuickViewThumbnailProvider,
    0x4F8C2A6E, 0x3B5D, 0x4E7F, 0x9A, 0x1C, 0x2D, 0x3E, 0x4F, 0x5A, 0x6B, 0x7C);

// ============================================================================
// Reference counting
// ============================================================================
static LONG g_cRefModule = 0;
static LONG g_cRefServer = 0;
static HMODULE g_hModule = nullptr;

// ============================================================================
// Minimal ComPtr
// ============================================================================
template<typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (ptr) ptr->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (ptr != other.ptr) {
            if (ptr) ptr->Release();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }
    T* operator->() const { return ptr; }
    T* const* GetAddressOf() { return &ptr; }
    T** ReleaseAndGetAddressOf() { if (ptr) { ptr->Release(); ptr = nullptr; } return &ptr; }
    T* Get() const { return ptr; }
    operator bool() const { return ptr != nullptr; }
    template<typename U>
    ComPtr<U> As() {
        ComPtr<U> result;
        if (ptr) {
            void* p = nullptr;
            if (SUCCEEDED(ptr->QueryInterface(__uuidof(U), &p)))
                result.Attach(static_cast<U*>(p));
        }
        return result;
    }
    void Attach(T* p) { if (ptr) ptr->Release(); ptr = p; }
    T* ptr = nullptr;
};

// ============================================================================
// CThumbnailProvider
// ============================================================================
class CThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    CThumbnailProvider() : m_cRef(1), m_pStream(nullptr) {
        InterlockedIncrement(&g_cRefModule);
    }
    virtual ~CThumbnailProvider() {
        if (m_pStream) m_pStream->Release();
        InterlockedDecrement(&g_cRefModule);
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IInitializeWithStream))
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == __uuidof(IThumbnailProvider))
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

    // IInitializeWithStream
    HRESULT STDMETHODCALLTYPE Initialize(IStream* pStream, DWORD) override {
        if (!pStream) return E_POINTER;
        if (m_pStream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        m_pStream = pStream;
        m_pStream->AddRef();
        return S_OK;
    }

    // IThumbnailProvider
    HRESULT STDMETHODCALLTYPE GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* pdwAlpha) override {
        if (!phbmp || !pdwAlpha) return E_POINTER;
        *phbmp = nullptr;
        *pdwAlpha = WTSAT_RGB;
        if (!m_pStream) return E_UNEXPECTED;

        // 1. Read stream
        std::vector<uint8_t> fileData;
        BYTE buf[65536];
        ULONG cbRead;
        while (m_pStream->Read(buf, sizeof(buf), &cbRead) == S_OK && cbRead > 0)
            fileData.insert(fileData.end(), buf, buf + cbRead);
        if (fileData.empty()) return E_FAIL;

        // 2. CDR/CMX → SVG XML
        std::string svgXml;
        float svgW = 0, svgH = 0;
        if (!ParseToSvg(fileData, svgXml, svgW, svgH)) return E_FAIL;

        // 3. SVG → HBITMAP
        HBITMAP hbmp = RasterizeSvgToHBitmap(svgXml, svgW, svgH, cx);
        if (!hbmp) return E_FAIL;
        *phbmp = hbmp;
        *pdwAlpha = WTSAT_ARGB;
        return S_OK;
    }

private:
    LONG m_cRef;
    IStream* m_pStream;

    static bool ParseToSvg(const std::vector<uint8_t>& data,
                           std::string& outSvg, float& outW, float& outH) {
        librevenge::RVNGStringStream input(data.data(), (unsigned)data.size());
        bool isCdr = libcdr::CDRDocument::isSupported(&input);
        bool isCmx = false;
        if (!isCdr) {
            isCmx = libcdr::CMXDocument::isSupported(&input);
            if (!isCmx) return false;
        }
        librevenge::RVNGStringVector svgPages;
        librevenge::RVNGSVGDrawingGenerator painter(svgPages, "");
        bool parsed = isCdr ? libcdr::CDRDocument::parse(&input, &painter)
                            : libcdr::CMXDocument::parse(&input, &painter);
        if (!parsed || svgPages.empty()) return false;
        outSvg = svgPages[0].cstr();
        outW = ExtractAttr(outSvg, "width");
        outH = ExtractAttr(outSvg, "height");
        if (outW <= 0) outW = 512;
        if (outH <= 0) outH = 512;
        return true;
    }

    static float ExtractAttr(const std::string& svg, const char* attr) {
        std::string needle = std::string(attr) + "=\"";
        size_t pos = svg.find(needle);
        if (pos == std::string::npos) return 0;
        pos += needle.size();
        size_t end = svg.find('"', pos);
        if (end == std::string::npos) return 0;
        try { return std::stof(svg.substr(pos, end - pos)); }
        catch (...) { return 0; }
    }

    static HBITMAP RasterizeSvgToHBitmap(const std::string& svgXml,
                                         float svgW, float svgH, UINT targetSize) {
        float safeW = svgW > 0 ? svgW : 512.0f;
        float safeH = svgH > 0 ? svgH : 512.0f;
        float scale = (std::min)(1.0f, (float)targetSize / (std::max)(safeW, safeH));
        UINT outW = (UINT)(std::max)(1u, (UINT)(safeW * scale + 0.5f));
        UINT outH = (UINT)(std::max)(1u, (UINT)(safeH * scale + 0.5f));

        // D3D WARP device
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION,
            d3dDevice.GetAddressOf(), nullptr, d3dContext.GetAddressOf());
        if (FAILED(hr)) return nullptr;

        // Render target texture
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = outW; texDesc.Height = outH;
        texDesc.MipLevels = 1; texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> renderTex;
        hr = d3dDevice->CreateTexture2D(&texDesc, nullptr, renderTex.GetAddressOf());
        if (FAILED(hr)) return nullptr;

        // DXGI surface
        ComPtr<IDXGISurface> dxgiSurface;
        dxgiSurface = renderTex.As<IDXGISurface>();
        if (!dxgiSurface) return nullptr;

        // D2D factory + device
        ComPtr<ID2D1Factory1> d2dFactory;
        D2D1_FACTORY_OPTIONS opts = {};
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), &opts, (void**)d2dFactory.GetAddressOf());
        if (!d2dFactory) return nullptr;

        ComPtr<IDXGIDevice> dxgiDevice;
        dxgiDevice = d3dDevice.As<IDXGIDevice>();
        ComPtr<ID2D1Device> d2dDevice;
        d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.GetAddressOf());
        ComPtr<ID2D1DeviceContext> d2dCtx;
        d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dCtx.GetAddressOf());

        // Bitmap target
        D2D1_BITMAP_PROPERTIES1 bmpProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96, 96);
        ComPtr<ID2D1Bitmap1> targetBmp;
        d2dCtx->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bmpProps, targetBmp.GetAddressOf());

        // SVG document
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, svgXml.size());
        if (!hMem) return nullptr;
        void* mem = GlobalLock(hMem);
        memcpy(mem, svgXml.data(), svgXml.size());
        GlobalUnlock(hMem);
        ComPtr<IStream> stream;
        CreateStreamOnHGlobal(hMem, TRUE, stream.GetAddressOf());

        ComPtr<ID2D1DeviceContext5> d2dCtx5;
        d2dCtx5 = d2dCtx.As<ID2D1DeviceContext5>();
        if (!d2dCtx5) { GlobalFree(hMem); return nullptr; }

        ComPtr<ID2D1SvgDocument> svgDoc;
        hr = d2dCtx5->CreateSvgDocument(stream.Get(),
            D2D1::SizeF(safeW, safeH), svgDoc.GetAddressOf());
        if (FAILED(hr)) { GlobalFree(hMem); return nullptr; }

        // Render
        d2dCtx5->SetTarget(targetBmp.Get());
        d2dCtx5->BeginDraw();
        d2dCtx5->Clear(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        d2dCtx5->SetTransform(D2D1::Matrix3x2F::Scale(
            (float)outW / safeW, (float)outH / safeH));
        d2dCtx5->DrawSvgDocument(svgDoc.Get());
        d2dCtx5->EndDraw();

        // Staging texture
        D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        ComPtr<ID3D11Texture2D> stagingTex;
        d3dDevice->CreateTexture2D(&stagingDesc, nullptr, stagingTex.GetAddressOf());
        d3dContext->CopyResource(stagingTex.Get(), renderTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = d3dContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return nullptr;
        HBITMAP hbmp = CreateHBitmap(outW, outH, mapped.pData, mapped.RowPitch);
        d3dContext->Unmap(stagingTex.Get(), 0);
        return hbmp;
    }

    static HBITMAP CreateHBitmap(UINT w, UINT h, const void* pixels, UINT srcStride) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -(LONG)h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* pBits = nullptr;
        HDC hdc = GetDC(nullptr);
        HBITMAP hbmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        ReleaseDC(nullptr, hdc);
        if (!hbmp || !pBits) return nullptr;
        UINT dstStride = w * 4;
        for (UINT y = 0; y < h; ++y)
            memcpy((BYTE*)pBits + y * dstStride,
                   (BYTE*)pixels + y * srcStride, dstStride);
        return hbmp;
    }
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
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (pUnk) return CLASS_E_NOAGGREGATION;
        auto* p = new CThumbnailProvider();
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
// DllMain
// ============================================================================
BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID) {
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
    if (rclsid != CLSID_QuickViewThumbnailProvider) return CLASS_E_CLASSNOTAVAILABLE;
    auto* p = new CClassFactory();
    HRESULT hr = p->QueryInterface(riid, ppv);
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
    const wchar_t* thumbIID = L"{E357FCC4-A995-453C-BF9A-9B18E2BD4DCA}";
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
    // ShellEx for .cdr and .cmx
    const wchar_t* exts[] = { L".cdr", L".cmx" };
    for (auto ext : exts) {
        std::wstring key = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + thumbIID;
        RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr);
        if (hKey) { RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)clsid, (DWORD)((wcslen(clsid)+1)*sizeof(wchar_t))); RegCloseKey(hKey); }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

extern "C" HRESULT __stdcall DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{4F8C2A6E-3B5D-4E7F-9A1C-2D3E4F5A6B7C}");
    const wchar_t* thumbIID = L"{E357FCC4-A995-453C-BF9A-9B18E2BD4DCA}";
    const wchar_t* exts[] = { L".cdr", L".cmx" };
    for (auto ext : exts) {
        std::wstring key = L"Software\\Classes\\" + std::wstring(ext) + L"\\ShellEx\\" + thumbIID;
        RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
