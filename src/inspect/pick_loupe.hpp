#pragma once
//
// pick_loupe.hpp — magnifier ("spyglass") for the interactive picker.
//
// Shows a zoomed pixel grid around the cursor plus the sampled color and screen
// coordinates, so a pick can be placed on an exact pixel — required for the
// eyedropper, useful for lining up drag crop edges.
//
// Same overlay recipe as PickHud: layered, click-through, topmost, excluded from
// capture (so the loupe never magnifies itself).
//
// Win32 only.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <climits>
#include <string>

namespace media::win {

class PickLoupe {
public:
    PickLoupe() = default;
    ~PickLoupe();

    PickLoupe(const PickLoupe&)            = delete;
    PickLoupe& operator=(const PickLoupe&) = delete;

    bool create();

    /// Re-centre on @p cursor_screen and repaint. @p footer_extra is appended in
    /// the loupe status bar (e.g. eyedropper hints).
    void update(POINT cursor_screen,
                COLORREF color,
                bool color_ok,
                const wchar_t* footer_extra = nullptr);

    void hide();
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    bool valid() const noexcept { return hwnd_ != nullptr; }

private:
    friend LRESULT CALLBACK pick_loupe_proc(HWND, UINT, WPARAM, LPARAM);

    void paint(HDC dc, const RECT& client);
    void set_dpi(UINT dpi);

    HWND     hwnd_ = nullptr;
    POINT        center_{};
    COLORREF     color_ = 0;
    bool         color_ok_ = false;
    std::wstring footer_extra_;
    int          last_x_ = INT_MIN;
    int          last_y_ = INT_MIN;
    int          anchor_cursor_x_ = INT_MIN;
    int          anchor_cursor_y_ = INT_MIN;
    HMONITOR     anchor_monitor_ = nullptr;
    UINT         dpi_ = 96;
    int          zoom_px_ = 8;
    int          content_px_ = 136;
    int          footer_px_ = 20;
    bool         slow_gdi_ = false;
    HFONT        font_ = nullptr;
};

} // namespace media::win

#endif // _WIN32
