#pragma once
//
// inspect.hpp — interactive window / element picker with live visual feedback.
//
// Spy++ / WinSpy "Finder Tool" style picker for the CLI. The user invokes a pick,
// a translucent highlight follows the cursor over whatever window is underneath,
// and on click (or drag rectangle; Shift/Ctrl still work) we resolve a rich target descriptor.
// Alt+wheel or [ ] cycles the UIA/HWND stack (leaf → siblings → ∪siblings →
// parent → …) with a click-through HUD chip — useful when segments fold into a
// parent, or when sibling panes (Explorer | Center) should be picked together.
// `Z` opens an 8× magnifier for pixel-exact aiming, `C` copies the sampled
// pixel colour. format=selection skips the click and reads the focused
// selection (UIA TextPattern, then Chrome extension / in-app WebView).
// format=foreground snapshots the last focused window as JSON (title, hwnd,
// cursor parent) — remembers the previous app when this process is in front.
// A pick resolves:
//
//   • coords (fallback)            — the precise element / window rectangle
//   • HWND selectors               — hwnd, root hwnd, class names, window title
//   • UIA selectors (when present) — AutomationId, Name, ControlType, FrameworkId,
//                                    ClickablePoint, bounding rect (e.g. Chrome's
//                                    "shorts-player" video element)
//   • process info                 — pid, exe path, command line (best effort)
//
// The descriptor feeds downstream consumers: `video detect`/`video out`/window
// streaming (capture rect or window) and `assistant app-use`/`app-inspect`
// (drive the HWND / UIA element).
//
// Win32 only.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace media::win::inspect {

struct PickResult;

// ── Picker options ────────────────────────────────────────────────────────────
struct PickOptions {
    /// Pass fShowHidden to window_from_point_ex. Default ON — required to descend
    /// into Chrome's (often non-WS_VISIBLE) Chrome_RenderWidgetHostHWND child.
    bool show_hidden = true;

    /// Draw the live translucent highlight that follows the cursor.
    bool highlight = true;

    /// Show the informational HUD chip. Independent from rectangle highlighting.
    bool hud = true;

    /// Resolve the UIA element under the cursor on commit (click). Hover uses a
    /// deferred single-element UIA hit after the cursor settles; Alt+wheel / [ ]
    /// builds the full ancestor stack.
    bool resolve_uia = true;

    /// Best-effort read of the target process command line (needs PROCESS_VM_READ;
    /// fails silently for higher-integrity / protected processes).
    bool resolve_cmdline = true;

    /// Include sibling panes and ∪siblings union entries in the Alt+wheel / [ ]
    /// stack (leaf → siblings → parent). Default ON.
    bool siblings = true;

    /// Start with the magnifier open (8× pixel grid). `Z` toggles it in region mode.
    /// Color mode normally enables this, but callers may disable it for slow GDI/DWM.
    bool magnifier = false;

    /// Eyedropper-only session (set by picker `format=color`): pixel under cursor,
    /// optional loupe, and no region highlight, stack HUD, siblings, or hover UIA.
    bool eyedropper = false;

    /// Region picks: sample once at commit and return colorHex/rgb in the payload.
    bool sample_color = false;

    /// Resolve Chrome DOM/selectors at commit (requires extension).
    bool chrome_dom = false;

    /// Run live Chrome DOM enrichment for the HUD. Commit-time enrichment remains
    /// controlled by chrome_dom; this is off by default because the bridge is blocking.
    bool chrome_preview = false;

    /// Point-to-point measure (`format=measure`): snap clicks to the UIA/Chrome
    /// boundary under the cursor, draw an A→B line, show Δ/dist in the HUD.
    /// Respects resolveUia / chromeDom / siblings like region picks. No loupe by default.
    bool measure = false;

    /// Highlight outline color (0x00RRGGBB) and per-window alpha (0–255).
    COLORREF highlight_color = RGB(0x33, 0xC8, 0xFF);
    BYTE     highlight_alpha = 180;

    /// After a successful commit, before highlight/HUD windows are destroyed.
    /// imageScreenshot uses this so hudCapture=visible can BitBlt the live overlay.
    std::function<void(const PickResult&)> before_teardown;
};

// ── Result ─────────────────────────────────────────────────────────────────────
struct PickResult {
    bool   cancelled = false;   ///< true when the user pressed ESC.

    POINT  point{};             ///< Screen point where the pick was committed.
    int    monitor_idx = 0;     ///< Index (per enumerate order) of the monitor under point.

    // ── HWND selectors ──────────────────────────────────────────────────────
    HWND   hwnd = nullptr;       ///< Finest HWND under cursor (window_from_point_ex).
    HWND   root = nullptr;       ///< GetAncestor(hwnd, GA_ROOT) — the capturable frame.
    RECT   hwnd_rect{};          ///< visible_window_bounds(hwnd), physical px.
    RECT   root_rect{};          ///< visible_window_bounds(root), physical px.
    std::wstring class_name;     ///< Class of hwnd, e.g. "Chrome_RenderWidgetHostHWND".
    std::wstring root_class;     ///< Class of root, e.g. "Chrome_WidgetWin_1".
    std::wstring title;          ///< Root window title.

    // ── Process ─────────────────────────────────────────────────────────────
    DWORD        pid = 0;
    std::wstring exe;            ///< Full image path (QueryFullProcessImageNameW).
    std::wstring cmdline;        ///< Full command line (best effort; may be empty).

    // ── UIA element (resolve_uia) ───────────────────────────────────────────
    bool         uia_ok = false;
    RECT         uia_rect{};            ///< BoundingRectangle of the UIA element.
    std::wstring uia_name;              ///< e.g. "YouTube Video Player".
    std::wstring uia_automation_id;     ///< e.g. "shorts-player" (stable web selector).
    std::wstring uia_control_type;      ///< Localized control type, e.g. "group".
    std::wstring uia_framework_id;      ///< e.g. "Chrome", "Win32", "XAML".
    std::wstring uia_class_name;        ///< UIA ClassName (CSS classes for web).
    bool         uia_has_clickable = false;
    POINT        uia_clickable{};       ///< ClickablePoint, screen px.

    // ── Chosen target rect ──────────────────────────────────────────────────
    /// The precise rectangle this pick represents: the UIA element rect when a
    /// tighter element was resolved, otherwise the HWND/root window rect. This is
    /// the "coords fallback" the capture selector is built from.
    RECT target_rect{};

    /// True when the user dragged a free-form rectangle instead of accepting the
    /// hovered HWND/UIA target (plain drag, or Shift/Ctrl). Also set for measure span.
    bool custom_rect = false;

    // ── Measure (format=measure) ────────────────────────────────────────────
    bool  measure_ok = false;
    POINT measure_a{};       ///< First anchor (screen px, snapped boundary).
    POINT measure_b{};       ///< Second anchor (screen px, snapped boundary).
    RECT  measure_rect_a{};  ///< UIA/HWND target rect at A.
    RECT  measure_rect_b{};  ///< UIA/HWND target rect at B.
    RECT  measure_span{};    ///< Bounding rect of A↔B (≥1×1; encodes the span).

    // ── Eyedropper (sample_color) ───────────────────────────────────────────
    bool     color_ok = false;   ///< true when the pixel under `point` was read.
    COLORREF color    = 0;       ///< COLORREF (0x00BBGGRR) at `point`.
};

/// "#RRGGBB" for a COLORREF. Empty string when @p ok is false.
std::string color_to_hex(COLORREF color, bool ok = true);

/// 1×1 screen rect with top-left at @p pt (points-as-rects encoding).
inline RECT point_as_rect(POINT pt) noexcept {
    return RECT{pt.x, pt.y, pt.x + 1, pt.y + 1};
}

/// Compact wired string for a completed measure pick. Empty when not measure_ok.
std::string measure_to_string(const PickResult& pick);

/// Best-effort speakable text for a committed pick (UIA Text/Value/Name, then title).
bool spoken_text_from_pick(const PickResult& pick,
                           std::wstring& out,
                           std::string& err,
                           int max_chars = 0);

// ── Focused selection (picker format=selection) ─────────────────────────────
// Instant capture — does not run the interactive picker (a click would steal
// focus and drop the selection). UIA GetFocusedElement → TextPattern first;
// Chrome / in-app WebView2 is the fallback when chrome_dom is on.

struct SelectedTextOptions {
    /// Try the Tanit Chrome extension / in-app WebView2 probe after UIA.
    bool chrome_dom = true;
    /// Chrome/WebView bridge timeout. Default 8 s.
    int  timeout_ms = 8000;
    /// Cap on extracted characters. 0 = UIA hard cap (256 KiB).
    int  max_chars = 0;
};

struct SelectedText {
    std::wstring text;
    HWND         hwnd{};
    std::wstring process;         ///< Basename, e.g. "notepad.exe" / "chrome.exe".
    std::wstring control_type;    ///< Localized UIA control type when available.
    std::wstring framework_id;    ///< e.g. "Win32", "Chrome", "XAML".
    enum class Source {
        None,
        UIAutomation,
        ChromeCDP,
    } source{};
    bool password = false;        ///< UIA_IsPasswordPropertyId — never captured.
};

/// Capture the current selection in the keyboard-focused app.
/// Returns false on a hard error (UIA unavailable, password field). Empty
/// selection with no error is `true` and `out.text` empty / `Source::None`.
bool get_selected_text(const SelectedTextOptions& opts,
                       SelectedText& out,
                       std::string& err);

// ── Foreground / last-focus window (picker format=foreground) ───────────────
// Instant snapshot. If this process is in front, returns the last foreign
// foreground window remembered via EVENT_SYSTEM_FOREGROUND.

struct ForegroundWindow {
    HWND         hwnd{};
    HWND         root{};
    HWND         cursor_hwnd{};      ///< Finest HWND under the cursor.
    HWND         cursor_parent{};    ///< GetParent(cursor_hwnd), else root.
    HWND         cursor_root{};
    POINT        cursor{};
    DWORD        pid = 0;
    std::wstring title;
    std::wstring class_name;
    std::wstring process;
    /// "foreground" | "lastFocus" | "zorder" | "none"
    std::string  source = "none";
};

bool get_foreground_window(ForegroundWindow& out, std::string& err);

// ── WinSpy-equivalent: smallest (optionally hidden) HWND under a screen point ──
// Ported from ref/winspy/WindowFromPointEx.c. With show_hidden=true, considers
// non-IsWindowVisible children (lands on Chrome_RenderWidgetHostHWND etc.).
HWND window_from_point_ex(POINT pt, bool show_hidden) noexcept;

// ── Blocking interactive pick ──────────────────────────────────────────────────
// Installs low-level mouse + keyboard hooks and runs a local message loop until
// the user left-clicks (commit) or presses ESC (cancel). The committing click and
// its mouse-up are swallowed so the target app is not actually clicked.
// Returns true on a successful resolve (check out.cancelled for ESC); false on a
// hard error (hooks could not be installed), with @p err set.
bool pick_window(const PickOptions& opts, PickResult& out, std::string& err);

/// Visible picker HUD + highlight HWNDs for an in-app screenshot. Empty when
/// no pick session is running. Idle affinity is WDA_NONE (other-process
/// recorders / Snap see them); our auto/exclude captures hide them for the take.
std::vector<HWND> live_picker_capture_hwnds();

/// True while pick_window's local message loop owns g_ctx.
bool live_picker_active() noexcept;

} // namespace media::win::inspect

#endif // _WIN32
