#pragma once
#include "pch.h"
#include <vector>
#include <string>
#include <cstdint>
#include "GeekGlass.h"
#include "GeekIconLibrary.h"

// IDs for button actions
enum class ToolbarButtonID {
    None,
    Prev, Next,
    PageFirst, PageLast, 
    RotateL, RotateR, FlipH, 
    LockSize, Gallery, 
    Exif, RawToggle,
    GamutWarning,
    Pin,
    CompareToggle,
    CompareOpen,
    CompareSwap,
    CompareLayout,
    CompareInfo,
    CompareRawToggle,
    CompareDelete,
    CompareZoomIn,
    CompareZoomOut,
    CompareSyncZoom,
    CompareSyncPan,
    CompareExit,
    // Animation mode
    AnimPlayPause,
    AnimPrevFrame,
    AnimNextFrame,
    AnimDirtyRect,
    AnimSpeedUp,
    AnimSpeedDown,
    AnimSeek,
    // [Removed Overlay] Placeholder (keep enum for index alignment)
    OverlayAlphaUp,
    OverlayAlphaDown,
    OverlayPassthrough,
    OverlayExit,
    // Slideshow mode
    SlideshowImmersiveToggle,
    SlideshowExit,
    // Swatch background
    SwatchSelect
};

struct ToolbarButton {
    ToolbarButtonID id;
    GeekIcons::IconGlyph iconGlyph;  // Pointer to vector icon data
    D2D1_RECT_F rect;       // Runtime layout rect
    bool isEnabled = true;
    bool isToggled = false; // For Lock/Exif/Raw
    bool isWarning = false; // For CompareRawToggle
    bool isHovered = false;
    bool isPaired = false;  // [RAW+JPEG Pairing] RawToggle switches a pair
};

// Responsive hide: a group of buttons hidden together at the same priority level
struct ResponsiveHideGroup {
    ToolbarButtonID ids[4]{}; // Up to 4 buttons per group, None-terminated
};

class Toolbar {
public:
    Toolbar();
    ~Toolbar();

    void Init(ID2D1RenderTarget* pRT);
    void UpdateLayout(float winW, float winH);
    void Render(ID2D1RenderTarget* pRT);
    void SetUIScale(float scale);
    // Interaction
    bool OnMouseMove(float x, float y);
    bool OnClick(float x, float y, ToolbarButtonID& outId);
    bool HitTest(float x, float y); // New method to check if point is on toolbar
    bool GetAnimSeekTarget(float& outProgress) const { 
        if (m_animProgressHover) { outProgress = m_animSeekHoverProgress; return true; }
        return false;
    }
    
    bool IsVisible() const { return m_opacity > 0.0f; }
    void SetVisible(bool visible); // Triggers animation logic external to this class?
    // actually, we can just set a target state and let UpdateAnimation be called by Timer.
    void HideImmediately() { m_opacity = 0.0f; m_targetVisible = false; }
    bool IsPinned() const { return true; } // [A块] Always pinned
    void TogglePin() { m_isPinned = true; } // [A块] No-op, always pinned
    void SetPinned(bool /*pinned*/) { m_isPinned = true; } // [A块] Always pinned regardless of argument
    
    // Animation Step (returns true if still animating)
    bool UpdateAnimation(); 

    // State Setters
    void SetLockState(bool locked);
    void SetExifState(bool open);
    void SetRawState(bool isRaw, bool isFullDecode, bool isPaired = false);
    
    void SetGamutWarningAvailable(bool available);
    void SetGamutWarningActive(bool active);
    void SetCompareMode(bool enabled);
    bool IsCompareMode() const { return m_compareMode; }
    void SetComicMode(bool enabled) { m_comicMode = enabled; }
    bool IsComicMode() const { return m_comicMode; }
    void SetCompareSyncStates(bool syncZoom, bool syncPan);
    void SetCompareInfoState(bool active);
    void SetCompareRawState(bool anyRaw, bool selectedIsRaw, bool isFullDecode);
    float GetCompareZoomStepPercent() const { return m_compareZoomStepPercent; }
    float GetOverlayZoomStepPercent() const { return m_compareZoomStepPercent; }
    
    // [v10.5] Animation Mode
    void SetAnimationMode(bool enabled, bool playing = true, bool dirtyRect = false, bool supportsDirtyRect = true);
    bool IsAnimationMode() const { return m_animMode; }
    void SetSlideshowMode(bool enabled, bool playing = true);
    bool IsSlideshowMode() const { return m_slideshowMode; }
    void SetAnimProgress(float progress) { m_animProgress = progress; }
    void SetAnimFrameInfo(uint32_t currentFrame, uint32_t totalFrames) { 
        m_currentFrame = currentFrame; 
        m_totalFrames = totalFrames; 
    }
    float GetAnimSpeedMult() const { return m_animSpeedMult; }
    void SetAnimSpeedMult(float mult) { m_animSpeedMult = mult; }
    void SetDraggingProgress(bool dragging) { m_isDraggingProgress = dragging; }

    // [Overlay Mode]
    void SetOverlayMode(bool enabled); // [Removed Overlay] no-op stub
    bool IsOverlayMode() const { return false; }
    void SetOverlayAlpha(BYTE /*alpha*/) {} // [Removed Overlay] no-op stub
    
    // [Phase 3] Get minimum required width for toolbar
    float GetMinWidth() const { return m_minRequiredWidth > 0.0f ? m_minRequiredWidth : (PADDING_X * 2 + 8 * BUTTON_SIZE + 7 * GAP) * m_uiScale; }
    bool IsWindowTooNarrow() const { return m_windowTooNarrow; }

    // Total vertical space from window bottom to toolbar top edge (including bottom margin)
    float GetReservedHeight() const;

    // [Swatch] Get the last clicked swatch index (-1 if none)
    int GetClickedSwatchIndex() const { return m_swatchClickIndex; }
    void ClearClickedSwatchIndex() { m_swatchClickIndex = -1; }

    // [Geek Glass] Data Injection
    void SetGeekGlassData(ID2D1CommandList* list, const D2D1_MATRIX_3X2_F& transform) {
        m_bgCmdList = list;
        m_bgTransform = transform;
    }

    // [PDF/AI/CDR] Page indicator in toolbar center
    void SetPageIndicator(uint32_t current, uint32_t total) {
        m_currentPage = current;
        m_totalPages = total;
        m_showPageIndicator = (total > 1);
        if (!m_showPageIndicator) m_pageInputActive = false;
    }
    void ClearPageIndicator() { m_showPageIndicator = false; m_pageInputActive = false; }
    bool IsPageIndicatorVisible() const { return m_showPageIndicator; }
    bool IsPageIndicatorHit(float x, float y) const {
        if (!m_showPageIndicator) return false;
        return x >= m_pageIndicatorRect.left && x <= m_pageIndicatorRect.right &&
               y >= m_pageIndicatorRect.top && y <= m_pageIndicatorRect.bottom;
    }
    D2D1_RECT_F GetPageIndicatorRect() const { return m_pageIndicatorRect; }

    // [PDF] Inline page input
    bool IsPageInputActive() const { return m_pageInputActive; }
    void SetPageInputActive(bool active) {
        if (!active) m_pageInputText.clear();
        m_pageInputActive = active;
    }
    const std::wstring& GetPageInputText() const { return m_pageInputText; }
    void AppendPageInputChar(wchar_t c) {
        if (m_pageInputText.size() < 8) m_pageInputText += c;
    }
    void BackspacePageInput() {
        if (!m_pageInputText.empty()) m_pageInputText.pop_back();
    }
    void ClearPageInput() { m_pageInputText.clear(); }
    D2D1_RECT_F GetPageInputRect() const { return m_pageIndicatorRect; }

private:
    // Layout Constants
    const float BUTTON_SIZE = 24.0f;
    const float GAP = 6.0f;
    const float PADDING_X = 10.0f;
    const float PADDING_Y = 6.0f; // 6+24+6 = 36px = title bar height
    const float BOTTOM_MARGIN = 0.0f; // Docked to bottom edge

    // Animation
    float m_opacity = 0.0f;
    float m_uiScale = 1.0f;
    float m_uiFontScale = 0.0f;

    bool m_targetVisible = false;
    bool m_isPinned = true; // [A块] Always pinned - toolbar never auto-hides
    bool m_windowTooNarrow = false; // True only when even the last-priority buttons can't fit
    uint64_t m_responsiveHiddenSet = 0; // Bitmask of ToolbarButtonID values hidden by responsive layout
    bool m_compareMode = false;
    bool m_comicMode = false;
    bool m_animMode = false;
    bool m_slideshowMode = false;
    bool m_animPlaying = true;
    bool m_animDirtyRect = false;
    // [Removed Overlay] m_overlayMode/m_overlayAlphaPercent removed
    float m_animProgress = 0.0f;
    uint32_t m_currentFrame = 0;
    uint32_t m_totalFrames = 0;
    float m_animSpeedMult = 1.0f;
    float m_minRequiredWidth = 0.0f;
    float m_compareZoomStepPercent = 0.5f;
    
    D2D1_ROUNDED_RECT m_bgRect = {};
    std::vector<ToolbarButton> m_buttons;
    D2D1_RECT_F m_compareStepRect = {};
    D2D1_RECT_F m_compareStepUpRect = {};
    D2D1_RECT_F m_compareStepDownRect = {};
    bool m_compareStepHover = false;

    bool m_compareStepUpHover = false;
    bool m_compareStepDownHover = false;
    
    // [v10.5] Animation speed capsule rects
    D2D1_RECT_F m_animSpeedRect = {};
    D2D1_RECT_F m_animSpeedUpRect = {};
    D2D1_RECT_F m_animSpeedDownRect = {};
    bool m_animSpeedHover = false;
    bool m_animSpeedUpHover = false;
    bool m_animSpeedDownHover = false;
    
    // Progress bar interaction
    D2D1_RECT_F m_animProgressRect = {};
    bool m_animProgressHover = false;
    bool m_isDraggingProgress = false;
    float m_animSeekHoverProgress = 0.0f;

    // [Swatch] Color swatch circles on the right of toolbar
    D2D1_RECT_F m_swatchRects[9] = {};
    int m_swatchHoverIndex = -1;
    int m_swatchClickIndex = -1;

    // [PDF/AI/CDR] Page indicator state
    bool m_showPageIndicator = false;
    uint32_t m_currentPage = 0;
    uint32_t m_totalPages = 0;
    D2D1_RECT_F m_pageIndicatorRect = {};
    bool m_pageIndicatorHover = false;

    // [PDF] Inline page input state
    bool m_pageInputActive = false;
    std::wstring m_pageInputText;
    int m_pageInputCursorBlink = 0;
    
    // Resources
    ComPtr<ID2D1SolidColorBrush> m_brushBg;
    ComPtr<ID2D1SolidColorBrush> m_brushIcon;
    ComPtr<ID2D1SolidColorBrush> m_brushIconActive;
    ComPtr<ID2D1SolidColorBrush> m_brushIconDisabled;
    ComPtr<ID2D1SolidColorBrush> m_brushWarning;
    ComPtr<ID2D1SolidColorBrush> m_brushHover;
    
    ComPtr<IDWriteTextFormat> m_textFormatUI;
    ComPtr<IDWriteFactory> m_dwriteFactory; // Need factory to create format
    
    // Geek Glass properties
    QuickView::UI::GeekGlass::GeekGlassEngine m_geekGlass;
    ID2D1CommandList* m_bgCmdList = nullptr;
    D2D1_MATRIX_3X2_F m_bgTransform = D2D1::Matrix3x2F::Identity();

    void CreateResources(ID2D1RenderTarget* pRT);
};
