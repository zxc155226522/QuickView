#include "pch.h"
#include "DocumentRenderController.h"

#include <mupdf/fitz.h>
#include <libloader.h>  // LoadLibraryExW

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

void DocumentRenderController::WorkerMain() noexcept {
    while (true) {
        DocumentRenderRequest request;
        {
            std::unique_lock lock(m_mutex);
            m_condition.wait(lock, [this] { return m_stopping || m_pendingRequest.has_value(); });
            if (m_stopping) return;
            request = std::move(*m_pendingRequest);
            m_pendingRequest.reset();
        }

        DocumentRenderResult result;
        result.requestId = request.requestId;
        result.path = request.path;
        result.pageIndex = request.pageIndex;

        std::wstring errorMessage;
        HRESULT hr = EnsureDocument(request.path, errorMessage);
        if (SUCCEEDED(hr)) {
            // [WinRT PDF] Prefer native engine; fall back to MuPDF
            bool rendered = false;
            if (m_winRtDoc && m_winRtDoc->IsOpen()) {
                hr = m_winRtDoc->RenderPage(request.pageIndex,
                                            request.viewportWidth,
                                            request.viewportHeight,
                                            request.zoom,
                                            result);
                if (SUCCEEDED(hr)) {
                    rendered = true;
                } else {
                    // Native render failed, try MuPDF
                    std::wstring mupdfError;
                    HRESULT mupdfHr = m_document->Open(request.path, mupdfError);
                    if (SUCCEEDED(mupdfHr)) {
                        hr = m_document->RenderPage(request.pageIndex,
                                                    request.viewportWidth,
                                                    request.viewportHeight,
                                                    request.zoom,
                                                    result);
                        rendered = SUCCEEDED(hr);
                    }
                }
            }
            if (!rendered) {
                hr = m_document->RenderPage(request.pageIndex,
                                            request.viewportWidth,
                                            request.viewportHeight,
                                            request.zoom,
                                            result);
            }
        } else {
            result.status = hr;
            result.errorMessage = std::move(errorMessage);
        }
        result.requestId = request.requestId;
        result.path = request.path;
        result.pageIndex = request.pageIndex;

        bool publish = false;
        {
            std::scoped_lock lock(m_mutex);
            publish = !m_stopping && request.requestId == m_nextRequestId;
            if (publish) m_latestResult = std::move(result);
        }
        if (publish && request.notifyWindow) {
            PostMessageW(request.notifyWindow, ResultMessage, 0, 0);
        }
    }
}

} // namespace QuickView
