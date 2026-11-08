#include "pick_loupe.hpp"
#include "logging/logger.h"

#if defined(_WIN32)

#include <algorithm>
#include <climits>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace media::win {

LRESULT CALLBACK pick_loupe_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace {

constexpr wchar_t kClass[] = L"PmPickLoupe";
constexpr int kZoom96 = 8;   ///< screen px → logical loupe px at 96 DPI
constexpr int kCells  = 17;  ///< odd, so there is a true centre pixel
constexpr int kFooter96 = 20;
constexpr int kCursorClearance96 = 8;
constexpr COLORREF kPanelBg = RGB(24, 27, 33);

UINT system_dpi()
{
    HDC screen = ::GetDC(nullptr);
    const int dpi = screen ? ::GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ::ReleaseDC(nullptr, screen);
    return static_cast<UINT>(dpi > 0 ? dpi : 96);
}

void ensure_class_registered()
{
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = pick_loupe_proc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);
    registered = true;
}

std::wstring hex_of(COLORREF c)
{
    static const wchar_t* digits = L"0123456789ABCDEF";
    const int comp[3] = {GetRValue(c), GetGValue(c), GetBValue(c)};
    std::wstring out = L"#";
    for (int v : comp) {
        out.push_back(digits[(v >> 4) & 0xF]);
        out.push_back(digits[v & 0xF]);
    }
    return out;
}

} // namespace

LRESULT CALLBACK pick_loupe_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<PickLoupe*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_DPICHANGED: {
        if (!self) return 0;
        self->set_dpi(HIWORD(wp));
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        const int x = suggested ? suggested->left : self->last_x_;
        const int y = suggested ? suggested->top : self->last_y_;
        ::SetWindowPos(hwnd, nullptr, x, y,
                       self->content_px_, self->content_px_ + self->footer_px_,
                       SWP_NOACTIVATE | SWP_NOZORDER);
        self->last_x_ = x;
        self->last_y_ = y;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = ::BeginPaint(hwnd, &ps);
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        if (self) self->paint(hdc, rc);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

PickLoupe::~PickLoupe() { destroy(); }

bool PickLoupe::create()
{
    if (hwnd_) return true;
    ensure_class_registered();
    set_dpi(system_dpi());
    hwnd_ = ::CreateWindowExW(
        WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP,
        0, 0, content_px_, content_px_ + footer_px_,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        logger::warn("[pick-loupe] CreateWindowEx failed err=" +
                     std::to_string(::GetLastError()));
        return false;
    }
    // Keep the capture source away from this window instead of using
    // WDA_EXCLUDEFROMCAPTURE: display affinity is extremely slow on some
    // software-DWM/GDI drivers.
    return true;
}

void PickLoupe::set_dpi(UINT dpi)
{
    dpi_ = dpi ? dpi : 96;
    zoom_px_ = std::max(1, ::MulDiv(kZoom96, static_cast<int>(dpi_), 96));
    content_px_ = zoom_px_ * kCells;
    footer_px_ = std::max(1, ::MulDiv(kFooter96, static_cast<int>(dpi_), 96));
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    LOGFONTW lf{};
    lf.lfHeight = -::MulDiv(9, static_cast<int>(dpi_), 72);
    lf.lfWeight = FW_NORMAL;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    font_ = ::CreateFontIndirectW(&lf);
}

void PickLoupe::paint(HDC dc, const RECT& client)
{
    const int content = content_px_;

    if (!slow_gdi_) {
        const DWORD capture_started = ::GetTickCount();
        if (HDC screen = ::GetDC(nullptr)) {
            ::SetStretchBltMode(dc, COLORONCOLOR);  // keep pixels crisp, no smoothing
            ::StretchBlt(dc, 0, 0, content, content,
                         screen,
                         center_.x - kCells / 2, center_.y - kCells / 2,
                         kCells, kCells,
                         SRCCOPY);
            ::ReleaseDC(nullptr, screen);
        }
        const DWORD capture_ms = ::GetTickCount() - capture_started;
        if (capture_ms > 40) {
            slow_gdi_ = true;
            logger::warn("[pick-loupe] disabling slow GDI capture (" +
                         std::to_string(capture_ms) + " ms)");
        }
    } else {
        HBRUSH bg = ::CreateSolidBrush(kPanelBg);
        RECT body{0, 0, content, content};
        ::FillRect(dc, &body, bg);
        ::DeleteObject(bg);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, RGB(248, 249, 251));
        ::SelectObject(dc, font_ ? font_ : ::GetStockObject(DEFAULT_GUI_FONT));
        ::DrawTextW(dc, L"Magnifier disabled\r\nSlow GDI — click to pick",
                    -1, &body,
                    DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
    }

    if (!slow_gdi_) {
        // Centre-pixel crosshair.
        const int mid = (kCells / 2) * zoom_px_;
        RECT cell{mid, mid, mid + zoom_px_, mid + zoom_px_};
        ::FrameRect(dc, &cell, reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
        ::InflateRect(&cell, 1, 1);
        ::FrameRect(dc, &cell, reinterpret_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)));
    }

    RECT frame{0, 0, content, content};
    ::FrameRect(dc, &frame, reinterpret_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH)));

    RECT footer{0, content, client.right, client.bottom};
    HBRUSH bg = ::CreateSolidBrush(kPanelBg);
    ::FillRect(dc, &footer, bg);
    ::DeleteObject(bg);

    std::wstring text = color_ok_ ? hex_of(color_) : L"—";
    text += L"  " + std::to_wstring(center_.x) + L"," + std::to_wstring(center_.y);
    if (!footer_extra_.empty())
        text += L"  ·  " + footer_extra_;

    ::SetBkMode(dc, TRANSPARENT);
    ::SelectObject(dc, font_ ? font_ : ::GetStockObject(DEFAULT_GUI_FONT));
    ::SetTextColor(dc, RGB(248, 249, 251));
    RECT tr{6, content, client.right - 6, client.bottom};
    ::DrawTextW(dc, text.c_str(), -1, &tr,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void PickLoupe::update(POINT cursor_screen, COLORREF color, bool color_ok,
                       const wchar_t* footer_extra)
{
    if (!hwnd_) return;
    center_   = cursor_screen;
    color_    = color;
    color_ok_ = color_ok;
    footer_extra_ = footer_extra ? footer_extra : L"";

    const bool hidden = !::IsWindowVisible(hwnd_);
    const bool unanchored = anchor_cursor_x_ == INT_MIN;
    const int w = content_px_;
    const int h = content_px_ + footer_px_;
    const int cursor_clearance =
        std::max(1, ::MulDiv(kCursorClearance96, static_cast<int>(dpi_), 96));
    bool cursor_near_loupe = false;
    if (!unanchored) {
        RECT danger{
            last_x_ - cursor_clearance,
            last_y_ - cursor_clearance,
            last_x_ + w + cursor_clearance,
            last_y_ + h + cursor_clearance,
        };
        cursor_near_loupe = ::PtInRect(&danger, cursor_screen);
    }
    const HMONITOR cursor_monitor =
        ::MonitorFromPoint(cursor_screen, MONITOR_DEFAULTTONEAREST);
    const bool reanchor =
        hidden || unanchored || cursor_near_loupe ||
        cursor_monitor != anchor_monitor_;

    if (reanchor) {
        const int virt_l = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int virt_t = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int virt_r = virt_l + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int virt_b = virt_t + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

        // Above-right of the cursor; move only when it gets too far away.
        const int gap_x = ::MulDiv(18, static_cast<int>(dpi_), 96);
        const int gap_y = ::MulDiv(12, static_cast<int>(dpi_), 96);
        const int below = ::MulDiv(78, static_cast<int>(dpi_), 96);
        int x = cursor_screen.x + gap_x;
        int y = cursor_screen.y - h - gap_y;
        if (y < virt_t) y = cursor_screen.y + below;
        if (x + w > virt_r) x = cursor_screen.x - w - gap_y;
        x = std::clamp(x, virt_l, std::max(virt_l, virt_r - w));
        y = std::clamp(y, virt_t, std::max(virt_t, virt_b - h));

        UINT pos_flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW;
        if (x == last_x_ && y == last_y_)
            pos_flags |= SWP_NOMOVE;
        ::SetWindowPos(hwnd_, nullptr, x, y, w, h, pos_flags);
        last_x_ = x;
        last_y_ = y;
        anchor_cursor_x_ = cursor_screen.x;
        anchor_cursor_y_ = cursor_screen.y;
        anchor_monitor_ = cursor_monitor;
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PickLoupe::hide()
{
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
    anchor_cursor_x_ = INT_MIN;
    anchor_cursor_y_ = INT_MIN;
    anchor_monitor_ = nullptr;
    slow_gdi_ = false;
}

void PickLoupe::destroy()
{
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    color_ok_ = false;
    last_x_ = INT_MIN;
    last_y_ = INT_MIN;
    anchor_cursor_x_ = INT_MIN;
    anchor_cursor_y_ = INT_MIN;
    anchor_monitor_ = nullptr;
}

} // namespace media::win

#endif // _WIN32
