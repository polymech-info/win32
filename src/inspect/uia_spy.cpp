#include "uia_spy.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#ifndef interface
#  define interface struct __declspec(novtable)
#endif
#include <uiautomation.h>
#include <processthreadsapi.h>

// uiautomationcore.lib is added by CMakeLists.txt via target_link_libraries.
// #pragma comment kept as belt-and-suspenders for MSVC unity builds.
#pragma comment(lib, "uiautomationcore.lib")

#include <algorithm>
#include <string>
#include <utility>

namespace media::assistant {

// ════════════════════════════════════════════════════════════════════════════
// RAII helpers
// ════════════════════════════════════════════════════════════════════════════

namespace {

// Thin scope-exit guard for BSTR allocations.
struct BstrGuard {
    BSTR b = nullptr;
    ~BstrGuard() { if (b) { ::SysFreeString(b); b = nullptr; } }
    BSTR* operator&() noexcept { return &b; }
    std::wstring str() const { return b ? std::wstring(b, ::SysStringLen(b)) : std::wstring{}; }
    bool empty() const noexcept { return !b || b[0] == L'\0'; }
};

// Thin scope-exit guard for IUnknown-derived COM interfaces.
template<typename T>
struct ComGuard {
    T* p = nullptr;
    explicit ComGuard(T* ptr = nullptr) noexcept : p(ptr) {}
    ~ComGuard() { reset(); }
    ComGuard(const ComGuard&) = delete;
    ComGuard& operator=(const ComGuard&) = delete;
    void reset() noexcept { if (p) { p->Release(); p = nullptr; } }
    T** addr() noexcept { return &p; }
    T*  operator->() const noexcept { return p; }
    explicit operator bool() const noexcept { return p != nullptr; }
};

// ════════════════════════════════════════════════════════════════════════════
// UIA control-type names
// ════════════════════════════════════════════════════════════════════════════

const wchar_t* ctrl_type_name(CONTROLTYPEID id) noexcept {
    switch (id) {
    case UIA_ButtonControlTypeId:       return L"Button";
    case UIA_CalendarControlTypeId:     return L"Calendar";
    case UIA_CheckBoxControlTypeId:     return L"CheckBox";
    case UIA_ComboBoxControlTypeId:     return L"ComboBox";
    case UIA_EditControlTypeId:         return L"Edit";
    case UIA_HyperlinkControlTypeId:    return L"Hyperlink";
    case UIA_ImageControlTypeId:        return L"Image";
    case UIA_ListItemControlTypeId:     return L"ListItem";
    case UIA_ListControlTypeId:         return L"List";
    case UIA_MenuControlTypeId:         return L"Menu";
    case UIA_MenuBarControlTypeId:      return L"MenuBar";
    case UIA_MenuItemControlTypeId:     return L"MenuItem";
    case UIA_ProgressBarControlTypeId:  return L"ProgressBar";
    case UIA_RadioButtonControlTypeId:  return L"RadioButton";
    case UIA_ScrollBarControlTypeId:    return L"ScrollBar";
    case UIA_SliderControlTypeId:       return L"Slider";
    case UIA_SpinnerControlTypeId:      return L"Spinner";
    case UIA_StatusBarControlTypeId:    return L"StatusBar";
    case UIA_TabControlTypeId:          return L"Tab";
    case UIA_TabItemControlTypeId:      return L"TabItem";
    case UIA_TextControlTypeId:         return L"Text";
    case UIA_ToolBarControlTypeId:      return L"ToolBar";
    case UIA_ToolTipControlTypeId:      return L"ToolTip";
    case UIA_TreeControlTypeId:         return L"Tree";
    case UIA_TreeItemControlTypeId:     return L"TreeItem";
    case UIA_CustomControlTypeId:       return L"Custom";
    case UIA_GroupControlTypeId:        return L"Group";
    case UIA_ThumbControlTypeId:        return L"Thumb";
    case UIA_DataGridControlTypeId:     return L"DataGrid";
    case UIA_DataItemControlTypeId:     return L"DataItem";
    case UIA_DocumentControlTypeId:     return L"Document";
    case UIA_SplitButtonControlTypeId:  return L"SplitButton";
    case UIA_WindowControlTypeId:       return L"Window";
    case UIA_PaneControlTypeId:         return L"Pane";
    case UIA_HeaderControlTypeId:       return L"Header";
    case UIA_HeaderItemControlTypeId:   return L"HeaderItem";
    case UIA_TableControlTypeId:        return L"Table";
    case UIA_TitleBarControlTypeId:     return L"TitleBar";
    case UIA_SeparatorControlTypeId:    return L"Separator";
    case UIA_SemanticZoomControlTypeId: return L"SemanticZoom";
    case UIA_AppBarControlTypeId:       return L"AppBar";
    default:                            return L"Unknown";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Helper: process name from PID
// ════════════════════════════════════════════════════════════════════════════

std::wstring process_name_from_pid(DWORD pid) {
    if (pid == 0) return {};
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD   sz = static_cast<DWORD>(std::size(buf));
    const BOOL ok = ::QueryFullProcessImageNameW(h, 0, buf, &sz);
    ::CloseHandle(h);
    if (!ok || sz == 0) return {};
    // Extract basename
    std::wstring full(buf, static_cast<size_t>(sz));
    const auto slash = full.rfind(L'\\');
    return (slash != std::wstring::npos) ? full.substr(slash + 1) : full;
}

// ════════════════════════════════════════════════════════════════════════════
// Helper: walk parent chain to return the nearest Window UIA element (AddRef'd)
// Returns nullptr if not found. Caller must Release().
// ════════════════════════════════════════════════════════════════════════════

IUIAutomationElement* find_ancestor_window_elem(IUIAutomationElement*    elem,
                                                IUIAutomationTreeWalker* walker) {
    if (!elem || !walker) return nullptr;

    CONTROLTYPEID ct = 0;
    elem->get_CurrentControlType(&ct);
    if (ct == UIA_WindowControlTypeId) { elem->AddRef(); return elem; }

    elem->AddRef();
    IUIAutomationElement* cur = elem;

    for (int depth = 0; depth < 32; ++depth) {
        IUIAutomationElement* parent = nullptr;
        const HRESULT hr = walker->GetParentElement(cur, &parent);
        cur->Release();
        cur = nullptr;
        if (FAILED(hr) || !parent) break;

        CONTROLTYPEID pct = 0;
        parent->get_CurrentControlType(&pct);
        if (pct == UIA_WindowControlTypeId) return parent; // caller owns ref
        cur = parent;
    }
    if (cur) cur->Release();
    return nullptr;
}

// ════════════════════════════════════════════════════════════════════════════
// Helper: walk parent chain to find the nearest Window element title
// ════════════════════════════════════════════════════════════════════════════

std::wstring ancestor_window_title(IUIAutomationElement*    elem,
                                   IUIAutomationTreeWalker* walker) {
    ComGuard<IUIAutomationElement> win(find_ancestor_window_elem(elem, walker));
    if (!win) return {};
    BstrGuard n;
    if (SUCCEEDED(win->get_CurrentName(&n))) return n.str();
    return {};
}

// ════════════════════════════════════════════════════════════════════════════
// Helper: LibreOffice Calc formula bar ("Input Line") value.
// Cells report null IAccessible value; the content lives in the formula bar
// which is a separate UIA element in the window's descendant tree.
// Tries the element names LibreOffice has used across versions.
// ════════════════════════════════════════════════════════════════════════════

std::wstring find_lo_formula_bar_value(IUIAutomation*        uia,
                                       IUIAutomationElement* window_elem) {
    if (!uia || !window_elem) return {};

    static const wchar_t* k_names[] = { L"Input Line", L"Formula Bar", nullptr };

    for (const wchar_t** pn = k_names; *pn; ++pn) {
        VARIANT vName;
        ::VariantInit(&vName);
        vName.vt      = VT_BSTR;
        vName.bstrVal = ::SysAllocString(*pn);

        IUIAutomationCondition* rawCond = nullptr;
        HRESULT hr = uia->CreatePropertyCondition(UIA_NamePropertyId, vName, &rawCond);
        ::VariantClear(&vName);
        if (FAILED(hr) || !rawCond) continue;
        ComGuard<IUIAutomationCondition> cond(rawCond);

        IUIAutomationElement* rawEl = nullptr;
        hr = window_elem->FindFirst(TreeScope_Descendants, rawCond, &rawEl);
        if (FAILED(hr) || !rawEl) continue;
        ComGuard<IUIAutomationElement> el(rawEl);

        // ValuePattern (native UIA Edit)
        {
            IUIAutomationValuePattern* rawvp = nullptr;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId,
                    __uuidof(IUIAutomationValuePattern),
                    reinterpret_cast<void**>(&rawvp))) && rawvp) {
                ComGuard<IUIAutomationValuePattern> vp(rawvp);
                BstrGuard val;
                if (SUCCEEDED(vp->get_CurrentValue(&val)) && !val.empty())
                    return val.str();
            }
        }
        // LegacyIAccessiblePattern fallback
        {
            IUIAutomationLegacyIAccessiblePattern* rawlp = nullptr;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId,
                    __uuidof(IUIAutomationLegacyIAccessiblePattern),
                    reinterpret_cast<void**>(&rawlp))) && rawlp) {
                ComGuard<IUIAutomationLegacyIAccessiblePattern> lp(rawlp);
                BstrGuard val;
                if (SUCCEEDED(lp->get_CurrentValue(&val)) && !val.empty())
                    return val.str();
            }
        }
    }
    return {};
}

// ════════════════════════════════════════════════════════════════════════════
// Helper: read one complete snapshot from the currently focused element
// ════════════════════════════════════════════════════════════════════════════

FocusSnapshot read_snapshot(IUIAutomation*          uia,
                            IUIAutomationElement*   elem,
                            IUIAutomationTreeWalker* walker,
                            const SpyOptions&        opts)
{
    FocusSnapshot s;
    if (!elem) return s;

    // ── Process info ──────────────────────────────────────────────────────────
    int _pid = 0;
    elem->get_CurrentProcessId(&_pid);
    s.process_id = static_cast<DWORD>(_pid);
    s.process_name = process_name_from_pid(s.process_id);
    s.target       = classify_process(s.process_name);

    // ── Window title (nearest ancestor of type Window) ────────────────────────
    s.window_title = ancestor_window_title(elem, walker);

    // ── Control type ──────────────────────────────────────────────────────────
    CONTROLTYPEID ct = 0;
    elem->get_CurrentControlType(&ct);
    s.control_type_id   = ct;
    s.control_type_name = ctrl_type_name(ct);

    // ── String identity properties ────────────────────────────────────────────
    { BstrGuard b; if (SUCCEEDED(elem->get_CurrentClassName(&b)))    s.class_name    = b.str(); }
    { BstrGuard b; if (SUCCEEDED(elem->get_CurrentAutomationId(&b))) s.automation_id = b.str(); }
    { BstrGuard b; if (SUCCEEDED(elem->get_CurrentName(&b)))         s.name          = b.str(); }
    { BstrGuard b; if (SUCCEEDED(elem->get_CurrentFrameworkId(&b)))  s.framework_id  = b.str(); }

    // ── Bounding rectangle (screen coordinates) ───────────────────────────────
    elem->get_CurrentBoundingRectangle(&s.bounds);

    // ── IUIAutomationValuePattern ─────────────────────────────────────────────
    // Present on: single-line Edit, ComboBox, spreadsheet cells, etc.
    if (opts.dump_value) {
        IUIAutomationValuePattern* rawvp = nullptr;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern),
                reinterpret_cast<void**>(&rawvp))) && rawvp) {
            ComGuard<IUIAutomationValuePattern> vp(rawvp);
            BstrGuard val;
            if (SUCCEEDED(vp->get_CurrentValue(&val))) s.value_text = val.str();
        }
    }

    // ── IUIAutomationTextPattern ──────────────────────────────────────────────
    // Present on: multi-line Edit, Document, RichEdit, browser content areas.
    if (opts.dump_selection || opts.dump_full_text) {
        IUIAutomationTextPattern* rawtp = nullptr;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                reinterpret_cast<void**>(&rawtp))) && rawtp) {
            ComGuard<IUIAutomationTextPattern> tp(rawtp);

            // Current selection (may span multiple non-contiguous ranges)
            if (opts.dump_selection) {
                IUIAutomationTextRangeArray* rawsel = nullptr;
                if (SUCCEEDED(tp->GetSelection(&rawsel)) && rawsel) {
                    ComGuard<IUIAutomationTextRangeArray> sel(rawsel);
                    int count = 0;
                    sel->get_Length(&count);
                    for (int i = 0; i < count; ++i) {
                        IUIAutomationTextRange* rawrange = nullptr;
                        if (SUCCEEDED(sel->GetElement(i, &rawrange)) && rawrange) {
                            ComGuard<IUIAutomationTextRange> range(rawrange);
                            BstrGuard txt;
                            if (SUCCEEDED(range->GetText(-1, &txt))) {
                                if (!s.selected_text.empty()) s.selected_text += L"\n---\n";
                                s.selected_text += txt.str();
                            }
                        }
                    }
                }
            }

            // Full document range (capped)
            if (opts.dump_full_text) {
                IUIAutomationTextRange* rawdoc = nullptr;
                if (SUCCEEDED(tp->get_DocumentRange(&rawdoc)) && rawdoc) {
                    ComGuard<IUIAutomationTextRange> doc(rawdoc);
                    BstrGuard txt;
                    if (SUCCEEDED(doc->GetText(opts.text_max_chars, &txt)))
                        s.full_text = txt.str();
                }
            }
        }
    }

    // ── IUIAutomationLegacyIAccessiblePattern ─────────────────────────────────
    // Fallback for apps that surface IAccessible but not native UIA patterns,
    // e.g. LibreOffice (SALFRAME).  The UIA bridge wraps IAccessible::get_accValue
    // here, giving us the field content even when ValuePattern/TextPattern are absent.
    DWORD legacy_role = 0;
    if (s.value_text.empty() || s.window_title.empty()) {
        IUIAutomationLegacyIAccessiblePattern* rawlp = nullptr;
        if (SUCCEEDED(elem->GetCurrentPatternAs(
                UIA_LegacyIAccessiblePatternId,
                __uuidof(IUIAutomationLegacyIAccessiblePattern),
                reinterpret_cast<void**>(&rawlp))) && rawlp) {
            ComGuard<IUIAutomationLegacyIAccessiblePattern> lp(rawlp);

            lp->get_CurrentRole(&legacy_role);

            // Value (IAccessible::get_accValue)
            if (s.value_text.empty()) {
                BstrGuard val;
                if (SUCCEEDED(lp->get_CurrentValue(&val)) && !val.empty())
                    s.value_text = val.str();
            }

            // Window title via the IAccessible name of the top-level window.
            // LibreOffice's UIA bridge may not map role=window → WindowControlType,
            // so our tree-walker misses it; IAccessible::get_accName on the element
            // often contains the document title.
            if (s.window_title.empty()) {
                BstrGuard nm;
                if (SUCCEEDED(lp->get_CurrentName(&nm)) && !nm.empty())
                    s.window_title = nm.str();
            }
        }
    }

    // ── LibreOffice Calc formula bar ──────────────────────────────────────────
    // Cells (IAccessible role 0x1D = ROLE_SYSTEM_CELL) report null IAccessible
    // value; the cell content lives in the "Input Line" formula bar — a sibling
    // widget in the window's UIA subtree.  Walk up to the window and search for it.
    if (s.value_text.empty()
        && s.target.kind == AppKind::LibreOffice
        && legacy_role == 0x1DUL) {
        ComGuard<IUIAutomationElement> win_elem(
            find_ancestor_window_elem(elem, walker));
        if (win_elem) {
            std::wstring fb = find_lo_formula_bar_value(uia, win_elem.p);
            if (!fb.empty())
                s.value_text = L"\u2192 " + fb; // → prefix distinguishes formula-bar source
        }
    }

    return s;
}

} // namespace (anonymous)

// ════════════════════════════════════════════════════════════════════════════
// FocusSnapshot — change detection
// ════════════════════════════════════════════════════════════════════════════

bool FocusSnapshot::identity_changed_from(const FocusSnapshot& prev) const noexcept {
    return process_id       != prev.process_id
        || control_type_id  != prev.control_type_id
        || class_name       != prev.class_name
        || automation_id    != prev.automation_id
        || bounds.left      != prev.bounds.left
        || bounds.top       != prev.bounds.top
        || bounds.right     != prev.bounds.right
        || bounds.bottom    != prev.bounds.bottom;
}

bool FocusSnapshot::content_changed_from(const FocusSnapshot& prev) const noexcept {
    return value_text    != prev.value_text
        || selected_text != prev.selected_text
        || full_text     != prev.full_text;
}

// ════════════════════════════════════════════════════════════════════════════
// run_spy_loop — main entry point
// ════════════════════════════════════════════════════════════════════════════

void run_spy_loop(
    const SpyOptions&                                                              opts,
    std::function<bool()>                                                          cancel_fn,
    std::function<void(const FocusSnapshot&, bool, bool)>                         on_snapshot)
{
    // COM apartment: APARTMENTTHREADED is safest for UIA on Win32.
    const HRESULT com_hr   = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool    com_init = SUCCEEDED(com_hr) || com_hr == RPC_E_CHANGED_MODE;

    // Prefer CUIAutomation8 (Win8+) for IUIAutomation2 support; fall back to CUIAutomation.
    IUIAutomation* raw_uia = nullptr;
    HRESULT hr = ::CoCreateInstance(
        __uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(&raw_uia));
    if (FAILED(hr) || !raw_uia)
        hr = ::CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&raw_uia));

    if (FAILED(hr) || !raw_uia) {
        if (com_init) ::CoUninitialize();
        return; // CoCreateInstance failed — UIA not available
    }
    ComGuard<IUIAutomation> uia(raw_uia);

    // RawViewWalker traverses all elements (no condition filtering) — used for parent walk.
    IUIAutomationTreeWalker* raw_walker = nullptr;
    uia->get_RawViewWalker(&raw_walker);
    ComGuard<IUIAutomationTreeWalker> walker(raw_walker);

    FocusSnapshot prev;
    bool          first = true;

    while (!cancel_fn()) {
        // Yield without UIA work while the spy is paused by the toolbar toggle.
        if (opts.paused && opts.paused->load()) {
            ::Sleep(50);
            continue;
        }

        IUIAutomationElement* raw_elem = nullptr;
        hr = uia->GetFocusedElement(&raw_elem);

        if (SUCCEEDED(hr) && raw_elem) {
            ComGuard<IUIAutomationElement> elem(raw_elem);
            FocusSnapshot snap = read_snapshot(uia.p, elem.p, walker.p, opts);

            const bool id_changed  = first || snap.identity_changed_from(prev);
            const bool cnt_changed = first || snap.content_changed_from(prev);

            if (id_changed || cnt_changed || opts.log_unchanged) {
                on_snapshot(snap, id_changed, cnt_changed);
            }

            prev  = std::move(snap);
            first = false;
        }

        // Sleep in 50 ms slices so cancel_fn() is checked promptly.
        const int slices = std::max(1, opts.interval_ms / 50);
        for (int i = 0; i < slices && !cancel_fn(); ++i)
            ::Sleep(50);
    }
}

} // namespace media::assistant
