#pragma once

#include "MuPdfDocument.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace QuickView {

class DocumentRenderController {
public:
    static constexpr UINT ResultMessage = WM_APP + 24;

    DocumentRenderController();
    ~DocumentRenderController();

    DocumentRenderController(const DocumentRenderController&) = delete;
    DocumentRenderController& operator=(const DocumentRenderController&) = delete;

    [[nodiscard]] bool IsAvailable() const noexcept { return m_available; }
    uint64_t Request(DocumentRenderRequest request);
    void Cancel() noexcept;
    bool TakeLatestResult(DocumentRenderResult& result);

private:
    void WorkerMain() noexcept;
    HRESULT EnsureDocument(const std::wstring& path, std::wstring& errorMessage) noexcept;

    fz_context* m_context = nullptr;
    std::unique_ptr<MuPdfDocument> m_document;
    std::thread m_worker;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::optional<DocumentRenderRequest> m_pendingRequest;
    std::optional<DocumentRenderResult> m_latestResult;
    uint64_t m_nextRequestId = 0;
    bool m_stopping = false;
    bool m_available = false;
};

} // namespace QuickView
