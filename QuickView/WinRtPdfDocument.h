#pragma once

// WinRtPdfDocument: Windows 原生 PDF 渲染引擎
// 使用 Windows.Data.Pdf (WinRT ABI + WRL) + IPdfRendererNative
// 直接将 PDF 矢量内容渲染到 D2D 设备上下文，达到浏览器级清晰度。
//
// 此文件单独启用 /EHsc（异常），因为 WRL::Make<>() 和回调需要 C++ 异常。
// 其余项目文件保持 /EHs-c- 不变。

#include "PagedDocument.h"
#include "ImageTypes.h"
#include "MappedFile.h"

#include <cstdint>
#include <memory>
#include <string>

namespace QuickView {

class WinRtPdfDocument {
public:
    WinRtPdfDocument() noexcept;
    ~WinRtPdfDocument();

    WinRtPdfDocument(const WinRtPdfDocument&) = delete;
    WinRtPdfDocument& operator=(const WinRtPdfDocument&) = delete;

    // 打开 PDF/AI 文件并加载为 Windows.Data.Pdf.PdfDocument
    HRESULT Open(const std::wstring& path, std::wstring& errorMessage) noexcept;

    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return m_pdfDoc != nullptr; }
    [[nodiscard]] const std::wstring& Path() const noexcept { return m_path; }
    [[nodiscard]] uint32_t PageCount() const noexcept { return m_pageCount; }

    // 渲染指定页面到 BGRA 像素缓冲区
    // viewportWidth/Height: 目标视口尺寸（物理像素），用于计算渲染分辨率
    // zoom: 缩放因子
    HRESULT RenderPage(uint32_t pageIndex,
                       int viewportWidth,
                       int viewportHeight,
                       float zoom,
                       DocumentRenderResult& result) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    void* m_pdfDoc = nullptr;       // ABI::Windows::Data::Pdf::IPdfDocument*
    std::wstring m_path;
    uint32_t m_pageCount = 0;
};

} // namespace QuickView
