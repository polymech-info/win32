#include "highlight_overlay.hpp"
#include "capture/display_affinity.hpp"
#include "logging/logger.h"

#if defined(_WIN32)

#include <cstdint>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace media::win {
namespace {

constexpr wchar_t kClass[] = L"PmHighlightOverlay";
constexpr wchar_t kLineClass[] = L"PmLineOverlay";

bool same_rect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<HighlightOverlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        // Pass-through: WindowFromPoint / UIA ElementFromPoint hit the target below.
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);
        RECT rc;
        ::GetClientRect(hwnd, &rc);
        const COLORREF color = self ? self->options().color : RGB(0x33, 0xC8, 0xFF);
        HBRUSH br = ::CreateSolidBrush(color);
        ::FillRect(hdc, &rc, br);  // SetWindowRgn clips to the frame in outline mode
        ::DeleteObject(br);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_class_registered() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = overlay_proc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);
    registered = true;
}

} // namespace

HighlightOverlay::~HighlightOverlay() { destroy(); }

bool HighlightOverlay::create(const Options& opts) {
    if (hwnd_) return true;
    opts_ = opts;
    ensure_class_registered();
    ::SetLastError(ERROR_SUCCESS);
    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        logger::warn("[picker-overlay] CreateWindowEx failed err=" +
                     std::to_string(::GetLastError()));
        return false;
    }
    ::SetLayeredWindowAttributes(hwnd_, 0, opts_.alpha, LWA_ALPHA);
    // Affinity is retried in move_to after the first show — SetWindowDisplayAffinity
    // often rejects a still-hidden layered window.
    exclude_window_from_capture(hwnd_);
    return true;
}

void HighlightOverlay::apply_region(int w, int h) {
    if (opts_.filled) {
        ::SetWindowRgn(hwnd_, nullptr, TRUE);
        return;
    }
    const int t = opts_.thickness;
    HRGN outer = ::CreateRectRgn(0, 0, w, h);
    HRGN inner = ::CreateRectRgn(t, t, w - t, h - t);
    ::CombineRgn(outer, outer, inner, RGN_DIFF);
    ::DeleteObject(inner);
    ::SetWindowRgn(hwnd_, outer, TRUE);  // window takes ownership of `outer`
}

void HighlightOverlay::move_to(const RECT& r) {
    if (!hwnd_) return;
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    if (last_valid_ && same_rect(r, last_rect_)) return;  // nothing changed
    const bool first_show = !last_valid_ || !::IsWindowVisible(hwnd_);
    last_rect_  = r;
    last_valid_ = true;
    ::SetWindowPos(
        hwnd_, HWND_TOPMOST, r.left, r.top, w, h,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    apply_region(w, h);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    if (first_show)
        exclude_window_from_capture(hwnd_);
}

void HighlightOverlay::hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
    last_valid_ = false;
}

void HighlightOverlay::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    last_valid_ = false;
}

LRESULT CALLBACK line_overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<LineOverlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);
        if (self && self->valid()) {
            const auto& opts = self->options();
            HPEN pen = ::CreatePen(PS_SOLID, opts.thickness, opts.color);
            HGDIOBJ old_pen = ::SelectObject(hdc, pen);
            ::MoveToEx(hdc, self->pt_a_.x, self->pt_a_.y, nullptr);
            ::LineTo(hdc, self->pt_b_.x, self->pt_b_.y);
            ::SelectObject(hdc, old_pen);
            ::DeleteObject(pen);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_line_class_registered() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = line_overlay_proc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kLineClass;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);
    registered = true;
}

LineOverlay::~LineOverlay() { destroy(); }

bool LineOverlay::create(const Options& opts) {
    if (hwnd_) return true;
    opts_ = opts;
    ensure_line_class_registered();
    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kLineClass, L"", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    ::SetLayeredWindowAttributes(hwnd_, 0, opts_.alpha, LWA_ALPHA);
    exclude_window_from_capture(hwnd_);
    return true;
}

void LineOverlay::move_to(POINT a, POINT b) {
    if (!hwnd_) return;
    if (a.x == b.x && a.y == b.y) {
        hide();
        return;
    }
    const int pad = opts_.thickness + 4;
    const int left   = (a.x < b.x ? a.x : b.x) - pad;
    const int top    = (a.y < b.y ? a.y : b.y) - pad;
    const int right  = (a.x > b.x ? a.x : b.x) + pad;
    const int bottom = (a.y > b.y ? a.y : b.y) + pad;
    const int w = right - left;
    const int h = bottom - top;
    if (w <= 0 || h <= 0) return;
    pt_a_ = POINT{a.x - left, a.y - top};
    pt_b_ = POINT{b.x - left, b.y - top};
    const bool first_show = !has_line_ || !::IsWindowVisible(hwnd_);
    has_line_ = true;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, left, top, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    if (first_show)
        exclude_window_from_capture(hwnd_);
}

void LineOverlay::hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
    has_line_ = false;
}

void LineOverlay::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    has_line_ = false;
}

RECT virtual_screen_rect() noexcept {
    return RECT{
        ::GetSystemMetrics(SM_XVIRTUALSCREEN),
        ::GetSystemMetrics(SM_YVIRTUALSCREEN),
        ::GetSystemMetrics(SM_XVIRTUALSCREEN) + ::GetSystemMetrics(SM_CXVIRTUALSCREEN),
        ::GetSystemMetrics(SM_YVIRTUALSCREEN) + ::GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

namespace {

constexpr COLORREF kMeasureColorKey = RGB(255, 0, 255);

void draw_dashed_line(HDC hdc, POINT a, POINT b, COLORREF color, int thickness) {
    HPEN pen = ::CreatePen(PS_DOT, thickness, color);
    HGDIOBJ old = ::SelectObject(hdc, pen);
    ::MoveToEx(hdc, a.x, a.y, nullptr);
    ::LineTo(hdc, b.x, b.y);
    ::SelectObject(hdc, old);
    ::DeleteObject(pen);
}

void draw_rect_outline(HDC hdc, const RECT& r, COLORREF color, int thickness) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    HPEN pen = ::CreatePen(PS_SOLID, thickness, color);
    HGDIOBJ old = ::SelectObject(hdc, pen);
    HBRUSH hollow = static_cast<HBRUSH>(::GetStockObject(HOLLOW_BRUSH));
    HGDIOBJ old_br = ::SelectObject(hdc, hollow);
    ::Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    ::SelectObject(hdc, old_br);
    ::SelectObject(hdc, old);
    ::DeleteObject(pen);
}

void offset_rect(RECT& r, int dx, int dy) {
    r.left += dx;
    r.right += dx;
    r.top += dy;
    r.bottom += dy;
}

} // namespace

LRESULT CALLBACK measure_overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<MeasureOverlay*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);
        if (self) self->paint(hdc, ps.rcPaint);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

void ensure_measure_class_registered() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = measure_overlay_proc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PmMeasureOverlay";
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);
    registered = true;
}

MeasureOverlay::~MeasureOverlay() { destroy(); }

bool MeasureOverlay::create(const Options& opts) {
    if (hwnd_) return true;
    opts_ = opts;
    ensure_measure_class_registered();
    const RECT vr = virtual_screen_rect();
    const int w = vr.right - vr.left;
    const int h = vr.bottom - vr.top;
    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"PmMeasureOverlay", L"", WS_POPUP,
        vr.left, vr.top, w > 0 ? w : 1, h > 0 ? h : 1,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    ::SetLayeredWindowAttributes(hwnd_, kMeasureColorKey, 0, LWA_COLORKEY);
    exclude_window_from_capture(hwnd_);
    return true;
}

void MeasureOverlay::paint(HDC hdc, const RECT& client) {
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    HBRUSH key = ::CreateSolidBrush(kMeasureColorKey);
    RECT fill{0, 0, w, h};
    ::FillRect(hdc, &fill, key);
    ::DeleteObject(key);

    if (!active_) return;

    const RECT vr = virtual_screen_rect();
    const int ox = vr.left;
    const int oy = vr.top;

    const COLORREF accent = opts_.accent;
    const int t = opts_.thickness > 0 ? opts_.thickness : 2;

    RECT span{
        pt_a_.x < pt_b_.x ? pt_a_.x : pt_b_.x,
        pt_a_.y < pt_b_.y ? pt_a_.y : pt_b_.y,
        pt_a_.x > pt_b_.x ? pt_a_.x : pt_b_.x,
        pt_a_.y > pt_b_.y ? pt_a_.y : pt_b_.y,
    };
    if (span.right == span.left) ++span.right;
    if (span.bottom == span.top) ++span.bottom;
    offset_rect(span, -ox, -oy);

    if (has_rect_a_) {
        RECT ra = rect_a_;
        offset_rect(ra, -ox, -oy);
        draw_rect_outline(hdc, ra, accent, t);
    }
    if (preview_ && has_rect_b_) {
        RECT rb = rect_b_;
        offset_rect(rb, -ox, -oy);
        draw_rect_outline(hdc, rb, accent, t);
    }

    draw_rect_outline(hdc, span, accent, t);

    POINT ca{pt_a_.x - ox, pt_a_.y - oy};
    POINT cb{pt_b_.x - ox, pt_b_.y - oy};
    draw_dashed_line(hdc, POINT{span.left, ca.y}, POINT{span.right, ca.y}, accent, 1);
    draw_dashed_line(hdc, POINT{ca.x, span.top}, POINT{ca.x, span.bottom}, accent, 1);
    if (preview_) {
        draw_dashed_line(hdc, POINT{span.left, cb.y}, POINT{span.right, cb.y}, accent, 1);
        draw_dashed_line(hdc, POINT{cb.x, span.top}, POINT{cb.x, span.bottom}, accent, 1);
    }
}

void MeasureOverlay::update(bool active,
                            POINT a,
                            POINT b,
                            const RECT* rect_a,
                            const RECT* rect_b,
                            bool preview) {
    if (!hwnd_) return;
    active_  = active;
    preview_ = preview;
    pt_a_    = a;
    pt_b_    = b;
    has_rect_a_ = false;
    has_rect_b_ = false;
    if (rect_a && rect_a->right > rect_a->left && rect_a->bottom > rect_a->top) {
        rect_a_     = *rect_a;
        has_rect_a_ = true;
    }
    if (rect_b && rect_b->right > rect_b->left && rect_b->bottom > rect_b->top) {
        rect_b_     = *rect_b;
        has_rect_b_ = true;
    }

    if (!active_) {
        hide();
        return;
    }

    const RECT vr = virtual_screen_rect();
    const int w = vr.right - vr.left;
    const int h = vr.bottom - vr.top;
    const bool first_show = !::IsWindowVisible(hwnd_);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, vr.left, vr.top, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    if (first_show)
        exclude_window_from_capture(hwnd_);
}

void MeasureOverlay::hide() {
    if (hwnd_) ::ShowWindow(hwnd_, SW_HIDE);
    active_ = false;
}

void MeasureOverlay::destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    active_ = false;
}

} // namespace media::win

#endif // _WIN32
