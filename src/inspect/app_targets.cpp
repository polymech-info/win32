#include "app_targets.hpp"
#include <algorithm>
#include <cctype>

namespace media::assistant {

namespace {

// Extract and lowercase the base file name, stripping directory and .exe/.bin.
std::wstring base_lower(std::wstring_view path) {
    // Strip directory
    auto p = path.rfind(L'\\');
    if (p == std::wstring_view::npos) p = path.rfind(L'/');
    std::wstring_view file = (p != std::wstring_view::npos) ? path.substr(p + 1) : path;

    std::wstring out;
    out.reserve(file.size());
    for (wchar_t c : file)
        out.push_back(static_cast<wchar_t>(std::tolower(static_cast<int>(c))));

    // Strip common executable suffixes
    for (const std::wstring_view sfx : { std::wstring_view(L".exe"), std::wstring_view(L".bin") }) {
        if (out.size() > sfx.size() && out.compare(out.size() - sfx.size(), sfx.size(), sfx) == 0) {
            out.resize(out.size() - sfx.size());
            break;
        }
    }
    return out;
}

} // namespace

AppTarget classify_process(std::wstring_view proc_name) {
    const std::wstring name = base_lower(proc_name);

    if (name == L"notepad") {
        // Win10 Notepad: RichEditD2DPT class.
        // ValuePattern: whole buffer. TextPattern: selection + document range. Both reliable.
        return { AppKind::Notepad, "Notepad", { true, true, false } };
    }
    if (name == L"soffice") {
        // LibreOffice (Writer/Calc/Impress): SALFRAME container.
        // Writer has TextPattern. Calc cells have ValuePattern.
        // Clipboard most reliable across all components.
        return { AppKind::LibreOffice, "LibreOffice", { true, true, true } };
    }
    if (name == L"chrome") {
        // Google Chrome: framework_id "Chrome". Address bar is an Edit with ValuePattern.
        // Content-editable areas respond to TextPattern but clipboard is safer.
        return { AppKind::Chrome, "Chrome", { true, true, true } };
    }
    if (name == L"firefox") {
        // Mozilla Firefox: limited UIA coverage; clipboard usually wins.
        return { AppKind::Firefox, "Firefox", { false, true, true } };
    }
    if (name == L"msedge") {
        // Microsoft Edge (Chromium): same UIA coverage as Chrome.
        return { AppKind::Edge, "Edge", { true, true, true } };
    }
    if (name == L"code" || name == L"code - insiders") {
        // VSCode (Electron/Chromium): framework_id "Chrome".
        // Editor text areas surface TextPattern via Monaco a11y bridge.
        return { AppKind::Electron, "VSCode/Electron", { true, true, false } };
    }
    if (name == L"cursor") {
        // Cursor IDE (Electron/Chromium).
        return { AppKind::Electron, "Cursor IDE/Electron", { true, true, false } };
    }
    if (name == L"slack" || name == L"discord" || name == L"teams") {
        // Common Electron messaging apps.
        return { AppKind::Electron, "Electron app", { true, true, false } };
    }
    if (name == L"winword") {
        // Microsoft Word: COM object model is ideal; UIA TextPattern present but fragile.
        return { AppKind::Word, "Microsoft Word", { false, true, true } };
    }
    if (name == L"excel") {
        // Microsoft Excel: cells expose ValuePattern. Clipboard for ranges.
        return { AppKind::Excel, "Microsoft Excel", { true, false, true } };
    }

    return { AppKind::Unknown, "Unknown", { true, true, false } };
}

const char* app_kind_label(AppKind k) noexcept {
    switch (k) {
    case AppKind::Notepad:     return "Notepad";
    case AppKind::LibreOffice: return "LibreOffice";
    case AppKind::Chrome:      return "Chrome";
    case AppKind::Firefox:     return "Firefox";
    case AppKind::Edge:        return "Edge";
    case AppKind::Electron:    return "Electron";
    case AppKind::Word:        return "Microsoft Word";
    case AppKind::Excel:       return "Microsoft Excel";
    default:                   return "Unknown";
    }
}

} // namespace media::assistant
