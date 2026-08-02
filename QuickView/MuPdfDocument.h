#pragma once

#include "MappedFile.h"
#include "PagedDocument.h"

#include <mupdf/fitz.h>

#include <memory>
#include <string>

namespace QuickView {

class MuPdfDocument {
public:
    explicit MuPdfDocument(fz_context* context) noexcept;
    ~MuPdfDocument();

    MuPdfDocument(const MuPdfDocument&) = delete;
    MuPdfDocument& operator=(const MuPdfDocument&) = delete;

    HRESULT Open(const std::wstring& path, std::wstring& errorMessage) noexcept;
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return m_document != nullptr; }
    [[nodiscard]] const std::wstring& Path() const noexcept { return m_path; }
    [[nodiscard]] uint32_t PageCount() const noexcept { return m_pageCount; }

    HRESULT RenderPage(uint32_t pageIndex,
                       int viewportWidth,
                       int viewportHeight,
                       float zoom,
                       DocumentRenderResult& result) noexcept;

private:
    HRESULT EnsureDisplayList(uint32_t pageIndex,
                              float& pageWidthPoints,
                              float& pageHeightPoints,
                              std::wstring& errorMessage) noexcept;

    fz_context* m_context = nullptr;
    std::unique_ptr<MappedFile> m_file;
    fz_buffer* m_buffer = nullptr;
    fz_document* m_document = nullptr;
    fz_display_list* m_displayList = nullptr;
    std::wstring m_path;
    uint32_t m_pageCount = 0;
    uint32_t m_displayListPage = UINT32_MAX;
    float m_pageWidthPoints = 0.0f;
    float m_pageHeightPoints = 0.0f;
};

} // namespace QuickView
