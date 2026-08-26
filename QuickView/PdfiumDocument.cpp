// PdfiumDocument.cpp
// PDFium 引擎渲染实现
// 使用 PDFium (Google BSD, Edge/Chrome 同款引擎) 渲染 PDF/AI 到 BGRA 像素。
//
// PDFium API 线程安全注意:
//   PDFium 不是线程安全的。调用方必须确保同一时间只有一个线程在调用 PDFium。
//   DocumentRenderController 的工作线程串行处理请求，满足此约束。

#include "pch.h"
#include "PdfiumDocument.h"

#include <fpdfview.h>       // FPDF_InitLibrary, FPDF_LoadMemDocument, etc.
#include <fpdf_edit.h>      // FPDFBitmap_CreateEx, FPDFBitmap_GetBuffer, etc.

#include "ImageLoaderSimd.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace QuickView {

// ============================================================================
// 全局库初始化（引用计数）
// ============================================================================
int PdfiumDocument::s_refCount = 0;

void PdfiumDocument::EnsureLibraryInit() noexcept {
    static std::mutex s_libMutex;
    std::lock_guard lock(s_libMutex);
    if (s_refCount == 0) {
        FPDF_InitLibrary();
    }
    ++s_refCount;
}

// ============================================================================
// 构造/析构
// ============================================================================

PdfiumDocument::PdfiumDocument() noexcept {
    EnsureLibraryInit();
}

PdfiumDocument::~PdfiumDocument() {
    Close();
    // 减少引用计数，最后一个使用者关闭库
    static std::mutex s_libMutex;
    std::lock_guard lock(s_libMutex);
    if (s_refCount > 0) {
        --s_refCount;
        if (s_refCount == 0) {
            FPDF_DestroyLibrary();
        }
    }
}

// ============================================================================
// Open: 从文件加载 PDF
// ============================================================================
HRESULT PdfiumDocument::Open(const std::wstring& path,
                              std::wstring& errorMessage) noexcept {
    Close();
    if (path.empty()) return E_INVALIDARG;

    auto file = std::make_unique<MappedFile>(path);
    if (!file->IsValid()) {
        errorMessage = L"无法读取 PDF 文件。";
        return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    }

    // PDFium 的 FPDF_LoadMemDocument 需要文件数据在内存中
    // MappedFile 保持内存映射，生命周期与 document 相同
    FPDF_DOCUMENT doc = FPDF_LoadMemDocument(
        file->data(),
        static_cast<int>(file->size()),
        nullptr);  // 无密码

    if (!doc) {
        unsigned long err = FPDF_GetLastError();
        switch (err) {
            case FPDF_ERR_PASSWORD:
                errorMessage = L"PDF 需要密码。";
                break;
            case FPDF_ERR_SECURITY:
                errorMessage = L"PDF 安全限制。";
                break;
            case FPDF_ERR_FORMAT:
                errorMessage = L"PDF 格式错误。";
                break;
            case FPDF_ERR_FILE:
                errorMessage = L"PDF 文件读取失败。";
                break;
            default:
                errorMessage = L"PDF 加载失败 (错误码: " + std::to_wstring(err) + L")。";
                break;
        }
        return E_FAIL;
    }

    int pageCount = FPDF_GetPageCount(doc);
    if (pageCount <= 0) {
        errorMessage = L"PDF 文档没有页面。";
        FPDF_CloseDocument(doc);
        return E_FAIL;
    }

    m_file = std::move(file);
    m_document = doc;
    m_path = path;
    m_pageCount = static_cast<uint32_t>(pageCount);
    return S_OK;
}

// ============================================================================
// Close
// ============================================================================
void PdfiumDocument::Close() noexcept {
    if (m_document) {
        FPDF_CloseDocument(m_document);
        m_document = nullptr;
    }
    m_file.reset();
    m_path.clear();
    m_pageCount = 0;
}

// ============================================================================
// RenderPage: 渲染 PDF 页面到 BGRA 位图
// ============================================================================
HRESULT PdfiumDocument::RenderPage(uint32_t pageIndex,
                                    int viewportWidth,
                                    int viewportHeight,
                                    float zoom,
                                    DocumentRenderResult& result) noexcept {
    result = {};
    result.path = m_path;
    result.pageIndex = pageIndex;
    result.pageCount = m_pageCount;

    if (!m_document) return E_NOT_VALID_STATE;
    if (pageIndex >= m_pageCount) return E_INVALIDARG;

    // 1. 加载页面
    FPDF_PAGE page = FPDF_LoadPage(m_document, static_cast<int>(pageIndex));
    if (!page) {
        result.status = E_FAIL;
        result.errorMessage = L"FPDF_LoadPage 失败";
        return E_FAIL;
    }

    // 2. 获取页面尺寸（points, 72dpi）
    float pageWidthPoints = FPDF_GetPageWidthF(page);
    float pageHeightF = FPDF_GetPageHeightF(page);
    if (pageWidthPoints < 1.0f) pageWidthPoints = 1.0f;
    if (pageHeightF < 1.0f) pageHeightF = 1.0f;
    float pageHeightPoints = pageHeightF;

    // 3. 计算渲染缩放比例
    // 与旧 MuPDF/WinRT 逻辑一致：使用 max 覆盖视口长边
    const float safeViewportWidth = static_cast<float>(std::max(viewportWidth, 64));
    const float safeViewportHeight = static_cast<float>(std::max(viewportHeight, 64));
    const float fitScale = std::max(safeViewportWidth / pageWidthPoints,
                                    safeViewportHeight / pageHeightPoints);
    // [高清关键] zoom 直接乘到渲染比例上，放大时以更高分辨率重新光栅化
    const float requestedScale = std::clamp(fitScale * std::max(zoom, 0.05f), 0.05f, 32.0f);
    // 限制最大边长到 16384px（D2D 纹理限制）
    const float maxDimensionScale = 16384.0f / std::max(pageWidthPoints, pageHeightPoints);
    const float renderScale = std::min(requestedScale, std::max(maxDimensionScale, 0.05f));

    const int destWidth = static_cast<int>(std::max(1.0f, std::round(pageWidthPoints * renderScale)));
    const int destHeight = static_cast<int>(std::max(1.0f, std::round(pageHeightPoints * renderScale)));

    // 4. 创建目标位图（BGRA, premultiplied）
    //    使用 FPDFBitmap_CreateEx + 外部缓冲区，避免二次拷贝
    const int stride = CalculateAlignedStride(destWidth, 4, 4);
    const size_t bufferSize = static_cast<size_t>(stride) * static_cast<size_t>(destHeight);
    auto* pixels = static_cast<uint8_t*>(std::malloc(bufferSize));
    if (!pixels) {
        FPDF_ClosePage(page);
        result.status = E_OUTOFMEMORY;
        result.errorMessage = L"像素缓冲区分配失败";
        return E_OUTOFMEMORY;
    }

    // 5. 填充白色背景（PDF 页面默认白底）
    //    FPDFBitmap_FillRect 使用 0xAABBGGRR 格式
    //    白色不透明 = 0xFFFFFFFF
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(
        destWidth, destHeight,
        FPDFBitmap_BGRA,  // 4 bytes/pixel, BGRA, 非预乘
        pixels, stride);
    if (!bitmap) {
        std::free(pixels);
        FPDF_ClosePage(page);
        result.status = E_FAIL;
        result.errorMessage = L"FPDFBitmap_CreateEx 失败";
        return E_FAIL;
    }

    // 填充白色不透明背景
    FPDFBitmap_FillRect(bitmap, 0, 0, destWidth, destHeight, 0xFFFFFFFF);

    // 6. 渲染页面到位图
    //    flags: FPDF_ANNOT 渲染注释，FPDF_LCD_TEXT 渲染 LCD 文字
    const int flags = FPDF_ANNOT | FPDF_LCD_TEXT;
    FPDF_RenderPageBitmap(bitmap, page,
                          0, 0,           // start_x, start_y
                          destWidth, destHeight,
                          0,               // rotate (0 = normal)
                          flags);

// PDFium 输出的是 straight (非预乘) BGRA。
// D2D 需要 premultiplied alpha，通过 SIMD 转换。
::ImageLoaderSimd::PremultiplyAlpha(pixels, destWidth, destHeight, stride);

    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);

    // 7. 构建 RawImageFrame
    auto frame = std::make_shared<RawImageFrame>();
    frame->pixels = pixels;
    frame->width = destWidth;
    frame->height = destHeight;
    frame->stride = stride;
    frame->format = PixelFormat::BGRA8888;
    frame->quality = DecodeQuality::Full;
    frame->formatDetails = L"PDFium 154.0.8021.0";
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
