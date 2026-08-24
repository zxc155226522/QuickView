#include "pch.h"
#include "ImageViewportLayout.h"
#include "AppContext.h"
#include "GalleryOverlay.h"
#include "Toolbar.h"
#include "PageThumbnailPanel.h"

extern float g_uiScale;
extern GalleryOverlay g_gallery;
extern PageThumbnailPanel g_thumbnailPanel;  // [PDF Sidebar]
extern AppConfig g_config;

ImageViewportLayout ComputeImageViewportLayout(float windowWidth, float windowHeight) {
    const float safeWidth = (std::max)(1.0f, windowWidth);
    const float safeHeight = (std::max)(1.0f, windowHeight);
    const float padding = 0.0f;
    const float titleBarHeight = g_isFullScreen ? 0.0f : 36.0f * g_uiScale;
    const float galleryHeight = (!g_isFullScreen && g_gallery.IsPinned() && g_gallery.IsVisible())
        ? g_gallery.GetVisualHeight(safeHeight)
        : 0.0f;

    // [PDF Sidebar] Reserve space when thumbnail panel is visible
    const float sidebarWidth = g_thumbnailPanel.IsVisible() ? g_thumbnailPanel.GetWidth() : 0.0f;
    // Panel side: 0=Right (default), 1=Left
    const int panelSide = g_thumbnailPanel.GetPanelSide();

    // Always reserve bottom space for toolbar (non-fullscreen only)
    float toolbarReservedHeight = 0.0f;
    if (!g_isFullScreen && !g_toolbar.IsWindowTooNarrow()) {
        toolbarReservedHeight = g_toolbar.GetReservedHeight();
    }

    // Image area: top = titleBar + gallery + padding, bottom = window - toolbar height
    // Toolbar is docked at bottom with same height as title bar (36px)
    float horizontalMargin = g_isFullScreen ? 0.0f : padding;
    float topMargin = g_isFullScreen ? 0.0f : titleBarHeight + galleryHeight + padding;
    float bottomReserved = g_isFullScreen ? 0.0f : toolbarReservedHeight;

    ImageViewportLayout layout;
    if (panelSide == 1) {
        // Panel on LEFT: sidebar takes left space
        layout.Left = (std::min)(horizontalMargin, safeWidth - 1.0f) + sidebarWidth;
        layout.Right = (std::max)(layout.Left + 1.0f, safeWidth - horizontalMargin);
    } else {
        // Panel on RIGHT (default): sidebar takes right space
        layout.Left = (std::min)(horizontalMargin, safeWidth - 1.0f);
        layout.Right = (std::max)(layout.Left + 1.0f, safeWidth - horizontalMargin - sidebarWidth);
    }
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
