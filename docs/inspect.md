# Window Inspect / Picker (`src/win/inspect.{hpp,cpp}`)

Status: **Win32 picker implemented** in `src/win/inspect.{hpp,cpp}` — Spy++-style
`pick_window()` with live highlight, UIA enrichment, and xblox `picker` block integration.

**Chrome DOM picking** (Markdown/HTML from page content) is a separate layer — see
[Chrome Element Picker Architecture (`inspect-next-cdp.md`)](./inspect-next-cdp.md).
Flow today: Win32 pick → `picker_enrich` → Tanit Chrome extension → `inspectAtPoint`.

Goal (original request):

1. A lib function that lets the user **pick any window on any monitor** with **live
   visual feedback** (an on-screen highlight following the cursor), like Spy++ /
   WinSpy's "Finder Tool". The feedback should be analogous in spirit to
   `pm_image_cmd_video_detect.cpp --visualize` (a Win32 overlay window).
2. Feed the picked window into:
   - `video detect --input screen:0:wintitle=GLOB` (capture that window), and
   - `media::assistant::app_use` (drive that window: activate / click / type).

Verdict: **feasible**, and most of the moving parts already exist in the tree. The one
genuinely tricky case (landing on a Chrome inner element) is explained under
[Chrome / "show hidden windows"](#chrome--show-hidden-windows).

---

## What already exists (inventory)

### Assistant "spy" code — `src/win/assistant/`
- `uia_spy.{hpp,cpp}` — `run_spy_loop(...)`: polls the **focused** UIA element on an
  interval and emits `FocusSnapshot` (process, control type, class, automationId, name,
  framework, bounds, value/selection/full text). This is a *focus follower*, **not** a
  point-and-click picker.
- `app_inspect.{hpp,cpp}` — `dump_windows(Query)`, `find_element_rect`,
  `save_window_jpeg`, `save_screen_rect_jpeg`, `set_element_value`, etc. `Query` already
  supports `{foreground, pid, hwnd, process_contains, title_contains}`.
- `app_use.{hpp,cpp}` — window resolution + input synthesis:
  - `find_window_ex(FindWindowOptions{pid,hwnd,title_contains,class_name,process_name,foreground})`
    → ranked `FindWindowResult{hwnd,pid,title,class_name,process,score}`.
  - `activate_window(hwnd)` (robust SetForegroundWindow with AttachThreadInput trick),
    `click_point`, `type_text`, `virtual_screen()`, `scale_point_*`.
- `assistant_bar_spy.cpp` — **misleading name**: this is the toolbar's snap/position
  persistence, not a window spy.
- CLI surface lives in `src/cli/pm_image_cmd_assistant.cpp` (`assistant spy`,
  `assistant app-inspect …`, `assistant app-use …`).

### Visualization / overlay — `src/win/detect_overlay.{hpp,cpp}`
- `pm::win::DetectOverlay` already implements exactly the window technique we need for the
  picker highlight:
  - `show(RECT, opts)` → **transparent, click-through, always-on-top** window over a
    screen rect (uses `LWA_COLORKEY`), runs its own UI thread, `update()` is thread-safe.
  - `show_preview(w,h,opts)` → opaque preview window.
- `--visualize` in `pm_image_cmd_video_detect.cpp` decides overlay-vs-preview and, for
  `screen:` inputs, resolves the source rect by matching `wintitle_glob` against
  `ScreenSource::enumerate_windows()` then falls back to the monitor rect.

### Screen capture selector — `src/core/screen_input.{hpp,cpp}`
- Spec format: `screen:<monitorIdx>[:wintitle=GLOB]` (bare `:GLOB` also accepted).
  `parse_screen_spec`, `wintitle_glob_match` (case-insensitive `*`/`?`),
  `ScreenSource::enumerate_windows()` → `{hwnd,title,pid,x,y,w,h}` (visible top-level
  only), `enumerate_monitors()`.
- Window resolution for capture is **title-glob only** today (no class/pid selector).
- Note: enumeration uses a per-monitor-DPI scope so rects are physical pixels.

### Reference: WinSpy Finder Tool — `ref/winspy/`
- `FindTool.c` — `MakeFinderTool(hwndStatic, WNDFINDPROC)`: subclasses a STATIC control;
  on `WM_LBUTTONDOWN` it `SetCapture`s, swaps to a target cursor, installs a
  `WH_KEYBOARD` hook to trap ESC, and on each `WM_MOUSEMOVE` calls
  `WindowFromPointEx(pt, fShowHidden)` and re-draws the selection highlight
  (`ShowSel`/`HideSel`). Fires `WFN_SELCHANGED` / `WFN_END` / `WFN_CANCELLED` callbacks.
- `WindowFromPointEx.c` — better `WindowFromPoint`: walks up to the parent then
  `EnumChildWindows` to pick the **smallest** child rect under the cursor; with
  `fShowHidden==TRUE` it also considers non-`IsWindowVisible` children (key for Chrome).
- `FindToolTrans.c` — `ShowTransWindow(hwnd)`: a `WS_EX_LAYERED|WS_EX_TOOLWINDOW` topmost
  `WS_POPUP` highlight painted over the target's `GetWindowRect` via
  `UpdateLayeredWindow` (the "indicator" the user saw). Fallback is `InvertWindow`
  (XOR border via `InvertRect` on the window DC).
- `fShowHidden` is a global toggled by the `IDC_HIDDEN` checkbox ("show hidden windows").

### Build wiring (`CMakeLists.txt`)
- Assistant sources are listed **twice** (once for `tanit`, once for `tanit-cli`)
  under `if(WIN32 AND FEATURE_ASSISTANT)` (~L1659–1672) and `if(FEATURE_ASSISTANT)`
  (~L3169–3184). Both link `uiautomationcore.lib windowscodecs.lib gdi32 shlwapi`.
- `detect_overlay.cpp` + `screen_input.cpp` are added under the video-detection blocks
  (~L2121, ~L3161) guarded by `FEATURE_VIDEO_DETECTION`/`WIN32`.

---

## Chrome / "show hidden windows"

The screenshot shows the picker landing on a Chrome element reported as
`"YouTube Video Player"`, role `grouping (0x14)`, class `Chrome_RenderWidgetHostHWND`,
"Remote native /Accessible" — and this only worked with **show hidden windows** enabled.

Why:
- Chrome renders almost its entire UI inside **one** HWND
  (`Chrome_RenderWidgetHostHWND`, parented under `Chrome_WidgetWin_1`). That HWND is
  frequently **not** `WS_VISIBLE` in the classic sense, so plain `WindowFromPoint` /
  visible-only enumeration stops at the outer `Chrome_WidgetWin_*` frame.
  `WindowFromPointEx(pt, fShowHidden=TRUE)` descends into that hidden child → you land on
  the render widget HWND.
- The inner label "YouTube Video Player" is **not a window** — it's an
  **accessibility** node (MSAA/UIA) exposed *on* the render-widget HWND. WinSpy reads it
  via the accessibility APIs after picking the HWND.

Consequence for our two integrations:
- **HWND-level picking cannot target a sub-element inside Chrome's web content.** The
  finest HWND you can pick is `Chrome_RenderWidgetHostHWND`.
- For sub-element granularity (e.g. "the video player rect") you must combine the picked
  HWND with **UIA `ElementFromPoint`** (we already use UIA in `uia_spy`/`app_inspect`),
  not just `WindowFromPointEx`.
- For **`video detect` capture**, sub-window rects are moot anyway: GDI capture targets a
  top-level window rect. Resolve the pick to `GetAncestor(hwnd, GA_ROOT)` (the
  `Chrome_WidgetWin_*` frame) and capture that.

---

## Proposed library shape — `src/win/inspect.{hpp,cpp}`

Namespace suggestion: `media::win::inspect` (sibling to existing win libs). Keep it
**dependency-light** (Win32 + GDI only) so it compiles even when `FEATURE_ASSISTANT` is
off; gate UIA-enrichment behind the assistant feature.

```cpp
namespace media::win::inspect {

struct PickResult {
    HWND        hwnd        = nullptr;   // finest HWND under cursor (WindowFromPointEx)
    HWND        root        = nullptr;   // GetAncestor(hwnd, GA_ROOT) — capturable frame
    DWORD       pid         = 0;
    RECT        rect        {};          // GetWindowRect of `hwnd` (physical px)
    RECT        root_rect   {};          // GetWindowRect of `root`
    std::wstring title;                  // root window title
    std::wstring class_name;             // class of `hwnd`
    std::wstring process;                // exe base name
    // optional UIA enrichment (assistant builds): control type / name / automationId
    bool        cancelled   = false;     // true if user pressed ESC
};

struct PickOptions {
    bool show_hidden   = true;   // WindowFromPointEx fShowHidden (default ON — needed for Chrome)
    bool highlight     = true;   // draw the live highlight overlay
    bool resolve_uia   = false;  // also run UIA ElementFromPoint for the inner element
    int  highlight_argb = 0x6633CCFF; // overlay tint
};

// Blocking: runs a hooked pick loop until the user clicks (commit) or presses ESC.
bool pick_window(const PickOptions& opts, PickResult& out, std::string& err);

// WinSpy-equivalent primitive (smallest child under point; show_hidden walks invisibles).
HWND window_from_point_ex(POINT pt, bool show_hidden);

// Highlight overlay you can drive yourself (or reuse pm::win::DetectOverlay).
void highlight_show(const RECT& screen_rect, uint32_t argb);
void highlight_hide();

} // namespace media::win::inspect
```

### Picker mechanics — recommended approach
WinSpy's finder requires a visible STATIC control on a dialog the user drags from. We
have no dialog in the CLI, so prefer a **headless global hook loop** instead:

- Install `WH_MOUSE_LL` (track move, commit on `WM_LBUTTONDOWN`/up) and `WH_KEYBOARD_LL`
  (ESC = cancel). Low-level hooks work across the whole virtual desktop and don't need
  `SetCapture` / a foreground window, so "pick any window on any screen" just works.
- On each move: `pt = cursor`; `hwnd = window_from_point_ex(pt, show_hidden)`; reposition
  the highlight overlay to `GetWindowRect(hwnd)`.
- Pump messages on the calling thread (the LL hooks require a running message loop).
- For the highlight, **reuse `pm::win::DetectOverlay::show(rect)`** (already transparent,
  click-through, topmost, multi-monitor) rather than re-porting `ShowTransWindow`. Add a
  cheap `move(rect)` to `DetectOverlay` (today it has `update(boxes,...)` only).
- DPI: call `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)` (see
  `ThreadDpiPerMonitorScope` in `screen_input.cpp`) so `GetWindowRect` and the overlay
  agree in physical pixels.

Alternative (lower effort, worse UX): a draggable finder bullseye on the existing
`AssistantBar`, mirroring `MakeFinderTool` 1:1. Documented but not preferred.

---

## Integration

### A) `video detect --input screen:…`
- The capture pipeline keys off `screen:<idx>:wintitle=GLOB` (title glob only). After a
  pick, resolve to `root` and synthesize a selector:
  - monitor idx = index of the monitor containing `root_rect` (via `enumerate_monitors`).
  - glob = a title-derived glob, e.g. `*<distinctive title substring>*`. Beware: titles
    are dynamic (YouTube tab title changes); a glob can go stale mid-session.
- **Better**: extend `ScreenSpec` to accept an explicit `hwnd=` (and/or `pid=`/`class=`)
  selector so a pick can be passed losslessly without a fragile title glob. `--visualize`
  overlay rect resolution (in `pm_image_cmd_video_detect.cpp`) must learn the same
  selector so the overlay still lines up.
- Chrome caveat: capture target is the whole `Chrome_WidgetWin_*` frame, not the inner
  video element.

### B) `app_use`
- Easiest, lossless path: hand the picked **HWND** straight to
  `find_window_ex(FindWindowOptions{.hwnd=picked})` and `activate_window(picked_root)`.
  No glob needed. `app_inspect::Query.hwnd` already accepts an HWND too.
- Add an `assistant app-use pick` (and/or `assistant app-inspect pick`) subcommand that
  runs `pick_window`, prints the `PickResult` as JSON (hwnd/pid/title/class/rect), and
  optionally chains into a follow-up action.

### C) Chrome DOM content (xblox + extension)

When UIA only sees coarse nodes (`Chrome_RenderWidgetHostHWND`) but you need page
text/links/structure, use the Chrome bridge in
[`inspect-next-cdp.md`](./inspect-next-cdp.md):

- Install: `npm run install:tanit-chrome` (native host registry + extension path)
- xblox **`picker`** with `format: md` or `format: html` (`chromeDom: true` default)
- CLI: `tanit-cli assistant chrome-inspect at-point --format md …`

Win32 pick supplies screen/client coords; the extension returns DOM for the element at
that point. On bridge failure the block still succeeds with `format=screen` fallback and
`domError` in the payload.

---

## TODO

### Library — `src/win/inspect.{hpp,cpp}`
- [x] `PickResult`, `PickOptions`, `pick_window`, `window_from_point_ex` — see `inspect.hpp`
- [x] Low-level mouse hook pick loop; default `show_hidden = true` (Chrome render widget)
- [x] UIA enrichment optional via `resolve_uia`
- [ ] `highlight_show/hide` as standalone exports (overlay wired internally today)

### Highlight overlay (reuse, don't re-port)
- [x] Live highlight during pick via `highlight_overlay` / `DetectOverlay`
- [ ] `DetectOverlay::move(RECT)` for cheaper per-frame updates

### CLI surface — `src/cli/pm_image_cmd_assistant.cpp`
- [x] `assistant pick` — JSON pick result (see `pm_image_cmd_assistant.cpp`)
- [x] `assistant chrome-inspect ping|status|at-point|selector` — Chrome DOM bridge
- [ ] `--then <click|activate|screenshot>` chain after pick

### `video detect` integration
- [ ] Extend `ScreenSpec` + `parse_screen_spec` to accept `hwnd=<dec>` (and ideally
      `pid=`, `class=`) so a pick is passed losslessly (avoids stale title globs).
- [ ] Teach `ScreenSource` capture + the `--visualize` overlay-rect resolver
      (`pm_image_cmd_video_detect.cpp`) to honor the new `hwnd=`/`pid=` selectors.
- [ ] Helper: `PickResult` → `screen:<idx>:hwnd=<n>` (resolve monitor idx from
      `root_rect` via `enumerate_monitors`).
- [ ] Document Chrome caveat: capture = whole `Chrome_WidgetWin_*` frame.

### `app_use` integration
- [ ] Confirm `find_window_ex({.hwnd=picked})` + `activate_window(root)` round-trips a
      picked HWND (should already work — just expose via CLI).

### Build / CMake
- [x] `inspect.cpp` in assistant builds; xblox `picker` block in `app_blocks.cpp`
- [x] `chrome_inspect/bridge.cpp`, `picker_enrich.cpp` linked into `tanit` / `tanit-cli`
- [ ] Lossless `hwnd=` in `ScreenSpec` for video detect (still title-glob oriented)

### Open questions
- [x] Chrome sub-element content — **DOM path** via extension (see `inspect-next-cdp.md`)
- [ ] Lossless `hwnd=` selectors for `video detect` vs title globs
- [ ] In-page extension overlay vs Win32-only pick (overlay not built yet)
```