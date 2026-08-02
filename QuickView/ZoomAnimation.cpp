#include "pch.h"
#include "ZoomAnimation.h"
#include "QuickView.h"
#include "PaneContext.h"
#include "RenderEngine.h"
#include "CompositionEngine.h"
#include "CompareController.h"
#include "ImageViewportLayout.h"

extern std::atomic<uint64_t> g_currentImageId;
extern CompositionEngine* g_compEngine;
extern PaneContext g_panes[2];

static bool StepRectTowardTarget(RECT& current, const RECT& target, float alpha) {
    auto stepEdge = [alpha](LONG currentValue, LONG targetValue) -> LONG {
        const LONG diff = targetValue - currentValue;
        if (diff == 0) return currentValue;
        if (std::abs(diff) <= 1) return targetValue;
        const LONG step = static_cast<LONG>(std::round(static_cast<double>(diff) * static_cast<double>(alpha)));
        if (step == 0) {
            return currentValue + (diff > 0 ? 1 : -1);
        }
        return currentValue + step;
    };

    const RECT start = current;
    current.left = stepEdge(start.left, target.left);
    current.top = stepEdge(start.top, target.top);
    current.right = stepEdge(start.right, target.right);
    current.bottom = stepEdge(start.bottom, target.bottom);

    return memcmp(&start, &current, sizeof(RECT)) != 0;
}

static POINT GetResolvedAnchorScreenPoint(HWND hwnd, const POINT* anchorScreenPt) {
    if (anchorScreenPt) {
        return *anchorScreenPt;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT centerPt = { rc.right / 2, rc.bottom / 2 };
    ClientToScreen(hwnd, &centerPt);
    return centerPt;
}

static float ComputeBaseFitScaleForVisual(const VisualState& vs, float winW, float winH) {
    if (vs.VisualSize.width <= 0 || vs.VisualSize.height <= 0 || winW <= 0 || winH <= 0) return 1.0f;
    return std::min(winW / vs.VisualSize.width, winH / vs.VisualSize.height);
}

SmoothZoomController::SmoothZoomController(AppContext& context) : m_context(context) {}

bool SmoothZoomController::IsActive() const {
    return m_context.SmoothZoom.Active;
}

void SmoothZoomController::Reset() {
    m_context.SmoothZoom.Reset();
}

void SmoothZoomController::ResolvePan(HWND hwnd, float zoom, float& outPanX, float& outPanY) const {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float winW = (float)rc.right;
    const float winH = (float)rc.bottom;
    const ImageViewportLayout layout = ComputeImageViewportLayout(winW, winH);

    POINT anchorClient = m_context.SmoothZoom.AnchorScreenPt;
    ScreenToClient(hwnd, &anchorClient);

    const float dx = (float)anchorClient.x - (layout.Left + layout.Right) * 0.5f;
    const float dy = (float)anchorClient.y - (layout.Top + layout.Bottom) * 0.5f;
    outPanX = dx - zoom * m_context.SmoothZoom.AnchorImageX;
    outPanY = dy - zoom * m_context.SmoothZoom.AnchorImageY;
}

void SmoothZoomController::Configure(HWND hwnd,
                                     float sourceZoom,
                                     float sourcePanX,
                                     float sourcePanY,
                                     float targetZoom,
                                     float targetPanX,
                                     float targetPanY,
                                     const POINT* anchorScreenPt,
                                     bool animateWindow,
                                     const RECT* targetWindowRect) {
    RECT clientRc{};
    GetClientRect(hwnd, &clientRc);
    RECT windowRc{};
    GetWindowRect(hwnd, &windowRc);

    const POINT resolvedAnchorScreenPt = GetResolvedAnchorScreenPoint(hwnd, anchorScreenPt);
    POINT anchorClient = resolvedAnchorScreenPt;
    ScreenToClient(hwnd, &anchorClient);

    const float winW = (float)clientRc.right;
    const float winH = (float)clientRc.bottom;
    const ImageViewportLayout layout = ComputeImageViewportLayout(winW, winH);
    const float effWinH = layout.Height;

    const float safeSourceZoom = (sourceZoom > 0.0001f) ? sourceZoom : 0.0001f;
    const float dx = (float)anchorClient.x - (layout.Left + layout.Right) * 0.5f;
    const float dy = (float)anchorClient.y - (layout.Top + layout.Bottom) * 0.5f;

    m_context.SmoothZoom.Active = true;
    m_context.SmoothZoom.ImageId = g_currentImageId.load(std::memory_order_acquire);
    m_context.SmoothZoom.AnimateWindow = animateWindow;
    m_context.SmoothZoom.HasAnchor = true;
    m_context.SmoothZoom.AnchorScreenPt = resolvedAnchorScreenPt;
    m_context.SmoothZoom.SourceWindowRect = windowRc;
    m_context.SmoothZoom.TargetWindowRect = targetWindowRect ? *targetWindowRect : windowRc;
    m_context.SmoothZoom.SourceZoom = safeSourceZoom;
    m_context.SmoothZoom.CurrentZoom = sourceZoom;
    m_context.SmoothZoom.CurrentPanX = sourcePanX;
    m_context.SmoothZoom.CurrentPanY = sourcePanY;
    m_context.SmoothZoom.TargetZoom = targetZoom;
    m_context.SmoothZoom.TargetPanX = targetPanX;
    m_context.SmoothZoom.TargetPanY = targetPanY;
    m_context.SmoothZoom.AnchorImageX = (dx - sourcePanX) / safeSourceZoom;
    m_context.SmoothZoom.AnchorImageY = (dy - sourcePanY) / safeSourceZoom;
    m_context.SmoothZoom.LastWinW = winW;
    m_context.SmoothZoom.LastWinH = effWinH;
    m_context.SmoothZoom.LastTick = GetTickCount64();
}

void SmoothZoomController::SyncToLogical(float winW, float winH, bool activate) {
    if (!GetPaneContext(PaneSlot::Primary).resource || IsCompareModeActive()) {
        Reset();
        return;
    }

    extern VisualState GetVisualState();
    VisualState vs = GetVisualState();
    const ImageViewportLayout layout = ComputeImageViewportLayout(winW, winH);
    const float baseFit = ComputeBaseFitScaleForVisual(vs, layout.Width, layout.Height);
    const float targetZoom = baseFit * GetPaneContext(PaneSlot::Primary).view.Zoom;

    m_context.SmoothZoom.ImageId = g_currentImageId.load(std::memory_order_acquire);
    m_context.SmoothZoom.CurrentZoom = targetZoom;
    m_context.SmoothZoom.CurrentPanX = GetPaneContext(PaneSlot::Primary).view.PanX;
    m_context.SmoothZoom.CurrentPanY = GetPaneContext(PaneSlot::Primary).view.PanY;
    m_context.SmoothZoom.TargetZoom = targetZoom;
    m_context.SmoothZoom.TargetPanX = GetPaneContext(PaneSlot::Primary).view.PanX;
    m_context.SmoothZoom.TargetPanY = GetPaneContext(PaneSlot::Primary).view.PanY;
    m_context.SmoothZoom.LastWinW = winW;
    m_context.SmoothZoom.LastWinH = layout.Height;
    m_context.SmoothZoom.LastTick = GetTickCount64();
    m_context.SmoothZoom.Active = activate;
}

bool SmoothZoomController::Tick(HWND hwnd) {
    if (!m_context.SmoothZoom.Active || !g_compEngine || !g_compEngine->IsInitialized()) {
        return false;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    float winW = (float)rc.right;
    float winH = (float)rc.bottom;
    if (winW <= 0.0f || winH <= 0.0f) {
        Reset();
        return false;
    }

    float effWinH = ComputeImageViewportLayout(winW, winH).Height;

    const uint64_t currentImageId = g_currentImageId.load(std::memory_order_acquire);
    const bool sizeChangedUnexpectedly =
        !m_context.SmoothZoom.AnimateWindow &&
        (fabsf(m_context.SmoothZoom.LastWinW - winW) > 0.5f || fabsf(m_context.SmoothZoom.LastWinH - effWinH) > 0.5f);

    extern bool g_isInSizeMove;
    if (!GetPaneContext(PaneSlot::Primary).resource || IsCompareModeActive() || GetPaneContext(PaneSlot::Primary).view.IsDragging || g_isInSizeMove ||
        m_context.SmoothZoom.ImageId != currentImageId || sizeChangedUnexpectedly) {
        SyncToLogical(winW, winH, false);
        extern void SyncDCompState(HWND hwnd, float winW, float winH, bool animate = false);
        SyncDCompState(hwnd, winW, winH);
        g_compEngine->Commit();
        return false;
    }

    const ULONGLONG now = GetTickCount64();
    float dt = (m_context.SmoothZoom.LastTick > 0) ? (float)(now - m_context.SmoothZoom.LastTick) / 1000.0f : (1.0f / 60.0f);
    if (dt < (1.0f / 240.0f)) dt = (1.0f / 240.0f);
    if (dt > 0.05f) dt = 0.05f;
    m_context.SmoothZoom.LastTick = now;

    const float zoomAlpha = 1.0f - expf(-72.0f * dt);
    const float windowAlpha = 1.0f - expf(-50.0f * dt);
    m_context.SmoothZoom.CurrentZoom += (m_context.SmoothZoom.TargetZoom - m_context.SmoothZoom.CurrentZoom) * zoomAlpha;

    bool windowDone = true;
    extern bool g_programmaticResize;
    if (m_context.SmoothZoom.AnimateWindow) {
        RECT currentWindowRect{};
        GetWindowRect(hwnd, &currentWindowRect);
        RECT nextWindowRect = currentWindowRect;
        const bool moved = StepRectTowardTarget(nextWindowRect, m_context.SmoothZoom.TargetWindowRect, windowAlpha);
        windowDone = !moved && memcmp(&currentWindowRect, &m_context.SmoothZoom.TargetWindowRect, sizeof(RECT)) == 0;

        if (moved) {
            g_programmaticResize = true;
            SetWindowPos(hwnd, nullptr,
                         nextWindowRect.left, nextWindowRect.top,
                         nextWindowRect.right - nextWindowRect.left,
                         nextWindowRect.bottom - nextWindowRect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }

        GetClientRect(hwnd, &rc);
        winW = (float)rc.right;
        winH = (float)rc.bottom;
        effWinH = ComputeImageViewportLayout(winW, winH).Height;
    }

    ResolvePan(hwnd, m_context.SmoothZoom.CurrentZoom, m_context.SmoothZoom.CurrentPanX, m_context.SmoothZoom.CurrentPanY);
    m_context.SmoothZoom.LastWinW = winW;
    m_context.SmoothZoom.LastWinH = effWinH;

    bool done = fabsf(m_context.SmoothZoom.TargetZoom - m_context.SmoothZoom.CurrentZoom) < 0.0003f;
    if (m_context.SmoothZoom.AnimateWindow) {
        RECT currentWindowRect{};
        GetWindowRect(hwnd, &currentWindowRect);
        done = done && memcmp(&currentWindowRect, &m_context.SmoothZoom.TargetWindowRect, sizeof(RECT)) == 0 && windowDone;
    }

    if (done) {
        if (m_context.SmoothZoom.AnimateWindow) {
            g_programmaticResize = true;
            SetWindowPos(hwnd, nullptr,
                         m_context.SmoothZoom.TargetWindowRect.left, m_context.SmoothZoom.TargetWindowRect.top,
                         m_context.SmoothZoom.TargetWindowRect.right - m_context.SmoothZoom.TargetWindowRect.left,
                         m_context.SmoothZoom.TargetWindowRect.bottom - m_context.SmoothZoom.TargetWindowRect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            GetClientRect(hwnd, &rc);
            winW = (float)rc.right;
            winH = (float)rc.bottom;
            effWinH = ComputeImageViewportLayout(winW, winH).Height;
        }

        m_context.SmoothZoom.CurrentZoom = m_context.SmoothZoom.TargetZoom;
        ResolvePan(hwnd, m_context.SmoothZoom.CurrentZoom, m_context.SmoothZoom.CurrentPanX, m_context.SmoothZoom.CurrentPanY);
        m_context.SmoothZoom.LastWinW = winW;
        m_context.SmoothZoom.LastWinH = effWinH;
        GetPaneContext(PaneSlot::Primary).view.PanX = m_context.SmoothZoom.CurrentPanX;
        GetPaneContext(PaneSlot::Primary).view.PanY = m_context.SmoothZoom.CurrentPanY;
        g_programmaticResize = false;
        m_context.SmoothZoom.Active = false;
    }

    extern void SyncDCompState(HWND hwnd, float winW, float winH, bool animate = false);
    SyncDCompState(hwnd, winW, winH);
    g_compEngine->Commit();
    return m_context.SmoothZoom.Active;
}
