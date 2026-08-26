#pragma once

#include "PdfiumDocument.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <vector>

namespace QuickView {

// [PDF Sidebar] Thumbnail request for left-side page panel
struct ThumbnailRequest {
    uint64_t requestId = 0;
    HWND notifyWindow = nullptr;
    std::wstring path;
    uint32_t pageIndex = 0;
    int targetWidth = 200;
    int targetHeight = 280;
    int priority = 0;  // Lower = higher priority (distance from current page)
};

struct ThumbnailResult {
    uint64_t requestId = 0;
    uint32_t pageIndex = 0;
    HRESULT status = E_FAIL;
    std::shared_ptr<RawImageFrame> frame;
};

class DocumentRenderController {
public:
    static constexpr UINT ResultMessage = WM_APP + 24;
    static constexpr UINT ThumbnailResultMessage = WM_APP + 25;

    DocumentRenderController();
    ~DocumentRenderController();

    DocumentRenderController(const DocumentRenderController&) = delete;
    DocumentRenderController& operator=(const DocumentRenderController&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept { return m_available; }
    uint64_t Request(DocumentRenderRequest request);
    void Cancel() noexcept;
    bool TakeLatestResult(DocumentRenderResult& result);

    // [PDF Sidebar] Thumbnail request API
    uint64_t RequestThumbnail(ThumbnailRequest request);
    void CancelThumbnails() noexcept;
    bool TakeThumbnailResult(ThumbnailResult& result);
    void ClearThumbnailCache() noexcept;

private:
    void WorkerMain() noexcept;
    HRESULT EnsureDocument(const std::wstring& path, std::wstring& errorMessage) noexcept;
    void ProcessThumbnailRequest(const ThumbnailRequest& request, ThumbnailResult& result) noexcept;

    std::unique_ptr<PdfiumDocument> m_pdfiumDoc;  // [PDFium] Edge/Chrome PDF engine
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::optional<DocumentRenderRequest> m_pendingRequest;
    std::optional<DocumentRenderResult> m_latestResult;
    uint64_t m_nextRequestId = 0;
    bool m_stopping = false;
    bool m_available = false;

    // [PDF Sidebar] Thumbnail queue
    struct ThumbnailQueueEntry {
        ThumbnailRequest request;
        int orderBy;  // For priority_queue ordering
        bool operator<(const ThumbnailQueueEntry& other) const {
            return orderBy > other.orderBy;  // min-heap by orderBy
        }
    };
    std::priority_queue<ThumbnailQueueEntry> m_thumbQueue;
    std::vector<ThumbnailResult> m_thumbResults;
    uint64_t m_nextThumbId = 0;
    std::optional<uint64_t> m_currentFullDocPathHash;
};

} // namespace QuickView
