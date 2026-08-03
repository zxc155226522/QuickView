#pragma once
// ============================================================
// ColorPickerPopup.h - D2D 色盘弹窗
// ============================================================
// 复用 GeekContextMenu 的 D2D+DComp 弹窗模式
// 布局: SV方块 + 色相条 + 透明条 + 预览
// 拖动时实时回调 onChange, 关闭时回调 onConfirm
// ============================================================

#include "pch.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <functional>
#include <memory>

namespace QuickView::UI {

using Microsoft::WRL::ComPtr;

class ColorPickerPopup {
public:
    using ColorCallback = std::function<void(float r, float g, float b, float a)>;

    // 弹出色盘 (屏幕坐标)
    // initialRGBA: 初始颜色 [r,g,b,a] 0.0-1.0
    // onChange: 拖动时实时回调 (可为nullptr)
    // onConfirm: 关闭时回调 (可为nullptr)
    static void Show(HWND parent, int screenX, int screenY,
                     float initialR, float initialG, float initialB, float initialA,
                     ColorCallback onChange = nullptr,
                     ColorCallback onConfirm = nullptr);
    static void Dismiss();
    static bool IsOpen() { return s_instance != nullptr; }

private:
    // Window
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMsg(HWND, UINT, WPARAM, LPARAM);
    static void EnsureClassRegistered();
    static bool s_classRegistered;

    // D2D / DComp
    void CreateResources();
    void DiscardResources();
    void Render();

    // Layout
    void CalculateLayout();
    SIZE GetWindowSize() const;

    // Color math
    static void RgbToHsv(float r, float g, float b, float& h, float& s, float& v);
    static void HsvToRgb(float h, float s, float v, float& r, float& g, float& b);

    // Interaction
    void OnMouseMove(int x, int y);
    void OnLButtonDown(int x, int y);
    void OnLButtonUp();

    // Drawing helpers
    void DrawSvSquare();
    void DrawHueBar();
    void DrawAlphaBar();
    void DrawPreview();
    void DrawCheckerboard(const D2D1_RECT_F& rect, float squareSize,
                          D2D1_COLOR_F c1, D2D1_COLOR_F c2);

    // State
    float m_h = 0.0f, m_s = 1.0f, m_v = 1.0f, m_a = 1.0f;
    ColorCallback m_onChange;
    ColorCallback m_onConfirm;
    HWND m_hwnd = nullptr;
    HWND m_parentAppHwnd = nullptr;
    float m_scale = 1.0f;
    bool m_isLight = true;
    bool m_dragging = false;
    int m_dragTarget = 0; // 0=none, 1=SV, 2=hue, 3=alpha

    // Layout constants (DIPs)
    static constexpr float PADDING = 16.0f;
    static constexpr float SV_SIZE = 200.0f;
    static constexpr float BAR_W = 24.0f;
    static constexpr float BAR_GAP = 12.0f;
    static constexpr float PREVIEW_H = 32.0f;
    static constexpr float TOTAL_W = PADDING * 2 + SV_SIZE + BAR_GAP + BAR_W;

    // Layout rects (DIPs)
    D2D1_RECT_F m_svRect{};
    D2D1_RECT_F m_hueRect{};
    D2D1_RECT_F m_alphaRect{};
    D2D1_RECT_F m_previewRect{};

    // D2D resources
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<IDCompositionDevice> m_dcompDevice;
    ComPtr<IDCompositionTarget> m_dcompTarget;
    ComPtr<IDCompositionVisual> m_dcompVisual;
    ComPtr<IDCompositionSurface> m_dcompSurface;
    ComPtr<IDWriteFactory> m_dwFactory;
    ComPtr<IDWriteTextFormat> m_textFont;

    // Brushes (created on demand in Render)
    ComPtr<ID2D1SolidColorBrush> m_whiteBrush;
    ComPtr<ID2D1SolidColorBrush> m_blackBrush;
    ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    ComPtr<ID2D1SolidColorBrush> m_textBrush;

    static std::unique_ptr<ColorPickerPopup> s_instance;
};

} // namespace QuickView::UI
