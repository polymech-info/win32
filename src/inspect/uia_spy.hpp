#pragma once

// Foreground UIAutomation focus spy.
// Polls the focused element on a fixed interval, reads all available UIA properties
// and patterns, and invokes a callback on every snapshot.
// Win32 only; requires uiautomationcore.lib.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>   // MIDL_INTERFACE, CoCreateInstance (safe with WIN32_LEAN_AND_MEAN)
// UIAutomationCore.h uses 'interface' as a forward-declaration keyword.
// CLI11 and other cross-platform headers #undef it — restore before including UIA.
#ifndef interface
#  define interface struct __declspec(novtable)
#endif
#include <uiautomation.h>
#include "app_targets.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace media::assistant {

// ── Options ───────────────────────────────────────────────────────────────────
struct SpyOptions {
    int  interval_ms      = 500;    ///< Poll interval in milliseconds.
    bool dump_value       = true;   ///< Read IUIAutomationValuePattern (edit fields, cells…).
    bool dump_selection   = true;   ///< Read TextPattern selection ranges.
    bool dump_full_text   = true;   ///< Read TextPattern document range (whole buffer).
    bool log_unchanged    = false;  ///< Invoke callback even when nothing changed.
    int  text_max_chars   = 4096;   ///< Cap passed to TextRange::GetText (–1 = no cap).

    /// Optional pause flag. When set to true the loop sleeps each tick without
    /// performing any UIA work, resuming immediately when cleared.
    std::shared_ptr<std::atomic<bool>> paused;
};

// ── Snapshot ──────────────────────────────────────────────────────────────────
struct FocusSnapshot {
    // ── Element identity ──────────────────────────────────────────────────────
    DWORD          process_id      = 0;
    std::wstring   process_name;        ///< Base name, e.g. "notepad.exe".
    AppTarget      target;              ///< Classified kind + read hints.
    std::wstring   window_title;        ///< Nearest ancestor Window's accessible name.
    CONTROLTYPEID  control_type_id  = 0;
    std::wstring   control_type_name;   ///< e.g. "Edit", "Document", "Pane".
    std::wstring   class_name;          ///< Win32 class name (HWND class), e.g. "RichEditD2DPT".
    std::wstring   automation_id;       ///< AutomationId property.
    std::wstring   name;                ///< Accessible name (label / aria-label).
    std::wstring   framework_id;        ///< e.g. "Win32", "Chrome", "XAML", "Firefox".
    RECT           bounds           {}; ///< Bounding rectangle in screen coordinates.

    // ── Extracted content ─────────────────────────────────────────────────────
    std::wstring   value_text;          ///< IUIAutomationValuePattern::get_CurrentValue.
    std::wstring   selected_text;       ///< TextPattern selection (all ranges joined).
    std::wstring   full_text;           ///< TextPattern document range (capped at text_max_chars).

    // ── Change detection helpers ──────────────────────────────────────────────
    /// True if the focused element or app identity changed vs. @p prev.
    bool identity_changed_from(const FocusSnapshot& prev) const noexcept;
    /// True if value/selection/fullText changed vs. @p prev.
    bool content_changed_from(const FocusSnapshot& prev) const noexcept;
};

// ── Foreground spy loop ───────────────────────────────────────────────────────
/**
 * Polls the focused element at opts.interval_ms intervals until cancel_fn()
 * returns true (e.g. on Ctrl+C). Blocks the calling thread.
 *
 * on_snapshot is called:
 *   - always when identity changed (different element / process / bounds)
 *   - always when content changed (value / selection / full_text changed)
 *   - additionally on every tick if opts.log_unchanged == true
 *
 * @param opts          Extraction & timing options.
 * @param cancel_fn     Return true to stop the loop.
 * @param on_snapshot   Callback: (snapshot, identity_changed, content_changed).
 */
void run_spy_loop(
    const SpyOptions&                                                          opts,
    std::function<bool()>                                                      cancel_fn,
    std::function<void(const FocusSnapshot&, bool /*identity*/, bool /*content*/)> on_snapshot);

} // namespace media::assistant
