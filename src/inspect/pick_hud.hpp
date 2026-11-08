#pragma once
//
// pick_hud.hpp — click-through status chip for the interactive picker.
// Same overlay recipe as the recording zoom HUD (layered, transparent hit-test,
// topmost). Idle capture affinity is WDA_NONE so other-process recorders see
// it; our auto/exclude takes hide it for the scope.
//
// Win32 only.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

namespace media::win {

class PickHud {
public:
    PickHud() = default;
    ~PickHud();

    PickHud(const PickHud&)            = delete;
    PickHud& operator=(const PickHud&) = delete;

    bool create();
    void show_near(POINT cursor_screen,
                   const std::wstring& primary,
                   const std::wstring& secondary);
    void hide();
    void destroy();

    HWND hwnd() const noexcept { return hwnd_; }
    bool valid() const noexcept { return hwnd_ != nullptr; }

private:
    friend LRESULT CALLBACK pick_hud_proc(HWND, UINT, WPARAM, LPARAM);

    void paint(HDC dc, const RECT& client) const;

    HWND         hwnd_ = nullptr;
    std::wstring primary_;
    std::wstring secondary_;
    int          last_x_ = INT_MIN;
    int          last_y_ = INT_MIN;
    int          anchor_cursor_x_ = INT_MIN;
    int          anchor_cursor_y_ = INT_MIN;
    int          width_ = 0;
    int          height_ = 0;
};

} // namespace media::win

#endif // _WIN32
