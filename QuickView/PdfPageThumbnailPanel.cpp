#include "pch.h"
#include "PdfPageThumbnailPanel.h"
#include "AppContext.h"
#include "ImageLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

extern float g_uiScale;
extern AppConfig g_config;

PdfPageThumbnailPanel::~PdfPageThumbnailPanel() {
    DiscardDeviceResources();
}

void PdfPageThumbnailPanel::InitializeEx(HWND hwnd, QuickView::DocumentRenderController* controller) {
    Initialize(hwnd);
    m_controller = controller;
}

void PdfPageThumbnailPanel::OnDocumentOpened(const std::wstring& path, uint32_t totalPages) {
    m_currentPath = path;
    if (totalPages <= 1) {
        OnDocumentClosed();
        return;
    }
    m_visible = true;
    m_totalPages = totalPages;
    m_currentPage = 0;
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
    m_scrollX = 0.0f;
    m_targetScrollX = 0.0f;
    m_pendingFrames.clear();
    ScrollToCurrentPage(true);
}

void PdfPageThumbnailPanel::OnDocumentClosed() {
    m_visible = false;
    m_totalPages = 0;
    m_currentPage = 0;
    m_slots.clear();
    m_pendingFrames.clear();
    m_scrollY = 0.0f;
    m_targetScrollY = 0.0f;
    m_scrollX = 0.0f;
    m_targetScrollX = 0.0f;
    m_currentPath.clear();
    if (m_controller) {
        m_controller->CancelThumbnails();
    }
}

void PdfPageThumbnailPanel::SetCurrentPage(uint32_t page) {
    if (page >= m_totalPages) return;
    if (m_currentPage != page) {
        m_currentPage = page;
        ScrollToCurrentPage(false); // keep the selected page centered
    }
}

ComPtr<ID2D1Bitmap> PdfPageThumbnailPanel::CreateBitmapFromFrame(ID2D1RenderTarget* pRT, const QuickView::RawImageFrame& frame) {
    if (!pRT || frame.width <= 0 || frame.height <= 0 || !frame.pixels) return nullptr;

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    D2D1_SIZE_U size = D2D1::SizeU(frame.width, frame.height);

    ComPtr<ID2D1Bitmap> bmp;
    HRESULT hr = pRT->CreateBitmap(size, frame.pixels, frame.stride, &props, &bmp);
    return SUCCEEDED(hr) ? bmp : nullptr;
}

ComPtr<ID2D1Bitmap> PdfPageThumbnailPanel::GetItemBitmap(uint32_t index) {
    if (index >= m_slots.size()) return nullptr;
    return m_slots[index].bitmap;
}

std::wstring PdfPageThumbnailPanel::GetItemLabel(uint32_t index) const {
    wchar_t label[32];
    swprintf_s(label, L"第 %u 页", index + 1);
    return label;
}

std::wstring PdfPageThumbnailPanel::GetItemFullPath([[maybe_unused]] uint32_t index) const {
    return m_currentPath;
}

void PdfPageThumbnailPanel::OnDeviceResourcesCreated() {
    // Nothing extra needed — base class creates all shared brushes
}

void PdfPageThumbnailPanel::OnDeviceResourcesDiscarded() {
    // RT changed — all D2D bitmaps are invalid
    for (auto& slot : m_slots) {
        slot.bitmap.Reset();
        slot.needsRender = true;
        slot.isRendering = false;
    }
}

void PdfPageThumbnailPanel::OnLayoutChanged() {
    // Layout changed — no special action needed
}

bool PdfPageThumbnailPanel::OnIsLoading() const {
    for (const auto& slot : m_slots) {
        if (slot.isRendering || slot.needsRender) return true;
    }
    return false;
}

void PdfPageThumbnailPanel::DrawItems(ID2D1RenderTarget* pRT) {
    if (!m_visible || !pRT) return;
    uint32_t itemCount = m_totalPages;
    if (itemCount == 0) return;

    // Convert pending frames to D2D bitmaps
    if (!m_pendingFrames.empty()) {
        for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end(); ) {
            if (it->first < m_slots.size() && it->first < m_totalPages && !m_slots[it->first].bitmap) {
                auto bmp = CreateBitmapFromFrame(pRT, *(it->second));
                if (bmp) {
                    m_slots[it->first].bitmap = std::move(bmp);
                }
            }
            it = m_pendingFrames.erase(it);
        }
    }

    const float itemHeight = kThumbnailTargetHeight * g_uiScale + kPageLabelHeight * g_uiScale + kItemSpacing;
    const float itemWidthBottom = BottomItemStride();
    const int startPage = m_panelSide == 3
        ? std::max(0, static_cast<int>(m_scrollX / itemWidthBottom) - 1)
        : std::max(0, static_cast<int>(m_scrollY / itemHeight) - 1);
    const int endPage = m_panelSide == 3
        ? std::min(static_cast<int>(itemCount),
            static_cast<int>((m_scrollX + m_panelWidth) / itemWidthBottom) + 2)
        : std::min(static_cast<int>(itemCount),
            static_cast<int>((m_scrollY + m_panelHeight) / itemHeight) + 2);

    for (int i = startPage; i < endPage; ++i) {
        const uint32_t pageIndex = static_cast<uint32_t>(i);
        const D2D1_RECT_F itemRect = GetItemRect(pageIndex);
        if (m_panelSide == 3) {
            if (itemRect.right < m_panelRect.left || itemRect.left > m_panelRect.right) continue;
        } else {
            if (itemRect.bottom < m_panelRect.top || itemRect.top > m_panelRect.bottom) continue;
        }

        const bool isCurrentPage = (pageIndex == m_currentPage);
        const bool isHovered = (static_cast<int>(pageIndex) == m_hoverIndex);

        // Hover background
        if (!isCurrentPage && isHovered && m_brushHover) {
            D2D1_ROUNDED_RECT hovRect = D2D1::RoundedRect(itemRect, 4.0f * g_uiScale, 4.0f * g_uiScale);
            pRT->FillRoundedRectangle(hovRect, m_brushHover.Get());
        }

        // Thumbnail image
        const D2D1_RECT_F thumbRect = GetThumbnailRect(itemRect);
        ID2D1Bitmap* pBitmap = nullptr;
        if (pageIndex < m_slots.size() && m_slots[pageIndex].bitmap) {
            pBitmap = m_slots[pageIndex].bitmap.Get();
        }

        if (pBitmap) {
            // Letterbox inside the square cell — aspect preserved, no distortion
            const D2D1_SIZE_F bs = pBitmap->GetSize();
            const D2D1_RECT_F drawRect = FitRectInside(thumbRect, bs.width, bs.height);
            pRT->DrawBitmap(pBitmap, drawRect, 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            if (m_brushThumbnailBg) {
                D2D1_ROUNDED_RECT bgRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
                pRT->FillRoundedRectangle(bgRect, m_brushThumbnailBg.Get());
            }
        }

        // Current page border (blue)
        if (isCurrentPage && m_brushBorder) {
            D2D1_ROUNDED_RECT borderRect = D2D1::RoundedRect(thumbRect, 3.0f * g_uiScale, 3.0f * g_uiScale);
            pRT->DrawRoundedRectangle(borderRect, m_brushBorder.Get(), 2.0f * g_uiScale);
        }

        // Page label (single line, ellipsis when too long)
        if (m_textFormatLabel && m_brushText) {
            std::wstring label = GetItemLabel(pageIndex);
            D2D1_RECT_F labelRect = D2D1::RectF(itemRect.left, thumbRect.bottom + 2.0f * g_uiScale,
                                                 itemRect.right, itemRect.bottom);
            pRT->DrawText(label.c_str(), static_cast<UINT32>(label.size()), m_textFormatLabel.Get(),
                         labelRect, m_brushText.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
}

int PdfPageThumbnailPanel::OnItemClick(int index) {
    // Return the page index — main.cpp will handle the actual navigation
    return index;
}

void PdfPageThumbnailPanel::OnUpdateThumbnailRequests() {
    if (!m_visible || m_totalPages == 0) return;
    if (!m_controller) return;

    if (m_panelSide == 3) {
        // Bottom Mode: Horizontal visible range
        const float itemWidth = BottomItemStride();
        const int visibleStart = static_cast<int>(std::max(0.0f, m_scrollX / itemWidth)) - 2;
        const int visibleEnd = static_cast<int>((m_scrollX + m_panelWidth) / itemWidth) + 3;

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
        return;
    }

    // Side mode: Vertical visible range
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

void PdfPageThumbnailPanel::ProcessThumbnailResults() {
    if (!m_visible || !m_controller) return;

    QuickView::ThumbnailResult result;
    while (m_controller->TakeThumbnailResult(result)) {
        if (result.pageIndex >= m_slots.size()) continue;
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

void PdfPageThumbnailPanel::ConvertPendingFrames(ID2D1RenderTarget* pRT) {
    if (m_pendingFrames.empty()) return;
    for (auto it = m_pendingFrames.begin(); it != m_pendingFrames.end(); ) {
        if (it->first < m_slots.size() && it->first < m_totalPages && !m_slots[it->first].bitmap) {
            auto bmp = CreateBitmapFromFrame(pRT, *(it->second));
            if (bmp) {
                m_slots[it->first].bitmap = std::move(bmp);
            }
        }
        it = m_pendingFrames.erase(it);
    }
}
