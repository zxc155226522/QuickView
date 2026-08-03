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
                            bool initialIsChecker,
                            ColorCallback onChange, ColorCallback onConfirm) {
    // 同步销毁旧实例：调用 onConfirm，清 userdata，reset，DestroyWindow
    if (s_instance) {
        if (!s_instance->m_dismissing && s_instance->m_onConfirm) {
            float r, g, b;
            HsvToRgb(s_instance->m_h, s_instance->m_s, s_instance->m_v, r, g, b);
            s_instance->m_onConfirm(r, g, b, s_instance->m_a, s_instance->m_isChecker);
        }
        HWND oldHwnd = s_instance->m_hwnd;
        if (oldHwnd) SetWindowLongPtrW(oldHwnd, GWLP_USERDATA, 0);
        s_instance.reset();
        if (oldHwnd) DestroyWindow(oldHwnd);
    }

    EnsureClassRegistered();

    auto picker = std::make_unique<ColorPickerPopup>();
    picker->m_parentAppHwnd = parent;
    picker->m_onChange = std::move(onChange);
    picker->m_onConfirm = std::move(onConfirm);

    extern bool IsLightThemeActive();
    picker->m_isLight = ::IsLightThemeActive();

    HMONITOR hMon = MonitorFromPoint({ screenX, screenY }, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    auto pGetDpi = reinterpret_cast<BOOL(WINAPI*)(HMONITOR, int, UINT*, UINT*)>(
        GetProcAddress(GetModuleHandleW(L"shcore.dll"), "GetDpiForMonitor"));
    if (pGetDpi) pGetDpi(hMon, 0, &dpiX, &dpiY);
    picker->m_scale = dpiX / 96.0f;

    RgbToHsv(initialR, initialG, initialB, picker->m_h, picker->m_s, picker->m_v);
    picker->m_a = initialA;
    picker->m_isChecker = initialIsChecker;

    picker->CalculateLayout();
    SIZE winSize = picker->GetWindowSize();

    MONITORINFO mi = {}; mi.cbSize = sizeof(mi);
    GetMonitorInfoW(hMon, &mi);
    RECT wa = mi.rcWork;
    int x = screenX, y = screenY;
    if (x + winSize.cx > wa.right) x = wa.right - winSize.cx;
    if (y + winSize.cy > wa.bottom) y = wa.bottom - winSize.cy;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    picker->m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
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

    BOOL darkMode = picker->m_isLight ? FALSE : TRUE;
    DwmSetWindowAttribute(picker->m_hwnd, 20, &darkMode, sizeof(darkMode));
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(picker->m_hwnd, &margins);
    if (g_config.RoundedCorners) {
        DWORD pref = 2;
        DwmSetWindowAttribute(picker->m_hwnd, 33, &pref, sizeof(pref));
    }

    picker->Render();
    ShowWindow(picker->m_hwnd, SW_SHOWNOACTIVATE);
    SetTimer(picker->m_hwnd, 1, 50, nullptr);

    s_instance = std::move(picker);
}

void ColorPickerPopup::Dismiss() {
    if (!s_instance) return;
    if (s_instance->m_dismissing) return;
    s_instance->m_dismissing = true;
    // PostMessage 延迟关闭：WM_CLOSE 在 WndProc 中处理，避免消息处理中析构自身
    if (s_instance->m_hwnd) PostMessageW(s_instance->m_hwnd, WM_CLOSE, 0, 0);
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
    float toggleY = alphaY + BAR_W + BAR_GAP;
    m_toggleRect = D2D1::RectF(pad, toggleY, pad + SV_SIZE, toggleY + TOGGLE_H);
    float previewY = toggleY + TOGGLE_H + BAR_GAP;
    m_previewRect = D2D1::RectF(pad, previewY, pad + SV_SIZE, previewY + PREVIEW_H);
}

SIZE ColorPickerPopup::GetWindowSize() const {
    float totalH = PADDING + SV_SIZE + BAR_GAP + BAR_W + BAR_GAP + TOGGLE_H + BAR_GAP + PREVIEW_H + PADDING;
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
    if (FAILED(g_pRenderEngine->GetD3DDevice()->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || !dxgiDevice) return;

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
    if (FAILED(m_dcompDevice->CreateSurface(winSize.cx, winSize.cy,
            DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &m_dcompSurface))) return;
    m_dcompVisual->SetContent(m_dcompSurface.Get());
    m_dcompDevice->Commit();

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
    m_dcompSurface.Reset(); m_dcompVisual.Reset(); m_dcompTarget.Reset();
    m_dcompDevice.Reset(); m_d2dContext.Reset(); m_dwFactory.Reset(); m_textFont.Reset();
    m_whiteBrush.Reset(); m_blackBrush.Reset(); m_borderBrush.Reset(); m_textBrush.Reset();
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
    // WM_CLOSE: 同步调用 onConfirm，然后销毁窗口
    // 必须在 WndProc 中处理（而非 HandleMsg），因为 s_instance.reset() 会析构 self
    if (msg == WM_CLOSE) {
        auto* self = reinterpret_cast<ColorPickerPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (self && s_instance.get() == self) {
            if (self->m_onConfirm) {
                float r, g, b;
                HsvToRgb(self->m_h, self->m_s, self->m_v, r, g, b);
                self->m_onConfirm(r, g, b, self->m_a, self->m_isChecker);
            }
            HWND appHwnd = self->m_parentAppHwnd;
            s_instance.reset();
            if (appHwnd) SetFocus(appHwnd);
        }
        DestroyWindow(hwnd);
        return 0;
    }
    // WM_NCDESTROY: 窗口被外部销毁时的安全网
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (s_instance && s_instance->m_hwnd == hwnd) {
            s_instance.reset();
        }
        return 0;
    }
    auto* self = reinterpret_cast<ColorPickerPopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMsg(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT ColorPickerPopup::HandleMsg(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { ValidateRect(hwnd, nullptr); Render(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        OnLButtonDown(static_cast<int>(GET_X_LPARAM(lp) / m_scale),
                      static_cast<int>(GET_Y_LPARAM(lp) / m_scale));
        return 0;
    }
    case WM_MOUSEMOVE: {
        OnMouseMove(static_cast<int>(GET_X_LPARAM(lp) / m_scale),
                    static_cast<int>(GET_Y_LPARAM(lp) / m_scale));
        return 0;
    }
    case WM_LBUTTONUP: OnLButtonUp(); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == VK_RETURN) { Dismiss(); return 0; }
        break;
    case WM_TIMER:
        if (wp == 1) {
            // 鼠标在窗口外且没有按钮按下 → 关闭
            POINT pt; GetCursorPos(&pt);
            RECT rc; GetWindowRect(hwnd, &rc);
            if (!PtInRect(&rc, pt)) {
                if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                    Dismiss();
                }
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================
// Interaction
// ============================================================
void ColorPickerPopup::OnLButtonDown(int x, int y) {
    m_dragging = true;
    if (x >= m_svRect.left && x <= m_svRect.right && y >= m_svRect.top && y <= m_svRect.bottom) {
        m_dragTarget = 1;
    } else if (x >= m_hueRect.left && x <= m_hueRect.right && y >= m_hueRect.top && y <= m_hueRect.bottom) {
        m_dragTarget = 2;
    } else if (x >= m_alphaRect.left && x <= m_alphaRect.right && y >= m_alphaRect.top && y <= m_alphaRect.bottom) {
        m_dragTarget = 3;
    } else if (x >= m_toggleRect.left && x <= m_toggleRect.right && y >= m_toggleRect.top && y <= m_toggleRect.bottom) {
        // Toggle checker mode
        m_isChecker = !m_isChecker;
        if (m_onChange) {
            float r, g, b;
            HsvToRgb(m_h, m_s, m_v, r, g, b);
            m_onChange(r, g, b, m_a, m_isChecker);
        }
        Render();
        m_dragging = false;
        m_dragTarget = 0;
        return;
    } else {
        m_dragging = false;
        m_dragTarget = 0;
        Dismiss();
        return;
    }
    // 局部 SetCapture：拖拽期间保证鼠标消息不丢失
    if (m_hwnd) SetCapture(m_hwnd);
    OnMouseMove(x, y);
}

void ColorPickerPopup::OnMouseMove(int x, int y) {
    if (!m_dragging) return;
    switch (m_dragTarget) {
    case 1:
        m_s = std::clamp((x - m_svRect.left) / (m_svRect.right - m_svRect.left), 0.0f, 1.0f);
        m_v = std::clamp(1.0f - (y - m_svRect.top) / (m_svRect.bottom - m_svRect.top), 0.0f, 1.0f);
        break;
    case 2:
        m_h = std::clamp((y - m_hueRect.top) / (m_hueRect.bottom - m_hueRect.top) * 360.0f, 0.0f, 360.0f);
        break;
    case 3:
        m_a = std::clamp((x - m_alphaRect.left) / (m_alphaRect.right - m_alphaRect.left), 0.0f, 1.0f);
        break;
    default: return;
    }
    if (m_onChange) {
        float r, g, b;
        HsvToRgb(m_h, m_s, m_v, r, g, b);
        m_onChange(r, g, b, m_a, m_isChecker);
    }
    Render();
}

void ColorPickerPopup::OnLButtonUp() {
    m_dragging = false;
    m_dragTarget = 0;
    if (m_hwnd && GetCapture() == m_hwnd) ReleaseCapture();
}

// ============================================================
// Rendering helpers
// ============================================================
void ColorPickerPopup::DrawCheckerboard(const D2D1_RECT_F& rect, float sq,
                                        D2D1_COLOR_F c1, D2D1_COLOR_F c2) {
    float w = rect.right - rect.left;
    float h = rect.bottom - rect.top;
    ComPtr<ID2D1SolidColorBrush> b1, b2;
    m_d2dContext->CreateSolidColorBrush(c1, &b1);
    m_d2dContext->CreateSolidColorBrush(c2, &b2);
    m_d2dContext->FillRectangle(rect, b1.Get());
    for (float py = 0; py < h; py += sq) {
        for (float px = 0; px < w; px += sq) {
            int gx = static_cast<int>(px / sq);
            int gy = static_cast<int>(py / sq);
            if ((gx + gy) % 2 == 0) continue;
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
    ComPtr<ID2D1SolidColorBrush> hueBrush;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, 1.0f), &hueBrush);
    m_d2dContext->FillRectangle(m_svRect, hueBrush.Get());

    ComPtr<ID2D1GradientStopCollection> satColl;
    D2D1_GRADIENT_STOP satStops[2] = { {0, D2D1::ColorF(1,1,1,1)}, {1, D2D1::ColorF(1,1,1,0)} };
    m_d2dContext->CreateGradientStopCollection(satStops, 2, &satColl);
    if (satColl) {
        ComPtr<ID2D1LinearGradientBrush> satBrush;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(m_svRect.left, 0), D2D1::Point2F(m_svRect.right, 0)),
            satColl.Get(), &satBrush);
        if (satBrush) m_d2dContext->FillRectangle(m_svRect, satBrush.Get());
    }

    ComPtr<ID2D1GradientStopCollection> valColl;
    D2D1_GRADIENT_STOP valStops[2] = { {0, D2D1::ColorF(0,0,0,0)}, {1, D2D1::ColorF(0,0,0,1)} };
    m_d2dContext->CreateGradientStopCollection(valStops, 2, &valColl);
    if (valColl) {
        ComPtr<ID2D1LinearGradientBrush> valBrush;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, m_svRect.top), D2D1::Point2F(0, m_svRect.bottom)),
            valColl.Get(), &valBrush);
        if (valBrush) m_d2dContext->FillRectangle(m_svRect, valBrush.Get());
    }

    if (m_borderBrush) m_d2dContext->DrawRectangle(m_svRect, m_borderBrush.Get(), 1.0f);

    float cx = m_svRect.left + m_s * (m_svRect.right - m_svRect.left);
    float cy = m_svRect.top + (1.0f - m_v) * (m_svRect.bottom - m_svRect.top);
    float cursorR = 6.0f;
    float cr, cg, cb;
    HsvToRgb(m_h, m_s, m_v, cr, cg, cb);
    ComPtr<ID2D1SolidColorBrush> cursorFill;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(cr, cg, cb, 1.0f), &cursorFill);
    if (cursorFill) m_d2dContext->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cursorR, cursorR), cursorFill.Get());
    ComPtr<ID2D1SolidColorBrush> ringBrush;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &ringBrush);
    if (ringBrush) m_d2dContext->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), cursorR, cursorR), ringBrush.Get(), 2.0f);
}

void ColorPickerPopup::DrawHueBar() {
    ComPtr<ID2D1GradientStopCollection> hueColl;
    D2D1_GRADIENT_STOP stops[7] = {
        {0, D2D1::ColorF(1,0,0,1)}, {60.0f/360, D2D1::ColorF(1,1,0,1)},
        {120.0f/360, D2D1::ColorF(0,1,0,1)}, {180.0f/360, D2D1::ColorF(0,1,1,1)},
        {240.0f/360, D2D1::ColorF(0,0,1,1)}, {300.0f/360, D2D1::ColorF(1,0,1,1)},
        {1, D2D1::ColorF(1,0,0,1)},
    };
    m_d2dContext->CreateGradientStopCollection(stops, 7, &hueColl);
    if (hueColl) {
        ComPtr<ID2D1LinearGradientBrush> hueBrush;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, m_hueRect.top), D2D1::Point2F(0, m_hueRect.bottom)),
            hueColl.Get(), &hueBrush);
        if (hueBrush) m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(m_hueRect, 4, 4), hueBrush.Get());
    }
    if (m_borderBrush) m_d2dContext->DrawRoundedRectangle(D2D1::RoundedRect(m_hueRect, 4, 4), m_borderBrush.Get(), 1.0f);
    float cy = m_hueRect.top + (m_h / 360.0f) * (m_hueRect.bottom - m_hueRect.top);
    ComPtr<ID2D1SolidColorBrush> cs, cw;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0,0,0,0.5f), &cs);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &cw);
    if (cs) m_d2dContext->DrawLine(D2D1::Point2F(m_hueRect.left-2, cy), D2D1::Point2F(m_hueRect.right+2, cy), cs.Get(), 3.0f);
    if (cw) m_d2dContext->DrawLine(D2D1::Point2F(m_hueRect.left-2, cy), D2D1::Point2F(m_hueRect.right+2, cy), cw.Get(), 1.5f);
}

void ColorPickerPopup::DrawAlphaBar() {
    DrawCheckerboard(m_alphaRect, 6.0f, D2D1::ColorF(0.7f,0.7f,0.7f,1), D2D1::ColorF(0.4f,0.4f,0.4f,1));
    float r, g, b;
    HsvToRgb(m_h, m_s, m_v, r, g, b);
    ComPtr<ID2D1GradientStopCollection> alphaColl;
    D2D1_GRADIENT_STOP stops[2] = { {0, D2D1::ColorF(r,g,b,0)}, {1, D2D1::ColorF(r,g,b,1)} };
    m_d2dContext->CreateGradientStopCollection(stops, 2, &alphaColl);
    if (alphaColl) {
        ComPtr<ID2D1LinearGradientBrush> ab;
        m_d2dContext->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(m_alphaRect.left, 0), D2D1::Point2F(m_alphaRect.right, 0)),
            alphaColl.Get(), &ab);
        if (ab) m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(m_alphaRect, 4, 4), ab.Get());
    }
    if (m_borderBrush) m_d2dContext->DrawRoundedRectangle(D2D1::RoundedRect(m_alphaRect, 4, 4), m_borderBrush.Get(), 1.0f);
    float cx = m_alphaRect.left + m_a * (m_alphaRect.right - m_alphaRect.left);
    ComPtr<ID2D1SolidColorBrush> cs, cw;
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0,0,0,0.5f), &cs);
    m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &cw);
    if (cs) m_d2dContext->DrawLine(D2D1::Point2F(cx, m_alphaRect.top-2), D2D1::Point2F(cx, m_alphaRect.bottom+2), cs.Get(), 3.0f);
    if (cw) m_d2dContext->DrawLine(D2D1::Point2F(cx, m_alphaRect.top-2), D2D1::Point2F(cx, m_alphaRect.bottom+2), cw.Get(), 1.5f);
}

void ColorPickerPopup::DrawModeToggle() {
    float halfW = (m_toggleRect.right - m_toggleRect.left) * 0.5f;
    D2D1_RECT_F leftR = D2D1::RectF(m_toggleRect.left, m_toggleRect.top, m_toggleRect.left + halfW, m_toggleRect.bottom);
    D2D1_RECT_F rightR = D2D1::RectF(m_toggleRect.left + halfW, m_toggleRect.top, m_toggleRect.right, m_toggleRect.bottom);
    float radius = 4.0f;

    auto drawBtn = [&](const D2D1_RECT_F& rect, const wchar_t* text, bool active) {
        ComPtr<ID2D1SolidColorBrush> bg;
        if (active) {
            m_d2dContext->CreateSolidColorBrush(
                m_isLight ? D2D1::ColorF(0.15f, 0.45f, 0.85f, 1.0f) : D2D1::ColorF(0.20f, 0.50f, 0.90f, 1.0f), &bg);
        } else {
            m_d2dContext->CreateSolidColorBrush(
                m_isLight ? D2D1::ColorF(0.85f, 0.85f, 0.88f, 1.0f) : D2D1::ColorF(0.20f, 0.20f, 0.24f, 1.0f), &bg);
        }
        if (bg) m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), bg.Get());
        if (m_borderBrush) m_d2dContext->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), m_borderBrush.Get(), 1.0f);
        ComPtr<ID2D1SolidColorBrush> txt;
        if (active) m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &txt);
        else m_d2dContext->CreateSolidColorBrush(
            m_isLight ? D2D1::ColorF(0.2f,0.2f,0.2f) : D2D1::ColorF(0.8f,0.8f,0.8f), &txt);
        if (m_textFont && txt) {
            m_textFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_textFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_d2dContext->DrawText(text, (UINT32)wcslen(text), m_textFont.Get(), rect, txt.Get());
        }
    };
    drawBtn(leftR, L"纯色", !m_isChecker);
    drawBtn(rightR, L"棋盘格", m_isChecker);
}

void ColorPickerPopup::DrawPreview() {
    float r, g, b;
    HsvToRgb(m_h, m_s, m_v, r, g, b);

    if (m_isChecker) {
        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        D2D1_COLOR_F c1(r, g, b, 1.0f);
        D2D1_COLOR_F c2 = (lum > 0.5f)
            ? D2D1::ColorF(r*0.82f, g*0.82f, b*0.82f, 1.0f)
            : D2D1::ColorF(std::min(r*1.2f,1.0f), std::min(g*1.2f,1.0f), std::min(b*1.2f,1.0f), 1.0f);
        DrawCheckerboard(m_previewRect, 10.0f, c1, c2);
    } else {
        DrawCheckerboard(m_previewRect, 8.0f, D2D1::ColorF(0.7f,0.7f,0.7f,1), D2D1::ColorF(0.4f,0.4f,0.4f,1));
        ComPtr<ID2D1SolidColorBrush> colorBrush;
        m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(r, g, b, m_a), &colorBrush);
        if (colorBrush) m_d2dContext->FillRoundedRectangle(D2D1::RoundedRect(m_previewRect, 4, 4), colorBrush.Get());
    }

    if (m_borderBrush) m_d2dContext->DrawRoundedRectangle(D2D1::RoundedRect(m_previewRect, 4, 4), m_borderBrush.Get(), 1.0f);

    if (m_textFont) {
        int ir = static_cast<int>(r * 255 + 0.5f);
        int ig = static_cast<int>(g * 255 + 0.5f);
        int ib = static_cast<int>(b * 255 + 0.5f);
        wchar_t hex[32];
        if (m_isChecker) swprintf_s(hex, L"棋盘格 #%02X%02X%02X", ir, ig, ib);
        else { int ia = static_cast<int>(m_a * 255 + 0.5f); swprintf_s(hex, L"#%02X%02X%02X  %d%%", ir, ig, ib, ia * 100 / 255); }
        m_textFont->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_textFont->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        float lum = m_isChecker ? (0.2126f*r + 0.7152f*g + 0.0722f*b) : (0.2126f*r*m_a + 0.7152f*g*m_a + 0.0722f*b*m_a);
        ComPtr<ID2D1SolidColorBrush> hexBrush;
        if (lum > 0.5f) m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.1f,0.1f,0.1f,1), &hexBrush);
        else m_d2dContext->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &hexBrush);
        if (hexBrush) m_d2dContext->DrawText(hex, (UINT32)wcslen(hex), m_textFont.Get(), m_previewRect, hexBrush.Get());
    }
}

void ColorPickerPopup::Render() {
    if (!m_d2dContext || !m_hwnd || !m_dcompSurface || !m_dcompDevice) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    int width = rc.right - rc.left, height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset = {};
    if (FAILED(m_dcompSurface->BeginDraw(nullptr, IID_PPV_ARGS(&dxgiSurface), &offset))) return;

    ComPtr<ID2D1Bitmap1> targetBitmap;
    auto props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &targetBitmap))) {
        m_dcompSurface->EndDraw(); return;
    }

    float dipOx = static_cast<float>(offset.x) / m_scale;
    float dipOy = static_cast<float>(offset.y) / m_scale;
    m_d2dContext->SetTarget(targetBitmap.Get());
    m_d2dContext->BeginDraw();
    m_d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(dipOx, dipOy));

    D2D1_COLOR_F bg = m_isLight ? D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.96f) : D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.96f);
    m_d2dContext->Clear(bg);

    DrawSvSquare();
    DrawHueBar();
    DrawAlphaBar();
    DrawModeToggle();
    DrawPreview();

    m_d2dContext->EndDraw();
    m_d2dContext->SetTarget(nullptr);
    m_dcompSurface->EndDraw();
    m_dcompDevice->Commit();
}

} // namespace QuickView::UI
