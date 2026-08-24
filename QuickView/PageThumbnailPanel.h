#pragma once
// ============================================================================
// PageThumbnailPanel.h
// Side-embedded page thumbnail sidebar for multi-page documents and image lists.
// Non-floating: participates in the main layout, pushes the image viewport.
// Supports: left/right side, drag-to-resize, image-mode (folder thumbnails).
// ============================================================================

#include "PagedDocument.h"
#include "ImageTypes.h"
#include "DocumentRenderController.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

// [PDF Sidebar] Forward declare types in QuickView namespace
namespace QuickView {
    class DocumentRenderController;
}

class FileNavigator; // forward declare for image mode

class PageThumbnailPanel {
public:
    PageThumbnailPanel() = default;
    ~PageThumbnailPanel();

    PageThumbnailPanel(const PageThumbnailPanel&) = delete;
    PageThumbnailPanel& operator=(const PageThumbnailPanel&) = delete;

    // Lifecycle
    void Initialize(HWND hwnd, QuickView::DocumentRenderController* controller);

    // State control
    void Show(uint32_t totalPages, uint32_t currentPage);
    void Hide();
    [[nodiscard]] bool IsVisible() const { return m_visible; }

    // Page tracking
    [[nodiscard]] uint32_t GetCurrentPage() const { return m_currentPage; }
    void SetCurrentPage(uint32_t page);

    // Document lifecycle
    void OnDocumentOpened(const std::wstring& path, uint32_t totalPages);
    void OnDocumentClosed();

    // [Image Mode] Show folder image thumbnails
    void ShowImageThumbnails(FileNavigator* nav, int currentIndex, uint32_t totalFiles);
    void SetCurrentImageIndex(int index);
    [[nodiscard]] bool IsImageMode() const { return m_isImageMode; }

    // Panel side control (0=Right, 1=Left)
    void SetPanelSide(int side) { m_panelSide = side; }
    [[nodiscard]] int GetPanelSide() const { return m_panelSide; }

    // Layout
    [[nodiscard]] float GetWidth() const { return m_panelWidth; }
    [[nodiscard]] float GetPanelHeight() const { return m_panelHeight; }
    [[nodiscard]] D2D1_RECT_F GetPanelRect() const { return m_panelRect; }
    [[nodiscard]] float GetScrollY() const { return m_scrollY; }
    [[nodiscard]] float GetTargetScrollY() const { return m_targetScrollY; }
    [[nodiscard]] bool IsLoading() const;
    [[nodiscard]] bool ConsumeNeedsRepaint() { bool v = m_needsRepaint; m_needsRepaint = false; return v; }
    void UpdateLayout(const D2D1_RECT_F& clientRect);

    // Device resources
    void CreateDeviceResources(ID2D1RenderTarget* pRT);
    void DiscardDeviceResources();

    // Rendering
    void Render(ID2D1RenderTarget* pRT);

    // Interaction
    bool OnMouseMove(float x, float y);
    // Returns page index if a thumbnail was clicked, -1 otherwise
    // For image mode, returns the file index (non-negative) or -1
    int OnLButtonDown(float x, float y);
    bool OnMouseWheel(int delta);

    // [Resize] Drag-to-resize support
    bool IsResizeHit(float x, float y) const;
    void BeginResize(float x);
    void UpdateResize(float x, float windowWidth);
    void EndResize();
    [[nodiscard]] bool IsResizing() const { return m_isResizing; }
    // Returns true if the panel should capture the mouse (resize drag or panel click)
    bool HitTestPanel(float x, float y) const;

    // Called each frame to schedule thumbnail renders
    void UpdateThumbnailRequests();

    // Process any completed thumbnail results (call from main thread)
    void ProcessThumbnailResults();

private:
    static constexpr float kDefaultPanelWidth = 180.0f;
    static constexpr float kMinPanelWidth = 100.0f;
    static constexpr float kMaxPanelWidth = 400.0f;
    static constexpr float kItemPadding = 8.0f;
    static constexpr float kItemSpacing = 6.0f;
    static constexpr float kPageLabelHeight = 16.0f;
    static constexpr int kThumbnailTargetWidth = 160;
    static constexpr int kThumbnailTargetHeight = 120;
    static constexpr size_t kMaxCacheSize = 50;
    static constexpr float kResizeHitWidth = 6.0f; // Drag handle width

    struct ThumbnailSlot {
        ComPtr<ID2D1Bitmap> bitmap;
        uint32_t pageIndex = 0;
        bool needsRender = true;
        bool isRendering = false;
    };

    // Calculate item rect for a given page index
    D2D1_RECT_F GetItemRect(uint32_t pageIndex) const;
    D2D1_RECT_F GetThumbnailRect(const D2D1_RECT_F& itemRect) const;

    // Ensure scroll position keeps current page visible
    void ScrollToCurrentPage(bool instant = false);

    // Create a D2D bitmap from a RawImageFrame
    ComPtr<ID2D1Bitmap> CreateBitmapFromFrame(ID2D1RenderTarget* pRT, const QuickView::RawImageFrame& frame);

    // [Image Mode] Load a shell thumbnail for a file index
    ComPtr<ID2D1Bitmap> LoadImageThumbnail(ID2D1RenderTarget* pRT, const std::wstring& path);

    HWND m_hwnd = nullptr;
    QuickView::DocumentRenderController* m_controller = nullptr;
    ID2D1RenderTarget* m_currentRT = nullptr;

    bool m_visible = false;
    float m_panelWidth = kDefaultPanelWidth;
    float m_panelHeight = 0.0f;
    D2D1_RECT_F m_panelRect = {};
    int m_panelSide = 0; // 0=Right, 1=Left

    // [Resize] Drag state
    bool m_isResizing = false;
    float m_resizeStartX = 0.0f;
    float m_resizeStartWidth = 0.0f;
    bool m_resizeHover = false;

    // Document state
    std::wstring m_currentPath;
    uint32_t m_totalPages = 0;
    uint32_t m_currentPage = 0;

    // [Image Mode] Folder image thumbnails
    bool m_isImageMode = false;
    FileNavigator* m_navigator = nullptr;
    int m_currentImageIndex = -1;
    uint32_t m_totalImages = 0;
    std::vector<std::wstring> m_imagePaths; // cached paths for visible range
    std::map<uint32_t, ID2D1Bitmap*> m_imageThumbCache;

    // Thumbnail cache
    std::vector<ThumbnailSlot> m_slots;

    // Scroll state
    float m_scrollY = 0.0f;
    float m_targetScrollY = 0.0f;
    float m_maxScrollY = 0.0f;

    // Track which pages have requests in flight
    uint64_t m_generation = 0;

    // Hover state
    int m_hoverIndex = -1;

    // Flag: new thumbnails loaded, need one more repaint to show them
    bool m_needsRepaint = false;

    // Pending frames waiting for bitmap conversion (main thread, during Render)
    std::unordered_map<uint32_t, std::shared_ptr<QuickView::RawImageFrame>> m_pendingFrames;

    // D2D resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushSelection;
    ComPtr<ID2D1SolidColorBrush> m_brushHover;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushThumbnailBg;
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushResizeHandle;

    ComPtr<IDWriteTextFormat> m_textFormatPage;
    ComPtr<IDWriteFactory> m_dwriteFactory;
};
