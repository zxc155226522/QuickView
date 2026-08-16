// WinRtPdfDocument.cpp
// Windows 原生 PDF 渲染实现
// 使用 WRL + WinRT ABI COM 接口，不需要 C++/WinRT 头文件
// 此文件单独编译时启用 /EHsc（异常），因为 WRL::Make<>() 需要 try/catch

#include "pch.h"
#include "WinRtPdfDocument.h"

#include <windows.data.pdf.interop.h>    // IPdfRendererNative, PdfCreateRenderer, PDF_RENDER_PARAMS
#include <windows.data.pdf.h>            // ABI::Windows::Data::Pdf::IPdfDocument 等
#include <windows.foundation.h>           // IAsyncOperation, AsyncStatus
#include <windows.storage.h>              // IStorageFile
#include <roapi.h>                        // RoActivateInstance, RoGetActivationFactory
#include <shcore.h>                       // CreateRandomAccessStreamOnFile
#include <windowsstoragecom.h>           // IRandomAccessStreamByteAccess

#include <wrl/client.h>                   // ComPtr
#include <wrl/implements.h>               // Make<>

#include "ImageLoaderSimd.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// 链接 windows.data.pdf.lib (PdfCreateRenderer)
#pragma comment(lib, "windows.data.pdf.lib")
#pragma comment(lib, "runtimeobject.lib")  // RoActivateInstance 等

using namespace ABI::Windows::Data::Pdf;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage;
using namespace ABI::Windows::Storage::Streams;
using Microsoft::WRL::ComPtr;

namespace QuickView {

// ============================================================================
// 异步操作完成回调处理器
// WRL 实现 IAsyncOperationCompletedHandler<PdfDocument*>
// ============================================================================
class PdfLoadCompletedHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IAsyncOperationCompletedHandler<PdfDocument*>> {
public:
    PdfLoadCompletedHandler(HANDLE event, IPdfDocument** outDoc, HRESULT* outHr)
        : m_event(event), m_outDoc(outDoc), m_outHr(outHr) {}

    HRESULT STDMETHODCALLTYPE Invoke(IAsyncOperation<PdfDocument*>* asyncInfo,
                                     AsyncStatus status) noexcept override {
        if (status == AsyncStatus::Completed) {
            *m_outHr = asyncInfo->GetResults(m_outDoc);
        } else {
            *m_outHr = E_FAIL;
        }
        SetEvent(m_event);
        return S_OK;
    }

private:
    HANDLE m_event;
    IPdfDocument** m_outDoc;
    HRESULT* m_outHr;
};

// ============================================================================
// WinRtPdfDocument::Impl - 隐藏 COM 细节
// ============================================================================
struct WinRtPdfDocument::Impl {
    ComPtr<IPdfDocument> pdfDoc;
    ComPtr<IPdfRendererNative> pdfRenderer;
    ComPtr<IDXGIDevice> dxgiDevice;

    // 从 D2D 设备上下文获取 DXGI 设备
    HRESULT InitDxgiDevice(ID2D1DeviceContext* d2dContext) noexcept {
        if (!d2dContext) return E_POINTER;

        ComPtr<ID2D1Device> d2dDevice;
        d2dContext->GetDevice(&d2dDevice);
        if (!d2dDevice) return E_POINTER;

        ComPtr<IDXGIDevice> dxgiDev;
        HRESULT hr = d2dDevice.As(&dxgiDev);
        if (FAILED(hr)) return hr;

        dxgiDevice = dxgiDev;
        return S_OK;
    }
};

WinRtPdfDocument::WinRtPdfDocument() noexcept
    : m_impl(std::make_unique<Impl>()) {}

WinRtPdfDocument::~WinRtPdfDocument() {
    Close();
}

HRESULT WinRtPdfDocument::Open(const std::wstring& path,
                                std::wstring& errorMessage) noexcept {
    Close();
    if (path.empty()) return E_INVALIDARG;

    // 1. 通过 IStorageFile 代理打开文件
    //    使用 IStorageFolderHandleAccess -> 不需要 StorageFile::GetFileFromPathAsync
    //    但更简单的方式是直接用 CreateRandomAccessStreamOnFile + LoadFromStreamAsync
    ComPtr<IRandomAccessStream> randStream;
    HRESULT hr = CreateRandomAccessStreamOnFile(
        path.c_str(),
        0,  // FileAccessMode_Read = 0
        IID_PPV_ARGS(&randStream));
    if (FAILED(hr) || !randStream) {
        errorMessage = L"无法打开文件流: " + std::to_wstring(hr);
        return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    }

    // 2. 获取 IPdfDocumentStatics 工厂
    ComPtr<IPdfDocumentStatics> pdfStatics;
    HSTRING hstrClassName = nullptr;
    hr = WindowsCreateString(
        RuntimeClass_Windows_Data_Pdf_PdfDocument,
        static_cast<UINT32>(wcslen(RuntimeClass_Windows_Data_Pdf_PdfDocument)),
        &hstrClassName);
    if (FAILED(hr) || !hstrClassName) {
        errorMessage = L"WindowsCreateString 失败";
        return hr;
    }

    hr = RoGetActivationFactory(hstrClassName,
                                IID_PPV_ARGS(&pdfStatics));
    WindowsDeleteString(hstrClassName);
    if (FAILED(hr) || !pdfStatics) {
        errorMessage = L"RoGetActivationFactory 获取 PdfDocument 工厂失败";
        return hr;
    }

    // 3. 调用 LoadFromStreamAsync
    ComPtr<IAsyncOperation<PdfDocument*>> loadAsync;
    hr = pdfStatics->LoadFromStreamAsync(randStream.Get(), &loadAsync);
    if (FAILED(hr) || !loadAsync) {
        errorMessage = L"LoadFromStreamAsync 失败";
        return hr;
    }

    // 4. 等待异步完成
    HANDLE doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!doneEvent) return E_OUTOFMEMORY;

    ComPtr<IPdfDocument> resultDoc;
    HRESULT loadHr = E_FAIL;
    auto handler = Microsoft::WRL::Make<PdfLoadCompletedHandler>(
        doneEvent, &resultDoc, &loadHr);
    if (!handler) {
        CloseHandle(doneEvent);
        return E_OUTOFMEMORY;
    }

    hr = loadAsync->put_Completed(handler.Get());
    if (FAILED(hr)) {
        CloseHandle(doneEvent);
        errorMessage = L"put_Completed 失败";
        return hr;
    }
    handler->AddRef();  // 防止回调完成前 handler 被释放

    // 等待完成（30秒超时）
    DWORD waitResult = WaitForSingleObject(doneEvent, 30000);
    handler->Release();  // 释放上面的 AddRef
    CloseHandle(doneEvent);

    if (waitResult != WAIT_OBJECT_0) {
        errorMessage = L"PDF 加载超时";
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    if (FAILED(loadHr) || !resultDoc) {
        errorMessage = L"PDF 加载失败";
        return loadHr;
    }

    // 5. 获取页数
    UINT32 pageCount = 0;
    hr = resultDoc->get_PageCount(&pageCount);
    if (FAILED(hr) || pageCount == 0) {
        errorMessage = L"无法获取页数或文档为空";
        return E_FAIL;
    }

    m_impl->pdfDoc = resultDoc;
    m_path = path;
    m_pageCount = pageCount;
    m_pdfDoc = resultDoc.Get();  // 保存裸指针用于 IsOpen 判断
    return S_OK;
}

void WinRtPdfDocument::Close() noexcept {
    if (m_impl) {
        m_impl->pdfDoc.Reset();
        m_impl->pdfRenderer.Reset();
        m_impl->dxgiDevice.Reset();
    }
    m_pdfDoc = nullptr;
    m_path.clear();
    m_pageCount = 0;
}

HRESULT WinRtPdfDocument::RenderPage(uint32_t pageIndex,
                                     int viewportWidth,
                                     int viewportHeight,
                                     float zoom,
                                     DocumentRenderResult& result) noexcept {
    result = {};
    result.path = m_path;
    result.pageIndex = pageIndex;
    result.pageCount = m_pageCount;

    if (!m_impl || !m_impl->pdfDoc) return E_NOT_VALID_STATE;
    if (pageIndex >= m_pageCount) return E_INVALIDARG;

    // 1. 获取 PdfPage
    ComPtr<IPdfPage> pdfPage;
    HRESULT hr = m_impl->pdfDoc->GetPage(pageIndex, &pdfPage);
    if (FAILED(hr) || !pdfPage) {
        result.status = hr;
        result.errorMessage = L"GetPage 失败";
        return hr;
    }

    // 2. 获取页面尺寸（Size, 单位：DIPs/points, 72dpi）
    Size pageSize{};
    hr = pdfPage->get_Size(&pageSize);
    if (FAILED(hr)) {
        result.status = hr;
        result.errorMessage = L"get_Size 失败";
        return hr;
    }

    float pageWidthPoints = std::max(1.0f, static_cast<float>(pageSize.Width));
    float pageHeightPoints = std::max(1.0f, static_cast<float>(pageSize.Height));

    // 3. 计算渲染尺寸
    const float safeViewportWidth = static_cast<float>(std::max(viewportWidth, 64));
    const float safeViewportHeight = static_cast<float>(std::max(viewportHeight, 64));
    // 使用 max（与 MuPDF 路径一致）：覆盖视口长边，避免 DComp 上采样模糊
    const float fitScale = std::max(safeViewportWidth / pageWidthPoints,
                                    safeViewportHeight / pageHeightPoints);
    const float requestedScale = std::clamp(fitScale * std::max(zoom, 0.05f), 0.05f, 16.0f);
    const float maxDimensionScale = 16384.0f / std::max(pageWidthPoints, pageHeightPoints);
    const float renderScale = std::min(requestedScale, std::max(maxDimensionScale, 0.05f));

    const UINT32 destWidth = static_cast<UINT32>(std::max(1.0f, pageWidthPoints * renderScale));
    const UINT32 destHeight = static_cast<UINT32>(std::max(1.0f, pageHeightPoints * renderScale));

    // 4. 创建 D2D 设备上下文用于渲染
    //    需要从全局 D3D 设备获取 DXGI 设备
    //    使用 WARP 设备避免与主渲染管线的 D3D 设备冲突
    ComPtr<ID3D11Device> d3dDevice;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        &featureLevel, 1, D3D11_SDK_VERSION,
        &d3dDevice, nullptr, nullptr);
    if (FAILED(hr) || !d3dDevice) {
        result.status = hr;
        result.errorMessage = L"D3D11CreateDevice (WARP) 失败";
        return hr;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = d3dDevice.As(&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) {
        result.status = hr;
        result.errorMessage = L"获取 IDXGIDevice 失败";
        return hr;
    }

    // 5. 创建 D2D 设备
    ComPtr<ID2D1Factory1> d2dFactory1;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                           d2dFactory1.GetAddressOf());
    if (FAILED(hr) || !d2dFactory1) {
        result.status = hr;
        result.errorMessage = L"D2D1CreateFactory 失败";
        return hr;
    }

    ComPtr<ID2D1Device> d2dDevice;
    hr = d2dFactory1->CreateDevice(dxgiDevice.Get(), &d2dDevice);
    if (FAILED(hr) || !d2dDevice) {
        result.status = hr;
        result.errorMessage = L"CreateDevice 失败";
        return hr;
    }

    ComPtr<ID2D1DeviceContext> d2dContext;
    hr = d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext);
    if (FAILED(hr) || !d2dContext) {
        result.status = hr;
        result.errorMessage = L"CreateDeviceContext 失败";
        return hr;
    }

    // 6. 创建目标位图
    D2D1_BITMAP_PROPERTIES1 bmpProps = {};
    bmpProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bmpProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    bmpProps.dpiX = 96.0f * renderScale;  // 高 DPI 渲染
    bmpProps.dpiY = 96.0f * renderScale;
    // D2D1_BITMAP_OPTIONS_TARGET: 可作为渲染目标
    // D2D1_BITMAP_OPTIONS_CANNOT_DRAW: 不能直接绘制（但可通过 Map 读取）
    // 在 WARP 设备上，这种组合允许 Map(D2D1_MAP_OPTIONS_READ) 读取像素
    bmpProps.bitmapOptions = static_cast<D2D1_BITMAP_OPTIONS>(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);

    ComPtr<ID2D1Bitmap1> targetBitmap;
    D2D1_SIZE_U bitmapSize = { destWidth, destHeight };
    hr = d2dContext->CreateBitmap(bitmapSize, nullptr, 0, &bmpProps, &targetBitmap);
    if (FAILED(hr) || !targetBitmap) {
        result.status = hr;
        result.errorMessage = L"CreateBitmap 失败";
        return hr;
    }

    d2dContext->SetTarget(targetBitmap.Get());

    // 7. 渲染 PDF 页面到 D2D 上下文
    //    先填充白色背景
    d2dContext->BeginDraw();
    d2dContext->Clear(D2D1::ColorF(D2D1::ColorF::White, 1.0f));

    // 获取或创建 IPdfRendererNative
    if (!m_impl->pdfRenderer) {
        hr = PdfCreateRenderer(dxgiDevice.Get(), &m_impl->pdfRenderer);
        if (FAILED(hr) || !m_impl->pdfRenderer) {
            d2dContext->EndDraw();
            result.status = hr;
            result.errorMessage = L"PdfCreateRenderer 失败";
            return hr;
        }
    }

    // PDF_RENDER_PARAMS: DestinationWidth/Height 为 0 表示使用默认（页面原始尺寸）
    // 设置为目标尺寸以获得高分辨率渲染
    PDF_RENDER_PARAMS renderParams = {};
    renderParams.SourceRect = { 0.0f, 0.0f, 0.0f, 0.0f };  // 默认：整个页面
    renderParams.DestinationWidth = destWidth;
    renderParams.DestinationHeight = destHeight;
    renderParams.BackgroundColor = { 1.0f, 1.0f, 1.0f, 1.0f };  // 白色
    renderParams.IgnoreHighContrast = TRUE;

    // RenderPageToDeviceContext: 将 pdfPage 作为 IUnknown* 传入
    hr = m_impl->pdfRenderer->RenderPageToDeviceContext(
        pdfPage.Get(),            // IUnknown* pdfPage
        d2dContext.Get(),         // ID2D1DeviceContext*
        &renderParams);           // PDF_RENDER_PARAMS*
    if (FAILED(hr)) {
        d2dContext->EndDraw();
        result.status = hr;
        result.errorMessage = L"RenderPageToDeviceContext 失败";
        return hr;
    }

    hr = d2dContext->EndDraw();
    if (FAILED(hr)) {
        result.status = hr;
        result.errorMessage = L"EndDraw 失败";
        return hr;
    }

    // 8. 从目标位图拷贝像素数据
    D2D1_SIZE_U size = d2dContext->GetPixelSize();
    const int width = static_cast<int>(size.width);
    const int height = static_cast<int>(size.height);

    const int stride = CalculateAlignedStride(width, 4, 4);
    const size_t bufferSize = static_cast<size_t>(stride) * static_cast<size_t>(height);
    auto* pixels = static_cast<uint8_t*>(std::malloc(bufferSize));
    if (!pixels) {
        result.status = E_OUTOFMEMORY;
        result.errorMessage = L"像素缓冲区分配失败";
        return result.status;
    }

    // CopyFromMemory: dstPitch = stride, srcRect = nullptr 表示整个位图
    // 注意：D2D 位图在 GPU 上，CopyFromMemory 是从 CPU 写入 GPU。
    // 我们需要的是从 GPU 位图读取到 CPU：用 ID2D1Bitmap1::CopyFromMemory
    // 的反向操作 = ID2D1Bitmap1::Map(D2D1_MAP_OPTIONS_READ)
    D2D1_MAPPED_RECT mappedRect = {};
    hr = targetBitmap->Map(D2D1_MAP_OPTIONS_READ, &mappedRect);
    if (FAILED(hr)) {
        std::free(pixels);
        result.status = hr;
        result.errorMessage = L"Map (READ) 失败";
        return hr;
    }

    // 从映射的内存拷贝到我们的缓冲区
    const int srcStride = static_cast<int>(mappedRect.pitch);
    const uint8_t* srcData = mappedRect.bits;
    for (int y = 0; y < height; ++y) {
        std::memcpy(pixels + static_cast<size_t>(y) * stride,
                    srcData + static_cast<size_t>(y) * srcStride,
                    static_cast<size_t>(width) * 4);
    }
    targetBitmap->Unmap();

    // D2D 位图输出已是 premultiplied BGRA，无需再处理

    auto frame = std::make_shared<RawImageFrame>();
    frame->pixels = pixels;
    frame->width = width;
    frame->height = height;
    frame->stride = stride;
    frame->format = PixelFormat::BGRA8888;
    frame->quality = DecodeQuality::Full;
    frame->formatDetails = L"Windows.Data.Pdf (Native)";
    frame->dpiX = 72.0f * renderScale;
    frame->dpiY = 72.0f * renderScale;
    frame->memoryDeleter = MemoryDeleter::FromFree();

    result.pageWidthPoints = pageWidthPoints;
    result.pageHeightPoints = pageHeightPoints;
    result.renderScale = renderScale;
    result.frame = std::move(frame);
    result.status = S_OK;
    return S_OK;
}

} // namespace QuickView
