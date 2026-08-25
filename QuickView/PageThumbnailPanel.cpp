#include "pch.h"
#include "PageThumbnailPanel.h"
#include "ImageEngine.h"
#include "AppContext.h"
#include "FileNavigator.h"
#include "ImageLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>

extern float g_uiScale;
extern AppConfig g_config;
extern std::unique_ptr<CImageLoader> g_imageLoader;

// [Debug] Diagnostic tracing
static void ThumbDbgLog(const wchar_t* fmt, ...) {
    wchar_t _buf[512];
    va_list _args;
    va_start(_args, fmt);
    int _n = vswprintf_s(_buf, fmt, _args);
    va_end(_args);
    if (_n <= 0) return;
    _buf[_n++] = L'\n';
    _buf[_n] = 0;
    OutputDebugStringW(_buf);
}
#define THUMB_DBG(fmt, ...) ThumbDbgLog(L"[ThumbPanel] " fmt, __VA_ARGS__)

PageThumbnailPanel::~PageThumbnailPanel() {
    for (auto& pair : m_imageThumbCache) { if (pair.second) pair.second->Release(); }
    m_imageThumbCache.clear();
    DiscardDeviceResources();
}

void PageThumbnailPanel::Initialize(HWND hwnd, QuickView::DocumentRenderController* controller) {
    m_hwnd = hwnd;
    m_controller = controller;
    // Restore persisted width
    if (g_config.ThumbnailPanelWidth > 0.0f) {
        m_panelWidth = std::clamp(g_config.ThumbnailPanelWidth, kMinPanelWidth, kMaxPanelWidth);
    }
    m_panelSide = g_config.ThumbnailPanelSide;
}

void PageThumbnailPanel::Show(uint32_t totalPages, uint32_t currentPage) {
    if (totalPages <= 1) {
        THUMB_DBG(L"Show -> Hide (totalPages <= 1)", 0, 0);
        Hide();
        return;
    }
    m_visible = true;
    m_isImageMode = false;
    m_totalPages = totalPages;
    m_currentPage = currentPage;
    m_generation++;

    m_slots.clear();
    m_slots.resize(totalPages);
    for (uint32_t i = 0; i < totalPages; ++i) {
        m_slots[i].pageIndex = i;
        m_slots[i].needsRender = true;
        m_slots[i].isRendering = false;
    }

    m_scrollY = 0.0f;
    m_targetScrollY = 0.0f;
    m_pendingFrames.clear();
    ScrollToCurrentPage(true);
}

void PageThumbnailPanel::Hide() {
    m_visible = false;
    m_totalPages = 0;
    m_currentPage = 0;
    m_slots.clear();
    m_pendingFrames.clear();
    m_scrollY = 0.0f;
    m_targetScrollY = 0.0f;
    m_isImageMode = false;
    m_navigator = nullptr;
    m_currentImageIndex = -1;
    m_totalImages = 0;
    m_imagePaths.clear();
    for (auto& pair : m_imageThumbCache) { if (pair.second) pair.second->Release(); }
    m_imageThumbCache.clear();
    if (m_controller) {
        m_controller->CancelThumbnails();
    }
}

void PageThumbnailPanel::SetCurrentPage(uint32_t page) {
    if (page >= m_totalPages) return;
    if (m_currentPage != page) {
        m_currentPage = page;
    }
}

void PageThumbnailPanel::OnDocumentOpened(const std::wstring& path, uint32_t totalPages) {
    THUMB_DBG(L"OnDocumentOpened(path=%ls, totalPages=%u)", path.c_str(), totalPages);
    m_currentPath = path;
    m_isImageMode = false;
    if (totalPages > 1) {
        Show(totalPages, 0);
    }
}

void PageThumbnailPanel::OnDocumentClosed() {
    Hide();
    m_currentPath.clear();
}

void PageThumbnailPanel::ShowImageThumbnails(FileNavigator* nav, int currentIndex, uint32_t totalFiles) {
    if (!nav || totalFiles == 0) {
        Hide();
        return;
    }
    // Don't show for single-file folders
    if (totalFiles <= 1) {
        Hide();
        return;
    }

    m_visible = true;
    m_isImageMode = true;
    m_navigator = nav;
    m_currentImageIndex = currentIndex;
    m_totalImages = totalFiles;
    m_totalPages = totalFiles; // Reuse pagination logic
    m_currentPage = (uint32_t)std::max(0, currentIndex);

    // Cache file paths for rendering
    m_imagePaths.clear();
    m_imagePaths.resize(totalFiles);
    for (uint32_t i = 0; i < totalFiles; ++i) {
        m_imagePaths[i] = nav->GetFile((int)i);
    }

    // Clear document thumbnail slots, use image cache instead
    bool wasVisible = m_visible && m_isImageMode;
    m_slots.clear();
    m_pendingFrames.clear();
    for (auto& pair : m_imageThumbCache) { if (pair.second) pair.second->Release(); }
    m_imageThumbCache.clear();

    // Only reset scroll position on first show, not when navigating between images
    if (!wasVisible) {
        m_scrollY = 0.0f;
        m_targetScrollY = 0.0f;
        ScrollToCurrentPage(true);
    }
}

void PageThumbnailPanel::SetCurrentImageIndex(int index) {
    if (index < 0 || index >= (int)m_totalImages) return;
    if (m_currentImageIndex != index) {
        m_currentImageIndex = index;
        m_currentPage = (uint32_t)index;
    }
}

void PageThumbnailPanel::UpdateLayout(const D2D1_RECT_F& clientRect) {
    // Don't overwrite user-resized width; only initialize if zero
    if (m_panelWidth <= 0.0f) {
        m_panelWidth = kDefaultPanelWidth * g_uiScale;
    }
    m_panelWidth = std::clamp(m_panelWidth, kMinPanelWidth, kMaxPanelWidth);

    const float titleBarH = g_isFullScreen ? 0.0f : 36.0f * g_uiScale;
    const float toolbarH  = g_isFullScreen ? 0.0f : 36.0f * g_uiScale;

    const float clientH = clientRect.bottom - clientRect.top;


    if (m_panelSide == 1) {
        // Left side
        m_panelRect.left = clientRect.left;
        m_panelRect.right = m_panelRect.left + m_panelWidth;
    } else {
        // Right side (default)
        m_panelRect.left = clientRect.right - m_panelWidth;
        m_panelRect.right = clientRect.right;
    }
    m_panelRect.top = titleBarH;
    m_panelRect.bottom = clientH - toolbarH;
    m_panelHeight = m_panelRect.bottom - m_panelRect.top;


    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    uint32_t itemCount = m_isImageMode ? m_totalImages : m_totalPages;
    const float contentHeight = static_cast<float>(itemCount) * itemHeight;
    m_maxScrollY = std::max(0.0f, contentHeight - m_panelHeight + kItemPadding * 2.0f);

    if (m_scrollY > m_maxScrollY) m_scrollY = m_maxScrollY;
    if (m_targetScrollY > m_maxScrollY) m_targetScrollY = m_maxScrollY;
}

void PageThumbnailPanel::CreateDeviceResources(ID2D1RenderTarget* pRT) {
    THUMB_DBG(L"CreateDeviceResources(pRT=%p)", pRT, 0);
    DiscardDeviceResources();
    if (!pRT) return;

    m_currentRT = pRT;
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f), &m_brushBg);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f, 0.35f), &m_brushSelection);
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), &m_brushHover);
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), &m_brushText);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f), &m_brushThumbnailBg);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f), &m_brushBorder);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.6f, 1.0f, 0.6f), &m_brushResizeHandle);

    if (!m_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }
    if (m_dwriteFactory) {
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          11.0f * g_uiScale, L"en-US", &m_textFormatPage);
        if (m_textFormatPage) {
            m_textFormatPage->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormatPage->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
}

void PageThumbnailPanel::DiscardDeviceResources() {
    m_currentRT = nullptr;
    m_brushBg.Reset();
    m_brushSelection.Reset();
    m_brushHover.Reset();
    m_brushText.Reset();
    m_brushThumbnailBg.Reset();
    m_brushBorder.Reset();
    m_brushResizeHandle.Reset();
    m_textFormatPage.Reset();
    m_dwriteFactory.Reset();
}

D2D1_RECT_F PageThumbnailPanel::GetItemRect(uint32_t pageIndex) const {
    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float y = m_panelRect.top + kItemPadding + static_cast<float>(pageIndex) * itemHeight - m_scrollY;
    const float x = m_panelRect.left + kItemPadding;
    const float w = m_panelWidth - kItemPadding * 2.0f;
    return D2D1::RectF(x, y, x + w, y + itemHeight);
}

D2D1_RECT_F PageThumbnailPanel::GetThumbnailRect(const D2D1_RECT_F& itemRect) const {
    const float thumbH = kThumbnailTargetHeight * g_uiScale;
    const float thumbW = std::min(thumbH / 1.4f, itemRect.right - itemRect.left);
    const float cx = (itemRect.left + itemRect.right) * 0.5f;
    return D2D1::RectF(cx - thumbW * 0.5f, itemRect.top,
                        cx + thumbW * 0.5f, itemRect.top + thumbH);
}

void PageThumbnailPanel::ScrollToCurrentPage(bool instant) {
    uint32_t itemCount = m_isImageMode ? m_totalImages : m_totalPages;
    if (itemCount == 0 || m_panelHeight <= 0.0f) return;
    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float currentPageY = static_cast<float>(m_currentPage) * itemHeight;
    const float pageBottom = currentPageY + itemHeight;

    if (currentPageY < m_scrollY) {
        m_targetScrollY = currentPageY;
    } else if (pageBottom > m_scrollY + m_panelHeight - kItemPadding * 2.0f) {
        m_targetScrollY = pageBottom - m_panelHeight + kItemPadding * 2.0f;
    }

    m_targetScrollY = std::max(0.0f, std::min(m_targetScrollY, m_maxScrollY));
    if (instant) {
        m_scrollY = m_targetScrollY;
    }
}

ComPtr<ID2D1Bitmap> PageThumbnailPanel::CreateBitmapFromFrame(ID2D1RenderTarget* pRT, const QuickView::RawImageFrame& frame) {
    if (!pRT || frame.width <= 0 || frame.height <= 0 || !frame.pixels) return nullptr;

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    D2D1_SIZE_U size = D2D1::SizeU(frame.width, frame.height);

    ComPtr<ID2D1Bitmap> bmp;
    HRESULT hr = pRT->CreateBitmap(size, frame.pixels, frame.stride, &props, &bmp);
    return SUCCEEDED(hr) ? bmp : nullptr;
}

ComPtr<ID2D1Bitmap> PageThumbnailPanel::LoadImageThumbnail(ID2D1RenderTarget* pRT, const std::wstring& path) {
    if (!pRT || path.empty()) return nullptr;

    // Use the existing LoadThumbnail function to get a shell thumbnail
    // allowSlow=true: allow full decode for reliable thumbnail generation
    CImageLoader::ThumbData thumbData;
    HRESULT hr = g_imageLoader->LoadThumbnail(path.c_str(), kThumbnailTargetWidth, &thumbData, true, false);
    if (FAILED(hr) || !thumbData.isValid || thumbData.pixels.empty()) {
        return nullptr;
    }

    // Create D2D bitmap from BGRA pixel data
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    D2D1_SIZE_U size = D2D1::SizeU(thumbData.width, thumbData.height);

    ComPtr<ID2D1Bitmap> bmp;
    hr = pRT->CreateBitmap(size, thumbData.pixels.data(), thumbData.stride, &props, &bmp);
    if (FAILED(hr)) {
        THUMB_DBG(L"CreateBitmap FAIL hr=0x%08X w=%d h=%d", (unsigned)hr, thumbData.width, thumbData.height);
    }
    return SUCCEEDED(hr) ? bmp : nullptr;
}

void PageThumbnailPanel::Render(ID2D1RenderTarget* pRT) {
    if (!m_visible || !pRT) return;
    uint32_t itemCount = m_isImageMode ? m_totalImages : m_totalPages;
    if (itemCount == 0) return;

    // Debug
    THUMB_DBG(L"Render(items=%u, current=%u, imageMode=%d)", itemCount, m_currentPage, m_isImageMode);

    if (!m_brushBg || m_currentRT != pRT) {
        if (m_currentRT != nullptr && m_currentRT != pRT) {
            THUMB_DBG(L"Render: RT CHANGED old=%p new=%p -> reset slots & image cache", m_currentRT, pRT);
            for (auto& slot : m_slots) {
                slot.bitmap.Reset();
                slot.needsRender = true;
                slot.isRendering = false;
            }
            // Also reset image cache since RT changed - bitmaps are bound to RT
            for (auto& pair : m_imageThumbCache) { if (pair.second) { pair.second->Release(); pair.second = nullptr; } }
        }
        CreateDeviceResources(pRT);
    }

    // Smooth scroll animation
    if (std::abs(m_scrollY - m_targetScrollY) > 0.5f) {
        m_scrollY += (m_targetScrollY - m_scrollY) * 0.25f;
    } else {
        m_scrollY = m_targetScrollY;
    }

    // Convert any pending frames to D2D bitmaps (PDF mode only)
    if (!m_isImageMode && !m_pendingFrames.empty()) {
        for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end(); ) {
            if (it->first < m_totalPages && !m_slots[it->first].bitmap) {
                auto bmp = CreateBitmapFromFrame(pRT, *(it->second));
                if (bmp) {
                    m_slots[it->first].bitmap = std::move(bmp);
                }
            }
            it = m_pendingFrames.erase(it);
        }
    }

    // Panel background
    if (m_brushBg) {
        pRT->FillRectangle(m_panelRect, m_brushBg.Get());
    }

    // Separator line on the inner edge (towards image area)
    if (m_brushThumbnailBg) {
        float sepX;
        if (m_panelSide == 1) {
            // Left side: separator on right edge
            sepX = m_panelRect.right - 1.0f;
        } else {
            // Right side: separator on left edge
            sepX = m_panelRect.left;
        }
        D2D1_RECT_F sepRect = D2D1::RectF(sepX, m_panelRect.top, sepX + 1.0f, m_panelRect.bottom);
        pRT->FillRectangle(sepRect, m_brushThumbnailBg.Get());
    }

    // [Resize handle] Visual indicator on the drag edge
    if (m_brushResizeHandle && (m_resizeHover || m_isResizing)) {
        float handleX;
        if (m_panelSide == 1) {
            handleX = m_panelRect.right - kResizeHitWidth;
        } else {
            handleX = m_panelRect.left;
        }
        D2D1_RECT_F handleRect = D2D1::RectF(handleX, m_panelRect.top, handleX + kResizeHitWidth, m_panelRect.bottom);
        pRT->FillRectangle(handleRect, m_brushResizeHandle.Get());
    }

    // Clip to panel area
    pRT->PushAxisAlignedClip(m_panelRect, D2D1_ANTIALIAS_MODE_ALIASED);

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const int startPage = std::max(0, static_cast<int>(m_scrollY / itemHeight) - 1);
    const int endPage = std::min(static_cast<int>(itemCount),
        static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 2);

    for (int i = startPage; i < endPage; ++i) {
        const uint32_t pageIndex = static_cast<uint32_t>(i);
        const D2D1_RECT_F itemRect = GetItemRect(pageIndex);
        if (itemRect.bottom < m_panelRect.top || itemRect.top > m_panelRect.bottom) continue;

        const bool isCurrentPage = (pageIndex == m_currentPage);
        const bool isHovered = (static_cast<int>(pageIndex) == m_hoverIndex);

        // Selection/hover background (removed: no selection background)
        if (!isCurrentPage && isHovered && m_brushHover) {
            D2D1_ROUNDED_RECT hovRect = D2D1::RoundedRect(itemRect, 4.0f * g_uiScale, 4.0f * g_uiScale);
            pRT->FillRoundedRectangle(hovRect, m_brushHover.Get());
        }

        // Thumbnail image
        const D2D1_RECT_F thumbRect = GetThumbnailRect(itemRect);

        // Get bitmap: PDF mode from slots, image mode from cache
        ID2D1Bitmap* pBitmap = nullptr;
        if (m_isImageMode) {
            auto it = m_imageThumbCache.find(pageIndex);
            if (it != m_imageThumbCache.end()) {
                pBitmap = it->second;
            }
        } else {
            if (pageIndex < m_slots.size() && m_slots[pageIndex].bitmap) {
                pBitmap = m_slots[pageIndex].bitmap.Get();
            }
        }

        if (pBitmap) {
            pRT->DrawBitmap(pBitmap, thumbRect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            if (m_brushThumbnailBg) {
                D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
                pRT->FillRoundedRectangle(bgRect, m_brushThumbnailBg.Get());
            }
        }

        // Current page border
        if (isCurrentPage && m_brushBorder) {
            D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
            pRT->DrawRoundedRectangle(borderRect, m_brushBorder.Get(), 2.0f * g_uiScale);
        }

        // Page label
        if (m_textFormatPage && m_brushText) {
            wchar_t label[16];
            swprintf_s(label, L"%u", pageIndex + 1);
            D2D1_RECT_F labelRect = D2D1::RectF(itemRect.left, thumbRect.bottom + 2.0f * g_uiScale,
                                                 itemRect.right, itemRect.bottom);
            pRT->DrawText(label, static_cast<UINT32>(wcslen(label)), m_textFormatPage.Get(),
                         labelRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    pRT->PopAxisAlignedClip();
}

bool PageThumbnailPanel::OnMouseMove(float x, float y) {
    if (!m_visible) return false;

    // Update resize hover state
    bool wasResizeHover = m_resizeHover;
    m_resizeHover = IsResizeHit(x, y);
    if (m_resizeHover != wasResizeHover) {
        // Need repaint for resize handle visual
    }

    int newHover = -1;
    if (x >= m_panelRect.left && x <= m_panelRect.right &&
        y >= m_panelRect.top && y <= m_panelRect.bottom) {
        const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
        const float localY = y - m_panelRect.top - kItemPadding + m_scrollY;
        if (localY >= 0.0f) {
            newHover = static_cast<int>(localY / itemHeight);
            uint32_t itemCount = m_isImageMode ? m_totalImages : m_totalPages;
            if (newHover < 0 || newHover >= static_cast<int>(itemCount)) {
                newHover = -1;
            }
        }
    }

    if (newHover != m_hoverIndex || m_resizeHover != wasResizeHover) {
        m_hoverIndex = newHover;
        return true;
    }
    return false;
}

int PageThumbnailPanel::OnLButtonDown(float x, float y) {
    if (!m_visible) return -1;

    if (x < m_panelRect.left || x > m_panelRect.right ||
        y < m_panelRect.top || y > m_panelRect.bottom) {
        return -1;
    }

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float localY = y - m_panelRect.top - kItemPadding + m_scrollY;
    if (localY < 0.0f) return -1;

    const int pageIndex = static_cast<int>(localY / itemHeight);
    uint32_t itemCount = m_isImageMode ? m_totalImages : m_totalPages;
    if (pageIndex >= 0 && pageIndex < static_cast<int>(itemCount)) {
        return pageIndex;
    }
    return -1;
}

bool PageThumbnailPanel::OnMouseWheel(int delta) {
    if (!m_visible) return false;
    if (m_maxScrollY <= 0.0f) return false;

    const float scrollStep = 60.0f * g_uiScale;
    m_targetScrollY -= static_cast<float>(delta / 120) * scrollStep;
    m_targetScrollY = std::max(0.0f, std::min(m_targetScrollY, m_maxScrollY));
    return true;
}

bool PageThumbnailPanel::IsResizeHit(float x, float y) const {
    if (!m_visible) return false;
    if (y < m_panelRect.top || y > m_panelRect.bottom) return false;

    if (m_panelSide == 1) {
        // Left side: drag handle on right edge
        return x >= m_panelRect.right - kResizeHitWidth && x <= m_panelRect.right + kResizeHitWidth;
    } else {
        // Right side: drag handle on left edge
        return x >= m_panelRect.left - kResizeHitWidth && x <= m_panelRect.left + kResizeHitWidth;
    }
}

void PageThumbnailPanel::BeginResize(float x) {
    m_isResizing = true;
    m_resizeStartX = x;
    m_resizeStartWidth = m_panelWidth;
}

void PageThumbnailPanel::UpdateResize(float x, [[maybe_unused]] float windowWidth) {
    if (!m_isResizing) return;

    float dx = x - m_resizeStartX;
    float newWidth;

    if (m_panelSide == 1) {
        // Left side: dragging right edge → width increases with dx
        newWidth = m_resizeStartWidth + dx;
    } else {
        // Right side: dragging left edge → width increases when dx is negative
        newWidth = m_resizeStartWidth - dx;
    }

    newWidth = std::clamp(newWidth, kMinPanelWidth, kMaxPanelWidth);
    if (newWidth != m_panelWidth) {
        m_panelWidth = newWidth;
        // Persist to config
        g_config.ThumbnailPanelWidth = m_panelWidth;
    }
}

void PageThumbnailPanel::EndResize() {
    m_isResizing = false;
    // Final persist
    g_config.ThumbnailPanelWidth = m_panelWidth;
}

bool PageThumbnailPanel::HitTestPanel(float x, float y) const {
    if (!m_visible) return false;
    // Include resize hit area in panel capture zone
    if (IsResizeHit(x, y)) return true;
    return x >= m_panelRect.left && x <= m_panelRect.right &&
           y >= m_panelRect.top && y <= m_panelRect.bottom;
}

bool PageThumbnailPanel::IsLoading() const {
    if (m_isImageMode) {
        // Check if any visible slot lacks a bitmap
        const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
        const int visibleStart = std::max(0, static_cast<int>(m_scrollY / itemHeight)) - 2;
        const int visibleEnd = std::min(static_cast<int>(m_totalImages),
            static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 3);
        for (int i = visibleStart; i < visibleEnd; ++i) {
            if (i < 0 || i >= (int)m_totalImages) continue;
            uint32_t idx = (uint32_t)i;
            if (m_imageThumbCache.find(idx) == m_imageThumbCache.end()) return true;
        }
        return false;
    }
    for (const auto& slot : m_slots) {
        if (slot.isRendering || slot.needsRender) return true;
    }
    return false;
}

void PageThumbnailPanel::UpdateThumbnailRequests() {
    if (!m_visible || m_totalPages == 0) return;

    // [Image Mode] Lazy-load shell thumbnails for visible range
    // Limit to 1 thumbnail per frame to prevent UI stutter
    if (m_isImageMode && m_currentRT && m_navigator) {
        const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
        const int visibleStart = std::max(0, static_cast<int>(m_scrollY / itemHeight)) - 2;
        const int visibleEnd = std::min(static_cast<int>(m_totalImages),
            static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 3);

        int loadedThisFrame = 0;
        for (int i = visibleStart; i < visibleEnd && loadedThisFrame < 1; ++i) {
            if (i < 0 || i >= (int)m_totalImages) continue;
            uint32_t idx = (uint32_t)i;
            if (m_imageThumbCache.find(idx) == m_imageThumbCache.end()) {
                if (idx < m_imagePaths.size()) {
                    auto bmp = LoadImageThumbnail(m_currentRT, m_imagePaths[idx]);
                    if (bmp) {
                        m_imageThumbCache[idx] = bmp.Detach();
                        loadedThisFrame++;
                        m_needsRepaint = true;
                    } else {
                        m_imageThumbCache[idx] = nullptr;
                        loadedThisFrame++;
                    }
                }
            }
        }

        // Clean up off-screen cache entries to limit memory
        if (m_imageThumbCache.size() > kMaxCacheSize) {
            // Remove entries far from visible range
            int center = static_cast<int>(m_scrollY / itemHeight) + static_cast<int>(m_panelHeight / itemHeight / 2);
            for (auto it = m_imageThumbCache.begin(); it != m_imageThumbCache.end(); ) {
                int dist = std::abs(static_cast<int>(it->first) - center);
                if (dist > 30) {
                    if (it->second) it->second->Release();
                    it = m_imageThumbCache.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return;
    }

    // PDF mode: use DocumentRenderController
    if (!m_controller || m_isImageMode) return;

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const int visibleStart = static_cast<int>(std::max(0.0f, m_scrollY / itemHeight)) - 2;
    const int visibleEnd = static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 3;

    int inFlight = 0;
    for (auto& slot : m_slots) {
        if (slot.isRendering) inFlight++;
    }

    int submitted = 0;
    const int maxInFlight = 2;

    auto trySubmit = [&](uint32_t idx) {
        if (submitted + inFlight >= maxInFlight) return;
        if (idx >= m_totalPages) return;
        if (!m_slots[idx].needsRender || m_slots[idx].isRendering) return;
        if (m_slots[idx].bitmap) {
            m_slots[idx].needsRender = false;
            return;
        }

        QuickView::ThumbnailRequest req;
        req.notifyWindow = m_hwnd;
        req.path = m_currentPath;
        req.pageIndex = idx;
        req.targetWidth = kThumbnailTargetWidth;
        req.targetHeight = kThumbnailTargetHeight;
        req.priority = static_cast<int>(std::abs(static_cast<int>(idx) - static_cast<int>(m_currentPage)));

        uint64_t id = m_controller->RequestThumbnail(req);
        if (id != 0) {
            m_slots[idx].isRendering = true;
            submitted++;
        }
    };

    trySubmit(m_currentPage);

    for (int i = visibleStart; i < visibleEnd && submitted + inFlight < maxInFlight; ++i) {
        if (i >= 0 && i < static_cast<int>(m_totalPages)) {
            trySubmit(static_cast<uint32_t>(i));
        }
    }

    for (uint32_t offset = 1; offset <= 5 && submitted + inFlight < maxInFlight; ++offset) {
        if (m_currentPage + offset < m_totalPages) trySubmit(m_currentPage + offset);
        if (submitted + inFlight >= maxInFlight) break;
        if (m_currentPage >= offset) trySubmit(m_currentPage - offset);
    }
}

void PageThumbnailPanel::ProcessThumbnailResults() {
    if (!m_visible || !m_controller || m_isImageMode) return;

    QuickView::ThumbnailResult result;
    while (m_controller->TakeThumbnailResult(result)) {
        if (result.status == S_OK && result.frame && result.frame->IsValid()) {
            if (result.pageIndex < m_totalPages) {
                auto& slot = m_slots[result.pageIndex];
                slot.isRendering = false;
                slot.needsRender = false;

                if (!slot.bitmap) {
                    m_pendingFrames[result.pageIndex] = std::move(result.frame);
                    m_needsRepaint = true;
                }
            }
        } else {
            if (result.pageIndex < m_totalPages) {
                m_slots[result.pageIndex].isRendering = false;
                m_slots[result.pageIndex].needsRender = true;
            }
        }
    }
}
