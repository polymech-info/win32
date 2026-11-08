#pragma once
// Win32 clipboard snapshot helpers for OS context + the launcher Clipboard tab.
// Formats are the CF_* constants from <winuser.h>, plus registered PNG.

#include "win/context/context_store.hpp"

#if defined(_WIN32)

#include <optional>
#include <string>
#include <vector>

namespace media::win::context {

/// CF_* / registered-format token (`CF_UNICODETEXT`, `PNG`, …).
std::wstring clipboard_format_name(UINT format);

/// Row title for a format (`Image`, `Text`, `HTML Format`). Never `CF_DIB`.
std::wstring clipboard_format_display(UINT format);

/// Read the current clipboard. Prefers OLE shell file objects
/// (`IShellItem` / `CFSTR_SHELLIDLIST`) so Explorer copies match Recent.
/// Empty optional when OpenClipboard / OleGetClipboard fails or there is
/// nothing usable. Does not take ContextStore locks.
std::optional<ClipboardHistoryItem> capture_clipboard_snapshot();

/// Stretch a CF_DIB payload to a square ARGB thumb. Caller owns the HBITMAP.
HBITMAP clipboard_dib_thumb(const std::vector<uint8_t>& dib, int dim);

/// Write image / text / audio bytes to a temp file so commands can use
/// `${CURRENT_FILE}`. Files kind already has paths. Returns false when nothing
/// usable can be written.
bool materialize_clipboard_item(ClipboardHistoryItem& item, std::wstring& path);

/// Put a history snapshot back on the OS clipboard (files as shell items /
/// CF_HDROP, text/image/audio/raw via SetClipboardData). Does not touch
/// ContextStore. False when nothing usable can be written.
bool restore_clipboard_snapshot(const ClipboardHistoryItem& item);

const wchar_t* clipboard_kind_name(ClipboardPayloadKind kind);

} // namespace media::win::context

#endif
