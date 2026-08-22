#include "pch.h"
#include "PageThumbnailPanel.h"
#include "ImageEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>

extern float g_uiScale;

// [Debug] Diagnostic tracing via file log
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
    // Also write to file for debugging without DebugView
    static FILE* _fp = nullptr;
    if (!_fp) {
        _wfopen_s(&_fp, L"E:\\qv_thumb_debug.log", L"a");
    }
    if (_fp) {
        fputws(_buf, _fp);
        fflush(_fp);
    }
}
#define THUMB_DBG(fmt, ...) ThumbDbgLog(L"[ThumbPanel] " fmt, __VA_ARGS__)

PageThumbnailPanel::~PageThumbnailPanel() {
    DiscardDeviceResources();
}

void PageThumbnailPanel::Initialize(HWND hwnd, QuickView::DocumentRenderController* controller) {
    m_hwnd = hwnd;
    m_controller = controller;
}

void PageThumbnailPanel::Show(uint32_t totalPages, uint32_t currentPage) {
    THUMB_DBG(L"Show(totalPages=%u, currentPage=%u)", totalPages, currentPage);
    if (totalPages <= 1) {
        THUMB_DBG(L"Show -> Hide (totalPages <= 1)", 0, 0);
        Hide();
        return;
    }
    m_visible = true;
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
    if (m_controller) {
        m_controller->CancelThumbnails();
    }
}

void PageThumbnailPanel::SetCurrentPage(uint32_t page) {
    if (page >= m_totalPages) return;
    if (m_currentPage != page) {
        m_currentPage = page;
        ScrollToCurrentPage(false);
    }
}

void PageThumbnailPanel::OnDocumentOpened(const std::wstring& path, uint32_t totalPages) {
    THUMB_DBG(L"OnDocumentOpened(path=%ls, totalPages=%u)", path.c_str(), totalPages);
    m_currentPath = path;
    if (totalPages > 1) {
        Show(totalPages, 0);
    }
}

void PageThumbnailPanel::OnDocumentClosed() {
    Hide();
    m_currentPath.clear();
}

void PageThumbnailPanel::UpdateLayout(const D2D1_RECT_F& clientRect) {
    m_panelWidth = kDefaultPanelWidth * g_uiScale;
    m_panelWidth = std::clamp(m_panelWidth, kMinPanelWidth, kMaxPanelWidth);

    m_panelRect.left = 0.0f;
    m_panelRect.top = 0.0f;
    m_panelRect.right = m_panelWidth;
    m_panelRect.bottom = clientRect.bottom - clientRect.top;
    m_panelHeight = m_panelRect.bottom - m_panelRect.top;

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float contentHeight = static_cast<float>(m_totalPages) * itemHeight;
    m_maxScrollY = std::max(0.0f, contentHeight - m_panelHeight + kItemPadding * 2.0f);

    if (m_scrollY > m_maxScrollY) m_scrollY = m_maxScrollY;
    if (m_targetScrollY > m_maxScrollY) m_targetScrollY = m_maxScrollY;
}

void PageThumbnailPanel::CreateDeviceResources(ID2D1RenderTarget* pRT) {
    THUMB_DBG(L"CreateDeviceResources(pRT=%p)", pRT, 0);
    if (!pRT) return;

    pRT->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.12f), &m_brushBg);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f, 0.35f), &m_brushSelection);
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f), &m_brushHover);
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), &m_brushText);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.2f, 0.2f), &m_brushThumbnailBg);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f), &m_brushBorder);

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
    m_brushBg.Reset();
    m_brushSelection.Reset();
    m_brushHover.Reset();
    m_brushText.Reset();
    m_brushThumbnailBg.Reset();
    m_brushBorder.Reset();
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
    if (m_totalPages == 0 || m_panelHeight <= 0.0f) return;
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

void PageThumbnailPanel::Render(ID2D1RenderTarget* pRT) {
    if (!m_visible || !pRT || m_totalPages == 0) return;
    THUMB_DBG(L"Render(panelW=%.1f, pages=%u, current=%u, slots=%zu)", m_panelWidth, m_totalPages, m_currentPage, m_slots.size());

    // Smooth scroll animation
    if (std::abs(m_scrollY - m_targetScrollY) > 0.5f) {
        m_scrollY += (m_targetScrollY - m_scrollY) * 0.25f;
    } else {
        m_scrollY = m_targetScrollY;
    }

    // Convert any pending frames to D2D bitmaps
    if (!m_pendingFrames.empty()) {
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

    // Separator line on right edge
    if (m_brushThumbnailBg) {
        D2D1_RECT_F sepRect = D2D1::RectF(m_panelRect.right - 1.0f, m_panelRect.top,
                                            m_panelRect.right, m_panelRect.bottom);
        pRT->FillRectangle(sepRect, m_brushThumbnailBg.Get());
    }

    // Clip to panel area
    pRT->PushAxisAlignedClip(m_panelRect, D2D1_ANTIALIAS_MODE_ALIASED);

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const uint32_t startPage = static_cast<uint32_t>(std::max(0.0f, m_scrollY / itemHeight)) - 1;
    const uint32_t endPage = std::min(m_totalPages,
        static_cast<uint32_t>((m_scrollY + m_panelHeight) / itemHeight) + 2);

    for (uint32_t i = startPage; i < endPage; ++i) {
        const D2D1_RECT_F itemRect = GetItemRect(i);
        if (itemRect.bottom < m_panelRect.top || itemRect.top > m_panelRect.bottom) continue;

        const bool isCurrentPage = (i == m_currentPage);
        const bool isHovered = (static_cast<int>(i) == m_hoverIndex);

        // Selection/hover background
        if (isCurrentPage && m_brushSelection) {
            D2D1_ROUNDED_RECT selRect = D2D1::RoundedRect(itemRect, 4.0f * g_uiScale, 4.0f * g_uiScale);
            pRT->FillRoundedRectangle(selRect, m_brushSelection.Get());
        } else if (isHovered && m_brushHover) {
            D2D1_ROUNDED_RECT hovRect = D2D1::RoundedRect(itemRect, 4.0f * g_uiScale, 4.0f * g_uiScale);
            pRT->FillRoundedRectangle(hovRect, m_brushHover.Get());
        }

        // Thumbnail image
        const D2D1_RECT_F thumbRect = GetThumbnailRect(itemRect);
        if (m_slots[i].bitmap) {
            pRT->DrawBitmap(m_slots[i].bitmap.Get(), thumbRect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else if (m_brushThumbnailBg) {
            D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
            pRT->FillRoundedRectangle(bgRect, m_brushThumbnailBg.Get());
        }

        // Current page border
        if (isCurrentPage && m_brushBorder) {
            D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
            pRT->DrawRoundedRectangle(borderRect, m_brushBorder.Get(), 2.0f * g_uiScale);
        }

        // Page label
        if (m_textFormatPage && m_brushText) {
            wchar_t label[16];
            swprintf_s(label, L"%u", i + 1);
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

    int newHover = -1;
    if (x >= m_panelRect.left && x <= m_panelRect.right &&
        y >= m_panelRect.top && y <= m_panelRect.bottom) {
        const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
        const float localY = y - m_panelRect.top - kItemPadding + m_scrollY;
        if (localY >= 0.0f) {
            newHover = static_cast<int>(localY / itemHeight);
            if (newHover < 0 || newHover >= static_cast<int>(m_totalPages)) {
                newHover = -1;
            }
        }
    }

    if (newHover != m_hoverIndex) {
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
    if (pageIndex >= 0 && pageIndex < static_cast<int>(m_totalPages)) {
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

void PageThumbnailPanel::UpdateThumbnailRequests() {
    if (!m_visible || !m_controller || m_totalPages == 0) return;

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
    if (!m_visible || !m_controller) return;

    QuickView::ThumbnailResult result;
    while (m_controller->TakeThumbnailResult(result)) {
        if (result.status == S_OK && result.frame && result.frame->IsValid()) {
            if (result.pageIndex < m_totalPages) {
                auto& slot = m_slots[result.pageIndex];
                slot.isRendering = false;
                slot.needsRender = false;

                if (!slot.bitmap) {
                    m_pendingFrames[result.pageIndex] = std::move(result.frame);
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
