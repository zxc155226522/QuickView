#pragma once
// ============================================================================
// ThumbnailPanelBase.h
// Base class for thumbnail sidebar panels.
// Provides shared layout, scrolling, rendering framework, D2D resources,
// mouse interaction, and drag-to-resize.
//
// Subclasses (PdfPageThumbnailPanel, ImageListThumbnailPanel) implement:
//   - DrawItems(): render thumbnail items
//   - OnItemClick(): handle click on an item
//   - IsLoading(): loading state
//   - GetPanelTitle(): visual identification
//   - GetAccentColor(): selection border color for visual distinction
// ============================================================================
#include "pch.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class ThumbnailPanelBase {
public:
    virtual ~ThumbnailPanelBase() = default;

    ThumbnailPanelBase(const ThumbnailPanelBase&) = delete;
    ThumbnailPanelBase& operator=(const ThumbnailPanelBase&) = delete;

    // --- Lifecycle ---
    virtual void Initialize(HWND hwnd);

    // --- State control ---
    [[nodiscard]] bool IsVisible() const { return m_visible; }
    void SetVisible(bool v) { m_visible = v; }

    // --- Page tracking ---
    [[nodiscard]] uint32_t GetCurrentPage() const { return m_currentPage; }
    void SetCurrentPage(uint32_t page) { m_currentPage = page; }

    // --- Panel side control (0=Right, 1=Left, 2=Off, 3=Bottom) ---
    void SetPanelSide(int side) { m_panelSide = side; }
    [[nodiscard]] int GetPanelSide() const { return m_panelSide; }

    // --- Layout ---
    [[nodiscard]] float GetWidth() const { return m_panelWidth; }
    [[nodiscard]] float GetPanelHeight() const { return m_panelHeight; }
    [[nodiscard]] float GetBottomPanelHeight() const { return m_panelHeightUser; }
    [[nodiscard]] D2D1_RECT_F GetPanelRect() const { return m_panelRect; }
    [[nodiscard]] float GetScrollY() const { return m_scrollY; }
    [[nodiscard]] float GetTargetScrollY() const { return m_targetScrollY; }
    [[nodiscard]] bool ConsumeNeedsRepaint() { bool v = m_needsRepaint; m_needsRepaint = false; return v; }
    void UpdateLayout(const D2D1_RECT_F& clientRect);

    // --- Device resources ---
    void CreateDeviceResources(ID2D1RenderTarget* pRT);
    void DiscardDeviceResources();

    // --- Rendering ---
    void Render(ID2D1RenderTarget* pRT);

    // --- Interaction ---
    bool OnMouseMove(float x, float y);
    int OnLButtonDown(float x, float y);
    bool OnMouseWheel(int delta);

    // --- Resize ---
    bool IsResizeHit(float x, float y) const;
    void BeginResize(float x, float y);
    void UpdateResize(float x, float y, float windowWidth);
    void EndResize();
    [[nodiscard]] bool IsResizing() const { return m_isResizing; }
    bool HitTestPanel(float x, float y) const;

    // --- Per-frame update ---
    void UpdateThumbnailRequests() { OnUpdateThumbnailRequests(); }
    void ProcessThumbnailResults() { OnProcessThumbnailResults(); }
    [[nodiscard]] bool IsLoading() const { return OnIsLoading(); }

protected:
    ThumbnailPanelBase() = default;

    // Subclass interface
    virtual void OnUpdateThumbnailRequests() = 0;
    virtual void OnProcessThumbnailResults() = 0;
    virtual bool OnIsLoading() const = 0;
    virtual void DrawItems(ID2D1RenderTarget* pRT) = 0;
    virtual int OnItemClick(int index) = 0;
    virtual void OnDeviceResourcesCreated() {}
    virtual void OnDeviceResourcesDiscarded() {}
    virtual void OnLayoutChanged() {}

    // Subclass accessors for rendering
    virtual uint32_t GetItemCount() const = 0;
    virtual ComPtr<ID2D1Bitmap> GetItemBitmap(uint32_t index) = 0;
    virtual std::wstring GetItemLabel(uint32_t index) const = 0;
    virtual const wchar_t* GetPanelTitle() const = 0;
    virtual D2D1::ColorF GetAccentColor() const = 0;  // Selection border color

    // Shared constants
    static constexpr float kDefaultPanelWidth = 180.0f;
    static constexpr float kMinPanelWidth = 20.0f;
    static constexpr float kMaxPanelWidth = 400.0f;
    static constexpr float kDefaultPanelHeightBottom = 140.0f;
    static constexpr float kMinPanelHeightBottom = 40.0f;
    static constexpr float kMaxPanelHeightBottom = 400.0f;
    static constexpr float kBottomItemWidth = 160.0f;
    static constexpr float kItemPadding = 8.0f;
    static constexpr float kItemSpacing = 6.0f;
    static constexpr float kPageLabelHeight = 16.0f;
    static constexpr int kThumbnailTargetWidth = 160;
    static constexpr int kThumbnailTargetHeight = 120;
    static constexpr float kResizeHitWidth = 6.0f;
    static constexpr float kTitleBarHeight = 24.0f; // Panel title bar height

    // Layout helpers
    D2D1_RECT_F GetItemRect(uint32_t pageIndex) const;
    D2D1_RECT_F GetThumbnailRect(const D2D1_RECT_F& itemRect) const;
    void ScrollToCurrentPage(bool instant = false);

    // D2D resources (shared)
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushSelection;
    ComPtr<ID2D1SolidColorBrush> m_brushHover;
    ComPtr<ID2D1SolidColorBrush> m_brushText;
    ComPtr<ID2D1SolidColorBrush> m_brushThumbnailBg;
    ComPtr<ID2D1SolidColorBrush> m_brushBorder;
    ComPtr<ID2D1SolidColorBrush> m_brushResizeHandle;
    ComPtr<ID2D1SolidColorBrush> m_brushTitleBg;
    ComPtr<IDWriteTextFormat> m_textFormatPage;
    ComPtr<IDWriteTextFormat> m_textFormatTitle;
    ComPtr<IDWriteFactory> m_dwriteFactory;

    HWND m_hwnd = nullptr;
    ID2D1RenderTarget* m_currentRT = nullptr;

    bool m_visible = false;
    float m_panelWidth = kDefaultPanelWidth;
    float m_panelHeight = 0.0f;
    D2D1_RECT_F m_panelRect = {};
    int m_panelSide = 0;
    float m_panelHeightUser = kDefaultPanelHeightBottom;

    // Scroll state
    float m_scrollY = 0.0f;
    float m_targetScrollY = 0.0f;
    float m_maxScrollY = 0.0f;
    float m_scrollX = 0.0f;
    float m_targetScrollX = 0.0f;
    float m_maxScrollX = 0.0f;

    // Resize state
    bool m_isResizing = false;
    float m_resizeStartX = 0.0f;
    float m_resizeStartWidth = 0.0f;
    bool m_resizeHover = false;

    // Document state
    std::wstring m_currentPath;
    uint32_t m_totalPages = 0;
    uint32_t m_currentPage = 0;

    // Hover state
    int m_hoverIndex = -1;

    bool m_needsRepaint = false;
};
