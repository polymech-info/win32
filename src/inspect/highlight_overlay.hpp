#pragma once
//
// highlight_overlay.hpp — a translucent, click-through, always-on-top rectangle
// outline that can be moved around the screen. A reusable "rect painter".
//
// Used by the window/element picker (packages/win32/src/inspect/inspect.cpp) to follow the cursor,
// but it is intentionally standalone so other features can reuse it to flash /
// frame a region (e.g. show a capture target, highlight a detected box, mark a
// drop zone). Single-threaded: create / move / hide / destroy from the thread
// that owns a message loop.
//
// Win32 only.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace media::win {

struct HighlightOverlayOptions {
    COLORREF color     = RGB(0x33, 0xC8, 0xFF); ///< Outline color (0x00RRGGBB).
    BYTE     alpha     = 180;                    ///< Whole-window opacity 0–255.
    int      thickness = 3;                      ///< Border thickness in px.
    bool     filled    = false;                  ///< true = solid translucent fill, false = outline only.
};

class HighlightOverlay {
public:
    using Options = HighlightOverlayOptions;

    HighlightOverlay() = default;
    ~HighlightOverlay();

    HighlightOverlay(const HighlightOverlay&)            = delete;
    HighlightOverlay& operator=(const HighlightOverlay&) = delete;

    /// Create the (initially hidden) overlay window. Returns false on failure.
    bool create(const Options& opts);
    bool create() { return create(Options{}); }  // default-styled overlay

    /// Reposition over @p screen_rect (physical px) and show. Redundant moves to
    /// the same rectangle are ignored. No-op if the rect is empty.
    void move_to(const RECT& screen_rect);

    /// Hide without destroying (cheap to show again via move_to).
    void hide();

    /// Destroy the window. Safe to call multiple times; called by the destructor.
    void destroy();

    HWND           hwnd() const noexcept { return hwnd_; }
    bool           valid() const noexcept { return hwnd_ != nullptr; }
    const Options& options() const noexcept { return opts_; }

private:
    void apply_region(int w, int h);

    HWND    hwnd_       = nullptr;
    Options opts_;
    RECT    last_rect_{};
    bool    last_valid_ = false;
};

/// Click-through overlay that draws a straight line between two screen points.
class LineOverlay {
public:
    struct Options {
        COLORREF color     = RGB(0xFF, 0xA5, 0x33);
        BYTE     alpha     = 220;
        int      thickness = 2;
    };

    LineOverlay() = default;
    ~LineOverlay();

    LineOverlay(const LineOverlay&)            = delete;
    LineOverlay& operator=(const LineOverlay&) = delete;

    bool create(const Options& opts);
    bool create() { return create(Options{}); }

    /// Draw a line from @p a to @p b (screen px) and show.
    void move_to(POINT a, POINT b);

    void hide();
    void destroy();

    HWND           hwnd() const noexcept { return hwnd_; }
    bool           valid() const noexcept { return hwnd_ != nullptr; }
    const Options& options() const noexcept { return opts_; }

private:
    friend LRESULT CALLBACK line_overlay_proc(HWND, UINT, WPARAM, LPARAM);

    HWND    hwnd_ = nullptr;
    Options opts_;
    POINT   pt_a_{};
    POINT   pt_b_{};
    bool    has_line_ = false;
};

/// Full-screen measure stage: dim scrim after anchor A, span rect, H/V guides, target rects.
class MeasureOverlay {
public:
    struct Options {
        COLORREF accent    = RGB(0xFF, 0xA5, 0x33);
        COLORREF dim       = RGB(0x00, 0x00, 0x00);
        BYTE     dim_alpha = 120;
        int      thickness = 2;
    };

    MeasureOverlay() = default;
    ~MeasureOverlay();

    MeasureOverlay(const MeasureOverlay&)            = delete;
    MeasureOverlay& operator=(const MeasureOverlay&) = delete;

    bool create(const Options& opts);
    bool create() { return create(Options{}); }

    /// @p active false hides. When true, draws dim scrim plus optional anchor/preview rects
    /// and span/guides between @p a and @p b (screen px).
    void update(bool active,
                POINT a,
                POINT b,
                const RECT* rect_a,
                const RECT* rect_b,
                bool preview);

    void hide();
    void destroy();

    HWND           hwnd() const noexcept { return hwnd_; }
    bool           valid() const noexcept { return hwnd_ != nullptr; }
    const Options& options() const noexcept { return opts_; }

private:
    friend LRESULT CALLBACK measure_overlay_proc(HWND, UINT, WPARAM, LPARAM);

    void paint(HDC hdc, const RECT& client);

    HWND    hwnd_ = nullptr;
    Options opts_;
    bool    active_    = false;
    bool    preview_   = false;
    POINT   pt_a_{};
    POINT   pt_b_{};
    RECT    rect_a_{};
    RECT    rect_b_{};
    bool    has_rect_a_ = false;
    bool    has_rect_b_ = false;
};

/// Virtual desktop bounds (multi-monitor), physical pixels.
RECT virtual_screen_rect() noexcept;

} // namespace media::win

#endif // _WIN32
