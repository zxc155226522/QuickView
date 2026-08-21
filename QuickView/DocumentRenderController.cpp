#include "pch.h"
#include "DocumentRenderController.h"

#include <mupdf/fitz.h>

namespace QuickView {

// [WinRT PDF] Check if Windows.Data.Pdf.dll is available on this system.
// On Windows Server / LTSC / stripped images the DLL may be absent.
// We use a lightweight LoadLibraryEx probe (LOAD_LIBRARY_AS_IMAGE_RESOURCE
// to avoid executing any code) before creating WinRtPdfDocument.
static bool IsWinRtPdfAvailable() noexcept {
    // Both DLLs must be present for WinRT PDF rendering to work.
    // LOAD_LIBRARY_AS_IMAGE_RESOURCE avoids executing any DLL code.
    HMODULE h1 = LoadLibraryExW(L"Windows.Data.Pdf.dll", nullptr,
                                LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h1) return false;
    FreeLibrary(h1);

    HMODULE h2 = LoadLibraryExW(L"runtimeobject.dll", nullptr,
                                LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h2) return false;
    FreeLibrary(h2);
    return true;
}

// [PDF Sidebar] Helper: compute a stable hash from path for cache invalidation
static uint64_t HashPath(const std::wstring& path) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : path) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

DocumentRenderController::DocumentRenderController() {
    m_context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!m_context) return;

    fz_try(m_context) {
        fz_register_document_handlers(m_context);
    }
    fz_catch(m_context) {
        fz_drop_context(m_context);
        m_context = nullptr;
        return;
    }

    m_document = std::make_unique<MuPdfDocument>(m_context);
    if (IsWinRtPdfAvailable()) {
        m_winRtDoc = std::make_unique<WinRtPdfDocument>();  // [WinRT PDF] Native engine
    }
    m_available = true;
    m_worker = std::thread(&DocumentRenderController::WorkerMain, this);
}

DocumentRenderController::~DocumentRenderController() {
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
        m_pendingRequest.reset();
    }
    m_condition.notify_one();
    if (m_worker.joinable()) m_worker.join();
    m_winRtDoc.reset();
    m_document.reset();
    if (m_context) fz_drop_context(m_context);
    m_context = nullptr;
}

uint64_t DocumentRenderController::Request(DocumentRenderRequest request) {
    if (!m_available || request.path.empty()) return 0;

    uint64_t requestId = 0;
    {
        std::scoped_lock lock(m_mutex);
        requestId = ++m_nextRequestId;
        request.requestId = requestId;
        m_pendingRequest = std::move(request);
        // Mark current full document path for thumbnail invalidation
        m_currentFullDocPathHash = HashPath(request.path);
    }
    m_condition.notify_one();
    return requestId;
}

void DocumentRenderController::Cancel() noexcept {
    std::scoped_lock lock(m_mutex);
    ++m_nextRequestId;
    m_pendingRequest.reset();
    m_latestResult.reset();
}

bool DocumentRenderController::TakeLatestResult(DocumentRenderResult& result) {
    std::scoped_lock lock(m_mutex);
    if (!m_latestResult) return false;
    result = std::move(*m_latestResult);
    m_latestResult.reset();
    return true;
}

// [PDF Sidebar] Request a thumbnail render for the left sidebar panel
uint64_t DocumentRenderController::RequestThumbnail(ThumbnailRequest request) {
    if (!m_available || request.path.empty()) return 0;

    uint64_t thumbId = 0;
    {
        std::scoped_lock lock(m_mutex);
        thumbId = ++m_nextThumbId;
        request.requestId = thumbId;

        ThumbnailQueueEntry entry;
        entry.request = std::move(request);
        entry.orderBy = entry.request.priority;
        m_thumbQueue.push(std::move(entry));
    }
    m_condition.notify_one();
    return thumbId;
}

void DocumentRenderController::CancelThumbnails() noexcept {
    std::scoped_lock lock(m_mutex);
    m_thumbQueue = {};
}

bool DocumentRenderController::TakeThumbnailResult(ThumbnailResult& result) {
    std::scoped_lock lock(m_mutex);
    if (m_thumbResults.empty()) return false;
    result = std::move(m_thumbResults.front());
    m_thumbResults.erase(m_thumbResults.begin());
    return true;
}

void DocumentRenderController::ClearThumbnailCache() noexcept {
    std::scoped_lock lock(m_mutex);
    m_thumbResults.clear();
}

HRESULT DocumentRenderController::EnsureDocument(const std::wstring& path,
                                                  std::wstring& errorMessage) noexcept {
    if (!m_document) return E_UNEXPECTED;

    // [WinRT PDF] Try native engine first (browser-quality vector rendering)
    if (m_winRtDoc) {
        if (m_winRtDoc->IsOpen() && m_winRtDoc->Path() == path) return S_OK;
        HRESULT hr = m_winRtDoc->Open(path, errorMessage);
        if (SUCCEEDED(hr)) return S_OK;
        // Fall back to MuPDF if native engine fails
    }

    // [Fallback] MuPDF engine
    if (m_document->IsOpen() && m_document->Path() == path) return S_OK;
    return m_document->Open(path, errorMessage);
}

// [PDF Sidebar] Render a single thumbnail page and produce a ThumbnailResult
void DocumentRenderController::ProcessThumbnailRequest(const ThumbnailRequest& request,
                                                        ThumbnailResult& result) noexcept {
    result.requestId = request.requestId;
    result.pageIndex = request.pageIndex;
    result.status = E_FAIL;

    std::wstring errorMessage;
    HRESULT hr = EnsureDocument(request.path, errorMessage);
    if (FAILED(hr)) {
        result.status = hr;
        return;
    }

    DocumentRenderResult fullResult;
    int w = (std::max)(request.targetWidth, 64);
    int h = (std::max)(request.targetHeight, 64);

    if (m_winRtDoc && m_winRtDoc->IsOpen()) {
        hr = m_winRtDoc->RenderPage(request.pageIndex, w, h, 1.0f, fullResult);
        if (FAILED(hr)) {
            // Fallback to MuPDF
            fullResult = {};
            hr = m_document->RenderPage(request.pageIndex, w, h, 1.0f, fullResult);
        }
    } else {
        hr = m_document->RenderPage(request.pageIndex, w, h, 1.0f, fullResult);
    }

    if (SUCCEEDED(hr) && fullResult.frame && fullResult.frame->IsValid()) {
        // Deep copy the frame pixels for safe hand-off to the main thread
        auto copied = std::make_shared<RawImageFrame>();
        if (fullResult.frame->IsSvg()) {
            // Should not happen for PDF thumbnails
            result.status = E_FAIL;
            return;
        }
        size_t bufSize = fullResult.frame->GetBufferSize();
        if (bufSize == 0) {
            result.status = E_FAIL;
            return;
        }
        uint8_t* heapPixels = new uint8_t[bufSize];
        std::memcpy(heapPixels, fullResult.frame->pixels, bufSize);
        copied->pixels = heapPixels;
        copied->width = fullResult.frame->width;
        copied->height = fullResult.frame->height;
        copied->stride = fullResult.frame->stride;
        copied->format = fullResult.frame->format;
        copied->formatDetails = fullResult.frame->formatDetails;
        copied->quality = DecodeQuality::Preview;
        copied->memoryDeleter = MemoryDeleter::FromDeleteArray();
        std::memcpy(copied->colorInfo.hasEmbeddedIcc ? &copied->colorInfo : &fullResult.frame->colorInfo,
                    &fullResult.frame->colorInfo, sizeof(fullResult.frame->colorInfo));
        result.frame = std::move(copied);
        result.status = S_OK;
    } else {
        result.status = hr;
    }
}

void DocumentRenderController::WorkerMain() noexcept {
    while (true) {
        enum class TaskType { None, FullRender, Thumbnail };
        TaskType taskType = TaskType::None;
        DocumentRenderRequest fullRequest;
        ThumbnailRequest thumbRequest;

        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] {
                return m_stopping || m_pendingRequest.has_value() || !m_thumbQueue.empty();
            });
            if (m_stopping) return;

            // Priority: full-size render requests first
            if (m_pendingRequest.has_value()) {
                taskType = TaskType::FullRender;
                fullRequest = std::move(*m_pendingRequest);
                m_pendingRequest.reset();
            } else if (!m_thumbQueue.empty()) {
                taskType = TaskType::Thumbnail;
                thumbRequest = std::move(const_cast<ThumbnailRequest&>(m_thumbQueue.top().request));
                m_thumbQueue.pop();
            }
        }

        if (taskType == TaskType::FullRender) {
            DocumentRenderResult result;
            result.requestId = fullRequest.requestId;
            result.path = fullRequest.path;
            result.pageIndex = fullRequest.pageIndex;

            std::wstring errorMessage;
            HRESULT hr = EnsureDocument(fullRequest.path, errorMessage);
            if (SUCCEEDED(hr)) {
                // [WinRT PDF] Prefer native engine; fall back to MuPDF
                bool rendered = false;
                if (m_winRtDoc && m_winRtDoc->IsOpen()) {
                    hr = m_winRtDoc->RenderPage(fullRequest.pageIndex,
                                                fullRequest.viewportWidth,
                                                fullRequest.viewportHeight,
                                                fullRequest.zoom,
                                                result);
                    if (SUCCEEDED(hr)) {
                        rendered = true;
                    } else {
                        // Native render failed, try MuPDF
                        std::wstring mupdfError;
                        HRESULT mupdfHr = m_document->Open(fullRequest.path, mupdfError);
                        if (SUCCEEDED(mupdfHr)) {
                            hr = m_document->RenderPage(fullRequest.pageIndex,
                                                        fullRequest.viewportWidth,
                                                        fullRequest.viewportHeight,
                                                        fullRequest.zoom,
                                                        result);
                            rendered = SUCCEEDED(hr);
                        }
                    }
                }
                if (!rendered) {
                    hr = m_document->RenderPage(fullRequest.pageIndex,
                                                fullRequest.viewportWidth,
                                                fullRequest.viewportHeight,
                                                fullRequest.zoom,
                                                result);
                }
            } else {
                result.status = hr;
                result.errorMessage = std::move(errorMessage);
            }
            result.requestId = fullRequest.requestId;
            result.path = fullRequest.path;
            result.pageIndex = fullRequest.pageIndex;

            bool publish = false;
            {
                std::scoped_lock lock(m_mutex);
                publish = !m_stopping && fullRequest.requestId == m_nextRequestId;
                if (publish) m_latestResult = std::move(result);
            }
            if (publish && fullRequest.notifyWindow) {
                PostMessageW(fullRequest.notifyWindow, ResultMessage, 0, 0);
            }
        } else if (taskType == TaskType::Thumbnail) {
            ThumbnailResult thumbResult;
            ProcessThumbnailRequest(thumbRequest, thumbResult);

            {
                std::scoped_lock lock(m_mutex);
                if (!m_stopping) {
                    m_thumbResults.push_back(std::move(thumbResult));
                }
            }
            if (thumbRequest.notifyWindow) {
                PostMessageW(thumbRequest.notifyWindow, ThumbnailResultMessage, 0, 0);
            }
        }
    }
}

} // namespace QuickView
