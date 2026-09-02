#include "pch.h"
#include "ThumbnailPanelBase.h"
#include "AppContext.h"
#include "Toolbar.h"
#include "PdfPageThumbnailPanel.h"
#include "ImageListThumbnailPanel.h"

extern float g_uiScale;
extern AppConfig g_config;
extern bool IsLightThemeActive();
extern PdfPageThumbnailPanel g_pdfThumbPanel;
extern ImageListThumbnailPanel g_imageThumbPanel;

namespace {
// [Theme] Panels follow the app theme automatically: dark/light mode, the
// user's custom accent color and the global panel opacity — instead of the
// old hardcoded dark palette that clashed with light themes.
struct ThemePalette {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F titleBg;
    D2D1_COLOR_F thumbBg;
    D2D1_COLOR_F text;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F resizeHandle;
};

ThemePalette ResolveThemePalette() {
    ThemePalette p;
    const bool isLight = IsLightThemeActive();
    const float panelAlpha = (std::clamp)(g_config.GlassPanelsOpacity / 100.0f, 0.30f, 1.0f);
    if (isLight) {
        p.bg        = D2D1::ColorF(0.95f, 0.95f, 0.97f, panelAlpha);
        p.titleBg   = D2D1::ColorF(0.90f, 0.91f, 0.94f, panelAlpha);
        p.thumbBg   = D2D1::ColorF(0.85f, 0.85f, 0.88f, 1.0f);
        p.text      = D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.95f);
        p.hover     = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f);
        p.resizeHandle = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.25f);
    } else {
        p.bg        = D2D1::ColorF(0.08f, 0.08f, 0.10f, panelAlpha);
        p.titleBg   = D2D1::ColorF(0.04f, 0.04f, 0.06f, panelAlpha);
        p.thumbBg   = D2D1::ColorF(0.18f, 0.18f, 0.20f, 1.0f);
        p.text      = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f);
        p.hover     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
        p.resizeHandle = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f);
    }
    return p;
}

// Accent follows the user's theme accent color; falls back to the panel's own
// color (blue=PDF / green=images) only if the config stores zeros.
D2D1_COLOR_F ResolveAccentColor(D2D1::ColorF fallback) {
    if (g_config.ThemeCustomAccentR <= 0.0f && g_config.ThemeCustomAccentG <= 0.0f &&
        g_config.ThemeCustomAccentB <= 0.0f) {
        return fallback;
    }
    return D2D1::ColorF(g_config.ThemeCustomAccentR, g_config.ThemeCustomAccentG,
                        g_config.ThemeCustomAccentB, 1.0f);
}
} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void ThumbnailPanelBase::Initialize(HWND hwnd) {
    m_hwnd = hwnd;
}

// ============================================================================
// Layout
// ============================================================================

float ThumbnailPanelBase::BottomThumbHeight() const {
    // Adaptive: thumbnails shrink automatically as the bottom bar is dragged shorter
    const float availH = m_panelHeight - kItemPadding * 2.0f - kPageLabelHeight * g_uiScale;
    return std::max(12.0f * g_uiScale, std::min(availH, kThumbnailTargetHeight * g_uiScale));
}

float ThumbnailPanelBase::BottomItemStride() const {
    // Stride follows the cell aspect (square for images, A4 portrait for PDF pages)
    return BottomThumbHeight() * GetCellAspect() + kItemSpacing;
}

void ThumbnailPanelBase::UpdateLayout(const D2D1_RECT_F& clientRect) {
    if (m_panelWidth <= 0.0f) {
        m_panelWidth = kDefaultPanelWidth * g_uiScale;
    }
    m_panelWidth = std::clamp(m_panelWidth, kMinPanelWidth, kMaxPanelWidth);
    m_panelHeightUser = std::clamp(m_panelHeightUser, kMinPanelHeightBottom, kMaxPanelHeightBottom);

    const float titleBarH = g_isFullScreen ? 0.0f : 36.0f * g_uiScale;
    // [PDF] Reserve the same strip the toolbar reports so the page-turn bar
    // below the preview never overlaps a bottom-docked thumbnail panel.
    const float toolbarH  = g_isFullScreen ? 0.0f : g_toolbar.GetReservedHeight();
    const float clientH = clientRect.bottom - clientRect.top;

    if (m_panelSide == 3) {
        // Bottom Mode: height = user setting, positioned above toolbar.
        // [Same container] The strip is inset by visible side panels so all
        // panels share one grid with the image — nothing overlaps.
        float stripLeft = clientRect.left;
        float stripRight = clientRect.right;
        auto insetFor = [&](const ThumbnailPanelBase* other) {
            if (!other || other == this || !other->IsVisible()) return;
            const int oside = other->GetPanelSide();
            if (oside == 1) stripLeft = (std::max)(stripLeft, clientRect.left + other->GetWidth());
            else if (oside == 0) stripRight = (std::min)(stripRight, clientRect.right - other->GetWidth());
        };
        insetFor(&g_pdfThumbPanel);
        insetFor(&g_imageThumbPanel);
        if (stripRight - stripLeft < 40.0f) { stripLeft = clientRect.left; stripRight = clientRect.right; }

        m_panelRect.left = stripLeft;
        m_panelRect.right = stripRight;
        m_panelRect.top = clientH - toolbarH - m_panelHeightUser;
        m_panelRect.bottom = clientH - toolbarH;
        m_panelWidth = m_panelRect.right - m_panelRect.left;
        m_panelHeight = m_panelRect.bottom - m_panelRect.top;

        const float itemWidth = BottomItemStride();
        uint32_t itemCount = GetItemCount();
        const float contentWidth = static_cast<float>(itemCount) * itemWidth;
        m_maxScrollX = std::max(0.0f, contentWidth - m_panelWidth + kItemPadding * 2.0f);
        if (m_scrollX > m_maxScrollX) m_scrollX = m_maxScrollX;
        if (m_targetScrollX > m_maxScrollX) m_targetScrollX = m_maxScrollX;
        if (m_pendingCenter) ScrollToCurrentPage(true);
        OnLayoutChanged();
        return;
    }

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
    uint32_t itemCount = GetItemCount();
    const float contentHeight = static_cast<float>(itemCount) * itemHeight;
    m_maxScrollY = std::max(0.0f, contentHeight - m_panelHeight + kItemPadding * 2.0f);

    if (m_scrollY > m_maxScrollY) m_scrollY = m_maxScrollY;
    if (m_targetScrollY > m_maxScrollY) m_targetScrollY = m_maxScrollY;
    if (m_pendingCenter) ScrollToCurrentPage(true);

    OnLayoutChanged();
}

D2D1_RECT_F ThumbnailPanelBase::GetItemRect(uint32_t pageIndex) const {
    if (m_panelSide == 3) {
        // Bottom Mode: Horizontal layout, no title bar
        const float itemWidth = BottomItemStride();
        const float x = m_panelRect.left + kItemPadding + static_cast<float>(pageIndex) * itemWidth - m_scrollX;
        const float y = m_panelRect.top + kItemPadding;
        const float h = m_panelHeight - kItemPadding * 2.0f;
        return D2D1::RectF(x, y, x + itemWidth, y + h);
    }
    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float titleH = kTitleBarHeight * g_uiScale;
    const float y = m_panelRect.top + kItemPadding + titleH + static_cast<float>(pageIndex) * itemHeight - m_scrollY;
    const float x = m_panelRect.left + kItemPadding;
    const float w = m_panelWidth - kItemPadding * 2.0f;
    return D2D1::RectF(x, y, x + w, y + itemHeight);
}

D2D1_RECT_F ThumbnailPanelBase::GetThumbnailRect(const D2D1_RECT_F& itemRect) const {
    // Cell with the panel's aspect (square for images, A4 portrait for PDF pages);
    // the bitmap itself is letterboxed inside without distortion.
    const float aspect = GetCellAspect(); // width / height
    if (m_panelSide == 3) {
        const float availW = itemRect.right - itemRect.left - kItemSpacing;
        const float availH = itemRect.bottom - itemRect.top - kPageLabelHeight * g_uiScale;
        float h = std::min(availH, kThumbnailTargetHeight * g_uiScale);
        float w = h * aspect;
        if (w > availW) { w = availW; h = w / aspect; }
        const float cx = itemRect.left + availW * 0.5f;
        const float cy = itemRect.top + availH * 0.5f;
        return D2D1::RectF(cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f);
    }
    const float availW = itemRect.right - itemRect.left;
    float h = kThumbnailTargetHeight * g_uiScale;
    float w = h * aspect;
    if (w > availW) { w = availW; h = w / aspect; }
    const float cx = (itemRect.left + itemRect.right) * 0.5f;
    return D2D1::RectF(cx - w * 0.5f, itemRect.top, cx + w * 0.5f, itemRect.top + h);
}

D2D1_RECT_F ThumbnailPanelBase::FitRectInside(const D2D1_RECT_F& box, float w, float h) {
    // Contain-fit: scale the bitmap to fit inside the box, centered, aspect preserved
    if (w <= 0.0f || h <= 0.0f) return box;
    const float boxW = box.right - box.left;
    const float boxH = box.bottom - box.top;
    const float scale = std::min(boxW / w, boxH / h);
    const float dw = w * scale;
    const float dh = h * scale;
    const float cx = (box.left + box.right) * 0.5f;
    const float cy = (box.top + box.bottom) * 0.5f;
    return D2D1::RectF(cx - dw * 0.5f, cy - dh * 0.5f, cx + dw * 0.5f, cy + dh * 0.5f);
}

void ThumbnailPanelBase::ScrollToCurrentPage(bool instant) {
    uint32_t itemCount = GetItemCount();
    if (itemCount == 0) return;

    if (m_panelSide == 3) {
        if (m_panelWidth <= 0.0f || m_panelHeight <= 0.0f) {
            m_pendingCenter = true; // Not laid out yet — retry after the next UpdateLayout
            return;
        }
        const float itemWidth = BottomItemStride();
        // Compute the scroll limit from the live item count/geometry —
        // m_maxScrollX may be stale when this runs before UpdateLayout.
        const float maxScrollX =
            std::max(0.0f, static_cast<float>(itemCount) * itemWidth - m_panelWidth + kItemPadding * 2.0f);
        // Center the selected thumbnail in the strip
        m_targetScrollX = static_cast<float>(m_currentPage) * itemWidth + itemWidth * 0.5f - m_panelWidth * 0.5f;
        m_targetScrollX = std::max(0.0f, std::min(m_targetScrollX, maxScrollX));
        if (instant) m_scrollX = m_targetScrollX;
        m_pendingCenter = false;
        return;
    }

    if (m_panelHeight <= 0.0f) {
        m_pendingCenter = true; // Not laid out yet — retry after the next UpdateLayout
        return;
    }
    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float maxScrollY =
        std::max(0.0f, static_cast<float>(itemCount) * itemHeight - m_panelHeight + kItemPadding * 2.0f);
    const float currentPageY = static_cast<float>(m_currentPage) * itemHeight;
    // Center the selected thumbnail in the panel
    m_targetScrollY = currentPageY + itemHeight * 0.5f - m_panelHeight * 0.5f;
    m_targetScrollY = std::max(0.0f, std::min(m_targetScrollY, maxScrollY));
    if (instant) m_scrollY = m_targetScrollY;
    m_pendingCenter = false;
}

// ============================================================================
// Device resources
// ============================================================================

void ThumbnailPanelBase::CreateDeviceResources(ID2D1RenderTarget* pRT) {
    DiscardDeviceResources();
    if (!pRT) return;

    m_currentRT = pRT;
    const ThemePalette pal = ResolveThemePalette();
    const D2D1_COLOR_F accent = ResolveAccentColor(GetAccentColor());
    m_brushesThemeLight = IsLightThemeActive();
    pRT->CreateSolidColorBrush(pal.bg, &m_brushBg);
    pRT->CreateSolidColorBrush(D2D1::ColorF(accent.r, accent.g, accent.b, 0.35f), &m_brushSelection);
    pRT->CreateSolidColorBrush(pal.hover, &m_brushHover);
    pRT->CreateSolidColorBrush(pal.text, &m_brushText);
    pRT->CreateSolidColorBrush(pal.thumbBg, &m_brushThumbnailBg);
    pRT->CreateSolidColorBrush(accent, &m_brushBorder);
    pRT->CreateSolidColorBrush(pal.resizeHandle, &m_brushResizeHandle);
    pRT->CreateSolidColorBrush(pal.titleBg, &m_brushTitleBg);
    // Tooltip: near-opaque backdrop so text stays readable over any content
    const bool tipLight = IsLightThemeActive();
    const D2D1_COLOR_F tipBg = tipLight
        ? D2D1::ColorF(0.98f, 0.98f, 0.99f, 0.97f)
        : D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.97f);
    pRT->CreateSolidColorBrush(tipBg, &m_brushTipBg);
    pRT->CreateSolidColorBrush(D2D1::ColorF(pal.text.r, pal.text.g, pal.text.b, 0.55f), &m_brushTipDim);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.75f), &m_brushBadgeBg);
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), &m_brushBadgeText);

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
        // Title font: bold, smaller
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          10.0f * g_uiScale, L"en-US", &m_textFormatTitle);
        if (m_textFormatTitle) {
            m_textFormatTitle->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_textFormatTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        // NULL trimming sign = default ellipsis glyph at the cut point
        const DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        // Item label: single line only — long names get an ellipsis, never wrap
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          11.0f * g_uiScale, L"en-US", &m_textFormatLabel);
        if (m_textFormatLabel) {
            m_textFormatLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormatLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_textFormatLabel->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            m_textFormatLabel->SetTrimming(&trimming, nullptr);
        }
        // Tooltip fonts (left aligned): name in bold, path in smaller dim text
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          11.0f * g_uiScale, L"en-US", &m_textFormatTipName);
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          10.0f * g_uiScale, L"en-US", &m_textFormatTipPath);
        if (m_textFormatTipName) {
            m_textFormatTipName->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            m_textFormatTipName->SetTrimming(&trimming, nullptr);
        }
        if (m_textFormatTipPath) {
            m_textFormatTipPath->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            m_textFormatTipPath->SetTrimming(&trimming, nullptr);
        }
        // Badge font: bold, small for crisp caps
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          9.0f * g_uiScale, L"en-US", &m_textFormatBadge);
        if (m_textFormatBadge) {
            m_textFormatBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFormatBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_textFormatBadge->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    OnDeviceResourcesCreated();
}

void ThumbnailPanelBase::DiscardDeviceResources() {
    m_currentRT = nullptr;
    m_brushBg.Reset();
    m_brushSelection.Reset();
    m_brushHover.Reset();
    m_brushText.Reset();
    m_brushThumbnailBg.Reset();
    m_brushBorder.Reset();
    m_brushResizeHandle.Reset();
    m_brushTitleBg.Reset();
    m_brushTipBg.Reset();
    m_brushTipDim.Reset();
    m_brushBadgeBg.Reset();
    m_brushBadgeText.Reset();
    m_textFormatPage.Reset();
    m_textFormatTitle.Reset();
    m_textFormatLabel.Reset();
    m_textFormatTipName.Reset();
    m_textFormatTipPath.Reset();
    m_textFormatBadge.Reset();
    m_dwriteFactory.Reset();
    OnDeviceResourcesDiscarded();
}

// ============================================================================
// Rendering
// ============================================================================

void ThumbnailPanelBase::Render(ID2D1RenderTarget* pRT) {
    if (!m_visible || !pRT) return;
    uint32_t itemCount = GetItemCount();
    if (itemCount == 0) return;

    if (!m_brushBg || m_currentRT != pRT) {
        if (m_currentRT != nullptr && m_currentRT != pRT) {
            OnDeviceResourcesDiscarded();
        }
        CreateDeviceResources(pRT);
    } else if (m_brushesThemeLight != IsLightThemeActive()) {
        CreateDeviceResources(pRT); // Theme flipped — rebuild palette
    }

    // Smooth scroll animation
    if (m_panelSide == 3) {
        if (std::abs(m_scrollX - m_targetScrollX) > 0.5f) {
            m_scrollX += (m_targetScrollX - m_scrollX) * 0.25f;
        } else {
            m_scrollX = m_targetScrollX;
        }
    } else {
        if (std::abs(m_scrollY - m_targetScrollY) > 0.5f) {
            m_scrollY += (m_targetScrollY - m_scrollY) * 0.25f;
        } else {
            m_scrollY = m_targetScrollY;
        }
    }

    // Panel background
    if (m_brushBg) {
        pRT->FillRectangle(m_panelRect, m_brushBg.Get());
    }

    // Title bar (side panels only — bottom strip has no title)
    if (m_panelSide != 3 && m_brushTitleBg && m_textFormatTitle && m_brushText) {
        float titleH = kTitleBarHeight * g_uiScale;
        D2D1_RECT_F titleRect;
        if (m_panelSide == 3) {
            titleRect = D2D1::RectF(m_panelRect.left, m_panelRect.top, m_panelRect.right, m_panelRect.top + titleH);
        } else {
            titleRect = D2D1::RectF(m_panelRect.left, m_panelRect.top, m_panelRect.right, m_panelRect.top + titleH);
        }
        pRT->FillRectangle(titleRect, m_brushTitleBg.Get());

        // Draw accent color line under title
        if (m_brushBorder) {
            D2D1_RECT_F accentLine = D2D1::RectF(titleRect.left, titleRect.bottom - 1.0f, titleRect.right, titleRect.bottom);
            pRT->FillRectangle(accentLine, m_brushBorder.Get());
        }

        // Draw title text with padding
        D2D1_RECT_F textRect = D2D1::RectF(titleRect.left + 8.0f * g_uiScale, titleRect.top,
                                           titleRect.right - 8.0f * g_uiScale, titleRect.bottom);
        const wchar_t* title = GetPanelTitle();
        if (title) {
            pRT->DrawText(title, static_cast<UINT32>(wcslen(title)), m_textFormatTitle.Get(),
                         textRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }

    // Separator line on the inner edge (towards image area)
    if (m_brushThumbnailBg) {
        if (m_panelSide == 3) {
            float sepY = m_panelRect.top;
            D2D1_RECT_F sepRect = D2D1::RectF(m_panelRect.left, sepY, m_panelRect.right, sepY + 1.0f);
            pRT->FillRectangle(sepRect, m_brushThumbnailBg.Get());
        } else if (m_panelSide == 1) {
            float sepX = m_panelRect.right - 1.0f;
            D2D1_RECT_F sepRect = D2D1::RectF(sepX, m_panelRect.top, sepX + 1.0f, m_panelRect.bottom);
            pRT->FillRectangle(sepRect, m_brushThumbnailBg.Get());
        } else {
            float sepX = m_panelRect.left;
            D2D1_RECT_F sepRect = D2D1::RectF(sepX, m_panelRect.top, sepX + 1.0f, m_panelRect.bottom);
            pRT->FillRectangle(sepRect, m_brushThumbnailBg.Get());
        }
    }

    // Resize handle visual indicator
    if (m_brushResizeHandle && (m_resizeHover || m_isResizing)) {
        if (m_panelSide == 3) {
            float handleY = m_panelRect.top;
            D2D1_RECT_F handleRect = D2D1::RectF(m_panelRect.left, handleY, m_panelRect.right, handleY + kResizeHitWidth);
            pRT->FillRectangle(handleRect, m_brushResizeHandle.Get());
        } else {
            float handleX;
            if (m_panelSide == 1) {
                handleX = m_panelRect.right - kResizeHitWidth;
            } else {
                handleX = m_panelRect.left;
            }
            D2D1_RECT_F handleRect = D2D1::RectF(handleX, m_panelRect.top, handleX + kResizeHitWidth, m_panelRect.bottom);
            pRT->FillRectangle(handleRect, m_brushResizeHandle.Get());
        }
    }

    // Clip to panel area and draw items
    pRT->PushAxisAlignedClip(m_panelRect, D2D1_ANTIALIAS_MODE_ALIASED);
    DrawItems(pRT);
    pRT->PopAxisAlignedClip();

    // Hover tooltip floats above everything inside the panel
    DrawTooltip(pRT);
}

// ============================================================================
// Interaction
// ============================================================================

bool ThumbnailPanelBase::OnMouseMove(float x, float y) {
    if (!m_visible) return false;

    bool wasResizeHover = m_resizeHover;
    m_resizeHover = IsResizeHit(x, y);

    int newHover = -1;
    if (x >= m_panelRect.left && x <= m_panelRect.right &&
        y >= m_panelRect.top && y <= m_panelRect.bottom) {
        if (m_panelSide == 3) {
            const float itemWidth = BottomItemStride();
            const float localX = x - m_panelRect.left - kItemPadding + m_scrollX;
            if (localX >= 0.0f && y >= m_panelRect.top + kItemPadding) {
                newHover = static_cast<int>(localX / itemWidth);
                uint32_t itemCount = GetItemCount();
                if (newHover < 0 || newHover >= static_cast<int>(itemCount)) {
                    newHover = -1;
                }
            }
        } else {
            const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
            const float titleH = kTitleBarHeight * g_uiScale;
            const float localY = y - m_panelRect.top - kItemPadding - titleH + m_scrollY;
            if (localY >= 0.0f) {
                newHover = static_cast<int>(localY / itemHeight);
                uint32_t itemCount = GetItemCount();
                if (newHover < 0 || newHover >= static_cast<int>(itemCount)) {
                    newHover = -1;
                }
            }
        }
    }

    if (newHover != m_hoverIndex || m_resizeHover != wasResizeHover) {
        if (newHover != m_hoverIndex) {
            HideTooltip();
            if (newHover >= 0 && m_hwnd) {
                SetTimer(m_hwnd, (UINT_PTR)this, 500, nullptr); // tooltip delay
            }
        }
        m_hoverIndex = newHover;
        return true;
    }
    return false;
}

// --- Hover tooltip ----------------------------------------------------------

bool ThumbnailPanelBase::OnTooltipTimer(WPARAM wParam) {
    if (wParam != (WPARAM)(UINT_PTR)this) return false;
    if (m_hwnd) KillTimer(m_hwnd, (UINT_PTR)this);
    if (m_visible && m_hoverIndex >= 0 && (int)GetItemCount() > m_hoverIndex) {
        m_tooltipVisible = true;
        return true;
    }
    return false;
}

void ThumbnailPanelBase::HideTooltip() {
    m_tooltipVisible = false;
    if (m_hwnd) KillTimer(m_hwnd, (UINT_PTR)this);
}

void ThumbnailPanelBase::DrawTooltip(ID2D1RenderTarget* pRT) {
    if (!m_tooltipVisible || m_hoverIndex < 0) return;
    if (!m_brushTipBg || !m_brushBorder || !m_textFormatTipName || !m_textFormatTipPath) return;

    const std::wstring name = GetItemLabel((uint32_t)m_hoverIndex);
    std::wstring path = GetItemFullPath((uint32_t)m_hoverIndex);
    if (name.empty() && path.empty()) return;
    if (path == name) path.clear(); // nothing new to show under the name

    // Measure both lines for a snug tooltip box
    const float padX = 8.0f * g_uiScale, padY = 5.0f * g_uiScale;
    const float lineGap = 2.0f * g_uiScale;
    const float maxW = 340.0f * g_uiScale;
    auto measure = [&](IDWriteTextFormat* fmt, const std::wstring& s) -> float {
        if (!fmt || s.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(m_dwriteFactory->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt,
                                                     maxW, 100.0f * g_uiScale, &layout))) return 0.0f;
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    };
    float nameW = measure(m_textFormatTipName.Get(), name);
    float pathW = measure(m_textFormatTipPath.Get(), path);
    float boxW = std::max(nameW, pathW) + padX * 2.0f;
    float boxH = padY * 2.0f + (nameW > 0.0f ? 16.0f * g_uiScale : 0.0f) +
                 (pathW > 0.0f ? 14.0f * g_uiScale + (nameW > 0.0f ? lineGap : 0.0f) : 0.0f);

    // Anchor under the hovered item, centered, clamped to the render target
    const D2D1_SIZE_F rtSize = pRT->GetSize();
    const D2D1_RECT_F itemRect = GetItemRect((uint32_t)m_hoverIndex);
    float x = (itemRect.left + itemRect.right) * 0.5f - boxW * 0.5f;
    float y = itemRect.bottom + 4.0f * g_uiScale;
    x = std::max(2.0f, std::min(x, rtSize.width - boxW - 2.0f));
    if (y + boxH > rtSize.height - 2.0f) y = itemRect.top - boxH - 4.0f * g_uiScale; // flip above
    y = std::max(2.0f, y);

    D2D1_ROUNDED_RECT tipRect = D2D1::RoundedRect(D2D1::RectF(x, y, x + boxW, y + boxH),
                                                  4.0f * g_uiScale, 4.0f * g_uiScale);
    pRT->FillRoundedRectangle(tipRect, m_brushTipBg.Get());
    pRT->DrawRoundedRectangle(tipRect, m_brushBorder.Get(), 1.0f);

    float ty = y + padY;
    if (nameW > 0.0f && m_brushText) {
        D2D1_RECT_F r = D2D1::RectF(x + padX, ty, x + padX + nameW + 2.0f, ty + 16.0f * g_uiScale);
        pRT->DrawText(name.c_str(), (UINT32)name.size(), m_textFormatTipName.Get(), r, m_brushText.Get());
        ty += 16.0f * g_uiScale + lineGap;
    }
    if (pathW > 0.0f && !path.empty()) {
        D2D1_RECT_F r = D2D1::RectF(x + padX, ty, x + padX + pathW + 2.0f, ty + 14.0f * g_uiScale);
        pRT->DrawText(path.c_str(), (UINT32)path.size(), m_textFormatTipPath.Get(), r, m_brushTipDim.Get());
    }
}

int ThumbnailPanelBase::HitTestItem(float x, float y) const {
    if (!m_visible) return -1;

    if (x < m_panelRect.left || x > m_panelRect.right ||
        y < m_panelRect.top || y > m_panelRect.bottom) {
        return -1;
    }

    if (m_panelSide == 3) {
        const float itemWidth = BottomItemStride();
        const float localX = x - m_panelRect.left - kItemPadding + m_scrollX;
        if (localX < 0.0f || y < m_panelRect.top + kItemPadding) return -1;
        const int pageIndex = static_cast<int>(localX / itemWidth);
        uint32_t itemCount = GetItemCount();
        if (pageIndex >= 0 && pageIndex < static_cast<int>(itemCount)) {
            return pageIndex;
        }
        return -1;
    }

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float titleH = kTitleBarHeight * g_uiScale;
    const float localY = y - m_panelRect.top - kItemPadding - titleH + m_scrollY;
    if (localY < 0.0f) return -1;

    const int pageIndex = static_cast<int>(localY / itemHeight);
    uint32_t itemCount = GetItemCount();
    if (pageIndex >= 0 && pageIndex < static_cast<int>(itemCount)) {
        return pageIndex;
    }
    return -1;
}

int ThumbnailPanelBase::OnLButtonDown(float x, float y) {
    if (!m_visible) return -1;
    HideTooltip();
    const int pageIndex = HitTestItem(x, y);
    if (pageIndex >= 0) {
        return OnItemClick(pageIndex);
    }
    return -1;
}

bool ThumbnailPanelBase::OnMouseWheel(int delta) {
    if (!m_visible) return false;
    HideTooltip();

    if (m_panelSide == 3) {
        if (m_maxScrollX <= 0.0f) return false;
        const float scrollStep = 60.0f * g_uiScale;
        m_targetScrollX -= static_cast<float>(delta / 120) * scrollStep;
        m_targetScrollX = std::max(0.0f, std::min(m_targetScrollX, m_maxScrollX));
        return true;
    }

    if (m_maxScrollY <= 0.0f) return false;
    const float scrollStep = 60.0f * g_uiScale;
    m_targetScrollY -= static_cast<float>(delta / 120) * scrollStep;
    m_targetScrollY = std::max(0.0f, std::min(m_targetScrollY, m_maxScrollY));
    return true;
}

// ============================================================================
// Resize
// ============================================================================

bool ThumbnailPanelBase::IsResizeHit(float x, float y) const {
    if (!m_visible) return false;

    if (m_panelSide == 3) {
        if (x < m_panelRect.left || x > m_panelRect.right) return false;
        return y >= m_panelRect.top - kResizeHitWidth && y <= m_panelRect.top + kResizeHitWidth;
    }

    if (y < m_panelRect.top || y > m_panelRect.bottom) return false;

    if (m_panelSide == 1) {
        return x >= m_panelRect.right - kResizeHitWidth && x <= m_panelRect.right + kResizeHitWidth;
    } else {
        return x >= m_panelRect.left - kResizeHitWidth && x <= m_panelRect.left + kResizeHitWidth;
    }
}

void ThumbnailPanelBase::BeginResize(float x, float y) {
    m_isResizing = true;
    m_resizeStartX = (m_panelSide == 3) ? y : x;
    m_resizeStartWidth = (m_panelSide == 3) ? m_panelHeightUser : m_panelWidth;
}

void ThumbnailPanelBase::UpdateResize(float x, float y, [[maybe_unused]] float windowWidth) {
    if (!m_isResizing) return;

    if (m_panelSide == 3) {
        float dy = y - m_resizeStartX;
        float newHeight = m_resizeStartWidth - dy;
        newHeight = std::clamp(newHeight, kMinPanelHeightBottom, kMaxPanelHeightBottom);
        if (newHeight != m_panelHeightUser) {
            m_panelHeightUser = newHeight;
        }
        return;
    }

    float dx = x - m_resizeStartX;
    float newWidth;

    if (m_panelSide == 1) {
        newWidth = m_resizeStartWidth + dx;
    } else {
        newWidth = m_resizeStartWidth - dx;
    }

    newWidth = std::clamp(newWidth, kMinPanelWidth, kMaxPanelWidth);
    if (newWidth != m_panelWidth) {
        m_panelWidth = newWidth;
    }
}

void ThumbnailPanelBase::EndResize() {
    m_isResizing = false;
}

bool ThumbnailPanelBase::HitTestPanel(float x, float y) const {
    if (!m_visible) return false;
    if (IsResizeHit(x, y)) return true;
    return x >= m_panelRect.left && x <= m_panelRect.right &&
           y >= m_panelRect.top && y <= m_panelRect.bottom;
}
