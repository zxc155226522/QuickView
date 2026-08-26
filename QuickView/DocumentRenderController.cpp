#include "pch.h"
#include "DocumentRenderController.h"

#include <objbase.h>     // CoInitializeEx/CoUninitialize (for thumbnail shell)

namespace QuickView {

// Helper: compute a stable hash from path for cache invalidation
static uint64_t HashPath(const std::wstring& path) noexcept {
    uint64_t h = 14695981039346656037ull;
    for (wchar_t c : path) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

DocumentRenderController::DocumentRenderController() {
    // [PDFium] Single engine — replaces WinRT PDF + MuPDF
    m_pdfiumDoc = std::make_unique<PdfiumDocument>();

    m_available = (m_pdfiumDoc != nullptr);
    if (m_available) {
        m_worker = std::thread(&DocumentRenderController::WorkerMain, this);
    }
    OutputDebugStringW(L"[PDFium] DocumentRenderController constructed!\n");
}

DocumentRenderController::~DocumentRenderController() {
    {
        std::scoped_lock lock(m_mutex);
        m_stopping = true;
        m_pendingRequest.reset();
    }
    m_condition.notify_one();
    if (m_worker.joinable()) m_worker.join();
    m_pdfiumDoc.reset();
}

uint64_t DocumentRenderController::Request(DocumentRenderRequest request) {
    if (!m_available || request.path.empty()) return 0;

    uint64_t requestId = 0;
    {
        std::scoped_lock lock(m_mutex);
        requestId = ++m_nextRequestId;
        request.requestId = requestId;
        m_pendingRequest = std::move(request);
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
    if (!m_pdfiumDoc) return E_UNEXPECTED;

    if (m_pdfiumDoc->IsOpen() && m_pdfiumDoc->Path() == path) return S_OK;
    return m_pdfiumDoc->Open(path, errorMessage);
}

// [PDF Sidebar] Render a single thumbnail page
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

    hr = m_pdfiumDoc->RenderPage(request.pageIndex, w, h, 1.0f, fullResult);

    if (SUCCEEDED(hr) && fullResult.frame && fullResult.frame->IsValid()) {
        auto copied = std::make_shared<RawImageFrame>();
        if (fullResult.frame->IsSvg()) {
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
    // COM init for shell interactions (thumbnail provider, etc.)
    HRESULT coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

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
            if (m_stopping) break;

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
                // [PDFium] Single engine — no fallback needed
                hr = m_pdfiumDoc->RenderPage(fullRequest.pageIndex,
                                             fullRequest.viewportWidth,
                                             fullRequest.viewportHeight,
                                             fullRequest.zoom,
                                             result);
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

    if (SUCCEEDED(coInitHr)) CoUninitialize();
}

} // namespace QuickView
