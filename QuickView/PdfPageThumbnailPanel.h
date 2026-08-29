#pragma once
// ============================================================================
// PdfPageThumbnailPanel.h
// PDF/multi-page document thumbnail sidebar.
// Uses DocumentRenderController to async-render page thumbnails.
// Visually distinct from ImageListThumbnailPanel: blue accent, "页面" title.
// ============================================================================

#include "ThumbnailPanelBase.h"
#include "PagedDocument.h"
#include "ImageTypes.h"
#include "DocumentRenderController.h"

#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace QuickView { class DocumentRenderController; }

class PdfPageThumbnailPanel : public ThumbnailPanelBase {
public:
    PdfPageThumbnailPanel() = default;
    ~PdfPageThumbnailPanel() override;

    // Lifecycle
    void InitializeEx(HWND hwnd, QuickView::DocumentRenderController* controller);

    // Document lifecycle
    void OnDocumentOpened(const std::wstring& path, uint32_t totalPages);
    void OnDocumentClosed();

    // Page tracking
    void SetCurrentPage(uint32_t page);

    // Process completed thumbnail results (call from main thread)
    void ProcessThumbnailResults();

    // Pending frames conversion (call during render)
    void ConvertPendingFrames(ID2D1RenderTarget* pRT);

    // Pending frame storage (for main thread render conversion)
    std::unordered_map<uint32_t, std::shared_ptr<QuickView::RawImageFrame>> m_pendingFrames;

protected:
    // ThumbnailPanelBase interface
    void OnUpdateThumbnailRequests() override;
    void OnProcessThumbnailResults() override { ProcessThumbnailResults(); }
    bool OnIsLoading() const override;
    void DrawItems(ID2D1RenderTarget* pRT) override;
    int OnItemClick(int index) override;
    void OnDeviceResourcesCreated() override;
    void OnDeviceResourcesDiscarded() override;
    void OnLayoutChanged() override;

    uint32_t GetItemCount() const override { return m_totalPages; }
    ComPtr<ID2D1Bitmap> GetItemBitmap(uint32_t index) override;
    std::wstring GetItemLabel(uint32_t index) const override;
    const wchar_t* GetPanelTitle() const override { return L"页面"; }
    D2D1::ColorF GetAccentColor() const override { return D2D1::ColorF(0.23f, 0.51f, 0.96f); } // Blue
    float GetCellAspect() const override { return 1.0f / 1.4142f; } // A4 portrait (default)

private:
    struct ThumbnailSlot {
        ComPtr<ID2D1Bitmap> bitmap;
        uint32_t pageIndex = 0;
        bool needsRender = true;
        bool isRendering = false;
    };

    ComPtr<ID2D1Bitmap> CreateBitmapFromFrame(ID2D1RenderTarget* pRT, const QuickView::RawImageFrame& frame);

    QuickView::DocumentRenderController* m_controller = nullptr;
    std::vector<ThumbnailSlot> m_slots;
    uint64_t m_generation = 0;
};
