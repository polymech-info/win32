#include "app_use.hpp"
#include "win/context/context_store.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <thread>

namespace media {
namespace assistant {
namespace app_use {
namespace {

std::string winerr(const char* what) {
    std::ostringstream oss;
    oss << what << " failed: " << ::GetLastError();
    return oss.str();
}

std::wstring quote_arg(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L'"';
    return out;
}

struct FindPidWindowCtx {
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

BOOL CALLBACK enum_pid_window(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindPidWindowCtx*>(lp);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && ::IsWindowVisible(hwnd)) {
        RECT r{};
        if (::GetWindowRect(hwnd, &r) && r.right > r.left && r.bottom > r.top) {
            ctx->hwnd = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

std::wstring lower_ascii(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

bool contains_ci(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return lower_ascii(haystack).find(lower_ascii(needle)) != std::wstring::npos;
}

struct FindWindowCandidate {
    HWND hwnd  = nullptr;
    int  score = 0;   // higher = better match
    LONG area  = 0;   // tiebreaker (main UI beats tooltip)
};

struct FindWindowCtx {
    DWORD pid = 0;
    HWND  hwnd = nullptr;
    std::wstring title;          // substring query (empty = match any)
    HWND  found = nullptr;       // legacy single-result slot (unused by ranked path)
    std::vector<FindWindowCandidate> candidates;
};

// Rank a title against a (case-insensitive) substring query. Larger score wins.
// Priority: exact match > starts with > word-boundary contains > contains.
// We add a small bonus inversely proportional to the title length so a short
// title containing the needle beats a long one with the needle embedded in
// noise (e.g. "Wispow Freepiano 2" beats "freepiano_2.2.2_win32 - Altap
// Salamander 4.0 (x64)" for the query "freepiano" -- the second one is a file
// explorer, not the app we want).
int score_title_match(const std::wstring& haystack_w, const std::wstring& needle_w) {
    if (needle_w.empty()) return 1;
    const std::wstring h = lower_ascii(haystack_w);
    const std::wstring n = lower_ascii(needle_w);
    const auto pos = h.find(n);
    if (pos == std::wstring::npos) return 0;
    int score = 100; // base for any contains-match

    // Word-boundary check: a "word" char touching the query LOWERS the score
    // because the query is embedded in a larger token (freepiano vs freepiano_).
    auto is_wordchar = [](wchar_t c) {
        return std::iswalnum(c) || c == L'_';
    };
    const bool left_boundary  = (pos == 0) || !is_wordchar(h[pos - 1]);
    const bool right_boundary = (pos + n.size() == h.size()) || !is_wordchar(h[pos + n.size()]);
    if (left_boundary && right_boundary) score += 300;       // clean word match
    else if (left_boundary || right_boundary) score += 100;  // half-bounded

    if (pos == 0) score += 200;                              // starts with
    if (h == n)   score += 1000;                             // exact match

    // Signal-to-noise: penalize 1 point per character of "noise" up to 200.
    // Caps so short titles can't dominate over much-better matches.
    const int noise = std::min<int>(200, static_cast<int>(h.size()) - static_cast<int>(n.size()));
    score -= noise;
    return score;
}

BOOL CALLBACK enum_matching_window(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<FindWindowCtx*>(lp);
    if (!::IsWindowVisible(hwnd)) return TRUE;
    if (ctx->hwnd && hwnd != ctx->hwnd) return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (ctx->pid && pid != ctx->pid) return TRUE;
    wchar_t title_buf[1024] = {};
    ::GetWindowTextW(hwnd, title_buf, static_cast<int>(std::size(title_buf)));
    const std::wstring title{title_buf};
    const int score = score_title_match(title, ctx->title);
    if (score <= 0) return TRUE;
    RECT r{};
    if (!::GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;
    // Off-screen sentinels at (-32000,-32000) are how the OS parks minimized
    // top-level windows; they're not real candidates for input.
    if (r.left <= -32000 || r.top <= -32000) return TRUE;
    const LONG area = static_cast<LONG>(r.right - r.left) * static_cast<LONG>(r.bottom - r.top);
    ctx->candidates.push_back({hwnd, score, area});
    return TRUE; // keep enumerating; we rank after EnumWindows returns
}

HWND wait_for_main_window(DWORD pid, int wait_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(wait_ms > 0 ? wait_ms : 0);
    FindPidWindowCtx ctx{pid, nullptr};
    do {
        ctx.hwnd = nullptr;
        ::EnumWindows(enum_pid_window, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.hwnd) return ctx.hwnd;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (std::chrono::steady_clock::now() < deadline);
    return nullptr;
}

// Set TRUE for VK codes whose physical scancode carries the E0 (extended) prefix.
// Apps that consume RawInput / DirectInput / GetKeyboardState look at the
// extended flag together with the scancode to disambiguate (e.g. left vs right
// Ctrl, numpad-enter vs main-enter, arrow keys vs numpad digits).
bool vk_is_extended_key(WORD vk) {
    switch (vk) {
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_APPS:
        case VK_INSERT: case VK_DELETE:
        case VK_HOME:   case VK_END:
        case VK_PRIOR:  case VK_NEXT:
        case VK_LEFT:   case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_DIVIDE: case VK_NUMLOCK:
        case VK_SNAPSHOT: case VK_CANCEL:
            return true;
        default:
            return false;
    }
}

void key_input(std::vector<INPUT>& in, WORD vk, bool up) {
    INPUT k{};
    k.type = INPUT_KEYBOARD;
    k.ki.wVk = vk;

    // Fill the hardware scancode for the current keyboard layout. SendInput
    // does NOT populate this automatically from wVk: if we leave it at 0,
    // the receiving WM_KEYDOWN lParam has scancode bits = 0. Most plain
    // Win32 apps don't care (they use the wParam VK), but audio / MIDI /
    // game apps (FreePiano, DAWs, anything built on DirectInput or
    // GetKeyboardState) read the scancode and silently drop events whose
    // scancode is zero. Filling it makes our synthetic keystroke
    // indistinguishable from a real one at the WM_KEYDOWN layer.
    //
    // MAPVK_VK_TO_VSC_EX includes the E0/E1 prefix in the high byte for
    // extended keys; we strip it (scan is a single byte in INPUT.ki.wScan)
    // and set KEYEVENTF_EXTENDEDKEY separately so the LParam carries bit 24.
    UINT scan_ex = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    if (scan_ex == 0) {
        // Fallback: layout-independent table lookup. Some VKs (mostly
        // F-keys and named keys) only resolve via the non-EX variant on
        // older Windows versions.
        scan_ex = ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    }
    k.ki.wScan = static_cast<WORD>(scan_ex & 0xFF);

    DWORD flags = 0;
    if (up) flags |= KEYEVENTF_KEYUP;
    if (vk_is_extended_key(vk) || (scan_ex & 0xE000)) flags |= KEYEVENTF_EXTENDEDKEY;
    k.ki.dwFlags = flags;
    in.push_back(k);
}

WORD vk_from_key(std::wstring key) {
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    if (key == L"ctrl" || key == L"control") return VK_CONTROL;
    if (key == L"shift") return VK_SHIFT;
    if (key == L"alt") return VK_MENU;
    if (key == L"win" || key == L"meta") return VK_LWIN;
    if (key == L"enter" || key == L"return") return VK_RETURN;
    if (key == L"esc" || key == L"escape") return VK_ESCAPE;
    if (key == L"tab") return VK_TAB;
    if (key == L"space") return VK_SPACE;
    if (key == L"backspace") return VK_BACK;
    if (key == L"delete" || key == L"del") return VK_DELETE;
    if (key == L"home") return VK_HOME;
    if (key == L"end") return VK_END;
    if (key == L"left") return VK_LEFT;
    if (key == L"right") return VK_RIGHT;
    if (key == L"up") return VK_UP;
    if (key == L"down") return VK_DOWN;
    if (key.size() >= 2 && key[0] == L'f') {
        int n = 0;
        for (size_t i = 1; i < key.size(); ++i) {
            if (key[i] < L'0' || key[i] > L'9') return 0;
            n = n * 10 + (key[i] - L'0');
        }
        if (n >= 1 && n <= 24) return static_cast<WORD>(VK_F1 + n - 1);
    }
    if (key.size() == 1) {
        const wchar_t c = key[0];
        if (c >= L'a' && c <= L'z') return static_cast<WORD>(L'A' + (c - L'a'));
        if (c >= L'0' && c <= L'9') return static_cast<WORD>(c);
    }
    return 0;
}

bool is_modifier(WORD vk) {
    return vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN;
}

bool mouse_button_flags(const std::wstring& button, DWORD& down, DWORD& up) {
    std::wstring b = lower_ascii(button);
    if (b.empty() || b == L"left") {
        down = MOUSEEVENTF_LEFTDOWN; up = MOUSEEVENTF_LEFTUP; return true;
    }
    if (b == L"right") {
        down = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP; return true;
    }
    if (b == L"middle") {
        down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; return true;
    }
    return false;
}

std::string ascii_key_name(const std::wstring& key) {
    std::string out;
    out.reserve(key.size());
    for (wchar_t c : key)
        out.push_back(c >= 32 && c <= 126 ? static_cast<char>(c) : '?');
    return out;
}

int DpiScaleInt(HWND hwnd, int value) {
    UINT dpi = 96;
    if (hwnd && ::IsWindow(hwnd))
        dpi = ::GetDpiForWindow(hwnd);
    return ::MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

} // namespace

bool open_app(const LaunchOptions& opts, LaunchResult& out, std::string& err) {
    if (opts.exe.empty()) {
        err = "--exe is required";
        return false;
    }
    std::wstring cmd = quote_arg(opts.exe);
    if (!opts.args.empty()) {
        cmd += L" ";
        cmd += opts.args;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd_buf = cmd;
    LPWSTR cwd = opts.cwd.empty() ? nullptr : const_cast<LPWSTR>(opts.cwd.c_str());
    if (!::CreateProcessW(nullptr, cmd_buf.empty() ? nullptr : &cmd_buf[0],
                          nullptr, nullptr, FALSE, 0, nullptr,
                          cwd, &si, &pi)) {
        err = winerr("CreateProcessW");
        return false;
    }
    out.pid = pi.dwProcessId;
    ::CloseHandle(pi.hThread);
    out.hwnd = wait_for_main_window(out.pid, opts.wait_ms);
    if (out.hwnd) {
        if (opts.w > 0 && opts.h > 0) {
            ::SetWindowPos(out.hwnd, nullptr, opts.x, opts.y,
                           DpiScaleInt(out.hwnd, opts.w), DpiScaleInt(out.hwnd, opts.h),
                           SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (opts.x != 0 || opts.y != 0) {
            RECT r{};
            if (::GetWindowRect(out.hwnd, &r))
                ::SetWindowPos(out.hwnd, nullptr, opts.x, opts.y, r.right - r.left, r.bottom - r.top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ::SetForegroundWindow(out.hwnd);
    }
    ::CloseHandle(pi.hProcess);
    return true;
}

ScreenInfo virtual_screen() {
    return ScreenInfo{
        ::GetSystemMetrics(SM_XVIRTUALSCREEN),
        ::GetSystemMetrics(SM_YVIRTUALSCREEN),
        ::GetSystemMetrics(SM_CXVIRTUALSCREEN),
        ::GetSystemMetrics(SM_CYVIRTUALSCREEN),
    };
}

Point cursor_position() {
    POINT pt{};
    ::GetCursorPos(&pt);
    return Point{pt.x, pt.y};
}

Point scale_point_from_api(Point api_point, int api_width, int api_height) {
    const ScreenInfo s = virtual_screen();
    if (api_width <= 0 || api_height <= 0 || s.width <= 0 || s.height <= 0)
        return api_point;
    return Point{
        s.x + static_cast<int>((static_cast<double>(api_point.x) / api_width) * s.width),
        s.y + static_cast<int>((static_cast<double>(api_point.y) / api_height) * s.height),
    };
}

Point scale_point_to_api(Point screen_point, int api_width, int api_height) {
    const ScreenInfo s = virtual_screen();
    if (api_width <= 0 || api_height <= 0 || s.width <= 0 || s.height <= 0)
        return screen_point;
    return Point{
        static_cast<int>((static_cast<double>(screen_point.x - s.x) / s.width) * api_width),
        static_cast<int>((static_cast<double>(screen_point.y - s.y) / s.height) * api_height),
    };
}

bool move_mouse(int x, int y, std::string& err) {
    if (!::SetCursorPos(x, y)) {
        err = winerr("SetCursorPos");
        return false;
    }
    // SetCursorPos teleports the cursor but does NOT post WM_MOUSEMOVE to the
    // focused window, so apps that drive their tools from mouse messages
    // (Paint's brush, color pickers, drag handles) miss intermediate points
    // during a drag. Follow up with a SendInput MOUSEEVENTF_MOVE in absolute
    // virtual-desktop coordinates so the window manager dispatches a real move
    // event at the new position.
    const ScreenInfo vs = virtual_screen();
    if (vs.width > 0 && vs.height > 0) {
        const long long nx = (static_cast<long long>(x - vs.x) * 65535LL) / vs.width;
        const long long ny = (static_cast<long long>(y - vs.y) * 65535LL) / vs.height;
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = static_cast<LONG>(nx);
        in.mi.dy = static_cast<LONG>(ny);
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        ::SendInput(1, &in, sizeof(INPUT));
    }
    return true;
}

// Linear animated cursor walk. One sample per frame (~16 ms) so app message pumps see intermediate
// positions — important for hover-driven UI, sticky-drag handles, and any app that pre-highlights
// on mouseover. Distance-based default duration: ~4000 px/sec, clamped 60..350 ms.
bool smooth_move(int x, int y, int duration_ms, std::string& err) {
    POINT start{};
    if (!::GetCursorPos(&start))
        return move_mouse(x, y, err);  // fall back to teleport if we can't read current pos
    const long long dx = static_cast<long long>(x) - start.x;
    const long long dy = static_cast<long long>(y) - start.y;
    const double distance = std::sqrt(static_cast<double>(dx * dx + dy * dy));
    if (duration_ms < 0) {
        // ~4000 px/sec by default. Floor 60 ms keeps brief moves visible to the UI; ceiling 350 ms
        // keeps long jumps from feeling sluggish.
        const int d = static_cast<int>(distance / 4.0);  // 4 px/ms => 4000 px/sec
        duration_ms = (std::max)(60, (std::min)(350, d));
    }
    if (duration_ms <= 0 || distance < 1.5)
        return move_mouse(x, y, err);
    constexpr int frame_ms = 16;
    int steps = (std::max)(1, duration_ms / frame_ms);
    if (steps > 240) steps = 240;
    const int step_ms = duration_ms / steps;
    for (int i = 1; i <= steps; ++i) {
        const int ix = static_cast<int>(start.x + (dx * i) / steps);
        const int iy = static_cast<int>(start.y + (dy * i) / steps);
        if (!move_mouse(ix, iy, err))
            return false;
        if (step_ms > 0 && i < steps)
            std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return true;
}

bool click_point(int x, int y, const std::wstring& button, int count, bool virtual_click, std::string& err) {
    POINT pt{x, y};
    HWND hwnd = ::WindowFromPoint(pt);
    UINT down_msg = WM_LBUTTONDOWN;
    UINT up_msg = WM_LBUTTONUP;
    DWORD input_down = MOUSEEVENTF_LEFTDOWN;
    DWORD input_up = MOUSEEVENTF_LEFTUP;
    WPARAM down_wparam = MK_LBUTTON;
    std::wstring b = lower_ascii(button);
    if (b == L"right") {
        down_msg = WM_RBUTTONDOWN; up_msg = WM_RBUTTONUP;
        input_down = MOUSEEVENTF_RIGHTDOWN; input_up = MOUSEEVENTF_RIGHTUP;
        down_wparam = MK_RBUTTON;
    } else if (b == L"middle") {
        down_msg = WM_MBUTTONDOWN; up_msg = WM_MBUTTONUP;
        input_down = MOUSEEVENTF_MIDDLEDOWN; input_up = MOUSEEVENTF_MIDDLEUP;
        down_wparam = MK_MBUTTON;
    } else if (!b.empty() && b != L"left") {
        err = "button must be left, right, or middle";
        return false;
    }
    count = count < 1 ? 1 : count;
    if (virtual_click) {
        if (!hwnd) { err = "no HWND under requested point"; return false; }
        POINT client = pt;
        ::ScreenToClient(hwnd, &client);
        LPARAM lp = MAKELPARAM(client.x, client.y);
        for (int i = 0; i < count; ++i) {
            ::PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp);
            ::PostMessageW(hwnd, down_msg, down_wparam, lp);
            ::PostMessageW(hwnd, up_msg, 0, lp);
        }
        return true;
    }
    if (hwnd) ::SetForegroundWindow(::GetAncestor(hwnd, GA_ROOT));
    // Animate the cursor to the click target so the destination app receives WM_MOUSEMOVE for
    // intermediate positions. Apps with hover-driven UI (tooltips, custom highlights, drag preview)
    // miss state transitions otherwise; some sticky-button widgets simply don't register a click
    // when the cursor teleports onto them at the same instant the button goes down.
    if (!smooth_move(x, y, /*duration_ms=*/-1, err)) return false;
    std::vector<INPUT> in;
    in.reserve(static_cast<size_t>(count) * 2);
    for (int i = 0; i < count; ++i) {
        INPUT down{};
        down.type = INPUT_MOUSE;
        down.mi.dwFlags = input_down;
        in.push_back(down);
        INPUT up{};
        up.type = INPUT_MOUSE;
        up.mi.dwFlags = input_up;
        in.push_back(up);
    }
    if (::SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) != in.size()) {
        err = winerr("SendInput(click)");
        return false;
    }
    return true;
}

bool drag_path(const std::vector<Point>& path, const std::wstring& button, int duration_ms, std::string& err) {
    if (path.size() < 2) {
        err = "drag_path: need at least 2 points";
        return false;
    }
    if (duration_ms < 0) duration_ms = 0;

    // Move to the first point with the usual smooth animation BEFORE pressing — gives apps a chance
    // to see the cursor enter the target region (highlights, drop-target previews).
    if (!smooth_move(path.front().x, path.front().y, /*duration_ms=*/-1, err)) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (!mouse_button_down(button, err)) return false;

    // Walk all segments. Time is distributed proportional to segment length so a long-then-short
    // path doesn't suddenly accelerate. Each segment is itself sampled at ~one frame per step so
    // free-draw apps (Paint brush, signature pads) get a smooth ink trail.
    double total_dist = 0.0;
    std::vector<double> seg_dist;
    seg_dist.reserve(path.size() - 1);
    for (std::size_t i = 1; i < path.size(); ++i) {
        const double sx = path[i].x - path[i - 1].x;
        const double sy = path[i].y - path[i - 1].y;
        const double d  = std::sqrt(sx * sx + sy * sy);
        seg_dist.push_back(d);
        total_dist += d;
    }
    if (total_dist < 1.0) total_dist = 1.0;
    constexpr int frame_ms = 16;

    for (std::size_t i = 1; i < path.size(); ++i) {
        const Point from = path[i - 1];
        const Point to   = path[i];
        const int   seg_ms = (duration_ms > 0)
                              ? static_cast<int>((duration_ms * seg_dist[i - 1]) / total_dist)
                              : 0;
        int sub_steps = (seg_ms > 0) ? (std::max)(1, seg_ms / frame_ms) : 1;
        if (sub_steps > 120) sub_steps = 120;
        const int sub_ms  = (sub_steps > 0) ? (seg_ms / sub_steps) : 0;
        for (int k = 1; k <= sub_steps; ++k) {
            const int xk = from.x + static_cast<int>((static_cast<long long>(to.x - from.x) * k) / sub_steps);
            const int yk = from.y + static_cast<int>((static_cast<long long>(to.y - from.y) * k) / sub_steps);
            if (!move_mouse(xk, yk, err)) {
                std::string up_err;
                mouse_button_up(button, up_err);
                return false;
            }
            if (sub_ms > 0 && !(i + 1 == path.size() && k == sub_steps))
                std::this_thread::sleep_for(std::chrono::milliseconds(sub_ms));
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    if (!mouse_button_up(button, err)) return false;
    return true;
}

bool drag_point(int x1, int y1, int x2, int y2, const std::wstring& button, int steps, int duration_ms, std::string& err) {
    // Preserve the historical default total duration so callers that don't pass duration_ms still
    // get a perceivable drag (matters for paint/select-rectangle UIs that key off motion).
    if (duration_ms < 0) duration_ms = 250;
    (void)steps;  // `steps` is now derived from duration_ms inside drag_path; kept in the signature
                  // for ABI compatibility.
    return drag_path({{x1, y1}, {x2, y2}}, button, duration_ms, err);
}

bool scroll_wheel(int clicks, bool horizontal, std::string& err) {
    INPUT in{};
    in.type        = INPUT_MOUSE;
    in.mi.dwFlags  = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(clicks * WHEEL_DELTA);
    if (::SendInput(1, &in, sizeof(INPUT)) != 1) {
        err = winerr(horizontal ? "SendInput(hscroll)" : "SendInput(scroll)");
        return false;
    }
    return true;
}

bool mouse_button_down(const std::wstring& button, std::string& err) {
    DWORD down = 0, up = 0;
    if (!mouse_button_flags(button, down, up)) {
        err = "button must be left, right, or middle";
        return false;
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down;
    if (::SendInput(1, &in, sizeof(INPUT)) != 1) {
        err = winerr("SendInput(mouse down)");
        return false;
    }
    return true;
}

bool mouse_button_up(const std::wstring& button, std::string& err) {
    DWORD down = 0, up = 0;
    if (!mouse_button_flags(button, down, up)) {
        err = "button must be left, right, or middle";
        return false;
    }
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = up;
    if (::SendInput(1, &in, sizeof(INPUT)) != 1) {
        err = winerr("SendInput(mouse up)");
        return false;
    }
    return true;
}

bool activate_window(HWND hwnd, std::string& err) {
    if (!hwnd || !::IsWindow(hwnd)) {
        err = "target window not found";
        return false;
    }

    // SetForegroundWindow alone is unreliable: Windows blocks it (anti
    // focus-stealing) when our process didn't recently have focus, and the
    // call returns success while silently doing nothing. Workaround: attach
    // to the current foreground thread's input queue, then SetForegroundWindow
    // takes effect. We retry up to 3 times with growing waits because the
    // OS focus change is asynchronous and can race with input synthesis.
    //
    // References:
    //   https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow
    //   https://devblogs.microsoft.com/oldnewthing/20081124-00/?p=20083 (attach-input trick)

    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (::IsIconic(root)) ::ShowWindow(root, SW_RESTORE);

    const HWND already_fg = ::GetForegroundWindow();
    if (::GetAncestor(already_fg, GA_ROOT) == root) {
        // Already foreground; just raise it (cheap) and return.
        ::BringWindowToTop(root);
        return true;
    }

    const DWORD self_tid  = ::GetCurrentThreadId();
    auto try_set_fg = [&](HWND target) {
        const HWND fg     = ::GetForegroundWindow();
        const DWORD fg_tid  = fg ? ::GetWindowThreadProcessId(fg, nullptr) : 0;
        const DWORD tgt_tid = ::GetWindowThreadProcessId(target, nullptr);

        bool attached_fg  = (fg_tid  && fg_tid  != self_tid) && (::AttachThreadInput(self_tid, fg_tid,  TRUE) != 0);
        bool attached_tgt = (tgt_tid && tgt_tid != self_tid) && (::AttachThreadInput(self_tid, tgt_tid, TRUE) != 0);

        ::AllowSetForegroundWindow(ASFW_ANY);
        ::BringWindowToTop(target);
        ::SetForegroundWindow(target);
        ::SetActiveWindow(target);
        ::SetFocus(target);

        if (attached_tgt) ::AttachThreadInput(self_tid, tgt_tid, FALSE);
        if (attached_fg)  ::AttachThreadInput(self_tid, fg_tid,  FALSE);
    };

    for (int attempt = 0; attempt < 3; ++attempt) {
        try_set_fg(root);
        // Let the OS settle the focus change.
        std::this_thread::sleep_for(std::chrono::milliseconds(40 + 60 * attempt));
        const HWND now_fg = ::GetForegroundWindow();
        if (now_fg && ::GetAncestor(now_fg, GA_ROOT) == root)
            return true;
    }

    const HWND giveup_fg = ::GetForegroundWindow();
    std::ostringstream oss;
    oss << "could not bring target window to foreground after 3 attempts; "
        << "current foreground hwnd=" << reinterpret_cast<std::uintptr_t>(giveup_fg)
        << " (target root=" << reinterpret_cast<std::uintptr_t>(root) << ")";
    err = oss.str();
    return false;
}

bool click_in_window(HWND hwnd, int rx, int ry, const std::wstring& button,
                     int count, bool virtual_click, bool activate, std::string& err) {
    if (!hwnd || !::IsWindow(hwnd)) {
        err = "target window not found";
        return false;
    }
    // Click at a point expressed RELATIVE to the window's top-left, recomputed
    // from the live window rect — so the same (rx,ry) hits the same control even
    // after the window is moved/restored. Mirrors the capture-side rrect= model
    // emitted by `assistant pick`.
    if (activate) {
        std::string aerr;
        if (!activate_window(hwnd, aerr)) {
            // Non-fatal: a background click can still land; surface as a warning
            // only if the subsequent click fails.
        }
    }
    RECT wr{};
    if (!::GetWindowRect(hwnd, &wr)) {
        err = "GetWindowRect failed";
        return false;
    }
    return click_point(wr.left + rx, wr.top + ry, button, count, virtual_click, err);
}

bool set_window_bounds(HWND hwnd, int x, int y, int w, int h, std::string& err) {
    if (!hwnd || !::IsWindow(hwnd)) {
        err = "target window not found";
        return false;
    }
    const HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (::IsIconic(root)) ::ShowWindow(root, SW_RESTORE);

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    if (w <= 0 || h <= 0) flags |= SWP_NOSIZE;  // move only

    if (!::SetWindowPos(root, nullptr, x, y, (w > 0 ? w : 0), (h > 0 ? h : 0), flags)) {
        err = "SetWindowPos failed (err=" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    return true;
}

bool find_window(DWORD pid, HWND hwnd, const std::wstring& title_contains, HWND& out, std::string& err) {
    FindWindowCtx ctx;
    ctx.pid = pid;
    ctx.hwnd = hwnd;
    ctx.title = title_contains;
    ::EnumWindows(enum_matching_window, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.candidates.empty()) {
        err = "matching window not found";
        return false;
    }
    // Rank: highest match score wins; tiebreak by larger window area
    // (so the main UI beats tooltips / hidden helper windows).
    std::sort(ctx.candidates.begin(), ctx.candidates.end(),
              [](const FindWindowCandidate& a, const FindWindowCandidate& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.area > b.area;
              });
    out = ctx.candidates.front().hwnd;
    return true;
}

bool type_text(const std::wstring& text, std::string& err) {
    // Some controls (WordPad RichEdit, classic single-line edits) ignore the
    // Unicode line-feed code point; they want a real VK_RETURN keystroke.
    // Special-case '\n' and '\r' so multi-line app_type calls actually work.
    // A '\r\n' pair is collapsed into one VK_RETURN.
    std::vector<INPUT> in;
    in.reserve(text.size() * 2);
    auto push_unicode = [&](wchar_t c) {
        INPUT k{};
        k.type = INPUT_KEYBOARD;
        k.ki.wScan = c;
        k.ki.dwFlags = KEYEVENTF_UNICODE;
        in.push_back(k);
        k.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        in.push_back(k);
    };
    auto push_vk = [&](WORD vk) {
        INPUT k{};
        k.type = INPUT_KEYBOARD;
        k.ki.wVk = vk;
        in.push_back(k);
        k.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(k);
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c == L'\r' || c == L'\n') {
            push_vk(VK_RETURN);
            if (c == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') ++i;
        } else if (c == L'\t') {
            push_vk(VK_TAB);
        } else {
            push_unicode(c);
        }
    }
    if (!in.empty() && ::SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) != in.size()) {
        err = winerr("SendInput(type)");
        return false;
    }
    return true;
}

bool send_hotkey(const std::vector<std::wstring>& keys, std::string& err) {
    std::vector<WORD> modifiers;
    std::vector<WORD> normals;
    for (const auto& key : keys) {
        WORD vk = vk_from_key(key);
        if (!vk) {
            err = "unsupported key: " + ascii_key_name(key);
            return false;
        }
        (is_modifier(vk) ? modifiers : normals).push_back(vk);
    }
    if (normals.empty() && modifiers.empty()) {
        err = "no keys specified";
        return false;
    }
    std::vector<INPUT> in;
    for (WORD vk : modifiers) key_input(in, vk, false);
    for (WORD vk : normals) key_input(in, vk, false);
    for (auto it = normals.rbegin(); it != normals.rend(); ++it) key_input(in, *it, true);
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) key_input(in, *it, true);
    if (::SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) != in.size()) {
        err = winerr("SendInput(hotkey)");
        return false;
    }
    return true;
}

bool key_down(const std::wstring& key, std::string& err) {
    WORD vk = vk_from_key(key);
    if (!vk) { err = "unsupported key: " + ascii_key_name(key); return false; }
    std::vector<INPUT> in;
    key_input(in, vk, false);
    if (::SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) != in.size()) {
        err = winerr("SendInput(key_down)");
        return false;
    }
    return true;
}

bool key_up(const std::wstring& key, std::string& err) {
    WORD vk = vk_from_key(key);
    if (!vk) { err = "unsupported key: " + ascii_key_name(key); return false; }
    std::vector<INPUT> in;
    key_input(in, vk, true);
    if (::SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) != in.size()) {
        err = winerr("SendInput(key_up)");
        return false;
    }
    return true;
}

bool press_key(const std::wstring& key,
               const std::vector<std::wstring>& modifiers,
               int hold_ms,
               std::string& err) {
    WORD vk = vk_from_key(key);
    if (!vk) { err = "unsupported key: " + ascii_key_name(key); return false; }
    if (hold_ms < 0)     hold_ms = 0;
    if (hold_ms > 30000) hold_ms = 30000;

    std::vector<WORD> mods;
    mods.reserve(modifiers.size());
    for (const auto& m : modifiers) {
        WORD mvk = vk_from_key(m);
        if (!mvk || !is_modifier(mvk)) {
            err = "unsupported modifier: " + ascii_key_name(m);
            return false;
        }
        mods.push_back(mvk);
    }

    // Phase 1: press modifiers (if any) + the key.
    std::vector<INPUT> down_inputs;
    for (WORD m : mods) key_input(down_inputs, m, false);
    key_input(down_inputs, vk, false);
    if (::SendInput(static_cast<UINT>(down_inputs.size()), down_inputs.data(), sizeof(INPUT)) != down_inputs.size()) {
        err = winerr("SendInput(press_key down)");
        return false;
    }

    // Phase 2: hold. This is what makes a piano note sustain. Sleep -- NOT
    // a busy wait -- so the OS can deliver KEYDOWN -> auto-repeat -> NoteOn
    // events to the target app while we wait.
    if (hold_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));

    // Phase 3: release the key first, then modifiers in reverse order.
    std::vector<INPUT> up_inputs;
    key_input(up_inputs, vk, true);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) key_input(up_inputs, *it, true);
    if (::SendInput(static_cast<UINT>(up_inputs.size()), up_inputs.data(), sizeof(INPUT)) != up_inputs.size()) {
        err = winerr("SendInput(press_key up)");
        return false;
    }
    return true;
}

bool set_clipboard_text(const std::wstring& text, std::string& err) {
    if (!::OpenClipboard(nullptr)) {
        err = winerr("OpenClipboard");
        return false;
    }
    struct CloseClipboardGuard { ~CloseClipboardGuard() { ::CloseClipboard(); } } guard;
    if (!::EmptyClipboard()) {
        err = winerr("EmptyClipboard");
        return false;
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        err = winerr("GlobalAlloc");
        return false;
    }
    void* p = ::GlobalLock(mem);
    if (!p) {
        ::GlobalFree(mem);
        err = winerr("GlobalLock");
        return false;
    }
    std::memcpy(p, text.c_str(), bytes);
    ::GlobalUnlock(mem);
    if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
        ::GlobalFree(mem);
        err = winerr("SetClipboardData");
        return false;
    }
    return true;
}

bool get_clipboard_text(std::wstring& out, std::string& err) {
    if (!::OpenClipboard(nullptr)) {
        err = winerr("OpenClipboard");
        return false;
    }
    struct CloseClipboardGuard { ~CloseClipboardGuard() { ::CloseClipboard(); } } guard;
    HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        err = "clipboard does not contain Unicode text";
        return false;
    }
    const wchar_t* p = static_cast<const wchar_t*>(::GlobalLock(h));
    if (!p) {
        err = winerr("GlobalLock");
        return false;
    }
    out = p;
    ::GlobalUnlock(h);
    return true;
}

std::vector<std::wstring> split_keys(const std::wstring& spec) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : spec) {
        if (c == L'+' || c == L',' || std::iswspace(c)) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// --- Extended selector implementations (CLI-Anything inspired) ---

// FNV-1a hash for stable reference generation
static constexpr std::uint32_t kFnv1aOffset = 0x811c9dc5;
static constexpr std::uint32_t kFnv1aPrime = 0x01000193;

std::uint32_t fnv1a_wstring(const std::wstring& s) {
    std::uint32_t hash = kFnv1aOffset;
    for (wchar_t c : s) {
        hash ^= static_cast<std::uint32_t>(c);
        hash *= kFnv1aPrime;
    }
    return hash;
}

std::wstring hash_short(const std::wstring& s, int chars = 6) {
    const std::uint32_t h = fnv1a_wstring(s);
    wchar_t buf[16];
    const wchar_t* hex = L"0123456789abcdef";
    for (int i = 0; i < chars; ++i) buf[i] = hex[(h >> (i * 4)) & 0xf];
    buf[chars] = L'\0';
    return std::wstring(buf);
}

std::wstring compute_element_ref(
    const std::wstring& control_type,
    const std::wstring& name,
    const std::wstring& automation_id,
    int nth) {
    // Format: "{control_type}:{name_hash}:{automation_id_hash}:{nth}"
    // Allows re-targeting even if element index changes between sessions.
    std::wostringstream oss;
    oss << (control_type.empty() ? L"Unknown" : control_type) << L':'
        << hash_short(name, 6) << L':'
        << hash_short(automation_id, 6) << L':'
        << nth;
    return oss.str();
}

// Word-boundary check: returns true if query is cleanly bounded by non-word chars
static bool is_word_boundary(const std::wstring& haystack, size_t pos, size_t len) {
    auto is_wordchar = [](wchar_t c) {
        return std::iswalnum(c) || c == L'_';
    };
    const bool left_ok = (pos == 0) || !is_wordchar(haystack[pos - 1]);
    const bool right_ok = (pos + len >= haystack.size()) || !is_wordchar(haystack[pos + len]);
    return left_ok && right_ok;
}

int score_element_match(
    const std::wstring& control_type,
    const std::wstring& name,
    const std::wstring& automation_id,
    const std::wstring& value,
    const ElementSelector& selector) {
    int score = 0;
    const std::wstring lc_name = lower_ascii(name);
    const std::wstring lc_auto = lower_ascii(automation_id);
    const std::wstring lc_ctrl = lower_ascii(control_type);
    const std::wstring lc_val = lower_ascii(value);

    // Control type match (exact) - bonus
    if (!selector.control_type.empty()) {
        if (lc_ctrl == lower_ascii(selector.control_type)) score += 150;
        else if (contains_ci(control_type, selector.control_type)) score += 50;
        else return 0;  // Control type is a hard filter if specified
    }

    // Automation ID match - highest priority (stable programmatic ID)
    if (!selector.automation_id.empty()) {
        const std::wstring lc_sel_auto = lower_ascii(selector.automation_id);
        if (lc_auto == lc_sel_auto) score += 500;
        else if (contains_ci(automation_id, selector.automation_id)) score += 300;
        else if (!automation_id.empty()) score += 10;  // Has ID but doesn't match
    }

    // Name match - scored by quality
    if (!selector.name.empty()) {
        const std::wstring lc_sel_name = lower_ascii(selector.name);
        if (lc_name == lc_sel_name) {
            score += 400;
        } else {
            const auto pos = lc_name.find(lc_sel_name);
            if (pos != std::wstring::npos) {
                if (is_word_boundary(lc_name, pos, lc_sel_name.size())) {
                    score += 300;  // Clean word boundary match
                } else if (pos == 0) {
                    score += 200;  // Prefix match
                } else {
                    score += 100;  // Contains match
                }
            }
        }
    }

    // Value match (for Edit, Document controls)
    if (!selector.value.empty()) {
        if (contains_ci(value, selector.value)) score += 80;
    }

    // Ref match - exact reference ID (highest possible score if matched)
    if (!selector.ref.empty()) {
        const std::wstring computed = compute_element_ref(control_type, name, automation_id, 0);
        if (selector.ref == computed || selector.ref == compute_element_ref(control_type, name, automation_id, selector.nth)) {
            score += 1000;  // Exact reference match
        }
    }

    return score;
}

// Window search scoring - similar to existing enum_matching_window but extended
struct FindWindowCandidateEx {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    int score = 0;
    LONG area = 0;
    std::wstring title;
    std::wstring class_name;
    std::wstring process;
};

std::wstring get_window_class(HWND hwnd) {
    wchar_t buf[256] = {};
    ::GetClassNameW(hwnd, buf, static_cast<int>(std::size(buf)));
    return std::wstring(buf);
}

std::wstring get_process_name_from_pid(DWORD pid) {
    if (!pid) return {};
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = static_cast<DWORD>(std::size(buf));
    const BOOL ok = ::QueryFullProcessImageNameW(h, 0, buf, &sz);
    ::CloseHandle(h);
    if (!ok || sz == 0) return {};
    std::wstring full(buf, static_cast<size_t>(sz));
    const auto slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? full : full.substr(slash + 1);
}

int score_window_title(const std::wstring& title, const std::wstring& query) {
    if (query.empty()) return 1;
    const std::wstring h = lower_ascii(title);
    const std::wstring n = lower_ascii(query);
    const auto pos = h.find(n);
    if (pos == std::wstring::npos) return 0;
    int score = 100;  // Base for any contains-match
    if (is_word_boundary(h, pos, n.size())) score += 300;
    else if (pos == 0) score += 200;
    if (h == n) score += 500;  // Exact match
    // Signal-to-noise: penalize noise, capped
    const int noise = static_cast<int>(std::min<size_t>(200, h.size() - n.size()));
    score -= noise;
    return score;
}

BOOL CALLBACK enum_matching_window_ex(HWND hwnd, LPARAM lp) {
    auto* candidates = reinterpret_cast<std::vector<FindWindowCandidateEx>*>(lp);
    if (!::IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    RECT r{};
    if (!::GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;
    if (r.left <= -32000 || r.top <= -32000) return TRUE;  // Minimized sentinel

    wchar_t title_buf[1024] = {};
    ::GetWindowTextW(hwnd, title_buf, static_cast<int>(std::size(title_buf)));
    const std::wstring title(title_buf);
    if (title.empty()) return TRUE;

    FindWindowCandidateEx cand;
    cand.hwnd = hwnd;
    cand.pid = pid;
    cand.title = title;
    cand.class_name = get_window_class(hwnd);
    cand.process = get_process_name_from_pid(pid);
    cand.area = static_cast<LONG>(r.right - r.left) * static_cast<LONG>(r.bottom - r.top);
    candidates->push_back(cand);
    return TRUE;
}

bool find_window_ex(const FindWindowOptions& opts, FindWindowResult& out, std::string& err) {
    if (opts.foreground) {
        HWND fg = ::GetForegroundWindow();
        const HWND preferred = media::win::context::ContextStore::instance().PreferredExternalHwnd();
        if (preferred && preferred != fg && ::IsWindow(preferred)) {
            DWORD fg_pid = 0;
            ::GetWindowThreadProcessId(fg, &fg_pid);
            if (!fg || fg_pid == ::GetCurrentProcessId())
                fg = preferred;
        }
        if (!fg) { err = "no foreground window"; return false; }
        out.hwnd = fg;
        DWORD pid = 0;
        ::GetWindowThreadProcessId(fg, &pid);
        out.pid = pid;
        out.score = 1000;
        wchar_t title_buf[1024] = {};
        ::GetWindowTextW(fg, title_buf, static_cast<int>(std::size(title_buf)));
        out.title = title_buf;
        out.class_name = get_window_class(fg);
        out.process = get_process_name_from_pid(pid);
        return true;
    }

    if (opts.hwnd) {
        if (!::IsWindow(opts.hwnd)) { err = "hwnd is not a valid window"; return false; }
        out.hwnd = opts.hwnd;
        DWORD pid = 0;
        ::GetWindowThreadProcessId(opts.hwnd, &pid);
        out.pid = pid;
        out.score = 1000;
        wchar_t title_buf[1024] = {};
        ::GetWindowTextW(opts.hwnd, title_buf, static_cast<int>(std::size(title_buf)));
        out.title = title_buf;
        out.class_name = get_window_class(opts.hwnd);
        out.process = get_process_name_from_pid(pid);
        return true;
    }

    std::vector<FindWindowCandidateEx> candidates;
    ::EnumWindows(enum_matching_window_ex, reinterpret_cast<LPARAM>(&candidates));

    std::vector<FindWindowCandidateEx> matches;
    for (auto& c : candidates) {
        // PID filter
        if (opts.pid != 0 && c.pid != opts.pid) continue;
        // Process name filter
        if (!opts.process_name.empty() && !contains_ci(c.process, opts.process_name)) continue;
        // Class name filter
        if (!opts.class_name.empty() && !contains_ci(c.class_name, opts.class_name)) continue;
        // Title score
        c.score = score_window_title(c.title, opts.title_contains);
        if (c.score <= 0 && !opts.title_contains.empty()) continue;
        if (c.score <= 0) c.score = 1;  // Matches other criteria but no title filter
        matches.push_back(c);
    }

    if (matches.empty()) {
        err = "no matching window found";
        return false;
    }

    // Rank by score desc, then by area desc (main window beats tooltips)
    std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.area > b.area;
    });

    const auto& best = matches.front();
    out.hwnd = best.hwnd;
    out.pid = best.pid;
    out.score = best.score;
    out.title = best.title;
    out.class_name = best.class_name;
    out.process = best.process;
    return true;
}

bool close_app(const CloseAppOptions& opts, CloseAppResult& out, std::string& err)
{
    // Resolve HWND: explicit > pid > title search.
    HWND hwnd = opts.hwnd;
    DWORD pid = opts.pid;

    if (!hwnd) {
        FindWindowCtx ctx;
        ctx.pid   = opts.pid;
        ctx.title = opts.title_contains;
        ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<FindWindowCtx*>(lp);
            if (!::IsWindowVisible(h)) return TRUE;
            DWORD wp = 0;
            ::GetWindowThreadProcessId(h, &wp);
            if (c->pid && wp != c->pid) return TRUE;
            if (!c->title.empty()) {
                wchar_t buf[512] = {};
                ::GetWindowTextW(h, buf, static_cast<int>(std::size(buf)));
                if (!contains_ci(std::wstring(buf), c->title)) return TRUE;
            }
            RECT r{};
            if (::GetWindowRect(h, &r) && r.right > r.left && r.bottom > r.top) {
                c->found = h;
                if (!c->pid) {
                    ::GetWindowThreadProcessId(h, &wp);
                    // store pid in hwnd slot for retrieval below
                }
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        hwnd = ctx.found;
    }

    // Derive pid from hwnd if we still don't have it.
    if (hwnd && !pid) {
        ::GetWindowThreadProcessId(hwnd, &pid);
    }

    // Send WM_CLOSE.
    if (hwnd) {
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else if (!pid) {
        err = "no window or process to close";
        return false;
    }

    // Wait for process to exit.
    const int wait = opts.wait_ms > 0 ? opts.wait_ms : 2000;
    if (pid) {
        HANDLE hProc = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (hProc) {
            const DWORD r = ::WaitForSingleObject(hProc, static_cast<DWORD>(wait));
            if (r == WAIT_OBJECT_0) {
                ::CloseHandle(hProc);
                out.closed = true;
                return true;
            }
            // Still running: force-kill if requested.
            if (opts.force) {
                ::TerminateProcess(hProc, 1);
                out.killed = true;
                out.closed = true;
            }
            ::CloseHandle(hProc);
            if (!out.killed) {
                err = "process did not exit within " + std::to_string(wait) + " ms";
                return false;
            }
            return true;
        }
    }
    // No pid or could not open — assume WM_CLOSE was sent successfully.
    out.closed = true;
    return true;
}

} // namespace app_use
} // namespace assistant
} // namespace media
