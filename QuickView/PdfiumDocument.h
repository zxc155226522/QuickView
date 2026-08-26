#pragma once

// PdfiumDocument: PDFium 引擎适配层
// 使用 PDFium (Google BSD) 替代 MuPDF + WinRT PDF。
// PDFium 是 Edge/Chrome 浏览器内置的 PDF 引擎，渲染质量最高。
//
// 设计要点:
// - PDFium API 不是线程安全的，所有调用必须在同一线程或加锁。
// - 使用 FPDFBitmap_CreateEx + 外部缓冲区，避免额外拷贝。
// - 渲染输出 BGRA8888 premultiplied，与 D2D 管线直接兼容。
// - 支持 PDF 和 PDF-compatible AI 文件。

#include "PagedDocument.h"
#include "ImageTypes.h"
#include "MappedFile.h"

#include <cstdint>
#include <memory>
#include <string>

// PDFium C API 前向声明（避免在头文件中暴露 PDFium 头文件）
typedef struct fpdf_document_t__* FPDF_DOCUMENT;
typedef struct fpdf_page_t__* FPDF_PAGE;

namespace QuickView {

class PdfiumDocument {
public:
    PdfiumDocument() noexcept;
    ~PdfiumDocument();

    PdfiumDocument(const PdfiumDocument&) = delete;
    PdfiumDocument& operator=(const PdfiumDocument&) = delete;

    // 打开 PDF/AI 文件
    HRESULT Open(const std::wstring& path, std::wstring& errorMessage) noexcept;

    // 从内存缓冲区打开文档（用于 CDR/CMX SVG → PDFium 管线，如果将来需要）
    // HRESULT OpenFromBuffer(const uint8_t* data, size_t size,
    //                        const char* mimeType, const std::wstring& sourcePath,
    //                        std::wstring& errorMessage) noexcept;

    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return m_document != nullptr; }
    [[nodiscard]] const std::wstring& Path() const noexcept { return m_path; }
    [[nodiscard]] uint32_t PageCount() const noexcept { return m_pageCount; }

    // 渲染指定页面到 BGRA 像素缓冲区
    // viewportWidth/Height: 目标视口尺寸（物理像素），用于计算渲染分辨率
    // zoom: 缩放因子（放大时提高渲染分辨率 → 高清）
    HRESULT RenderPage(uint32_t pageIndex,
                       int viewportWidth,
                       int viewportHeight,
                       float zoom,
                       DocumentRenderResult& result) noexcept;

private:
    std::unique_ptr<MappedFile> m_file;
    FPDF_DOCUMENT m_document = nullptr;
    std::wstring m_path;
    uint32_t m_pageCount = 0;

    // PDFium 全局库引用计数（PDFium 要求 FPDF_InitLibrary/FPDF_DestroyLibrary 配对）
    static int s_refCount;
    static void EnsureLibraryInit() noexcept;
};

} // namespace QuickView
