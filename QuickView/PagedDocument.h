#pragma once

#include "ImageTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace QuickView {

struct PagedDocumentState {
    bool active = false;
    bool isVector = false;  // true = CDR/CMX (SVG cache), false = PDF/AI (PDFium async)
    uint32_t currentPage = 0;
    uint32_t totalPages = 0;
    float pageWidthPoints = 0.0f;
    float pageHeightPoints = 0.0f;
    float renderScale = 1.0f;
    uint64_t requestId = 0;

    [[nodiscard]] bool TryMovePage(int direction) noexcept {
        if (!active || totalPages == 0 || direction == 0) return false;
        if (direction < 0) {
            if (currentPage == 0) return false;
            --currentPage;
            return true;
        }
        if (currentPage + 1 >= totalPages) return false;
        ++currentPage;
        return true;
    }

    void Reset() noexcept {
        active = false;
        isVector = false;
        currentPage = 0;
        totalPages = 0;
        pageWidthPoints = 0.0f;
        pageHeightPoints = 0.0f;
        renderScale = 1.0f;
        requestId = 0;
    }
};

struct DocumentRenderRequest {
    uint64_t requestId = 0;
    HWND notifyWindow = nullptr;
    std::wstring path;
    uint32_t pageIndex = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    float zoom = 1.0f;
};

struct DocumentRenderResult {
    uint64_t requestId = 0;
    std::wstring path;
    uint32_t pageIndex = 0;
    uint32_t pageCount = 0;
    float pageWidthPoints = 0.0f;
    float pageHeightPoints = 0.0f;
    float renderScale = 1.0f;
    HRESULT status = E_FAIL;
    std::wstring errorMessage;
    std::shared_ptr<RawImageFrame> frame;
};

} // namespace QuickView
