#include "pch.h"
#include "ImageViewportLayout.h"
#include "AppContext.h"
#include "GalleryOverlay.h"
#include "Toolbar.h"
#include "ThumbnailPanelBase.h"
#include "PdfPageThumbnailPanel.h"
#include "ImageListThumbnailPanel.h"

extern float g_uiScale;
extern GalleryOverlay g_gallery;
extern PdfPageThumbnailPanel g_pdfThumbPanel;
extern ImageListThumbnailPanel g_imageThumbPanel;
extern AppConfig g_config;

ImageViewportLayout ComputeImageViewportLayout(float windowWidth, float windowHeight) {
    const float safeWidth = (std::max)(1.0f, windowWidth);
    const float safeHeight = (std::max)(1.0f, windowHeight);
    const float padding = 0.0f;
    const float titleBarHeight = g_isFullScreen ? 0.0f : 36.0f * g_uiScale;
    const float galleryHeight = (!g_isFullScreen && g_gallery.IsPinned() && g_gallery.IsVisible())
        ? g_gallery.GetVisualHeight(safeHeight)
        : 0.0f;

    // [Split Panels] Calculate sidebar widths for both panels
    float leftSidebarWidth = 0.0f;
    float rightSidebarWidth = 0.0f;
    float bottomPanelHeight = 0.0f;

    // PDF panel
    if (g_pdfThumbPanel.IsVisible()) {
        int side = g_pdfThumbPanel.GetPanelSide();
        if (side == 1) leftSidebarWidth += g_pdfThumbPanel.GetWidth();
        else if (side == 0) rightSidebarWidth += g_pdfThumbPanel.GetWidth();
        else if (side == 3) bottomPanelHeight = (std::max)(bottomPanelHeight, g_pdfThumbPanel.GetBottomPanelHeight());
    }

    // Image panel
    if (g_imageThumbPanel.IsVisible()) {
        int side = g_imageThumbPanel.GetPanelSide();
        if (side == 1) leftSidebarWidth += g_imageThumbPanel.GetWidth();
        else if (side == 0) rightSidebarWidth += g_imageThumbPanel.GetWidth();
        else if (side == 3) bottomPanelHeight = (std::max)(bottomPanelHeight, g_imageThumbPanel.GetBottomPanelHeight());
    }

    // Always reserve bottom space for toolbar (non-fullscreen only)
    float toolbarReservedHeight = 0.0f;
    if (!g_isFullScreen && !g_toolbar.IsWindowTooNarrow()) {
        toolbarReservedHeight = g_toolbar.GetReservedHeight();
    }

    // Image area
    float horizontalMargin = g_isFullScreen ? 0.0f : padding;
    float topMargin = g_isFullScreen ? 0.0f : titleBarHeight + galleryHeight + padding;
    float bottomReserved = g_isFullScreen ? 0.0f : toolbarReservedHeight + bottomPanelHeight;

    ImageViewportLayout layout;
    // Left margin includes left sidebar(s)
    layout.Left = (std::min)(horizontalMargin, safeWidth - 1.0f) + leftSidebarWidth;
    // Right margin includes right sidebar(s)
    layout.Right = (std::max)(layout.Left + 1.0f, safeWidth - horizontalMargin - rightSidebarWidth);
    layout.Top = (std::min)(topMargin, safeHeight - 1.0f);
    layout.Bottom = (std::max)(layout.Top + 1.0f, safeHeight - bottomReserved);
    layout.Right = (std::min)(layout.Right, safeWidth);
    layout.Bottom = (std::min)(layout.Bottom, safeHeight);
    layout.Width = (std::max)(1.0f, layout.Right - layout.Left);
    layout.Height = (std::max)(1.0f, layout.Bottom - layout.Top);
    layout.CenterOffsetX = (layout.Left + layout.Right) * 0.5f - safeWidth * 0.5f;
    layout.CenterOffsetY = (layout.Top + layout.Bottom) * 0.5f - safeHeight * 0.5f;
    return layout;
}
