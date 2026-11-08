#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace media::assistant::app_use {

struct Point {
    int x = 0;
    int y = 0;
};

struct ScreenInfo {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct LaunchOptions {
    std::wstring exe;
    std::wstring args;
    std::wstring cwd;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int wait_ms = 1000;
};

struct LaunchResult {
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

bool open_app(const LaunchOptions& opts, LaunchResult& out, std::string& err);

/// Extended window search options for stable selectors.
/// Mirrors the selector pattern from app_inspect ElementInfo.
struct FindWindowOptions {
    DWORD pid = 0;
    HWND hwnd = nullptr;
    std::wstring title_contains;
    std::wstring class_name;       // Window class name (e.g., "Notepad", "Chrome_WidgetWin_1")
    std::wstring process_name;     // Process name substring (e.g., "notepad.exe")
    bool foreground = false;       // Match the foreground window
};

/// Extended result with match metadata for selector scoring.
struct FindWindowResult {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    int score = 0;                 // Higher = better match (0 = no match)
    std::wstring title;
    std::wstring class_name;
    std::wstring process;
};

/// Find window using extended selectors. Returns highest-scoring match.
/// Score priority: exact title > title start > title word-boundary > contains > any.
bool find_window_ex(const FindWindowOptions& opts, FindWindowResult& out, std::string& err);

/// Legacy find_window - preserved for backward compatibility.
/// Consider migrating to find_window_ex for new code.
bool find_window(DWORD pid, HWND hwnd, const std::wstring& title_contains, HWND& out, std::string& err);
ScreenInfo virtual_screen();
Point cursor_position();
Point scale_point_from_api(Point api_point, int api_width, int api_height);
Point scale_point_to_api(Point screen_point, int api_width, int api_height);

/// Instant cursor move (teleport + a synthetic absolute MOUSEEVENTF_MOVE so apps receive WM_MOUSEMOVE
/// at the final position). Cheapest, used internally by smooth_move per frame.
bool move_mouse(int x, int y, std::string& err);

/// Animated cursor move from current position to (x, y). `duration_ms == 0` teleports (equivalent
/// to `move_mouse`); otherwise we walk a linear path at roughly one frame (16 ms) per intermediate
/// point. If `duration_ms < 0`, a reasonable default is computed from travel distance:
///   ~4000 px/sec, clamped to [60, 350] ms. This is the default for click/drag pre-movements
/// because it lets sticky-hover apps (menus, tooltips, custom drag handles) react to the cursor.
bool smooth_move(int x, int y, int duration_ms, std::string& err);

bool click_point(int x, int y, const std::wstring& button, int count, bool virtual_click, std::string& err);

/// Point-to-point drag. Convenience for the common case. Internally just builds a 2-point path and
/// delegates to `drag_path`.
bool drag_point(int x1, int y1, int x2, int y2, const std::wstring& button, int steps, int duration_ms, std::string& err);

/// Drag along an arbitrary polyline. The first point is where the button goes down, the last is
/// where it comes up. Total time across all intermediate moves is `duration_ms`; pacing is
/// distance-proportional so a long-then-short segment doesn't suddenly speed up. Pass at least
/// 2 points. If fewer than 2, returns an error.
bool drag_path(const std::vector<Point>& path, const std::wstring& button, int duration_ms, std::string& err);

/// Mouse wheel. `clicks > 0` scrolls "up" (toward content top) by convention. `horizontal=true`
/// uses MOUSEEVENTF_HWHEEL (positive = right). The wheel event lands on the window currently under
/// the cursor — call `smooth_move`/`move_mouse` first to target a specific control.
bool scroll_wheel(int clicks, bool horizontal, std::string& err);

bool mouse_button_down(const std::wstring& button, std::string& err);
bool mouse_button_up(const std::wstring& button, std::string& err);
bool activate_window(HWND hwnd, std::string& err);

/// Click at a point given RELATIVE to a window's top-left (recomputed from the
/// live window rect each call), so the same (rx,ry) hits the same control after
/// the window moves/restores. This is the click-side counterpart to the capture
/// `hwnd=<root>:rrect=dx,dy,w,h` "follow" selector emitted by `assistant pick`:
/// feed the picked (dx,dy) (+ half the element size for the center) back here to
/// click a target that follows its window. When `activate` is true the window is
/// brought to the foreground first.
bool click_in_window(HWND hwnd, int rx, int ry, const std::wstring& button,
                     int count, bool virtual_click, bool activate, std::string& err);

/// Move/resize a window (its GA_ROOT). Restores it first if minimized. Pass
/// w<=0 or h<=0 to move without resizing. Useful to place a picked window into a
/// known rectangle before recording/detecting so a fixed `rect=` selector stays
/// valid, or to tile capture targets deterministically.
bool set_window_bounds(HWND hwnd, int x, int y, int w, int h, std::string& err);
bool find_window(DWORD pid, HWND hwnd, const std::wstring& title_contains, HWND& out, std::string& err);
bool type_text(const std::wstring& text, std::string& err);
bool send_hotkey(const std::vector<std::wstring>& keys, std::string& err);

// Low-level single-key control. Useful for melodic / rhythmic input where
// the duration the key is held matters (e.g. virtual pianos like FreePiano:
// the host treats key-down as NoteOn and key-up as NoteOff, so the time
// between them is the note's sustain length).
bool key_down(const std::wstring& key, std::string& err);
bool key_up  (const std::wstring& key, std::string& err);

// Press a key while holding `modifiers` (any of ctrl/shift/alt/win), wait
// `hold_ms` milliseconds, then release the key and the modifiers in reverse
// order. `hold_ms` is clamped to [0, 30000] -- the caller decides the rest
// gap between keystrokes (see app_batch's `delayMs`).
bool press_key(const std::wstring& key,
               const std::vector<std::wstring>& modifiers,
               int hold_ms,
               std::string& err);
bool set_clipboard_text(const std::wstring& text, std::string& err);
bool get_clipboard_text(std::wstring& out, std::string& err);
std::vector<std::wstring> split_keys(const std::wstring& spec);

// ─── close_app ───────────────────────────────────────────────────────────────

struct CloseAppOptions {
    HWND         hwnd            = nullptr; ///< Explicit window handle (takes precedence).
    DWORD        pid             = 0;       ///< Process id to close.
    std::wstring title_contains;            ///< Fallback: find window by title substring.
    bool         force           = false;   ///< When true, TerminateProcess after WM_CLOSE fails.
    int          wait_ms         = 2000;    ///< Timeout waiting for graceful exit (ms).
};

struct CloseAppResult {
    bool closed = false;   ///< Exited gracefully (WM_CLOSE / process termination detected).
    bool killed = false;   ///< Force-terminated (only set when opts.force=true and needed).
};

/// Send WM_CLOSE to a window (or process) and optionally wait for it to exit.
/// With force=true: graceful WM_CLOSE first; TerminateProcess if still running after wait_ms.
bool close_app(const CloseAppOptions& opts, CloseAppResult& out, std::string& err);

/// Element selector for stable UIA element targeting.
/// Encodes all matching criteria in one struct to avoid parameter bloat.
/// Use with app_inspect to generate stable 'ref' identifiers.
struct ElementSelector {
    std::wstring ref;              // Stable reference ID (e.g., "Button:OK:5:a1b2c3")
    std::wstring name;             // Element name (substring match, case-insensitive)
    std::wstring value;            // Element value/pattern (substring match)
    std::wstring control_type;     // Control type (e.g., "Button", "Edit", "MenuItem")
    std::wstring automation_id;    // AutomationId property (exact or substring)
    std::wstring class_name;       // ClassName property
    int nth = 0;                   // N-th match to select (0 = first)
    int min_score = 1;             // Minimum match score threshold (0-1000)
};

/// Element match result with scoring metadata.
struct ElementMatch {
    int index = 0;                 // Index in the window dump
    int score = 0;               // Match quality score (higher = better)
    Point center{0, 0};          // Clickable center point
    RECT rect{0, 0, 0, 0};       // Bounding rectangle
    std::wstring ref;            // Stable reference ID
    std::wstring name;
    std::wstring control_type;
    std::wstring automation_id;
    bool keyboard_focusable = false;
    bool has_invoke = false;
};

/// Compute a stable reference ID for an element.
/// Format: "{control_type}:{name_hash}:{automation_id_hash}:{nth}"
/// This allows re-targeting elements across sessions even if index changes.
std::wstring compute_element_ref(
    const std::wstring& control_type,
    const std::wstring& name,
    const std::wstring& automation_id,
    int nth = 0);

/// Score an element match against selector criteria.
/// Returns 0 for no match, positive values with higher = better match.
/// Scoring: exact automation_id (500) > exact name (400) > name word-boundary (300)
///          > name prefix (200) > name contains (100) + control_type match bonus.
int score_element_match(
    const std::wstring& control_type,
    const std::wstring& name,
    const std::wstring& automation_id,
    const std::wstring& value,
    const ElementSelector& selector);

} // namespace media::assistant::app_use
