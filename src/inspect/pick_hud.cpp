#include "pick_hud.hpp"
#include "capture/display_affinity.hpp"
#include "logging/logger.h"

#if defined(_WIN32)

#include <algorithm>
#include <climits>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace media::win {

LRESULT CALLBACK pick_hud_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

namespace {

constexpr wchar_t kClass[] = L"PmPickHud";
constexpr COLORREF kTransparentKey = RGB(1, 2, 3);
constexpr COLORREF kPanelBg = RGB(24, 27, 33);
constexpr COLORREF kAccent = RGB(0x33, 0xC8, 0xFF);
constexpr int kPadX = 14;
constexpr int kPadY = 6;
constexpr int kLineH = 18;
constexpr int kMinW = 180;
constexpr int kMaxW = 520;
constexpr int kReanchorDistance = 48;

void ensure_class_registered()
{
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = pick_hud_proc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);
    registered = true;
}

int measure_text_width(HDC dc, const std::wstring& text)
{
    if (text.empty()) return 0;
    SIZE sz{};
    ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
    return static_cast<int>(sz.cx);
}

} // namespace

LRESULT CALLBACK pick_hud_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<PickHud*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return TRUE;
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

PickHud::~PickHud() { destroy(); }

bool PickHud::create()
{
    if (hwnd_) return true;
    ensure_class_registered();
    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP,
        0, 0, kMinW, 44, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        logger::warn("[pick-hud] CreateWindowEx failed err=" +
                     std::to_string(::GetLastError()));
        return false;
    }
    ::SetLayeredWindowAttributes(hwnd_, kTransparentKey, 230, LWA_COLORKEY | LWA_ALPHA);
    exclude_window_from_capture(hwnd_);
    return true;
}

void PickHud::paint(HDC dc, const RECT& client) const
{
    HBRUSH key = ::CreateSolidBrush(kTransparentKey);
    ::FillRect(dc, &client, key);
    ::DeleteObject(key);

    HBRUSH bg = ::CreateSolidBrush(kPanelBg);
    ::FillRect(dc, &client, bg);
    ::DeleteObject(bg);

    RECT accent{0, 0, 4, client.bottom};
    HBRUSH ab = ::CreateSolidBrush(kAccent);
    ::FillRect(dc, &accent, ab);
    ::DeleteObject(ab);

    const int text_right = client.right - 8;

    ::SetBkMode(dc, TRANSPARENT);
    ::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));
    ::SetTextColor(dc, RGB(248, 249, 251));
    RECT r1{kPadX, kPadY, text_right, kPadY + kLineH};
    ::DrawTextW(dc, primary_.c_str(), -1, &r1,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    ::SetTextColor(dc, RGB(174, 181, 194));
    RECT r2{kPadX, kPadY + kLineH, text_right, kPadY + kLineH * 2};
    ::DrawTextW(dc, secondary_.c_str(), -1, &r2,
                DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void PickHud::show_near(POINT cursor_screen,
                        const std::wstring& primary,
                        const std::wstring& secondary)
{
    if (!hwnd_) return;
    const bool content_changed =
        primary != primary_ || secondary != secondary_;
    const bool hidden = !::IsWindowVisible(hwnd_);

    if (content_changed) {
        primary_ = primary;
        secondary_ = secondary;

        HDC dc = ::GetDC(hwnd_);
        HFONT font = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old = font ? ::SelectObject(dc, font) : nullptr;
        const int text_w = std::max(measure_text_width(dc, primary_),
                                    measure_text_width(dc, secondary_));
        if (old) ::SelectObject(dc, old);
        ::ReleaseDC(hwnd_, dc);

        width_ = std::clamp(text_w + kPadX + 16, kMinW, kMaxW);
        height_ = kPadY * 2 + kLineH * 2;
    }

    const bool unanchored = anchor_cursor_x_ == INT_MIN;
    const int anchor_dx = unanchored ? 0 : cursor_screen.x - anchor_cursor_x_;
    const int anchor_dy = unanchored ? 0 : cursor_screen.y - anchor_cursor_y_;
    const bool reanchor =
        hidden || content_changed || unanchored ||
        (anchor_dx * anchor_dx + anchor_dy * anchor_dy) >=
            kReanchorDistance * kReanchorDistance;
    if (!reanchor)
        return;

    const int virt_l = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virt_t = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virt_r = virt_l + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virt_b = virt_t + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int x = cursor_screen.x + 18;
    int y = cursor_screen.y + 22;
    if (x + width_ > virt_r) x = cursor_screen.x - width_ - 12;
    if (y + height_ > virt_b) y = cursor_screen.y - height_ - 12;
    x = std::clamp(x, virt_l, std::max(virt_l, virt_r - width_));
    y = std::clamp(y, virt_t, std::max(virt_t, virt_b - height_));

    UINT pos_flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW;
    if (x == last_x_ && y == last_y_)
        pos_flags |= SWP_NOMOVE;
    if (!content_changed)
        pos_flags |= SWP_NOSIZE;
    ::SetWindowPos(hwnd_, nullptr, x, y, width_, height_, pos_flags);
    if (hidden)
        exclude_window_from_capture(hwnd_);
    last_x_ = x;
    last_y_ = y;
    anchor_cursor_x_ = cursor_screen.x;
    anchor_cursor_y_ = cursor_screen.y;
    if (content_changed)
        ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PickHud::hide()
{
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
    anchor_cursor_x_ = INT_MIN;
    anchor_cursor_y_ = INT_MIN;
}

void PickHud::destroy()
{
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    primary_.clear();
    secondary_.clear();
    last_x_ = INT_MIN;
    last_y_ = INT_MIN;
    anchor_cursor_x_ = INT_MIN;
    anchor_cursor_y_ = INT_MIN;
    width_ = 0;
    height_ = 0;
}

} // namespace media::win

#endif // _WIN32
