#include "pch.h"
#include "MuPdfDocument.h"

#include <mupdf/fitz.h>

#include "ImageLoaderSimd.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace QuickView {
namespace {

std::wstring Utf8ToWide(const char* text) {
    if (!text || !*text) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), count);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

bool HasPdfHeader(const MappedFile& file) noexcept {
    static constexpr uint8_t kPdfHeader[] = {'%', 'P', 'D', 'F', '-'};
    return file.size() >= sizeof(kPdfHeader) &&
           std::memcmp(file.data(), kPdfHeader, sizeof(kPdfHeader)) == 0;
}

} // namespace

MuPdfDocument::MuPdfDocument(fz_context* context) noexcept : m_context(context) {}

MuPdfDocument::~MuPdfDocument() {
    Close();
}

HRESULT MuPdfDocument::Open(const std::wstring& path, std::wstring& errorMessage) noexcept {
    Close();
    if (!m_context || path.empty()) return E_INVALIDARG;

    auto file = std::make_unique<MappedFile>(path);
    if (!file->IsValid()) {
        errorMessage = L"无法读取文档文件。";
        return HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
    }
    if (!HasPdfHeader(*file)) {
        errorMessage = L"该 AI 文件未包含 PDF-compatible 数据。请在 Illustrator 中启用 Create PDF Compatible File 后重新保存。";
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }

    fz_buffer* buffer = nullptr;
    fz_document* document = nullptr;
    uint32_t pageCount = 0;

    fz_try(m_context) {
        buffer = fz_new_buffer_from_shared_data(m_context, file->data(), file->size());
        document = fz_open_document_with_buffer(m_context, "application/pdf", buffer);
        const int count = fz_count_pages(m_context, document);
        if (count <= 0) {
            fz_throw(m_context, FZ_ERROR_FORMAT, "document contains no pages");
        }
        pageCount = static_cast<uint32_t>(count);
    }
    fz_catch(m_context) {
        errorMessage = Utf8ToWide(fz_caught_message(m_context));
        if (document) fz_drop_document(m_context, document);
        if (buffer) fz_drop_buffer(m_context, buffer);
        return HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
    }

    m_file = std::move(file);
    m_buffer = buffer;
    m_document = document;
    m_path = path;
    m_pageCount = pageCount;
    return S_OK;
}

void MuPdfDocument::Close() noexcept {
    if (m_context) {
        if (m_displayList) fz_drop_display_list(m_context, m_displayList);
        if (m_document) fz_drop_document(m_context, m_document);
        if (m_buffer) fz_drop_buffer(m_context, m_buffer);
    }
    m_displayList = nullptr;
    m_document = nullptr;
    m_buffer = nullptr;
    m_file.reset();
    m_path.clear();
    m_pageCount = 0;
    m_displayListPage = UINT32_MAX;
    m_pageWidthPoints = 0.0f;
    m_pageHeightPoints = 0.0f;
}

HRESULT MuPdfDocument::EnsureDisplayList(uint32_t pageIndex,
                                         float& pageWidthPoints,
                                         float& pageHeightPoints,
                                         std::wstring& errorMessage) noexcept {
    if (!m_document || pageIndex >= m_pageCount) return E_INVALIDARG;

    if (m_displayList && m_displayListPage == pageIndex) {
        pageWidthPoints = m_pageWidthPoints;
        pageHeightPoints = m_pageHeightPoints;
        return S_OK;
    }

    fz_page* page = nullptr;
    fz_display_list* displayList = nullptr;
    fz_rect bounds = fz_empty_rect;

    fz_try(m_context) {
        page = fz_load_page(m_context, m_document, static_cast<int>(pageIndex));
        bounds = fz_bound_page(m_context, page);
        displayList = fz_new_display_list_from_page(m_context, page);
    }
    fz_always(m_context) {
        if (page) fz_drop_page(m_context, page);
    }
    fz_catch(m_context) {
        errorMessage = Utf8ToWide(fz_caught_message(m_context));
        if (displayList) fz_drop_display_list(m_context, displayList);
        return E_FAIL;
    }

    if (m_displayList) fz_drop_display_list(m_context, m_displayList);
    m_displayList = displayList;
    m_displayListPage = pageIndex;
    m_pageWidthPoints = std::max(1.0f, bounds.x1 - bounds.x0);
    m_pageHeightPoints = std::max(1.0f, bounds.y1 - bounds.y0);
    pageWidthPoints = m_pageWidthPoints;
    pageHeightPoints = m_pageHeightPoints;
    return S_OK;
}

HRESULT MuPdfDocument::RenderPage(uint32_t pageIndex,
                                  int viewportWidth,
                                  int viewportHeight,
                                  float zoom,
                                  DocumentRenderResult& result) noexcept {
    result = {};
    result.path = m_path;
    result.pageIndex = pageIndex;
    result.pageCount = m_pageCount;

    std::wstring errorMessage;
    float pageWidthPoints = 0.0f;
    float pageHeightPoints = 0.0f;
    HRESULT hr = EnsureDisplayList(pageIndex, pageWidthPoints, pageHeightPoints, errorMessage);
    if (FAILED(hr)) {
        result.status = hr;
        result.errorMessage = std::move(errorMessage);
        return hr;
    }

    const float safeViewportWidth = static_cast<float>(std::max(viewportWidth, 64));
    const float safeViewportHeight = static_cast<float>(std::max(viewportHeight, 64));
    const float fitScale = std::min(safeViewportWidth / pageWidthPoints,
                                    safeViewportHeight / pageHeightPoints);
    const float requestedScale = std::clamp(fitScale * std::max(zoom, 0.05f), 0.05f, 16.0f);
    const float maxDimensionScale = 16384.0f / std::max(pageWidthPoints, pageHeightPoints);
    const float renderScale = std::min(requestedScale, std::max(maxDimensionScale, 0.05f));

    fz_pixmap* pixmap = nullptr;
    fz_try(m_context) {
        pixmap = fz_new_pixmap_from_display_list(
            m_context,
            m_displayList,
            fz_scale(renderScale, renderScale),
            fz_device_bgr(m_context),
            1);
        // [Fix] Fill with opaque white so PDF/AI thumbnails have a solid
        // background instead of transparent (which causes fragmented appearance).
        fz_clear_pixmap_with_value(m_context, pixmap, 0xFF);
    }
    fz_catch(m_context) {
        result.status = E_FAIL;
        result.errorMessage = Utf8ToWide(fz_caught_message(m_context));
        return result.status;
    }

    const int width = pixmap->w;
    const int height = pixmap->h;
    const int components = fz_pixmap_components(m_context, pixmap);
    const int sourceStride = fz_pixmap_stride(m_context, pixmap);
    const uint8_t* samples = fz_pixmap_samples(m_context, pixmap);
    if (width <= 0 || height <= 0 || components != 4 || sourceStride < width * 4 || !samples) {
        fz_drop_pixmap(m_context, pixmap);
        result.status = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
        result.errorMessage = L"MuPDF 返回了不受支持的页面像素格式。";
        return result.status;
    }

    const int stride = CalculateAlignedStride(width, 4, 4);
    const size_t bufferSize = static_cast<size_t>(stride) * static_cast<size_t>(height);
    auto* pixels = static_cast<uint8_t*>(std::malloc(bufferSize));
    if (!pixels) {
        fz_drop_pixmap(m_context, pixmap);
        result.status = E_OUTOFMEMORY;
        result.errorMessage = L"页面像素缓冲区分配失败。";
        return result.status;
    }

    // MuPDF outputs straight BGRA with alpha. D2D expects premultiplied alpha
    // (D2D1_ALPHA_MODE_PREMULTIPLIED). Copy as-is, then premultiply via SIMD.
    for (int y = 0; y < height; ++y) {
        std::memcpy(pixels + static_cast<size_t>(y) * stride,
                    samples + static_cast<size_t>(y) * sourceStride,
                    static_cast<size_t>(width) * 4);
    }
    ImageLoaderSimd::PremultiplyAlpha(pixels, width, height, stride);
    fz_drop_pixmap(m_context, pixmap);

    auto frame = std::make_shared<RawImageFrame>();
    frame->pixels = pixels;
    frame->width = width;
    frame->height = height;
    frame->stride = stride;
    frame->format = PixelFormat::BGRA8888;
    frame->quality = DecodeQuality::Full;
    frame->formatDetails = L"MuPDF 1.27.2";
    frame->dpiX = 72.0 * renderScale;
    frame->dpiY = 72.0 * renderScale;
    frame->memoryDeleter = MemoryDeleter::FromFree();

    result.pageWidthPoints = pageWidthPoints;
    result.pageHeightPoints = pageHeightPoints;
    result.renderScale = renderScale;
    result.frame = std::move(frame);
    result.status = S_OK;
    return S_OK;
}

} // namespace QuickView
