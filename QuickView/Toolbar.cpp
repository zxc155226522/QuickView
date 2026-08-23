#include "pch.h"
#include "Toolbar.h"
#include "AppStrings.h"
#include "EditState.h"
#include "GeekIconRenderer.h"
#include "FileNavigator.h"
#include "GalleryOverlay.h"
#include "SettingsOverlay.h"
#include "HelpOverlay.h"

using QuickView::UI::GeekIconRenderer;

extern AppConfig g_config;
extern FileNavigator& g_navigator;

namespace Icons = GeekIcons;

Toolbar::Toolbar() {
  // Define Buttons — each uses a pointer to vector icon data
  m_buttons = {
      {ToolbarButtonID::PageFirst, Icons::SkipBack, {}, true},
      {ToolbarButtonID::Prev, Icons::ChevronLeft, {}, true},
      {ToolbarButtonID::Next, Icons::Chevron, {}, true},
      {ToolbarButtonID::PageLast, Icons::SkipFwd, {}, true},
      {ToolbarButtonID::RotateL, Icons::Transform, {}, true},
      {ToolbarButtonID::RotateR, Icons::Transform, {}, true},
      {ToolbarButtonID::FlipH, Icons::Flip, {}, true},

      {ToolbarButtonID::Gallery, Icons::Gallery, {}, true},

      {ToolbarButtonID::RawToggle, Icons::Raw, {}, false, false},
      {ToolbarButtonID::GamutWarning, Icons::Warning, {}, false, false},

      // Animation mode buttons (hidden in normal mode)
      {ToolbarButtonID::AnimPrevFrame, Icons::SkipBack, {}, true, false},
      {ToolbarButtonID::AnimPlayPause, Icons::Play, {}, true, false},
      {ToolbarButtonID::AnimNextFrame, Icons::SkipFwd, {}, true, false},
      {ToolbarButtonID::AnimDirtyRect, Icons::Diagnostic, {}, true, false},
      // Slideshow mode buttons
      {ToolbarButtonID::SlideshowImmersiveToggle, Icons::Eye, {}, true, false},
      {ToolbarButtonID::SlideshowExit,            Icons::ExitToolbar, {}, true, false},
  };
}

Toolbar::~Toolbar() {}

float Toolbar::GetReservedHeight() const {
  return 36.0f * m_uiScale; // Same as title bar height
}

void Toolbar::SetUIScale(float scale) {
  if (scale < 1.0f)
    scale = 1.0f;
  if (scale > 4.0f)
    scale = 4.0f;
  if (fabsf(m_uiScale - scale) < 0.001f)
    return;
  m_uiScale = scale;
  m_textFormatUI.Reset();
}

void Toolbar::CreateResources(ID2D1RenderTarget *pRT) {
  if (!m_brushBg) {
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                               &m_brushBg); // Master placeholder
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                               &m_brushIcon);
    pRT->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f),
                               &m_brushIconActive); // Blue for active
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f),
                               &m_brushIconDisabled);
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f),
                               &m_brushWarning); // Red for warning
    pRT->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f),
                               &m_brushHover); // Hover highlight

    // Font
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown **>(m_dwriteFactory.GetAddressOf()));
  }

  if (!m_dwriteFactory)
    return;
  if (m_textFormatUI &&
      fabsf(m_uiFontScale - m_uiScale) < 0.001f)
    return;

  m_textFormatUI.Reset();
  m_dwriteFactory->CreateTextFormat(
      L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, 11.5f * m_uiScale, AppStrings::CurrentLocale,
      &m_textFormatUI);
  if (m_textFormatUI) {
    m_textFormatUI->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormatUI->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_uiFontScale = m_uiScale;
  }
}

void Toolbar::Init(ID2D1RenderTarget *pRT) { CreateResources(pRT); }

void Toolbar::UpdateLayout(float winW, float winH) {
  // Skip layout if window has no valid size yet
  if (winW <= 0 || winH <= 0)
    return;

  const float buttonSize = BUTTON_SIZE * m_uiScale;
  const float gap = GAP * m_uiScale;
  const float padX = PADDING_X * m_uiScale;
  const float padY = PADDING_Y * m_uiScale;
  const float bottomMargin = BOTTOM_MARGIN * m_uiScale;

  auto isCompareButton = [](ToolbarButtonID id) {
    switch (id) {
    case ToolbarButtonID::CompareOpen:
    case ToolbarButtonID::CompareSwap:
    case ToolbarButtonID::CompareLayout:
    case ToolbarButtonID::CompareInfo:
    case ToolbarButtonID::CompareDelete:
    case ToolbarButtonID::CompareZoomIn:
    case ToolbarButtonID::CompareZoomOut:
    case ToolbarButtonID::CompareSyncZoom:
    case ToolbarButtonID::CompareSyncPan:
    case ToolbarButtonID::CompareRawToggle:
    case ToolbarButtonID::CompareExit:
      return true;
    default:
      return false;
    }
  };
  auto isAlwaysVisible = [](ToolbarButtonID id) {
    return id == ToolbarButtonID::Pin;
  };

  auto isAnimButton = [](ToolbarButtonID id) {
    if (id == ToolbarButtonID::AnimPlayPause ||
        id == ToolbarButtonID::AnimPrevFrame ||
        id == ToolbarButtonID::AnimNextFrame ||
        id == ToolbarButtonID::AnimDirtyRect) {
        return true;
    }
    return false;
  };

  // [PDF] PageFirst/PageLast buttons are only visible in paged mode
  auto isPageNavButton = [](ToolbarButtonID id) {
    return id == ToolbarButtonID::PageFirst || id == ToolbarButtonID::PageLast;
  };



  [[maybe_unused]] auto isNormalButton = [&](ToolbarButtonID id) {
    return !isCompareButton(id) && id != ToolbarButtonID::AnimPlayPause && id != ToolbarButtonID::AnimPrevFrame && id != ToolbarButtonID::AnimNextFrame && id != ToolbarButtonID::AnimDirtyRect && !isAlwaysVisible(id);
  };

  // --- Responsive Hide Priority Tables (per mode) ---
  // Each table lists groups of buttons to hide in order of decreasing priority.
  // The LAST group in each table is the "core" group — if it can't fit, hide the entire toolbar.
  static constexpr ResponsiveHideGroup kNormalHideOrder[] = {
      {ToolbarButtonID::FlipH},
      {ToolbarButtonID::Gallery},
      {ToolbarButtonID::CompareToggle},
      {ToolbarButtonID::RotateL, ToolbarButtonID::RotateR},
      {ToolbarButtonID::Prev, ToolbarButtonID::Next},  // Core — hide => hide toolbar
  };
  static constexpr ResponsiveHideGroup kCompareHideOrder[] = {
      {ToolbarButtonID::CompareOpen},
      {ToolbarButtonID::CompareSwap},
      {ToolbarButtonID::CompareDelete},
      {ToolbarButtonID::CompareRawToggle},
      {ToolbarButtonID::CompareZoomIn, ToolbarButtonID::CompareZoomOut},  // Zoom capsule group
      {ToolbarButtonID::CompareInfo},
      {ToolbarButtonID::CompareSyncPan},
      {ToolbarButtonID::CompareSyncZoom},
      {ToolbarButtonID::CompareLayout},
      {ToolbarButtonID::CompareExit},  // Core
  };
  static constexpr ResponsiveHideGroup kAnimHideOrder[] = {
      {ToolbarButtonID::AnimDirtyRect},
      {ToolbarButtonID::Prev, ToolbarButtonID::Next},
      // AnimSpeed capsule hides when AnimNextFrame is hidden (they are visually adjacent)
      {ToolbarButtonID::AnimPrevFrame, ToolbarButtonID::AnimNextFrame},
      {ToolbarButtonID::AnimPlayPause},  // Core
  };
  static constexpr ResponsiveHideGroup kComicHideOrder[] = {
      {ToolbarButtonID::Gallery},
      {ToolbarButtonID::CompareToggle},
      {ToolbarButtonID::Prev, ToolbarButtonID::Next},  // Core
  };
  static constexpr ResponsiveHideGroup kSlideshowHideOrder[] = {
      {ToolbarButtonID::Gallery},
      {ToolbarButtonID::SlideshowImmersiveToggle},
      {ToolbarButtonID::AnimPrevFrame, ToolbarButtonID::AnimNextFrame},
      {ToolbarButtonID::AnimPlayPause},
      {ToolbarButtonID::SlideshowExit},  // Core
  };

  // Select the appropriate hide table for the current mode
  const ResponsiveHideGroup* hideOrder = kNormalHideOrder;
  int hideOrderCount = (int)std::size(kNormalHideOrder);
  if (m_slideshowMode) {
      hideOrder = kSlideshowHideOrder;
      hideOrderCount = (int)std::size(kSlideshowHideOrder);
  } else if (m_animMode) {
      hideOrder = kAnimHideOrder;
      hideOrderCount = (int)std::size(kAnimHideOrder);
  } else if (m_compareMode) {
      hideOrder = kCompareHideOrder;
      hideOrderCount = (int)std::size(kCompareHideOrder);
  } else if (m_comicMode) {
      hideOrder = kComicHideOrder;
      hideOrderCount = (int)std::size(kComicHideOrder);
  }

  // Helper: check if a button ID is in the responsive hidden set
  auto isResponsiveHidden = [this](ToolbarButtonID id) -> bool {
      auto idx = static_cast<unsigned>(id);
      return idx < 64 && (m_responsiveHiddenSet & (1ULL << idx)) != 0;
  };

  auto isVisibleButton = [&](const ToolbarButton &btn) {
    // Responsive hide check (applied on top of mode-based visibility)
    if (isResponsiveHidden(btn.id)) return false;

    if (btn.id == ToolbarButtonID::Gallery && (winH < 450.0f || winW < 600.0f)) {
      return false;
    }
    auto isSlideshowButton = [](ToolbarButtonID id) {
      return id == ToolbarButtonID::SlideshowImmersiveToggle ||
             id == ToolbarButtonID::SlideshowExit ||
             id == ToolbarButtonID::AnimPrevFrame ||
             id == ToolbarButtonID::AnimPlayPause ||
             id == ToolbarButtonID::AnimNextFrame;
    };

    auto isSlideshowOnlyButton = [](ToolbarButtonID id) {
      return id == ToolbarButtonID::SlideshowImmersiveToggle ||
             id == ToolbarButtonID::SlideshowExit;
    };
    if (isSlideshowOnlyButton(btn.id) && !m_slideshowMode) {
      return false;
    }

    if (m_slideshowMode) {
      // White-list slideshow controls (play group, immersive toggle, exit, gallery, exif, pin)
      if (isSlideshowButton(btn.id) || btn.id == ToolbarButtonID::Gallery) {
        return btn.isEnabled;
      }
      return false;
    }

    if (m_animMode || m_slideshowMode) {
      if (btn.id == ToolbarButtonID::AnimDirtyRect) return m_animMode && g_config.ShowDirtyRectButton;
      if (btn.id == ToolbarButtonID::Prev || btn.id == ToolbarButtonID::Next) return true;
      if (isAnimButton(btn.id)) return true;
      if (isAlwaysVisible(btn.id)) return true;
      return false;
    }
    if (m_comicMode) {
      // In comic mode, we hide rotate and flip, raw, extension fix, and gamut warning
      if (btn.id == ToolbarButtonID::RotateL || btn.id == ToolbarButtonID::RotateR || btn.id == ToolbarButtonID::FlipH) return false;
      if (btn.id == ToolbarButtonID::RawToggle || btn.id == ToolbarButtonID::GamutWarning) return false;
      if (isCompareButton(btn.id)) return false;
      if (isAnimButton(btn.id)) return false;
      return true;
    }

    if (m_compareMode) {
      if (!isCompareButton(btn.id) && !isAlwaysVisible(btn.id)) return false;
      if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isWarning) return false;
      return true;
    }

    // [PDF] Page nav buttons: only visible when page indicator is shown
    if (isPageNavButton(btn.id)) return m_showPageIndicator;

    if (isCompareButton(btn.id) || isAnimButton(btn.id))
      return false;
    if (btn.id == ToolbarButtonID::RawToggle && !btn.isEnabled)
      return false;
    if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isWarning)
      return false;
    if (btn.id == ToolbarButtonID::GamutWarning && !btn.isEnabled)
      return false;
    return true;
  };

  // Helper: calculate total toolbar width for the current visible button set
  auto calcTotalWidth = [&]() -> float {
    int count = 0;
    bool hasZoom = false;
    bool hasSpeed = (m_animMode || m_slideshowMode);
    for (const auto &btn : m_buttons) {
      if (isVisibleButton(btn)) count++;
      if (m_compareMode && (btn.id == ToolbarButtonID::CompareZoomIn || btn.id == ToolbarButtonID::CompareZoomOut)) {
        if (isVisibleButton(btn)) hasZoom = true;
      }
    }
    // If AnimNextFrame is hidden (responsive), the speed capsule should also vanish
    if (hasSpeed && isResponsiveHidden(ToolbarButtonID::AnimNextFrame)) hasSpeed = false;

    float w = padX * 2 + (count * buttonSize);
    if (count > 1) w += (count - 1) * gap;
    // [PDF] Page indicator width — inline between Prev and Next (part of the flow)
    if (m_showPageIndicator) {
      w += 100.0f * m_uiScale; // page indicator pill width (input box + separator + total)
    }
    if (m_compareMode && hasZoom) {
      const float zoomGap = 2.0f * m_uiScale;
      w += (56.0f * m_uiScale) + (zoomGap * 2.0f) - gap;
    }
    if ((m_animMode || m_slideshowMode) && hasSpeed) {
      const float speedGap = 2.0f * m_uiScale;
      w += (56.0f * m_uiScale) + (speedGap * 2.0f) - gap;
    }
    return w;
  };

  // --- Responsive Hide Loop ---
  m_responsiveHiddenSet = 0;
  m_windowTooNarrow = false;
  float totalW = calcTotalWidth();

  if (totalW > winW) {
    // Progressively hide groups until it fits or we exhaust all groups
    for (int g = 0; g < hideOrderCount; ++g) {
      // Mark all buttons in this group as responsively hidden
      for (int k = 0; k < 4 && hideOrder[g].ids[k] != ToolbarButtonID::None; ++k) {
        auto idx = static_cast<unsigned>(hideOrder[g].ids[k]);
        if (idx < 64) m_responsiveHiddenSet |= (1ULL << idx);
      }
      totalW = calcTotalWidth();
      if (totalW <= winW) break;  // Fits now

      // If we just hid the last (core) group and it still doesn't fit → hide entire toolbar
      if (g == hideOrderCount - 1) {
        m_windowTooNarrow = true;
      }
    }
  }

  // [Swatch] Calculate swatch section within toolbar capsule
  bool showSwatches = !m_compareMode && !m_animMode && !m_slideshowMode && !m_comicMode;
  const float swatchDiameter = 14.0f * m_uiScale;
  const float swatchGap = 3.0f * m_uiScale;
  const float swatchSeparatorGap = 12.0f * m_uiScale;
  float swatchSectionW = 0.0f;
  if (showSwatches) {
    swatchSectionW = swatchSeparatorGap + 9.0f * swatchDiameter + 8.0f * swatchGap;
    if (totalW + swatchSectionW > winW) {
      showSwatches = false;
      swatchSectionW = 0.0f;
    }
  }
  totalW += swatchSectionW;

  m_minRequiredWidth = totalW + (PADDING_X * 2 * m_uiScale);

  float startX = (winW - totalW) / 2.0f;
  float startY = winH - bottomMargin - buttonSize - padY * 2;

  // Full-width docked bar (not floating capsule)
  m_bgRect =
      D2D1::RoundedRect(D2D1::RectF(0, startY, winW,
                                    startY + buttonSize + padY * 2 + bottomMargin),
                        0.0f, 0.0f // Sharp corners for docked bar
      );

  // Helper: check if a button is part of the page navigation group
  auto isPageNavGroup = [](ToolbarButtonID id) {
    return id == ToolbarButtonID::PageFirst ||
           id == ToolbarButtonID::Prev ||
           id == ToolbarButtonID::Next ||
           id == ToolbarButtonID::PageLast;
  };

  // Layout Buttons
  float cx = startX + padX;
  float cy = startY + padY;
  const float stepW = 56.0f * m_uiScale;
  const float stepH = buttonSize * 0.78f;
  const float stepY = cy + (buttonSize - stepH) * 0.5f;
  const float zoomGap = 2.0f * m_uiScale;
  m_compareStepRect = D2D1::RectF(0, 0, 0, 0);
  m_compareStepUpRect = D2D1::RectF(0, 0, 0, 0);
  m_compareStepDownRect = D2D1::RectF(0, 0, 0, 0);
  m_animSpeedRect = D2D1::RectF(0, 0, 0, 0);
  m_animSpeedUpRect = D2D1::RectF(0, 0, 0, 0);
  m_animSpeedDownRect = D2D1::RectF(0, 0, 0, 0);
  m_animProgressRect = D2D1::RectF(0, 0, 0, 0);
  bool stepInserted = false;
  bool speedInserted = false;
  
  if (m_animMode) {
      // Expand upwards to capture pointer tip and frame text, contract downwards to avoid buttons (+2.0f)
      m_animProgressRect = D2D1::RectF(m_bgRect.rect.left + 20.0f * m_uiScale, m_bgRect.rect.top - 28.0f * m_uiScale, m_bgRect.rect.right - 20.0f * m_uiScale, m_bgRect.rect.top + 2.0f * m_uiScale);
  }

  // [PDF] When page indicator is visible, use three-segment layout:
  // Left buttons | [PageFirst][Prev] [页码] [Next][PageLast] | Right buttons
  // The page nav group is centered in the window.
  if (m_showPageIndicator && !m_compareMode && !m_animMode && !m_slideshowMode) {
    const float indicatorW = 100.0f * m_uiScale;
    const float indicatorH = buttonSize * 0.82f;

    // All non-pagenav buttons go to the right side (since PageFirst is first in m_buttons array)

    // Calculate widths
    // Center group: PageFirst + Prev + indicator + Next + PageLast
    const float centerW = buttonSize * 4 + indicatorW + gap * 4; // 4 buttons + indicator + 4 gaps
    const float centerX = winW * 0.5f;
    float centerStartX = centerX - centerW * 0.5f;
    float rightStartX = centerStartX + centerW + gap;

    // Layout right-side buttons from rightStartX going right
    float rx = rightStartX;
    // Place center group buttons
    float ccx = centerStartX;
    for (auto &btn : m_buttons) {
      bool visible = isVisibleButton(btn);
      // Sync Pin State
      if (btn.id == ToolbarButtonID::Pin) {
        btn.isToggled = m_isPinned;
        btn.iconGlyph = m_isPinned ? Icons::Unpin : Icons::Pin;
      }
      if (btn.id == ToolbarButtonID::LockSize) {
          btn.isToggled = g_runtime.LockWindowSize;
          btn.iconGlyph = g_runtime.LockWindowSize ? Icons::Lock : Icons::Unlock;
      }
      if (btn.id == ToolbarButtonID::CompareToggle) {
        btn.iconGlyph = Icons::CompareToggle;
      }

      if (!visible) {
        btn.rect = D2D1::RectF(0, 0, 0, 0);
        continue;
      }

      if (isPageNavGroup(btn.id)) {
        // Place in center group
        btn.rect = D2D1::RectF(ccx, cy, ccx + buttonSize, cy + buttonSize);
        ccx += buttonSize + gap;
        // Insert page indicator after Prev
        if (btn.id == ToolbarButtonID::Prev) {
          const float indicatorY = cy + (buttonSize - indicatorH) * 0.5f;
          m_pageIndicatorRect = D2D1::RectF(ccx, indicatorY, ccx + indicatorW, indicatorY + indicatorH);
          ccx += indicatorW + gap;
        }
      } else {
        // Right-side button
        btn.rect = D2D1::RectF(rx, cy, rx + buttonSize, cy + buttonSize);
        rx += buttonSize + gap;
      }
    }
  } else {
    // Standard linear layout (no page indicator or compare/anim/slideshow mode)
    for (auto &btn : m_buttons) {
      bool visible = isVisibleButton(btn);
      // Sync Pin State
      if (btn.id == ToolbarButtonID::Pin) {
        btn.isToggled = m_isPinned;
        btn.iconGlyph = m_isPinned ? Icons::Unpin : Icons::Pin;
      }
      if (btn.id == ToolbarButtonID::LockSize) {
          btn.isToggled = g_runtime.LockWindowSize;
          btn.iconGlyph = g_runtime.LockWindowSize ? Icons::Lock : Icons::Unlock;
      }

      if (btn.id == ToolbarButtonID::CompareToggle) {
        btn.iconGlyph = Icons::CompareToggle;
      }

      if (visible) {
        btn.rect = D2D1::RectF(cx, cy, cx + buttonSize, cy + buttonSize);
        cx += buttonSize + gap;

        if (m_compareMode && btn.id == ToolbarButtonID::CompareZoomIn && !stepInserted) {
          cx -= gap; // Backtrack to remove standard gap
          cx += zoomGap; // Padding before capsule
          m_compareStepRect = D2D1::RectF(cx, stepY, cx + stepW, stepY + stepH);
          const float stepBtnW = 14.0f * m_uiScale;
          m_compareStepUpRect = D2D1::RectF(m_compareStepRect.right - stepBtnW,
                                            m_compareStepRect.top,
                                            m_compareStepRect.right,
                                            m_compareStepRect.top +
                                                (stepH * 0.5f));
          m_compareStepDownRect = D2D1::RectF(
              m_compareStepRect.right - stepBtnW,
              m_compareStepRect.top + (stepH * 0.5f),
              m_compareStepRect.right, m_compareStepRect.bottom);
          cx += stepW + zoomGap; // Capsule width + padding after
          stepInserted = true;
        }
        
        if ((m_animMode || m_slideshowMode) && btn.id == ToolbarButtonID::AnimNextFrame &&
            !speedInserted) {
          cx -= gap; // Backtrack standard gap
          const float speedGap = 2.0f * m_uiScale;
          cx += speedGap;
          m_animSpeedRect = D2D1::RectF(cx, stepY, cx + stepW, stepY + stepH);
          const float sBtnW = 14.0f * m_uiScale;
          m_animSpeedUpRect = D2D1::RectF(m_animSpeedRect.right - sBtnW,
                                          m_animSpeedRect.top,
                                          m_animSpeedRect.right,
                                          m_animSpeedRect.top + (stepH * 0.5f));
          m_animSpeedDownRect = D2D1::RectF(
              m_animSpeedRect.right - sBtnW,
              m_animSpeedRect.top + (stepH * 0.5f),
              m_animSpeedRect.right, m_animSpeedRect.bottom);
          cx += stepW + speedGap;
          speedInserted = true;
        }
      } else {
        btn.rect = D2D1::RectF(0, 0, 0, 0); // Hide
      }
    }
    // Page indicator not shown in this mode
    m_pageIndicatorRect = D2D1::RectF(0, 0, 0, 0);
  }

  // [Swatch] Position swatches within the toolbar capsule (right-aligned)
  if (showSwatches) {
    float sx = m_bgRect.rect.right - padX - (9.0f * swatchDiameter + 8.0f * swatchGap);
    float sy = startY + (buttonSize + padY * 2 - swatchDiameter) * 0.5f;
    for (int i = 0; i < 9; ++i) {
      m_swatchRects[i] = D2D1::RectF(sx, sy, sx + swatchDiameter, sy + swatchDiameter);
      sx += swatchDiameter + swatchGap;
    }
  } else {
    for (int i = 0; i < 9; ++i) m_swatchRects[i] = D2D1::RectF(0, 0, 0, 0);
  }

  // [PDF] Page indicator position is set during button layout loop
  // (three-segment layout places it between Prev and Next at window center)
}

const wchar_t *GetTooltipText(const ToolbarButton &btn) {
  switch (btn.id) {
  case ToolbarButtonID::SlideshowImmersiveToggle:
    return AppStrings::Toolbar_Tooltip_SlideshowImmersiveToggle;
  case ToolbarButtonID::SlideshowExit:
    return AppStrings::Toolbar_Tooltip_SlideshowExit;
  case ToolbarButtonID::Prev:
    return AppStrings::Toolbar_Tooltip_Prev;
  case ToolbarButtonID::Next:
    return AppStrings::Toolbar_Tooltip_Next;
  case ToolbarButtonID::PageFirst:
    return L"首页";
  case ToolbarButtonID::PageLast:
    return L"尾页";
  case ToolbarButtonID::RotateL:
    return AppStrings::Toolbar_Tooltip_RotateL;
  case ToolbarButtonID::RotateR:
    return AppStrings::Toolbar_Tooltip_RotateR;
  case ToolbarButtonID::FlipH:
    return AppStrings::Toolbar_Tooltip_FlipH;
  case ToolbarButtonID::LockSize:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_Unlock
                         : AppStrings::Toolbar_Tooltip_Lock;
  case ToolbarButtonID::Gallery:
    return AppStrings::Toolbar_Tooltip_Gallery;
  case ToolbarButtonID::Exif:
    return AppStrings::Toolbar_Tooltip_Info;
  case ToolbarButtonID::RawToggle:
    // [RAW+JPEG Pairing] On a paired item the button switches the displayed
    // file rather than the decode quality
    if (btn.isPaired)
      return btn.isToggled ? AppStrings::Toolbar_Tooltip_RawPairBack
                           : AppStrings::Toolbar_Tooltip_RawPairView;
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_RawFull
                         : AppStrings::Toolbar_Tooltip_RawPreview;
  case ToolbarButtonID::CompareRawToggle:
    if (!btn.isEnabled) return nullptr;
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_RawFull
                         : AppStrings::Toolbar_Tooltip_RawPreview;
  case ToolbarButtonID::GamutWarning:
    return AppStrings::Toolbar_Tooltip_GamutWarning;
  case ToolbarButtonID::Pin:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_Unpin
                         : AppStrings::Toolbar_Tooltip_Pin;
  case ToolbarButtonID::CompareToggle:
    if (g_navigator.GetArchive() != nullptr) {
      return btn.isToggled ? AppStrings::Toolbar_Tooltip_SinglePage : AppStrings::Toolbar_Tooltip_DualPage;
    }
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_NormalMode : AppStrings::Toolbar_Tooltip_CompareMode;
  case ToolbarButtonID::CompareOpen:
    return AppStrings::Toolbar_Tooltip_CompareOpen;
  case ToolbarButtonID::CompareSwap:
    return AppStrings::Toolbar_Tooltip_CompareSwap;
  case ToolbarButtonID::CompareLayout:
    return AppStrings::Toolbar_Tooltip_CompareLayout;
  case ToolbarButtonID::CompareInfo:
    return AppStrings::Toolbar_Tooltip_CompareInfo;
  case ToolbarButtonID::CompareDelete:
    return AppStrings::Toolbar_Tooltip_CompareDelete;
  case ToolbarButtonID::CompareZoomIn:
    return AppStrings::Toolbar_Tooltip_CompareZoomIn;
  case ToolbarButtonID::CompareZoomOut:
    return AppStrings::Toolbar_Tooltip_CompareZoomOut;
  case ToolbarButtonID::CompareSyncZoom:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_CompareSyncZoomOn : AppStrings::Toolbar_Tooltip_CompareSyncZoomOff;
  case ToolbarButtonID::CompareSyncPan:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_CompareSyncPanOn : AppStrings::Toolbar_Tooltip_CompareSyncPanOff;
  case ToolbarButtonID::CompareExit:
    return AppStrings::Toolbar_Tooltip_CompareExit;
  case ToolbarButtonID::AnimPlayPause:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_AnimPause : AppStrings::Toolbar_Tooltip_AnimPlay;
  case ToolbarButtonID::AnimPrevFrame:
    return AppStrings::Toolbar_Tooltip_AnimPrev;
  case ToolbarButtonID::AnimNextFrame:
    return AppStrings::Toolbar_Tooltip_AnimNext;
  case ToolbarButtonID::AnimDirtyRect:
    return btn.isToggled ? AppStrings::Toolbar_Tooltip_AnimDirtyOn : AppStrings::Toolbar_Tooltip_AnimDirtyOff;
  default:
    return nullptr;
  }
}

void Toolbar::Render(ID2D1RenderTarget *pRT) {
  if (m_opacity <= 0.0f)
    return;

  if (m_windowTooNarrow)
    return;

  extern GalleryOverlay g_gallery;
  extern SettingsOverlay g_settingsOverlay;
  extern HelpOverlay g_helpOverlay;
  bool isFullGridGallery = g_gallery.IsVisible() && g_gallery.GetMode() == GalleryMode::FullGrid;
  if (g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || isFullGridGallery) return;

  CreateResources(pRT);

  // Toolbar: theme-aware background and icon colors
  bool isLight = IsLightThemeActive();
  m_brushBg->SetColor(isLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : D2D1::ColorF(0.12f, 0.12f, 0.14f, 1.0f));
  m_brushIcon->SetColor(isLight ? D2D1::ColorF(D2D1::ColorF::Black) : D2D1::ColorF(D2D1::ColorF::White));
  m_brushIconActive->SetColor(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f));
  m_brushIconDisabled->SetColor(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.3f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.3f));
  m_brushWarning->SetColor(D2D1::ColorF(1.0f, 0.3f, 0.3f, 1.0f));
  m_brushHover->SetColor(isLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.1f));

  ComPtr<ID2D1Layer> layer;
  if (SUCCEEDED(pRT->CreateLayer(&layer))) {
    D2D1_LAYER_PARAMETERS params = D2D1::LayerParameters();
    params.contentBounds = m_bgRect.rect;
    
    if (m_animMode) {
      params.contentBounds.top -= 10.0f * m_uiScale; // Extra room for progress bar
    }
    params.opacity = m_opacity;

    pRT->PushLayer(params, layer.Get());

    ComPtr<ID2D1DeviceContext> dc;
    if (SUCCEEDED(pRT->QueryInterface(IID_PPV_ARGS(&dc))) && m_bgCmdList) {
        m_geekGlass.InitializeResources(dc.Get());
        
        QuickView::UI::GeekGlass::GeekGlassConfig config;
        config.theme = IsLightThemeActive() ? QuickView::UI::GeekGlass::ThemeMode::Light : QuickView::UI::GeekGlass::ThemeMode::Dark;
        config.panelBounds = m_bgRect.rect;
        config.cornerRadius = m_bgRect.radiusX;
        config.enableGeekGlass = g_config.EnableGeekGlass;
        config.tintProfile = g_config.GlassTintProfile;
        config.customTintColor = D2D1::ColorF(g_config.GlassCustomTintR, g_config.GlassCustomTintG, g_config.GlassCustomTintB, g_config.GlassTintAlpha);
        config.tintAlpha = g_config.GlassTintAlpha;
        config.specularOpacity = g_config.GlassSpecularOpacity;
        config.blurStandardDeviation = g_config.GlassBlurSigma * m_uiScale;
        config.opacity = g_config.GlassPanelsOpacity / 100.0f;
        if (g_config.EnableGeekGlass) {
            config.opacity = g_config.GlassPanelsOpacity / 100.0f;
        } 
        config.strokeWeight = g_config.GetVectorStrokeWeight();
        config.shadowOpacity = g_config.GlassShadowOpacity;
        config.pBackgroundCommandList = m_bgCmdList;
        config.backgroundTransform = m_bgTransform;
        
        m_geekGlass.DrawGeekGlassPanel(dc.Get(), config);

        // [Material Boost] Consistency
        if (g_config.EnableGeekGlass) {
            // Theme-aware Material Filler
            bool isLight = IsLightThemeActive();
            D2D1_COLOR_F fillerColor = isLight ? D2D1::ColorF(0.95f, 0.95f, 0.97f, 1.0f) : D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f);
            m_brushBg->SetColor(fillerColor);
            m_brushBg->SetOpacity(1.0f);

            // [Fix] Match corner radius exactly to prevent straight-edge leaking
            pRT->FillRoundedRectangle(D2D1::RoundedRect(m_bgRect.rect, config.cornerRadius, config.cornerRadius), m_brushBg.Get());
            
            // Restore High-end Reflexes
            m_geekGlass.DrawGeekGlassToppings(dc.Get(), config);
        }
    } else {
        m_brushBg->SetOpacity(1.0f);
        pRT->FillRoundedRectangle(m_bgRect, m_brushBg.Get());
    }

    // Separator line at top of docked toolbar (theme-aware)
    {
        ComPtr<ID2D1SolidColorBrush> lineBrush;
        D2D1_COLOR_F lineColor = isLight
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
        pRT->CreateSolidColorBrush(lineColor, &lineBrush);
        if (lineBrush) {
            float lineY = m_bgRect.rect.top;
            pRT->DrawLine(
                D2D1::Point2F(0, lineY),
                D2D1::Point2F(m_bgRect.rect.right, lineY),
                lineBrush.Get(), 1.0f, nullptr);
        }
    }
    
    // [v10.5] Animation Progress Bar - dynamic placement and thickness
    if (m_animMode && m_animProgress >= 0.0f) {
      float barLeft = m_bgRect.rect.left + 20.0f * m_uiScale;
      float barRight = m_bgRect.rect.right - 20.0f * m_uiScale;
      float barY = m_bgRect.rect.top - 1.0f * m_uiScale; // Draw outside the background rectangle
      if (m_animProgressHover) barY -= 1.0f * m_uiScale; // Shift up slightly when hovered
      float barW = barRight - barLeft;
      float displayProgress = m_isDraggingProgress ? m_animSeekHoverProgress : m_animProgress;
      float fillW = barW * displayProgress;
      float lineThickness = m_animProgressHover ? 4.0f * m_uiScale : 2.0f * m_uiScale;
      float halfH = lineThickness * 0.5f;
      float cornerRadius = halfH;
      
      D2D1_RECT_F trackRect = D2D1::RectF(barLeft, barY - halfH, barRight, barY + halfH);
      
      // Track (subtle)
      ComPtr<ID2D1SolidColorBrush> trackBr;
      pRT->CreateSolidColorBrush(D2D1::ColorF(0.3f, 0.3f, 0.4f, 0.3f), &trackBr);
      pRT->FillRoundedRectangle(D2D1::RoundedRect(trackRect, cornerRadius, cornerRadius), trackBr.Get());
      
      // Fill (accent blue)
      if (fillW > 0.5f) {
        D2D1_COLOR_F accentColor = m_animPlaying
          ? D2D1::ColorF(0.25f, 0.56f, 1.0f, 0.8f) // Blue when playing
          : D2D1::ColorF(1.0f, 0.72f, 0.18f, 0.85f); // Orange when paused
        ComPtr<ID2D1SolidColorBrush> fillBr;
        pRT->CreateSolidColorBrush(accentColor, &fillBr);
        
        D2D1_RECT_F fillRect = D2D1::RectF(barLeft, barY - halfH, barLeft + fillW, barY + halfH);
        pRT->FillRoundedRectangle(D2D1::RoundedRect(fillRect, cornerRadius, cornerRadius), fillBr.Get());
        
        // Playhead dot
        float dotRadius = m_animProgressHover ? 4.0f * m_uiScale : 3.0f * m_uiScale;
        D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(barLeft + fillW, barY), dotRadius, dotRadius);
        ComPtr<ID2D1SolidColorBrush> dotBr;
        pRT->CreateSolidColorBrush(D2D1::ColorF(m_animProgressHover ? 1.0f : 0.3f, 0.65f, 1.0f, 0.95f), &dotBr);
        pRT->FillEllipse(dot, dotBr.Get());
      }
      
      // Frame Index Text (Show on hover)
      if (m_totalFrames > 1 && m_animProgressHover) {
        uint32_t displayFrame = m_currentFrame;
        
        // When paused, show the frame at the mouse cursor position
        if (!m_animPlaying) {
             displayFrame = (uint32_t)round(m_animSeekHoverProgress * (m_totalFrames - 1));
             if (displayFrame >= m_totalFrames) displayFrame = m_totalFrames - 1;
        }

        wchar_t frameTxt[64];
        swprintf_s(frameTxt, L"%u / %u", displayFrame + 1, m_totalFrames);
        
        // Draw text centered above the progress bar with a transparent background
        if (m_textFormatUI && m_dwriteFactory) {
            ComPtr<IDWriteTextLayout> textLayout;
            if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(frameTxt, (UINT32)wcslen(frameTxt), m_textFormatUI.Get(), 500.0f, 50.0f, &textLayout))) {
                DWRITE_TEXT_METRICS metrics;
                textLayout->GetMetrics(&metrics);
                
                float pillW = metrics.width + 16.0f * m_uiScale;
                float pillH = 20.0f * m_uiScale;
                float pillX = barLeft + (barW - pillW) * 0.5f;
                float pillY = barY - pillH - 6.0f * m_uiScale;
                
                D2D1_RECT_F pillRect = D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH);
                
                // Draw background pill
                D2D1_COLOR_F tipBgBase = isLight
                    ? D2D1::ColorF(1.0f, 1.0f, 1.0f, g_config.GlassOsdOpacity / 100.0f)
                    : D2D1::ColorF(0.15f, 0.15f, 0.15f, g_config.GlassOsdOpacity / 100.0f);
                ComPtr<ID2D1SolidColorBrush> tipBg;
                pRT->CreateSolidColorBrush(tipBgBase, &tipBg);
                pRT->FillRoundedRectangle(D2D1::RoundedRect(pillRect, 4.0f * m_uiScale, 4.0f * m_uiScale), tipBg.Get());
                
                // Text color
                D2D1_COLOR_F textColor = m_animPlaying
                  ? D2D1::ColorF(0.25f, 0.56f, 1.0f, 0.9f) // Match active bar
                  : D2D1::ColorF(1.0f, 0.72f, 0.18f, 0.9f); // Match paused bar
                  
                ComPtr<ID2D1SolidColorBrush> textBr;
                pRT->CreateSolidColorBrush(textColor, &textBr);
                
                pRT->DrawText(frameTxt, (UINT32)wcslen(frameTxt), m_textFormatUI.Get(), pillRect, textBr.Get());
            }
        }
      }
    }

    for (const auto &btn : m_buttons) {
      if (btn.rect.right == 0)
        continue;

      if (btn.isHovered) {
        D2D1_ROUNDED_RECT hoverRect =
            D2D1::RoundedRect(btn.rect, 6.0f * m_uiScale, 6.0f * m_uiScale);
        pRT->FillRoundedRectangle(hoverRect, m_brushHover.Get());
      }

      ID2D1SolidColorBrush *pBrush = m_brushIcon.Get();
      if (btn.isToggled)
        pBrush = m_brushIconActive.Get();
      if (btn.isWarning && btn.id != ToolbarButtonID::CompareRawToggle)
        pBrush = m_brushWarning.Get();
      if (btn.id == ToolbarButtonID::CompareRawToggle && !btn.isEnabled)
        pBrush = m_brushIconDisabled.Get();
      if (btn.id == ToolbarButtonID::LockSize && btn.isToggled)
        pBrush = m_brushIconActive.Get();
      if (btn.id == ToolbarButtonID::Pin && btn.isToggled)
        pBrush = m_brushIconActive.Get();
      // [v10.5] Animation button states
      if (btn.id == ToolbarButtonID::AnimPlayPause && m_animPlaying)
        pBrush = m_brushIconActive.Get();
      if (btn.id == ToolbarButtonID::AnimDirtyRect && m_animDirtyRect)
        pBrush = m_brushIconActive.Get();

      // Scale down the vector icon to match original font sizes
      float targetSize = (btn.id == ToolbarButtonID::CompareExit) ? 14.0f * m_uiScale : 16.0f * m_uiScale;
      float padX = ((btn.rect.right - btn.rect.left) - targetSize) * 0.5f;
      float padY = ((btn.rect.bottom - btn.rect.top) - targetSize) * 0.5f;
      D2D1_RECT_F iconRect = D2D1::RectF(
          btn.rect.left + padX, btn.rect.top + padY,
          btn.rect.right - padX, btn.rect.bottom - padY);

      // RotateL: mirror the shared Transform icon horizontally
      if (btn.id == ToolbarButtonID::RotateL) {
        D2D1::Matrix3x2F originalTransform;
        pRT->GetTransform(&originalTransform);
        float cx = (btn.rect.left + btn.rect.right) * 0.5f;
        float cy = (btn.rect.top + btn.rect.bottom) * 0.5f;
        pRT->SetTransform(
            D2D1::Matrix3x2F::Scale(-1.0f, 1.0f, D2D1::Point2F(cx, cy)) *
            originalTransform);
        GeekIconRenderer::DrawVectorIcon(pRT, *btn.iconGlyph, iconRect, pBrush);
        pRT->SetTransform(originalTransform);
        continue;
      }

      GeekIconRenderer::DrawVectorIcon(pRT, *btn.iconGlyph, iconRect, pBrush);
    }

    // [PDF/AI/CDR] Draw page indicator at toolbar center
    // Layout: [当前页(input box)] / [总页数]
    if (m_showPageIndicator && m_pageIndicatorRect.right > m_pageIndicatorRect.left) {
      const float indicatorW = m_pageIndicatorRect.right - m_pageIndicatorRect.left;
      const float indicatorH = m_pageIndicatorRect.bottom - m_pageIndicatorRect.top;

      // Measure total text to center it within the indicator rect
      // Format: "123 / 456" — we split into input part and total part

      // Build display strings
      std::wstring curStr, totalStr;
      if (m_pageInputActive) {
        curStr = m_pageInputText;
        if (curStr.empty()) curStr = L"0";
      } else {
        curStr = std::to_wstring(m_currentPage + 1);
      }
      totalStr = std::to_wstring(m_totalPages);
      std::wstring sepStr = L"/";

      // Measure each part using IDWriteTextLayout
      auto measureText = [&](const std::wstring &text) -> float {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                text.c_str(), (UINT32)text.size(), m_textFormatUI.Get(),
                9999.0f, indicatorH, &layout))) {
          DWRITE_TEXT_METRICS metrics{};
          layout->GetMetrics(&metrics);
          return metrics.widthIncludingTrailingWhitespace;
        }
        return 0.0f;
      };
      float curW = measureText(curStr);
      float sepW = measureText(sepStr);
      float totalW = measureText(totalStr);
      // Use fixed minimum width for current page area (fit 3 digits)
      const float minCurW = measureText(L"000");
      if (curW < minCurW) curW = minCurW;
      float totalTextW = curW + sepW + totalW;
      // Add small gaps between parts
      const float partGap = 6.0f * m_uiScale;
      totalTextW += partGap * 2.0f;

      // Center the text block within indicator rect
      float textStartX = m_pageIndicatorRect.left + (indicatorW - totalTextW) * 0.5f;
      float textY = m_pageIndicatorRect.top;

      // Draw input box background for current page number
      const float inputBoxPad = 6.0f * m_uiScale;
      D2D1_RECT_F inputBoxRect = D2D1::RectF(
          textStartX - inputBoxPad,
          textY + 1.0f * m_uiScale,
          textStartX + curW + inputBoxPad,
          textY + indicatorH - 1.0f * m_uiScale);
      D2D1_ROUNDED_RECT inputBoxRounded = D2D1::RoundedRect(inputBoxRect, 5.0f * m_uiScale, 5.0f * m_uiScale);
      ComPtr<ID2D1SolidColorBrush> inputBg;
      D2D1_COLOR_F inputBgColor = isLight
          ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f)
          : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f);
      if (m_pageIndicatorHover || m_pageInputActive) {
        inputBgColor = isLight
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f);
      }
      pRT->CreateSolidColorBrush(inputBgColor, &inputBg);
      if (inputBg) {
        pRT->FillRoundedRectangle(inputBoxRounded, inputBg.Get());
      }

      // Draw current page text (or input text with cursor)
      // Use fixed width (curW) so the text area doesn't shrink/grow
      if (m_pageInputActive) {
        // Show input text, right-aligned within the fixed-width box
        std::wstring display = m_pageInputText;
        float inputW = measureText(display);
        // If input text wider than box, it will be clipped on the left (natural scroll)
        float offsetX = 0.0f;
        if (inputW < curW) offsetX = (curW - inputW) * 0.5f; // center if small
        D2D1_RECT_F inputTextRect = D2D1::RectF(
            textStartX + offsetX, textY, textStartX + curW, textY + indicatorH);
        // Draw text (same color as normal mode)
        if (!display.empty()) {
          pRT->DrawText(display.c_str(), (UINT32)display.size(), m_textFormatUI.Get(),
                        inputTextRect, m_brushIcon.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
      } else {
        // Right-align the page number within the fixed box
        float numW = measureText(curStr);
        float offsetX = (curW - numW) * 0.5f;
        D2D1_RECT_F numTextRect = D2D1::RectF(
            textStartX + offsetX, textY, textStartX + offsetX + numW, textY + indicatorH);
        pRT->DrawText(curStr.c_str(), (UINT32)curStr.size(), m_textFormatUI.Get(),
                      numTextRect, m_brushIcon.Get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
      }

      // Draw separator "/"
      float sepX = textStartX + curW + partGap;
      D2D1_RECT_F sepTextRect = D2D1::RectF(
          sepX, textY, sepX + sepW, textY + indicatorH);
      pRT->DrawText(sepStr.c_str(), (UINT32)sepStr.size(), m_textFormatUI.Get(),
                    sepTextRect, m_brushIcon.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);

      // Draw total page count
      float totalX = sepX + sepW + partGap;
      D2D1_RECT_F totalTextRect = D2D1::RectF(
          totalX, textY, totalX + totalW, textY + indicatorH);
      pRT->DrawText(totalStr.c_str(), (UINT32)totalStr.size(), m_textFormatUI.Get(),
                    totalTextRect, m_brushIcon.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (m_compareMode && m_compareStepRect.right > m_compareStepRect.left) {
      D2D1_ROUNDED_RECT stepRect = D2D1::RoundedRect(
          m_compareStepRect, 6.0f * m_uiScale, 6.0f * m_uiScale);
      pRT->FillRoundedRectangle(stepRect, m_brushHover.Get());

      if (m_compareStepUpHover) {
        D2D1_ROUNDED_RECT upRect = D2D1::RoundedRect(
            m_compareStepUpRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(upRect, m_brushIconActive.Get());
      } else if (m_compareStepDownHover) {
        D2D1_ROUNDED_RECT downRect = D2D1::RoundedRect(
            m_compareStepDownRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(downRect, m_brushIconActive.Get());
      }

      const float stepBtnW = 14.0f * m_uiScale;
      D2D1_RECT_F divider = D2D1::RectF(
          m_compareStepRect.right - stepBtnW, m_compareStepRect.top + 2.0f * m_uiScale,
          m_compareStepRect.right - stepBtnW + 1.0f, m_compareStepRect.bottom - 2.0f * m_uiScale);
      pRT->FillRectangle(divider, m_brushIconDisabled.Get());

      wchar_t buf[16]{};
      swprintf_s(buf, L"%.1f%%", m_compareZoomStepPercent);
      D2D1_RECT_F textRect = D2D1::RectF(
          m_compareStepRect.left + 2.0f * m_uiScale, m_compareStepRect.top,
          m_compareStepRect.right - stepBtnW, m_compareStepRect.bottom);
      IDWriteTextFormat *stepFormat = m_textFormatUI.Get();
      
      // Use standard DrawText which handles standard fonts correctly
      pRT->DrawText(buf, (UINT32)wcslen(buf), stepFormat, textRect, m_brushIcon.Get());

      ComPtr<ID2D1Factory> factory;
      pRT->GetFactory(&factory);
      if (factory) {
        auto drawChevron = [&](const D2D1_RECT_F &rect, bool up) {
          const float cx = (rect.left + rect.right) * 0.5f;
          const float cy = (rect.top + rect.bottom) * 0.5f;
          const float size = 3.5f * m_uiScale;
          ComPtr<ID2D1PathGeometry> path;
          factory->CreatePathGeometry(&path);
          ComPtr<ID2D1GeometrySink> sink;
          path->Open(&sink);
          if (up) {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy + size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy - size));
            sink->AddLine(D2D1::Point2F(cx + size, cy + size));
          } else {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy - size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy + size));
            sink->AddLine(D2D1::Point2F(cx + size, cy - size));
          }
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          sink->Close();
          pRT->DrawGeometry(path.Get(), m_brushIcon.Get(), 1.5f * m_uiScale);
        };
        drawChevron(m_compareStepUpRect, true);
        drawChevron(m_compareStepDownRect, false);
      }
    }
    
    // [v10.5] Animation speed capsule
    if ((m_animMode || m_slideshowMode) && m_animSpeedRect.right > m_animSpeedRect.left) {
      D2D1_ROUNDED_RECT speedRect = D2D1::RoundedRect(
          m_animSpeedRect, 6.0f * m_uiScale, 6.0f * m_uiScale);
      pRT->FillRoundedRectangle(speedRect, m_brushHover.Get());

      if (m_animSpeedUpHover) {
        D2D1_ROUNDED_RECT upRect = D2D1::RoundedRect(
            m_animSpeedUpRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(upRect, m_brushIconActive.Get());
      } else if (m_animSpeedDownHover) {
        D2D1_ROUNDED_RECT downRect = D2D1::RoundedRect(
            m_animSpeedDownRect, 4.0f * m_uiScale, 4.0f * m_uiScale);
        pRT->FillRoundedRectangle(downRect, m_brushIconActive.Get());
      }

      const float sBtnW = 14.0f * m_uiScale;
      D2D1_RECT_F divider = D2D1::RectF(
          m_animSpeedRect.right - sBtnW, m_animSpeedRect.top + 2.0f * m_uiScale,
          m_animSpeedRect.right - sBtnW + 1.0f, m_animSpeedRect.bottom - 2.0f * m_uiScale);
      pRT->FillRectangle(divider, m_brushIconDisabled.Get());

      wchar_t buf[16]{};
      swprintf_s(buf, L"%.2gx", m_animSpeedMult);
      D2D1_RECT_F textRect = D2D1::RectF(
          m_animSpeedRect.left + 2.0f * m_uiScale, m_animSpeedRect.top,
          m_animSpeedRect.right - sBtnW, m_animSpeedRect.bottom);
      IDWriteTextFormat *sFmt = m_textFormatUI.Get();
      pRT->DrawText(buf, (UINT32)wcslen(buf), sFmt, textRect, m_brushIcon.Get());

      ComPtr<ID2D1Factory> factory;
      pRT->GetFactory(&factory);
      if (factory) {
        auto drawChevron = [&](const D2D1_RECT_F &rect, bool up) {
          const float cx = (rect.left + rect.right) * 0.5f;
          const float cy = (rect.top + rect.bottom) * 0.5f;
          const float size = 3.5f * m_uiScale;
          ComPtr<ID2D1PathGeometry> path;
          factory->CreatePathGeometry(&path);
          ComPtr<ID2D1GeometrySink> sink;
          path->Open(&sink);
          if (up) {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy + size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy - size));
            sink->AddLine(D2D1::Point2F(cx + size, cy + size));
          } else {
            sink->BeginFigure(D2D1::Point2F(cx - size, cy - size), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(D2D1::Point2F(cx, cy + size));
            sink->AddLine(D2D1::Point2F(cx + size, cy - size));
          }
          sink->EndFigure(D2D1_FIGURE_END_OPEN);
          sink->Close();
          pRT->DrawGeometry(path.Get(), m_brushIcon.Get(), 1.5f * m_uiScale);
        };
        drawChevron(m_animSpeedUpRect, true);
        drawChevron(m_animSpeedDownRect, false);
      }
    }
    // [Swatch] Render color swatches within the toolbar layer
    {
      extern AppConfig g_config;
      const float swatchR = 7.0f * m_uiScale;
      ComPtr<ID2D1Factory> factory;
      pRT->GetFactory(&factory);

      for (int i = 0; i < 9; ++i) {
        if (m_swatchRects[i].right <= m_swatchRects[i].left) continue;
        float cx = (m_swatchRects[i].left + m_swatchRects[i].right) * 0.5f;
        float cy = (m_swatchRects[i].top + m_swatchRects[i].bottom) * 0.5f;
        D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), swatchR, swatchR);

        if (i < 3) {
          // Built-in checkerboard presets
          D2D1_COLOR_F c1, c2;
          if (i == 0) { c1 = D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f); c2 = D2D1::ColorF(0.80f, 0.80f, 0.80f, 1.0f); }
          else if (i == 1) { c1 = D2D1::ColorF(0.10f, 0.10f, 0.10f, 1.0f); c2 = D2D1::ColorF(0.18f, 0.18f, 0.18f, 1.0f); }
          else { c1 = D2D1::ColorF(0.50f, 0.50f, 0.50f, 1.0f); c2 = D2D1::ColorF(0.60f, 0.60f, 0.60f, 1.0f); }

          ComPtr<ID2D1EllipseGeometry> clipGeo;
          if (factory) factory->CreateEllipseGeometry(ellipse, &clipGeo);
          ComPtr<ID2D1Layer> swatchLayer;
          if (clipGeo && SUCCEEDED(pRT->CreateLayer(&swatchLayer))) {
            pRT->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), swatchLayer.Get());
            ComPtr<ID2D1SolidColorBrush> b1, b2;
            pRT->CreateSolidColorBrush(c1, &b1);
            pRT->CreateSolidColorBrush(c2, &b2);
            pRT->FillRectangle(m_swatchRects[i], b1.Get());
            pRT->FillRectangle(D2D1::RectF(cx, m_swatchRects[i].top, m_swatchRects[i].right, cy), b2.Get());
            pRT->FillRectangle(D2D1::RectF(m_swatchRects[i].left, cy, cx, m_swatchRects[i].bottom), b2.Get());
            pRT->PopLayer();
          }
        } else {
          // Custom RGBA color
          // [Fix] alpha 量化为 255 整数后判断是否完全不透明
          int a255 = g_config.SwatchColors[i][3];
          if (a255 > 255) a255 = 255;
          if (a255 < 0) a255 = 0;
          float alpha = C8(a255);
          if (a255 < 255) {
            // Mini checkerboard background for transparency
            ComPtr<ID2D1EllipseGeometry> clipGeo;
            if (factory) factory->CreateEllipseGeometry(ellipse, &clipGeo);
            ComPtr<ID2D1Layer> swatchLayer;
            if (clipGeo && SUCCEEDED(pRT->CreateLayer(&swatchLayer))) {
              pRT->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clipGeo.Get()), swatchLayer.Get());
              ComPtr<ID2D1SolidColorBrush> cb1, cb2;
              pRT->CreateSolidColorBrush(D2D1::ColorF(0.7f, 0.7f, 0.7f, 1.0f), &cb1);
              pRT->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.4f, 0.4f, 1.0f), &cb2);
              pRT->FillRectangle(m_swatchRects[i], cb1.Get());
              pRT->FillRectangle(D2D1::RectF(cx, m_swatchRects[i].top, m_swatchRects[i].right, cy), cb2.Get());
              pRT->FillRectangle(D2D1::RectF(m_swatchRects[i].left, cy, cx, m_swatchRects[i].bottom), cb2.Get());
              pRT->PopLayer();
            }
          }
          D2D1_COLOR_F color(C8(g_config.SwatchColors[i][0]), C8(g_config.SwatchColors[i][1]), C8(g_config.SwatchColors[i][2]), alpha);
          ComPtr<ID2D1SolidColorBrush> brush;
          pRT->CreateSolidColorBrush(color, &brush);
          pRT->FillEllipse(ellipse, brush.Get());
        }

        // Selected ring
        if (i == g_config.SwatchColorIndex && g_config.CanvasColor == 5) {
          ComPtr<ID2D1SolidColorBrush> ringBrush;
          pRT->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f), &ringBrush);
          pRT->DrawEllipse(ellipse, ringBrush.Get(), 2.0f * m_uiScale);
        }
        // Hover ring (theme-aware contrast)
        if (i == m_swatchHoverIndex) {
          ComPtr<ID2D1SolidColorBrush> hoverBrush;
          D2D1_COLOR_F hoverColor = isLight
              ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.4f)
              : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.6f);
          pRT->CreateSolidColorBrush(hoverColor, &hoverBrush);
          pRT->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), swatchR + 1.5f * m_uiScale, swatchR + 1.5f * m_uiScale), hoverBrush.Get(), 1.5f * m_uiScale);
        }
      }
    }
    pRT->PopLayer();
  }

  const wchar_t *activeTip = nullptr;
  D2D1_RECT_F activeRect = {0, 0, 0, 0};

  for (const auto &btn : m_buttons) {
    if (btn.isHovered) {
      activeTip = GetTooltipText(btn);
      activeRect = btn.rect;
      break;
    }
  }

  if (!activeTip && (m_animMode || m_slideshowMode) && m_animSpeedHover) {
    activeTip = AppStrings::Toolbar_Tooltip_AnimSpeed;
    activeRect = m_animSpeedRect;
  }

  if (activeTip && activeTip[0] != 0) {
    static ComPtr<IDWriteTextFormat> tooltipFormat;
    static float tooltipScale = 0.0f;
    if (tooltipFormat && fabsf(tooltipScale - m_uiScale) >= 0.001f) {
      tooltipFormat.Reset();
    }
    if (!tooltipFormat && m_dwriteFactory) {
      m_dwriteFactory->CreateTextFormat(
          L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          12.0f * m_uiScale, AppStrings::CurrentLocale, &tooltipFormat);
      if (tooltipFormat) {
        tooltipFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        tooltipFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        tooltipScale = m_uiScale;
      }
    }

    if (tooltipFormat) {
      size_t tipLen = wcslen(activeTip);
      ComPtr<IDWriteTextLayout> textLayout;
      float tipWidth = tipLen * 10.0f * m_uiScale + 16.0f * m_uiScale;
      if (m_dwriteFactory) {
        m_dwriteFactory->CreateTextLayout(activeTip, (UINT32)tipLen,
                                          tooltipFormat.Get(), 500.0f * m_uiScale,
                                          40.0f * m_uiScale, &textLayout);
        if (textLayout) {
          DWRITE_TEXT_METRICS metrics;
          textLayout->GetMetrics(&metrics);
          tipWidth = metrics.width + 16.0f * m_uiScale;
        }
      }
      float tipHeight = 22.0f * m_uiScale;
      float tipX = (activeRect.left + activeRect.right) / 2 - tipWidth / 2;
      float tipY = m_bgRect.rect.top - tipHeight - 8.0f * m_uiScale;
      if (tipX < 5.0f * m_uiScale)
        tipX = 5.0f * m_uiScale;
      D2D1_RECT_F tipRect =
          D2D1::RectF(tipX, tipY, tipX + tipWidth, tipY + tipHeight);
      ComPtr<ID2D1SolidColorBrush> tipBg;
      D2D1_COLOR_F tipBgBase = isLight
          ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f)
          : D2D1::ColorF(0.15f, 0.15f, 0.15f, 0.95f);
      pRT->CreateSolidColorBrush(tipBgBase, &tipBg);
      pRT->FillRoundedRectangle(
          D2D1::RoundedRect(tipRect, 4.0f * m_uiScale, 4.0f * m_uiScale),
          tipBg.Get());
      pRT->DrawText(activeTip, (UINT32)tipLen, tooltipFormat.Get(), tipRect,
                    m_brushIcon.Get());
    }
  }
}

bool Toolbar::OnMouseMove(float x, float y) {
  if (m_windowTooNarrow) return false;
  bool changed = false;
  bool stepHover = false, stepUpHover = false, stepDownHover = false;
  for (auto &btn : m_buttons) {
    bool wasHovered = btn.isHovered;
    btn.isHovered = (btn.rect.right > 0 && x >= btn.rect.left && x < btn.rect.right && y >= btn.rect.top && y < btn.rect.bottom);
    if (btn.isHovered != wasHovered) changed = true;
  }
  if (m_compareMode && m_compareStepRect.right > m_compareStepRect.left) {
    if (x >= m_compareStepRect.left && x < m_compareStepRect.right && y >= m_compareStepRect.top && y < m_compareStepRect.bottom) {
      stepHover = true;
      if (x >= m_compareStepUpRect.left && x < m_compareStepUpRect.right && y >= m_compareStepUpRect.top && y < m_compareStepUpRect.bottom) stepUpHover = true;
      else if (x >= m_compareStepDownRect.left && x < m_compareStepDownRect.right && y >= m_compareStepDownRect.top && y < m_compareStepDownRect.bottom) stepDownHover = true;
    }
  }
  if (!m_compareMode) { stepHover = stepUpHover = stepDownHover = false; }
  if (stepHover != m_compareStepHover || stepUpHover != m_compareStepUpHover || stepDownHover != m_compareStepDownHover) {
    changed = true;
    m_compareStepHover = stepHover; m_compareStepUpHover = stepUpHover; m_compareStepDownHover = stepDownHover;
  }
  // [v10.5] Animation speed capsule hover
  bool sHover = false, sUpHover = false, sDownHover = false;
  if ((m_animMode || m_slideshowMode) && m_animSpeedRect.right > m_animSpeedRect.left) {
    if (x >= m_animSpeedRect.left && x < m_animSpeedRect.right && y >= m_animSpeedRect.top && y < m_animSpeedRect.bottom) {
      sHover = true;
      if (x >= m_animSpeedUpRect.left && x < m_animSpeedUpRect.right && y >= m_animSpeedUpRect.top && y < m_animSpeedUpRect.bottom) sUpHover = true;
      else if (x >= m_animSpeedDownRect.left && x < m_animSpeedDownRect.right && y >= m_animSpeedDownRect.top && y < m_animSpeedDownRect.bottom) sDownHover = true;
    }
  }
  if (!m_animMode) { sHover = sUpHover = sDownHover = false; }
  if (sHover != m_animSpeedHover || sUpHover != m_animSpeedUpHover || sDownHover != m_animSpeedDownHover) {
    changed = true;
    m_animSpeedHover = sHover; m_animSpeedUpHover = sUpHover; m_animSpeedDownHover = sDownHover;
  }
  
  bool progHover = false;
  float newSeekHover = m_animSeekHoverProgress;
  if (m_animMode && m_animProgressRect.right > m_animProgressRect.left) {
      if ((x >= m_animProgressRect.left && x <= m_animProgressRect.right && y >= m_animProgressRect.top && y <= m_animProgressRect.bottom) || m_isDraggingProgress) {
          progHover = true; // Still show hover highlight while dragging
          newSeekHover = (x - m_animProgressRect.left) / (m_animProgressRect.right - m_animProgressRect.left);
          newSeekHover = (std::max)(0.0f, (std::min)(1.0f, newSeekHover));
      }
  }
  
  if (progHover != m_animProgressHover || (progHover && !m_animPlaying && fabsf(newSeekHover - m_animSeekHoverProgress) > 0.0001f)) {
      changed = true;
  }
  
  m_animProgressHover = progHover;
  m_animSeekHoverProgress = newSeekHover;
  
  // [Swatch] Hover tracking
  int newSwatchHover = -1;
  for (int i = 0; i < 9; ++i) {
    if (m_swatchRects[i].right > m_swatchRects[i].left &&
        x >= m_swatchRects[i].left && x <= m_swatchRects[i].right &&
        y >= m_swatchRects[i].top && y <= m_swatchRects[i].bottom) {
      newSwatchHover = i;
      break;
    }
  }
  if (newSwatchHover != m_swatchHoverIndex) {
    m_swatchHoverIndex = newSwatchHover;
    changed = true;
  }

  // [PDF/AI/CDR] Page indicator hover
  bool pageHover = false;
  if (m_showPageIndicator && m_pageIndicatorRect.right > m_pageIndicatorRect.left) {
    pageHover = (x >= m_pageIndicatorRect.left && x <= m_pageIndicatorRect.right &&
                 y >= m_pageIndicatorRect.top && y <= m_pageIndicatorRect.bottom);
  }
  if (pageHover != m_pageIndicatorHover) {
    m_pageIndicatorHover = pageHover;
    changed = true;
  }

  return changed;
}

bool Toolbar::OnClick(float x, float y, ToolbarButtonID &outId) {
  if (m_windowTooNarrow || !IsVisible()) return false;

  // [PDF/AI/CDR] Page indicator click
  if (m_showPageIndicator && IsPageIndicatorHit(x, y)) {
    outId = ToolbarButtonID::None;
    return true;
  }

  // Progress bar click
  if (m_animMode && m_animProgressHover) {
    outId = ToolbarButtonID::AnimSeek;
    return true;
  }

  // [Swatch] Check swatch clicks FIRST (before HitTest, so it works on welcome screen too)
  for (int i = 0; i < 9; ++i) {
    if (m_swatchRects[i].right > m_swatchRects[i].left &&
        x >= m_swatchRects[i].left && x <= m_swatchRects[i].right &&
        y >= m_swatchRects[i].top && y <= m_swatchRects[i].bottom) {
      m_swatchClickIndex = i;
      outId = ToolbarButtonID::SwatchSelect;
      return true;
    }
  }

  if (HitTest(x, y)) {
    if (m_compareMode && m_compareStepRect.right > m_compareStepRect.left) {
      if (x >= m_compareStepUpRect.left && x < m_compareStepUpRect.right && y >= m_compareStepUpRect.top && y < m_compareStepUpRect.bottom) {
        m_compareZoomStepPercent = (std::min)(5.0f, m_compareZoomStepPercent + 0.1f);
        outId = ToolbarButtonID::None; return true;
      }
      if (x >= m_compareStepDownRect.left && x < m_compareStepDownRect.right && y >= m_compareStepDownRect.top && y < m_compareStepDownRect.bottom) {
        m_compareZoomStepPercent = (std::max)(0.1f, m_compareZoomStepPercent - 0.1f);
        outId = ToolbarButtonID::None; return true;
      }
    }
    // [v10.5] Animation speed capsule clicks
    if ((m_animMode || m_slideshowMode) && m_animSpeedRect.right > m_animSpeedRect.left) {
      if (x >= m_animSpeedUpRect.left && x < m_animSpeedUpRect.right && y >= m_animSpeedUpRect.top && y < m_animSpeedUpRect.bottom) {
        m_animSpeedMult = (std::min)(4.0f, m_animSpeedMult + 0.25f);
        outId = ToolbarButtonID::AnimSpeedUp; return true;
      }
      if (x >= m_animSpeedDownRect.left && x < m_animSpeedDownRect.right && y >= m_animSpeedDownRect.top && y < m_animSpeedDownRect.bottom) {
        m_animSpeedMult = (std::max)(0.25f, m_animSpeedMult - 0.25f);
        outId = ToolbarButtonID::AnimSpeedDown; return true;
      }
    }
    // Progress bar click
    if (m_animMode && m_animProgressHover) {
      outId = ToolbarButtonID::AnimSeek;
      return true;
    }
    for (auto &btn : m_buttons) {
      if (btn.rect.right > 0 && x >= btn.rect.left && x < btn.rect.right && y >= btn.rect.top && y < btn.rect.bottom) {
        outId = btn.id; return true;
      }
    }
    return true;
  }
  return false;
}

bool Toolbar::HitTest(float x, float y) {
  if (!IsVisible() || m_windowTooNarrow) return false;
  extern GalleryOverlay g_gallery;
  extern SettingsOverlay g_settingsOverlay;
  extern HelpOverlay g_helpOverlay;
  extern std::wstring& g_imagePath;

  // [Swatch] Check swatch hits FIRST - allowed even on welcome screen
  for (int i = 0; i < 9; ++i) {
    if (m_swatchRects[i].right > m_swatchRects[i].left &&
        x >= m_swatchRects[i].left && x <= m_swatchRects[i].right &&
        y >= m_swatchRects[i].top && y <= m_swatchRects[i].bottom) return true;
  }

  bool isFullGridGallery = g_gallery.IsVisible() && g_gallery.GetMode() == GalleryMode::FullGrid;
  bool isWelcomeScreen = g_imagePath.empty() && !g_gallery.IsVisible();

  if (g_settingsOverlay.IsVisible() || g_helpOverlay.IsVisible() || isFullGridGallery || isWelcomeScreen) return false;

  // 1. Standard background capsule
  if (x >= m_bgRect.rect.left && x <= m_bgRect.rect.right && y >= m_bgRect.rect.top && y <= m_bgRect.rect.bottom) return true;

  // 2. Animation progress bar (this area is floating outside the capsule)
  if (m_animMode) {
      if (x >= m_animProgressRect.left && x <= m_animProgressRect.right && 
          y >= m_animProgressRect.top && y <= m_animProgressRect.bottom) {
          return true;
      }
  }

  return false;
}

void Toolbar::SetVisible(bool visible) { m_targetVisible = visible; }

bool Toolbar::UpdateAnimation() {
  if (!g_config.GlassUIAnimations) {
      if (m_targetVisible) {
          m_opacity = 1.0f;
      } else {
          m_opacity = 0.0f;
      }
      return false; // Fast cut
  }
  float speed = 0.34f;
  if (m_targetVisible) {
    if (m_opacity < 1.0f) { m_opacity += speed; if (m_opacity > 1.0f) m_opacity = 1.0f; return true; }
  } else {
    if (m_opacity > 0.0f) { m_opacity -= speed; if (m_opacity < 0.0f) m_opacity = 0.0f; return true; }
  }
  return false;
}

void Toolbar::SetLockState(bool locked) {
  for (auto &btn : m_buttons) { if (btn.id == ToolbarButtonID::LockSize) { btn.isToggled = locked; btn.iconGlyph = locked ? Icons::Lock : Icons::Unlock; } }
}

void Toolbar::SetExifState(bool /*open*/) {
  // [Removed] Info panel button removed; kept as no-op for call-site compatibility
}

void Toolbar::SetRawState(bool isRaw, bool isFullDecode, bool isPaired) {
  for (auto &btn : m_buttons) { if (btn.id == ToolbarButtonID::RawToggle) { btn.isEnabled = isRaw; btn.isPaired = isRaw && isPaired; if (isRaw) { btn.isToggled = isFullDecode; } } }
}

void Toolbar::SetGamutWarningAvailable(bool available) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::GamutWarning) {
      btn.isEnabled = available;
      if (!available) btn.isToggled = false;
    }
  }
}

void Toolbar::SetGamutWarningActive(bool active) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::GamutWarning && btn.isEnabled) {
      btn.isToggled = active;
    }
  }
}

void Toolbar::SetCompareMode(bool enabled) {
  m_compareMode = enabled;
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareToggle) btn.isToggled = enabled;
  }
}

void Toolbar::SetCompareSyncStates(bool syncZoom, bool syncPan) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareSyncZoom) btn.isToggled = syncZoom;
    if (btn.id == ToolbarButtonID::CompareSyncPan) btn.isToggled = syncPan;
  }
}

void Toolbar::SetCompareInfoState(bool active) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareInfo) {
      btn.isToggled = active;
    }
  }
}

void Toolbar::SetCompareRawState(bool anyRaw, bool selectedIsRaw, bool isFullDecode) {
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::CompareRawToggle) {
      // isWarning reused as 'visible in compare' flag
      btn.isWarning = anyRaw;
      btn.isEnabled = selectedIsRaw;
      if (selectedIsRaw) { btn.isToggled = isFullDecode; }
    }
  }
}

void Toolbar::SetSlideshowMode(bool enabled, bool playing) {
  m_slideshowMode = enabled;
  m_animPlaying = playing;
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::AnimPlayPause) {
      btn.iconGlyph = playing ? Icons::Pause : Icons::Play;
      btn.isToggled = playing;
    }
    if (btn.id == ToolbarButtonID::SlideshowImmersiveToggle) {
      btn.isToggled = (g_config.SlideshowImmersiveMode == 1);
    }
    if (btn.id == ToolbarButtonID::AnimPrevFrame || btn.id == ToolbarButtonID::AnimNextFrame) {
      btn.isEnabled = true;
    }
  }
}

void Toolbar::SetAnimationMode(bool enabled, bool playing, bool dirtyRect, bool supportsDirtyRect) {
  m_animMode = enabled;
  m_animDirtyRect = dirtyRect;
  if (!m_slideshowMode) {
    m_animPlaying = playing;
  }
  for (auto &btn : m_buttons) {
    if (btn.id == ToolbarButtonID::AnimPlayPause && !m_slideshowMode) {
      btn.iconGlyph = playing ? Icons::Pause : Icons::Play;
      btn.isToggled = playing;
    }
    if (btn.id == ToolbarButtonID::AnimPrevFrame || btn.id == ToolbarButtonID::AnimNextFrame) {
      btn.isEnabled = true;
    }
    if (btn.id == ToolbarButtonID::AnimDirtyRect) {
      btn.isToggled = dirtyRect;
      btn.isEnabled = supportsDirtyRect;
    }
  }
}

