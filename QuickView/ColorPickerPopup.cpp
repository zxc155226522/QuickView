#include "pch.h"
#include "ColorPickerPopup.h"
#include "EditState.h"
#include "SystemInfo.h"
#include "CompositionEngine.h"
#include "RenderEngine.h"
#include <cmath>
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dcomp.lib")

extern AppConfig g_config;
extern CompositionEngine* g_compEngine;
extern class CRenderEngine* g_pRenderEngine;

namespace QuickView::UI {

std::unique_ptr<ColorPickerPopup> ColorPickerPopup::s_instance;
bool ColorPickerPopup::s_classRegistered = false;

// ============================================================
// Color Math: RGB <-> HSV
// ============================================================
void ColorPickerPopup::RgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    float maxC = (std::max)((std::max)(r, g), b);
    float minC = (std::min)((std::min)(r, g), b);
    float delta = maxC - minC;
    v = maxC;
    s = (maxC > 1e-6f) ? (delta / maxC) : 0.0f;
    if (delta < 1e-6f) {
        h = 0.0f;
    } else if (maxC == r) {
        h = 60.0f * fmodf((g - b) / delta, 6.0f);
        if (h < 0) h += 360.0f;
    } else if (maxC == g) {
        h = 60.0f * ((b - r) / delta + 2.0f);
    } else {
        h = 60.0f * ((r - g) / delta + 4.0f);
    }
}

void ColorPickerPopup::HsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    if (h < 60)       { r = c + m; g = x + m; b = m; }
    else if (h < 120) { r = x + m; g = c + m; b = m; }
    else if (h < 180) { r = m;     g = c + m; b = x + m; }
    else if (h < 240) { r = m;     g = x + m; b = c + m; }
    else if (h < 300) { r = x + m; g = m;     b = c + m; }
    else              { r = c + m; g = m;     b = x + m; }
}

// ============================================================
// Window Class
// ============================================================
static const wchar_t* CP_CLASS_NAME = L"QuickViewColorPickerPopup";

void ColorPickerPopup::EnsureClassRegistered() {
    if (s_classRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = CP_CLASS_NAME;
    RegisterClassExW(&wc);
    s_classRegistered = true;
}

// ============================================================
// Show / Dismiss
// ============================================================
void ColorPickerPopup::Show(HWND parent, int screenX, int screenY,
                            float initialR, float initialG, float initialB, float initialA,
                            ColorCallback onChange, ColorCallback onConfirm) {
    if (s_instance) Dismiss();

    EnsureClassRegistered();

    auto picker = std::make_unique<ColorPickerPopup>();
    picker->m_parentAppHwnd = parent;
    picker->m_onChange = std::move(onChange);
    picker->m_onConfirm = std::move(onConfirm);

    // Detect light/dark theme
    extern bool IsLightThemeActive();
    picker->m_isLight = ::IsLightThemeActive();

    // DPI scale
    HMONITOR hMon = MonitorFromPoint({ screenX, screenY }, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    auto pGetDpi = reinterpret_cast<BOOL(WINAPI*)(HMONITOR, int, UINT*, UINT*)>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (pGetDpi) pGetDpi(hMon, 0, &dpiX, &dpiY);
    picker->m_scale = dpiX / 96.0f;

    // Convert initial RGB to HSV
    RgbToHsv(initialR, initialG, initialB, picker->m_h, picker->m_s, picker->m_v);
    picker->m_a = initialA;

    picker->CalculateLayout();
    SIZE winSize = picker->GetWindowSize();

    // Clamp position to work area
    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    RECT wa = mi.rcWork;
    int x = screenX, y = screenY;
    if (x + winSize.cx > wa.right) x = wa.right - winSize.cx;
    if (y + winSize.cy > wa.bottom) y = wa.bottom - winSize.cy;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    picker->m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        CP_CLASS_NAME, nullptr, WS_POPUP,
        x, y, winSize.cx, winSize.cy,
        parent, nullptr, GetModuleHandle(nullptr), picker.get());
    if (!picker->m_hwnd) return;

    SetWindowLongPtrW(picker->m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(picker.get()));

    picker->CreateResources();
    if (!picker->m_d2dContext || !picker->m_dcompSurface) {
        DestroyWindow(picker->m_hwnd);
        return;
    }

    // Dark mode for DWM
    BOOL darkMode = picker->m_isLight ? FALSE : TRUE;
    DwmSetWindowAttribute(picker->m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkMode, sizeof(darkMode));

    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(picker->m_hwnd, &margins);

    // Rounded corners
    if (g_config.RoundedCorners) {
        DWORD pref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(picker->m_hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &pref, sizeof(pref));
    }

    picker->Render();
    ShowWindow(picker->m_hwnd, SW_SHOW);
    SetForegroundWindow(picker->m_hwnd);
    SetCapture(picker->m_hwnd);
    SetTimer(picker->m_hwnd, 1, 100, nullptr);

    s_instance = std::move(picker);
}

void ColorPickerPopup::Dismiss() {
    if (!s_instance) return;
    HWND appHwnd = s_instance->m_parentAppHwnd;
    if (s_instance->m_hwnd && GetCapture() == s_instance->m_hwnd) {
        ReleaseCapture();
    }
    if (s_instance->m_onConfirm) {
        float r, g, b;
        HsvToRgb(s_instance->m_h, s_instance->m_s, s_instance->m_v, r, g, b);
        s_instance->m_onConfirm(r, g, b, s_instance->m_a);
    }
    s_instance.reset();
    if (appHwnd) SetFocus(appHwnd);
}

// ============================================================
// Layout
// ============================================================
void ColorPickerPopup::CalculateLayout() {
    float pad = PADDING;
    m_svRect = D2D1::RectF(pad, pad, pad + SV_SIZE, pad + SV_SIZE);

    float barX = pad + SV_SIZE + BAR_GAP;
    m_hueRect = D2D1::RectF(barX, pad, barX + BAR_W, pad + SV_SIZE);

    float alphaY = pad + SV_SIZE + BAR_GAP;
    m_alphaRect = D2D1::RectF(pad, alphaY, pad + SV_SIZE, alphaY + BAR_W);

    float previewY = alphaY + BAR_W + BAR_GAP;
    m_previewRect = D2D1::RectF(pad, previewY, pad + SV_SIZE, previewY + PREVIEW_H);
}

SIZE ColorPickerPopup::GetWindowSize() const {
    float totalH = PADDING + SV_SIZE + BAR_GAP + BAR_W + BAR_GAP + PREVIEW_H + PADDING;
    return { static_cast<LONG>(std::ceil(TOTAL_W * m_scale)),
             static_cast<LONG>(std::ceil(totalH * m_scale)) };
}

// ============================================================
// D2D Resources
// ============================================================
void ColorPickerPopup::CreateResources() {
    if (m_d2dContext) return;
    if (!g_compEngine || !g_pRenderEngine) return;

    ID2D1Device* d2dDevice = g_compEngine->GetD2DDevice();
    if (!d2dDevice) return;

    ComPtr<IDXGIDevice> dxgiDevice;
    if (!g_pRenderEngine->GetD3DDevice()) return;
    if (FAILED(g_pRenderEngine->GetD3DDevice()->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice)
        return;

    if (FAILED(DCompositionCreateDevice2(dxgiDevice.Get(), IID_PPV_ARGS(&m_dcompDevice)))) return;
    if (!m_dcompDevice) return;

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(m_dwFactory.GetAddressOf()));

    if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext))) return;
    if (!m_d2dContext) return;

    m_d2dContext->SetDpi(96.0f * m_scale, 96.0f * m_scale);

    SIZE winSize = GetWindowSize();
    if (FAILED(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget))) return;
    if (FAILED(m_dcompDevice->CreateVisual(&m_dcompVisual))) return;
    m_dcompTarget->SetRoot(m_dcompVisual.Get());

    if (FAILED(m_dcompDevice->CreateSurface(
            winSize.cx, winSize.cy,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            DXGI_ALPHA_MODE_PREMULTIPLIED, &m_dcompSurface))) return;

    m_dcompVisual->SetContent(m_dcompSurface.Get());
    m_dcompDevice->Commit();

    // Brushes
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &m_whiteBrush);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &m_blackBrush);
    m_d2dContext->CreateSolidColorBrush(
        m_isLight ? D2D1::ColorF(0, 0, 0, 0.15f) : D2D1::ColorF(1, 1, 1, 0.15f), &m_borderBrush);
    m_d2dContext->CreateSolidColorBrush(
        m_isLight ? D2D1::ColorF(0.1f, 0.1f, 0.1f) : D2D1::ColorF(0.9f, 0.9f, 0.9f), &m_textBrush);

    const wchar_t* font = SystemInfo::IsWindows11OrGreater() ? L"Segoe UI Variable Small" : L"Segoe UI";
    if (m_dwFactory) {
        m_dwFactory->CreateTextFormat(font, nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"", &m_textFont);
    }
}

void ColorPickerPopup::DiscardResources() {
    m_dcompSurface.Reset();
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();
    m_d2dContext.Reset();
    m_dwFactory.Reset();
    m_textFont.Reset();
    m_whiteBrush.Reset();
    m_blackBrush.Reset();
    m_borderBrush.Reset();
    m_textBrush.Reset();
}

// ============================================================
// WndProc
// ============================================================
LRESULT CALLBACK ColorPickerPopup::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }
    auto* self = reinterpret_cast<ColorPickerPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMsg(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ColorPickerPopup::HandleMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        ValidateRect(hwnd, nullptr);
        Render();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        // Convert to DIPs
        OnLButtonDown(static_cast<int>(x / m_scale), static_cast<int>(y / m_scale));
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        OnMouseMove(static_cast<int>(x / m_scale), static_cast<int>(y / m_scale));
        return 0;
    }
    case WM_LBUTTONUP:
        OnLButtonUp();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Dismiss(); return 0; }
        if (wp == VK_RETURN) { Dismiss(); return 0; }
        break;

    case WM_TIMER:
        if (wp == 1) {
            HWND fg = GetForegroundWindow();
            if (fg && fg != hwnd && fg != m_parentAppHwnd) Dismiss();
        }
        return 0;

    case WM_CAPTURECHANGED:
        // If capture lost to another window, dismiss
        if (reinterpret_cast<HWND>(lp) != hwnd) {
            Dismiss();
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND target = reinterpret_cast<HWND>(lp);
            if (target != hwnd) PostMessage(hwnd, WM_APP + 99, 0, 0);
        }
        return 0;

    case WM_APP + 99:
        Dismiss();
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        m_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// Interaction
// ============================================================
void ColorPickerPopup::OnLButtonDown(int x, int y) {
    m_dragging = true;
    // Check which area was clicked
    if (x >= m_svRect.left && x <= m_svRect.right && y >= m_svRect.top && y <= m_svRect.bottom) {
        m_dragTarget = 1; // SV
    } else if (x >= m_hueRect.left && x <= m_hueRect.right && y >= m_hueRect.top && y <= m_hueRect.bottom) {
        m_dragTarget = 2; // Hue
    } else if (x >= m_alphaRect.left && x <= m_alphaRect.right && y >= m_alphaRect.top && y <= m_alphaRect.bottom) {
        m_dragTarget = 3; // Alpha
    } else {
        // Clicked outside all controls - dismiss
        m_dragging = false;
        m_dragTarget = 0;
        Dismiss();
        return;
    }
    OnMouseMove(x, y);
}

void ColorPickerPopup::OnMouseMove(int x, int y) {
    if (!m_dragging) return;

    switch (m_dragTarget) {
    case 1: { // SV square
        m_s = std::clamp((x - m_svRect.left) / (m_svRect.right - m_svRect.left), 0.0f, 1.0f);
        m_v = std::clamp(1.0f - (y - m_svRect.top) / (m_svRect.bottom - m_svRect.top), 0.0f, 1.0f);
        break;
    }
    case 2: { // Hue bar
        m_h = std::clamp((y - m_hueRect.top) / (m_hueRect.bottom - m_hueRect.top) * 360.0f, 0.0f, 360.0f);
        break;
    }
    case 3: { // Alpha bar
        m_a = std::clamp((x - m_alphaRect.left) / (m_alphaRect.right - m_alphaRect.left), 0.0f, 1.0f);
        break;
    }
    default:
        return;
    }

    // Notify
    if (m_onChange) {
        float r, g, b;
        HsvToRgb(m_h, m_s, m_v, r, g, b);
        m_onChange(r, g, b, m_a);
    }

    Render();
}

void ColorPickerPopup::OnLButtonUp() {
    m_dragging = false;
    m_dragTarget = 0;
}

// ============================================================
// Rendering
// ============================================================
void ColorPickerPopup::DrawCheckerboard(const D2D1_RECT_F& rect, float sq,
                                        D2D1_COLOR_F c1, D2D1_COLOR_F c2) {
    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;
    ComPtr<ID2D1SolidColorBrush> b1, b2;
    m_d2dContext->CreateSolidColorBrush(c1, &b1);
    m_d2dContext->CreateSolidColorBrush(c2, &b2);
    // Fill base
    m_d2dContext->FillRectangle(rect, b1.Get());
    // Draw alternating squares
    for (float py = 0; py < h; py += sq) {
        for (float px = 0; px < w; px += sq) {
            int gx = static_cast<int>(px / sq);
            int gy = static_cast<int>(py / sq);
            if ((gx + gy) % 2 == 0) continue; // Already filled with c1
            D2D1_RECT_F sr = D2D1::RectF(
                rect.left + px, rect.top + py,
                std::min(rect.left + px + sq, rect.right),
                std::min(rect.top + py + sq, rect.bottom));
            m_d2dContext->FillRectangle(sr, b2.Get());
        }
    }
}

void ColorPickerPopup::DrawSvSquare() {
    float r, g, b;
    HsvToRgb(m_h, 1.0f, 1.0f, r, g, b);
    D2D1_COLOR_F hueColor(r, g, b, 1.0f);

    // 1. Fill with hue color
    ComPtr<ID2D1SolidColorBrush> hueBrush;
    m_d2dContext->CreateSolidColorBrush(hueColor, &hueBrush);
    m_d2dContext->FillRectangle(m_svRect, hueBrush.Get());

    // 2. White gradient (left to right) = saturation
    ComPtr<ID2D1GradientStopCollection> satColl;
    D2D1_GRADIENT_STOP satStops[2] = {
        { 0.0f, D2D1::ColorF(1, 1, 1, 1) },
        { 1.0f, D2D1::ColorF(1, 1, 1, 0) }
    };
    m_d2dContext->CreateGradientStopCollection(satStops, 2, &satColl);
    if (satColl) {
        ComPtr<ID2D1LinearGradientBrush> satBrush;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(m_svRect.left, 0), D2D1::Point2F(m_svRect.right, 0)),
            satColl.Get(), &satBrush);
        if (satBrush) m_d2dContext->FillRectangle(m_svRect, satBrush.Get());
    }

    // 3. Black gradient (bottom to top) = value
    ComPtr<ID2D1GradientStopCollection> valColl;
    D2D1_GRADIENT_STOP valStops[2] = {
        { 0.0f, D2D1::ColorF(0, 0, 0, 0) },
        { 1.0f, D2D1::ColorF(0, 0, 0, 1) }
    };
    m_d2dContext->CreateGradientStopCollection(valStops, 2, &valColl);
    if (valColl) {
        ComPtr<ID2D1LinearGradientBrush> valBrush;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0, m_svRect.top), D2D1::Point2F(0, m_svRect.bottom)),
            valColl.Get(), &valBrush);
        if (valBrush) m_d2dContext->FillRectangle(m_svRect, valBrush.Get());
    }

    // 4. Border
    if (m_borderBrush) m_d2dContext->DrawRectangle(m_svRect, m_borderBrush.Get(), 1.0f);

    // 5. Cursor (circle)
    float cx = m_svRect.left + m_s * (m_svRect.right - m_svRect.left);
    float cy = m_svRect.top + (1.0f - m_v) * (m_svRect.bottom - m_svRect.top);
    float cursorR = 6.0f;

    ComPtr<ID2D1SolidColorBrush> cursorFill;
    float cr, cg, cb;
    HsvToRgb(m_h, m_s, m_v, cr, cg, cb);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(cr, cg, cb, 1.0f), &cursorFill);
    if (cursorFill) {
        m_d2dContext->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy), cursorR, cursorR), cursorFill.Get());
    }
    // White ring
    ComPtr<ID2D1SolidColorBrush> ringBrush;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &ringBrush);
    if (ringBrush) {
        m_d2dContext->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy), cursorR, cursorR), ringBrush.Get(), 2.0f);
    }
}

void ColorPickerPopup::DrawHueBar() {
    // Vertical rainbow gradient
    ComPtr<ID2D1GradientStopCollection> hueColl;
    D2D1_GRADIENT_STOP stops[7] = {
        { 0.0f/360.0f, D2D1::ColorF(1, 0, 0, 1) },     // Red
        { 60.0f/360.0f, D2D1::ColorF(1, 1, 0, 1) },     // Yellow
        { 120.0f/360.0f, D2D1::ColorF(0, 1, 0, 1) },    // Green
        { 180.0f/360.0f, D2D1::ColorF(0, 1, 1, 1) },    // Cyan
        { 240.0f/360.0f, D2D1::ColorF(0, 0, 1, 1) },    // Blue
        { 300.0f/360.0f, D2D1::ColorF(1, 0, 1, 1) },    // Magenta
        { 1.0f, D2D1::ColorF(1, 0, 0, 1) },              // Red (wrap)
    };
    m_d2dContext->CreateGradientStopCollection(stops, 7, &hueColl);
    if (hueColl) {
        ComPtr<ID2D1LinearGradientBrush> hueBrush;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(0, m_hueRect.top), D2D1::Point2F(0, m_hueRect.bottom)),
            hueColl.Get(), &hueBrush);
        if (hueBrush) {
            m_d2dContext->FillRoundedRectangle(
                D2D1::RoundedRect(m_hueRect, 4.0f, 4.0f), hueBrush.Get());
        }
    }

    // Border
    if (m_borderBrush) m_d2dContext->DrawRoundedRectangle(
        D2D1::RoundedRect(m_hueRect, 4.0f, 4.0f), m_borderBrush.Get(), 1.0f);

    // Cursor (horizontal line/triangle)
    float cy = m_hueRect.top + (m_h / 360.0f) * (m_hueRect.bottom - m_hueRect.top);
    ComPtr<ID2D1SolidColorBrush> cursorBrush;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &cursorBrush);
    ComPtr<ID2D1SolidColorBrush> cursorShadow;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.5f), &cursorShadow);

    if (cursorShadow) {
        m_d2dContext->DrawLine(
            D2D1::Point2F(m_hueRect.left - 2, cy), D2D1::Point2F(m_hueRect.right + 2, cy),
            cursorShadow.Get(), 3.0f);
    }
    if (cursorBrush) {
        m_d2dContext->DrawLine(
            D2D1::Point2F(m_hueRect.left - 2, cy), D2D1::Point2F(m_hueRect.right + 2, cy),
            cursorBrush.Get(), 1.5f);
    }
}

void ColorPickerPopup::DrawAlphaBar() {
    // Checkerboard background
    DrawCheckerboard(m_alphaRect, 6.0f,
                     D2D1::ColorF(0.7f, 0.7f, 0.7f, 1.0f),
                     D2D1::ColorF(0.4f, 0.4f, 0.4f, 1.0f));

    // Color gradient with varying alpha
    float r, g, b;
    HsvToRgb(m_h, m_s, m_v, r, g, b);

    ComPtr<ID2D1GradientStopCollection> alphaColl;
    D2D1_GRADIENT_STOP stops[2] = {
        { 0.0f, D2D1::ColorF(r, g, b, 0.0f) },
        { 1.0f, D2D1::ColorF(r, g, b, 1.0f) }
    };
    m_d2dContext->CreateGradientStopCollection(stops, 2, &alphaColl);
    if (alphaColl) {
        ComPtr<ID2D1LinearGradientBrush> alphaBrush;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(m_alphaRect.left, 0), D2D1::Point2F(m_alphaRect.right, 0)),
            alphaColl.Get(), &alphaBrush);
        if (alphaBrush) {
            m_d2dContext->FillRoundedRectangle(
                D2D1::RoundedRect(m_alphaRect, 4.0f, 4.0f), alphaBrush.Get());
        }
    }

    // Border
    if (m_borderBrush) m_d2dContext->DrawRoundedRectangle(
        D2D1::RoundedRect(m_alphaRect, 4.0f, 4.0f), m_borderBrush.Get(), 1.0f);

    // Cursor
    float cx = m_alphaRect.left + m_a * (m_alphaRect.right - m_alphaRect.left);
    ComPtr<ID2D1SolidColorBrush> cursorShadow;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.5f), &cursorShadow);
    ComPtr<ID2D1SolidColorBrush> cursorWhite;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &cursorWhite);

    if (cursorShadow) {
        m_d2dContext->DrawLine(
            D2D1::Point2F(cx, m_alphaRect.top - 2), D2D1::Point2F(cx, m_alphaRect.bottom + 2),
            cursorShadow.Get(), 3.0f);
    }
    if (cursorWhite) {
        m_d2dContext->DrawLine(
            D2D1::Point2F(cx, m_alphaRect.top - 2), D2D1::Point2F(cx, m_alphaRect.bottom + 2),
            cursorWhite.Get(), 1.5f);
    }
}

void ColorPickerPopup::DrawPreview() {
    // Checkerboard background
    DrawCheckerboard(m_previewRect, 8.0f,
                     D2D1::ColorF(0.7f, 0.7f, 0.7f, 1.0f),
                     D2D1::ColorF(0.4f, 0.4f, 0.4f, 1.0f));

    // Current color
    float r, g, b;
    HsvToRgb(m_h, m_s, m_v, r, g, b);
    ComPtr<ID2D1SolidColorBrush> colorBrush;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, m_a), &colorBrush);
    if (colorBrush) {
        m_d2dContext->FillRoundedRectangle(
            D2D1::RoundedRect(m_previewRect, 4.0f, 4.0f), colorBrush.Get());
    }

    // Border
    if (m_borderBrush) m_d2dContext->DrawRoundedRectangle(
        D2D1::RoundedRect(m_previewRect, 4.0f, 4.0f), m_borderBrush.Get(), 1.0f);

    // Hex text
    if (m_textFont && m_textBrush) {
        int ir = static_cast<int>(r * 255 + 0.5f);
        int ig = static_cast<int>(g * 255 + 0.5f);
        int ib = static_cast<int>(b * 255 + 0.5f);
        int ia = static_cast<int>(m_a * 255 + 0.5f);
        wchar_t hex[16];
        swprintf_s(hex, L"#%02X%02X%02X  %d%%", ir, ig, ib, ia * 100 / 255);
        size_t len = wcslen(hex);

        m_textFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_textFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        // Choose text color based on luminance
        float lum = 0.2126f * r * m_a + 0.7152f * g * m_a + 0.0722f * b * m_a;
        ComPtr<ID2D1SolidColorBrush> hexBrush;
        if (lum > 0.5f) {
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.1f, 0.1f, 0.1f, 1.0f), &hexBrush);
        } else {
            m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1.0f), &hexBrush);
        }
        if (hexBrush) {
            m_d2dContext->DrawText(hex, (UINT32)len, m_textFont.Get(), m_previewRect, hexBrush.Get());
        }
    }
}

void ColorPickerPopup::Render() {
    if (!m_d2dContext || !m_hwnd || !m_dcompSurface || !m_dcompDevice) return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset = {};
    if (FAILED(m_dcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset))) return;

    ComPtr<ID2D1Bitmap1> targetBitmap;
    auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &targetBitmap))) {
        m_dcompSurface->EndDraw();
        return;
    }

    float dipOx = static_cast<float>(offset.x) / m_scale;
    float dipOy = static_cast<float>(offset.y) / m_scale;

    m_d2dContext->SetTarget(targetBitmap.Get());
    m_d2dContext->BeginDraw();
    m_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(dipOx, dipOy));

    // Clear: semi-opaque background (theme-aware)
    D2D1_COLOR_F bg = m_isLight
        ? D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.96f)
        : D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.96f);
    m_d2dContext->Clear(bg);

    // Draw all controls
    DrawSvSquare();
    DrawHueBar();
    DrawAlphaBar();
    DrawPreview();

    m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);

    m_dcompSurface->EndDraw();
    m_dcompDevice->Commit();
}

} // namespace QuickView::UI
