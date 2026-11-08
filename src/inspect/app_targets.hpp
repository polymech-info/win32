#pragma once

// App target classification for the UIAutomation assistant / spy PoC.
// Win32-only; all types/functions live in namespace media::assistant.

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <string_view>

namespace media::assistant {

// ── Known application families ───────────────────────────────────────────────
enum class AppKind : int {
    Unknown,
    Notepad,      // notepad.exe  — Win10 RichEdit2D + UIA ValuePattern + TextPattern
    LibreOffice,  // soffice.bin  — SALFRAME, limited UIA; clipboard fallback best
    Chrome,       // chrome.exe   — framework "Chrome"; TextPattern + clipboard
    Firefox,      // firefox.exe  — framework "Firefox"
    Edge,         // msedge.exe   — Chromium; TextPattern + clipboard
    Electron,     // code.exe etc — Electron (Chromium-based)
    Word,         // winword.exe  — COM object model preferred; clipboard fallback
    Excel,        // excel.exe    — COM/clipboard
};

// ── Per-app read strategy hints ───────────────────────────────────────────────
struct ReadHints {
    bool try_value_pattern  = true;   // IUIAutomationValuePattern
    bool try_text_pattern   = true;   // IUIAutomationTextPattern
    bool try_clipboard_copy = false;  // Ctrl+C → clipboard (fallback for apps with poor UIA)
    // Special Notepad notes: Win10 notepad uses RichEditD2DPT; ValuePattern gives full
    // buffer, TextPattern gives selection + document range. Both reliable.
    // LibreOffice: SALFRAME document elements expose TextPattern in Writer but content
    // extraction via clipboard (Ctrl+C on selection) is more reliable cross-suite.
    // Chrome/Edge/Firefox: TextPattern is available on focused address bar + input fields;
    // ContentEditable divs often need clipboard. framework_id == "Chrome"/"Firefox" etc.
};

// ── Classified target ─────────────────────────────────────────────────────────
struct AppTarget {
    AppKind     kind  = AppKind::Unknown;
    const char* label = "Unknown";
    ReadHints   hints;
};

// Classify by process base name (case-insensitive; .exe suffix optional; full path ok).
AppTarget classify_process(std::wstring_view proc_name);

// Human-readable label for AppKind.
const char* app_kind_label(AppKind k) noexcept;

} // namespace media::assistant
