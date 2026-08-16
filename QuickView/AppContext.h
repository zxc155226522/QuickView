#pragma once

#include <windows.h>
#include <d2d1_2.h>
#include <string>
#include <vector>
#include <chrono>
#include "QuickView.h"
#include "PaneTypes.h"

// --- Dialog Definitions ---
enum class DialogResult { None, Yes, No, Cancel, Custom1, Custom2 };

struct DialogButton {
    DialogResult Result;
    std::wstring Text;
    bool IsDefault;
    DialogButton(DialogResult r, const wchar_t* t, bool d = false) : Result(r), Text(t), IsDefault(d) {}
    DialogButton(const DialogButton&) = default;
    DialogButton(DialogButton&&) = default;
    DialogButton& operator=(const DialogButton&) = default;
    DialogButton& operator=(DialogButton&&) = default;
};

struct DialogLayout {
    D2D1_RECT_F Box;
    D2D1_RECT_F Checkbox;
    D2D1_RECT_F Input; 
    std::vector<D2D1_RECT_F> Buttons;
};

struct DialogState {
    bool IsVisible = false;
    std::wstring Title;
    std::wstring Message;
    std::wstring QualityText; 
    D2D1_COLOR_F AccentColor = D2D1::ColorF(D2D1::ColorF::DodgerBlue);
    std::vector<DialogButton> Buttons;
    int SelectedButtonIndex = 0;
    bool HasCheckbox = false;
    std::wstring CheckboxText;
    bool IsChecked = false;
    
    // [Input Mode]
    bool HasInput = false;
    std::wstring InputText;
    HWND hEdit = nullptr;
    HWND hInputHost = nullptr; 
    WNDPROC oldEditProc = nullptr;
    HFONT hFont = nullptr;

    DialogResult FinalResult = DialogResult::None;
    bool UseCustomCenter = false;
    D2D1_POINT_2F CustomCenter = D2D1::Point2F(0.0f, 0.0f);
};

// --- Smooth Zoom Definitions ---
struct SmoothZoomState {
    bool Active = false;
    uint64_t ImageId = 0;
    bool AnimateWindow = false;
    bool HasAnchor = false;
    POINT AnchorScreenPt = { 0, 0 };
    RECT SourceWindowRect = { 0, 0, 0, 0 };
    RECT TargetWindowRect = { 0, 0, 0, 0 };
    float SourceZoom = 1.0f;
    float CurrentZoom = 1.0f;
    float CurrentPanX = 0.0f;
    float CurrentPanY = 0.0f;
    float TargetZoom = 1.0f;
    float TargetPanX = 0.0f;
    float TargetPanY = 0.0f;
    float AnchorImageX = 0.0f;
    float AnchorImageY = 0.0f;
    float LastWinW = 0.0f;
    float LastWinH = 0.0f;
    ULONGLONG LastTick = 0;

    void Reset();
};

struct SmoothWindowZoomState {
    bool active = false;
    std::chrono::steady_clock::time_point startTime;
    float durationMs = 90.0f;
    RECT startRect{};
    RECT targetRect{};
    float startZoom = 1.0f;
    float targetZoom = 1.0f;
    float startPanX = 0.0f;
    float startPanY = 0.0f;
    float targetPanX = 0.0f;
    float targetPanY = 0.0f;
};

// --- Compare Mode Definitions ---
enum class ViewMode {
    Single = 0,
    CompareSideBySide,
    CompareWipe
};

enum class ComparePane {
    Left = 0,
    Right
};


#include "EditState.h"

using CompareView = ViewState;

struct CompareState {
    ViewMode mode = ViewMode::Single;
    float splitRatio = 0.5f;
    bool syncZoom = true;
    bool syncPan = true;
    bool draggingDivider = false;
    ComparePane activePane = ComparePane::Right;
    ComparePane contextPane = ComparePane::Right;
    ComparePane selectedPane = ComparePane::Right;
    bool dirty = false;
    bool autoExpandedWindow = false;
    bool pendingSnap = false;
    float dividerOpacity = 0.0f;
    bool showDividerHandle = false;
};

// --- Loupe Definitions ---
// A press-and-hold magnifier that pops up under the cursor and shows the local
// region at actual pixels (e.g. for quickly confirming focus while culling).
// Activated by holding the HotkeyAction::Loupe key (rebindable, default 'L');
// follows the cursor while held and disappears on release. Works in Compare
// mode too (the same image location is magnified on both panes).
struct LoupeState {
    bool active = false;
    POINT cursorClient = { 0, 0 }; // current cursor position in client coords
    bool sizeChanged = false;      // wheel resized the loupe this session -> persist on release
};

struct MinimapState {
    bool closedByUser = false;
    bool isDraggingWindow = false;
    bool isDraggingView = false;
    bool isEdgeHovered = false;
    bool isCloseHovered = false;
    POINT dragAnchor = { 0, 0 };
    float dragStartOffsetX = 0.0f;
    float dragStartOffsetY = 0.0f;
    float dragStartMinimapX = 0.0f;
    float dragStartMinimapY = 0.0f;
    float dragStartPanX = 0.0f;
    float dragStartPanY = 0.0f;
    D2D1_RECT_F layoutRect = { 0.0f, 0.0f, 0.0f, 0.0f };
    D2D1_RECT_F closeBtnRect = { 0.0f, 0.0f, 0.0f, 0.0f };
    D2D1_RECT_F innerRect = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool isResizing = false;
    int resizeCorner = -1; // 当前拖动的缩放角: 0=TL(左上), 2=BR(右下)
    D2D1_RECT_F resizeGripRectTL = { 0.0f, 0.0f, 0.0f, 0.0f };
    D2D1_RECT_F resizeGripRectBR = { 0.0f, 0.0f, 0.0f, 0.0f };

    void ResetLayout() {
        closedByUser = false;
        isDraggingWindow = false;
        isDraggingView = false;
        isEdgeHovered = false;
        isCloseHovered = false;
        isResizing = false;
        resizeCorner = -1;
    }
};

struct MinimapGeometry {
    float fitScale = 1.0f;
    float drawW = 0.0f;
    float drawH = 0.0f;
    float imgDrawX = 0.0f;
    float imgDrawY = 0.0f;
};

// ── 鸟瞰图布局统一计算（绘制 / 拖拽 / 缩放共用同一套公式）──
struct MinimapLayoutParams {
    D2D1_RECT_F vpRect;        // 当前面板视口矩形（屏幕物理 px）
    float winW = 0.0f;         // 窗口客户区宽度（用于判断是否在最右面板）
    float uiScale = 1.0f;      // DPI 缩放
    bool showControls = true;  // 标题栏是否可见
    float toolbarReserved = 0.0f; // 工具栏预留高度（0 表示无工具栏）
    // 配置参数
    int navigatorW = 150;      // 逻辑 px
    int navigatorH = 150;
    int navigatorAlignX = 1;   // 0=Left, 1=Right
    int navigatorAlignY = 1;   // 0=Top, 1=Bottom
    float navigatorOffsetX = 12.0f;
    float navigatorOffsetY = 12.0f;
};

struct MinimapLayoutResult {
    D2D1_RECT_F layoutRect = { 0, 0, 0, 0 };
    D2D1_RECT_F innerRect = { 0, 0, 0, 0 };
    D2D1_RECT_F closeBtnRect = { 0, 0, 0, 0 };
    D2D1_RECT_F resizeGripRectTL = { 0, 0, 0, 0 };
    D2D1_RECT_F resizeGripRectBR = { 0, 0, 0, 0 };
    float topOffset = 0.0f;    // 供拖拽/缩放反推 offset 时使用
    float vpBottom = 0.0f;     // 供拖拽/缩放反推 offset 时使用
};

// 从 MinimapLayoutResult 反推 viewport 坐标 → 可用于拖拽 / 缩放时固定锚点
inline D2D1_RECT_F MinimapViewportRectFromLayout(const MinimapLayoutParams& p) {
    const float s = p.uiScale;
    float minimapW = (std::max)((float)p.navigatorW * s, 40.0f * s);
    float minimapH = (std::max)((float)p.navigatorH * s, 40.0f * s);

    float topOffset = p.vpRect.top;
    if (p.vpRect.right >= p.winW - 1.0f && p.showControls) {
        topOffset += 32.0f * s;
    }
    float vpBottom = p.vpRect.bottom - p.toolbarReserved;

    float minimapX;
    if (p.navigatorAlignX == 0) {
        minimapX = p.vpRect.left + p.navigatorOffsetX * s;
    } else {
        minimapX = p.vpRect.right - minimapW - p.navigatorOffsetX * s;
    }
    float minimapY;
    if (p.navigatorAlignY == 0) {
        minimapY = topOffset + p.navigatorOffsetY * s;
    } else {
        minimapY = vpBottom - minimapH - p.navigatorOffsetY * s;
    }
    float margin = 8.0f * s;
    float minX = p.vpRect.left + margin;
    float maxX = p.vpRect.right - minimapW - margin;
    float minY = topOffset + margin;
    float maxY = vpBottom - minimapH - margin;
    minimapX = std::clamp(minimapX, minX, (std::max)(minX, maxX));
    minimapY = std::clamp(minimapY, minY, (std::max)(minY, maxY));
    return D2D1::RectF(minimapX, minimapY, minimapX + minimapW, minimapY + minimapH);
}

// 统一鸟瞰图布局计算：绘制 / 拖拽 / 缩放均调用此函数，确保公式一致
inline MinimapLayoutResult CalculateMinimapLayout(const MinimapLayoutParams& p) {
    MinimapLayoutResult r;
    const float s = p.uiScale;

    float minimapW = (std::max)((float)p.navigatorW * s, 40.0f * s);
    float minimapH = (std::max)((float)p.navigatorH * s, 40.0f * s);

    // topOffset：标题栏预留
    float topOffset = p.vpRect.top;
    if (p.vpRect.right >= p.winW - 1.0f && p.showControls) {
        topOffset += 32.0f * s;
    }

    // vpBottom：扣除工具栏预留
    float vpBottom = p.vpRect.bottom - p.toolbarReserved;

    // 水平位置
    float minimapX = 0.0f;
    if (p.navigatorAlignX == 0) {
        minimapX = p.vpRect.left + p.navigatorOffsetX * s;
    } else {
        minimapX = p.vpRect.right - minimapW - p.navigatorOffsetX * s;
    }

    // 垂直位置
    float minimapY = 0.0f;
    if (p.navigatorAlignY == 0) {
        minimapY = topOffset + p.navigatorOffsetY * s;
    } else {
        minimapY = vpBottom - minimapH - p.navigatorOffsetY * s;
    }

    // Clamp to keep fully within viewport
    float margin = 8.0f * s;
    float minX = p.vpRect.left + margin;
    float maxX = p.vpRect.right - minimapW - margin;
    float minY = topOffset + margin;
    float maxY = vpBottom - minimapH - margin;
    minimapX = std::clamp(minimapX, minX, (std::max)(minX, maxX));
    minimapY = std::clamp(minimapY, minY, (std::max)(minY, maxY));

    r.layoutRect = D2D1::RectF(minimapX, minimapY, minimapX + minimapW, minimapY + minimapH);
    r.innerRect = r.layoutRect;

    float closeBtnSize = 16.0f * s;
    r.closeBtnRect = D2D1::RectF(
        r.layoutRect.right - closeBtnSize, r.layoutRect.top,
        r.layoutRect.right, r.layoutRect.top + closeBtnSize);

    float grip = 16.0f * s;
    r.resizeGripRectTL = D2D1::RectF(
        r.layoutRect.left, r.layoutRect.top,
        r.layoutRect.left + grip, r.layoutRect.top + grip);
    r.resizeGripRectBR = D2D1::RectF(
        r.layoutRect.right - grip, r.layoutRect.bottom - grip,
        r.layoutRect.right, r.layoutRect.bottom);

    r.topOffset = topOffset;
    r.vpBottom = vpBottom;
    return r;
}

// Calculate isotropic scaling and centered drawing bounds for the thumbnail inside minimap container.
inline MinimapGeometry CalculateMinimapGeometry(D2D1_RECT_F innerRect, D2D1_SIZE_F orientedSize) {
    MinimapGeometry geo;
    if (orientedSize.width <= 0.0f || orientedSize.height <= 0.0f) return geo;

    const float minimapW = innerRect.right - innerRect.left;
    const float minimapH = innerRect.bottom - innerRect.top;
    if (minimapW <= 0.0f || minimapH <= 0.0f) return geo;

    const float scaleX = minimapW / orientedSize.width;
    const float scaleY = minimapH / orientedSize.height;
    geo.fitScale = (std::min)(scaleX, scaleY);

    geo.drawW = orientedSize.width * geo.fitScale;
    geo.drawH = orientedSize.height * geo.fitScale;

    const float centerX = (innerRect.left + innerRect.right) * 0.5f;
    const float centerY = (innerRect.top + innerRect.bottom) * 0.5f;
    geo.imgDrawX = centerX - geo.drawW * 0.5f;
    geo.imgDrawY = centerY - geo.drawH * 0.5f;

    return geo;
}


// --- Global App Context ---
// Using a Singleton for stage 1 refactoring, easy to migrate to DI later.
#include <memory>
class CompareController;
class DialogController;
class SmoothZoomController;

class AppContext {
public:
    static AppContext& GetInstance();

    DialogState Dialog;
    SmoothZoomState SmoothZoom;
    SmoothWindowZoomState SmoothWindowZoom;
    CompareState Compare;
    LoupeState Loupe;
    MinimapState Minimaps[2];

    std::unique_ptr<CompareController> CompareCtrl;
    std::unique_ptr<DialogController> DialogCtrl;
    std::unique_ptr<SmoothZoomController> ZoomAnimCtrl;

    bool IsFullScreen = false;
    bool IsDraggingAnimSeek = false;
    bool WindowSizeRestoredFromConfig = false;
    WINDOWPLACEMENT SavedWindowPlacement = { sizeof(WINDOWPLACEMENT), 0, 0, {0,0}, {0,0}, {0,0,0,0} };

    bool ShowDebugHUD = false;
    bool ShowTileGrid = false;
    float FPS = 0.0f;

    bool ProgrammaticResize = false;
    bool DeferProgrammaticZoomResizeSync = false;

    // Additional generic app flags that clutter main.cpp
    bool IsLoading = false;
    bool IsImageScaled = false;
    bool AnimPlaying = true;
    int AnimInspectorFrame = -1;
    bool ShowAnimDirtyRect = false;

private:
    AppContext();
    ~AppContext();

    AppContext(const AppContext&) = delete;
    AppContext& operator=(const AppContext&) = delete;
};

class Toolbar;
extern Toolbar g_toolbar;

class UIRenderer;
class CImageLoader;
extern std::unique_ptr<UIRenderer> g_uiRenderer;
extern std::unique_ptr<CImageLoader> g_imageLoader;
extern float g_uiScale;

namespace QuickView { enum class PaintLayer : uint32_t; }
void RequestRepaint(QuickView::PaintLayer layer);
void EnsureWindowSizeForDialog(HWND hwnd);

#define g_isFullScreen AppContext::GetInstance().IsFullScreen
#define g_isLoading AppContext::GetInstance().IsLoading

