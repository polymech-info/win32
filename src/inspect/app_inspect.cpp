#include "app_inspect.hpp"
#include "logging/logger.h"
#include "process/process_helpers.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#ifndef interface
#  define interface struct __declspec(novtable)
#endif
#include <uiautomation.h>
#include <wincodec.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <set>
#include <sstream>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace media {
namespace assistant {
namespace app_inspect {
namespace {

struct ComInit {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok() const noexcept { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    ~ComInit() { if (SUCCEEDED(hr)) ::CoUninitialize(); }
};

template<typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { reset(); }
    ComPtr() = default;
    explicit ComPtr(T* v) : p(v) {}
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** addr() noexcept { reset(); return &p; }
    T* operator->() const noexcept { return p; }
    explicit operator bool() const noexcept { return p != nullptr; }
    void reset() noexcept { if (p) { p->Release(); p = nullptr; } }
};

struct Bstr {
    BSTR b = nullptr;
    ~Bstr() { if (b) ::SysFreeString(b); }
    BSTR* addr() noexcept { return &b; }
    std::wstring str() const { return b ? std::wstring(b, ::SysStringLen(b)) : std::wstring{}; }
};

std::string hresult_error(const char* where, HRESULT hr) {
    std::ostringstream oss;
    oss << where << " failed: 0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

bool contains_ci(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return lower(haystack).find(lower(needle)) != std::wstring::npos;
}

std::wstring process_name_from_pid(DWORD pid) {
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

Rect rect_from(RECT r) {
    const LONG w = r.right > r.left ? r.right - r.left : 0;
    const LONG h = r.bottom > r.top ? r.bottom - r.top : 0;
    return Rect{r.left, r.top, static_cast<int>(w), static_cast<int>(h)};
}

const wchar_t* control_type_name(CONTROLTYPEID id) noexcept {
    switch (id) {
    case UIA_ButtonControlTypeId: return L"Button";
    case UIA_CheckBoxControlTypeId: return L"CheckBox";
    case UIA_ComboBoxControlTypeId: return L"ComboBox";
    case UIA_EditControlTypeId: return L"Edit";
    case UIA_HyperlinkControlTypeId: return L"Hyperlink";
    case UIA_ImageControlTypeId: return L"Image";
    case UIA_ListControlTypeId: return L"List";
    case UIA_ListItemControlTypeId: return L"ListItem";
    case UIA_MenuControlTypeId: return L"Menu";
    case UIA_MenuBarControlTypeId: return L"MenuBar";
    case UIA_MenuItemControlTypeId: return L"MenuItem";
    case UIA_PaneControlTypeId: return L"Pane";
    case UIA_TabControlTypeId: return L"Tab";
    case UIA_TabItemControlTypeId: return L"TabItem";
    case UIA_TextControlTypeId: return L"Text";
    case UIA_ToolBarControlTypeId: return L"ToolBar";
    case UIA_WindowControlTypeId: return L"Window";
    case UIA_DocumentControlTypeId: return L"Document";
    case UIA_GroupControlTypeId: return L"Group";
    case UIA_DataItemControlTypeId: return L"DataItem";
    case UIA_DataGridControlTypeId: return L"DataGrid";
    case UIA_TableControlTypeId: return L"Table";
    case UIA_HeaderControlTypeId: return L"Header";
    case UIA_HeaderItemControlTypeId: return L"HeaderItem";
    case UIA_TreeControlTypeId: return L"Tree";
    case UIA_TreeItemControlTypeId: return L"TreeItem";
    case UIA_ProgressBarControlTypeId: return L"ProgressBar";
    case UIA_RadioButtonControlTypeId: return L"RadioButton";
    case UIA_SliderControlTypeId: return L"Slider";
    case UIA_SpinnerControlTypeId: return L"Spinner";
    case UIA_StatusBarControlTypeId: return L"StatusBar";
    case UIA_ScrollBarControlTypeId: return L"ScrollBar";
    case UIA_SplitButtonControlTypeId: return L"SplitButton";
    case UIA_ToolTipControlTypeId: return L"ToolTip";
    case UIA_CustomControlTypeId: return L"Custom";
    case UIA_CalendarControlTypeId: return L"Calendar";
    case UIA_SeparatorControlTypeId: return L"Separator";
    case UIA_SemanticZoomControlTypeId: return L"SemanticZoom";
    case UIA_AppBarControlTypeId: return L"AppBar";
    case UIA_TitleBarControlTypeId: return L"TitleBar";
    case UIA_ThumbControlTypeId: return L"Thumb";
    default: return L"Unknown";
    }
}

bool useful_element(const ElementInfo& e) {
    if (e.offscreen || e.rect.w <= 0 || e.rect.h <= 0) return false;
    if (!e.name.empty() || !e.value.empty()) return true;
    return e.control_type == L"Button"      || e.control_type == L"MenuItem"   ||
           e.control_type == L"Edit"        || e.control_type == L"CheckBox"   ||
           e.control_type == L"RadioButton" || e.control_type == L"ComboBox"   ||
           e.control_type == L"List"        || e.control_type == L"ListItem"   ||
           e.control_type == L"TabItem"     || e.control_type == L"Document"   ||
           e.control_type == L"DataItem"    || e.control_type == L"Hyperlink"  ||
           e.control_type == L"Tree"        || e.control_type == L"TreeItem"   ||
           e.control_type == L"Window";
}

std::wstring read_value(IUIAutomationElement* el) {
    ComPtr<IUIAutomationValuePattern> vp;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern),
                                          reinterpret_cast<void**>(vp.addr()))) && vp) {
        Bstr b;
        if (SUCCEEDED(vp->get_CurrentValue(b.addr())))
            return b.str();
    }
    ComPtr<IUIAutomationTextPattern> tp;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
                                          reinterpret_cast<void**>(tp.addr()))) && tp) {
        ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(tp->get_DocumentRange(range.addr())) && range) {
            Bstr b;
            if (SUCCEEDED(range->GetText(4096, b.addr())))
                return b.str();
        }
    }
    return {};
}

bool bool_property(IUIAutomationElement* el, PROPERTYID id) {
    VARIANT v;
    ::VariantInit(&v);
    const HRESULT hr = el->GetCurrentPropertyValue(id, &v);
    const bool ok = SUCCEEDED(hr) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
    ::VariantClear(&v);
    return ok;
}

int int_property(IUIAutomationElement* el, PROPERTYID id) {
    VARIANT v;
    ::VariantInit(&v);
    const HRESULT hr = el->GetCurrentPropertyValue(id, &v);
    int out = 0;
    if (SUCCEEDED(hr)) {
        if (v.vt == VT_I4) out = v.lVal;
        else if (v.vt == VT_INT) out = v.intVal;
    }
    ::VariantClear(&v);
    return out;
}

ElementInfo read_element(IUIAutomationElement* el, HWND hwnd, DWORD pid,
                         const std::wstring& process, const std::wstring& title, int index) {
    ElementInfo e;
    e.index = index;
    e.hwnd = hwnd;
    e.pid = pid;
    e.process = process;
    e.window_title = title;

    CONTROLTYPEID ct = 0;
    el->get_CurrentControlType(&ct);
    e.control_type = control_type_name(ct);
    { Bstr b; if (SUCCEEDED(el->get_CurrentName(b.addr()))) e.name = b.str(); }
    { Bstr b; if (SUCCEEDED(el->get_CurrentAutomationId(b.addr()))) e.automation_id = b.str(); }
    { Bstr b; if (SUCCEEDED(el->get_CurrentClassName(b.addr()))) e.class_name = b.str(); }
    { Bstr b; if (SUCCEEDED(el->get_CurrentFrameworkId(b.addr()))) e.framework_id = b.str(); }
    BOOL enabled = FALSE;
    BOOL offscreen = FALSE;
    BOOL focused = FALSE;
    BOOL focusable = FALSE;
    el->get_CurrentIsEnabled(&enabled);
    el->get_CurrentIsOffscreen(&offscreen);
    el->get_CurrentHasKeyboardFocus(&focused);
    el->get_CurrentIsKeyboardFocusable(&focusable);
    e.enabled = enabled != FALSE;
    e.offscreen = offscreen != FALSE;
    e.focused = focused != FALSE;
    e.keyboard_focusable = focusable != FALSE;
    e.native_hwnd = int_property(el, UIA_NativeWindowHandlePropertyId);
    const int element_pid = int_property(el, UIA_ProcessIdPropertyId);
    if (element_pid > 0) e.pid = static_cast<DWORD>(element_pid);
    e.has_invoke = bool_property(el, UIA_IsInvokePatternAvailablePropertyId);
    e.has_value = bool_property(el, UIA_IsValuePatternAvailablePropertyId);
    e.has_text = bool_property(el, UIA_IsTextPatternAvailablePropertyId);
    e.has_selection_item = bool_property(el, UIA_IsSelectionItemPatternAvailablePropertyId);
    e.has_expand_collapse = bool_property(el, UIA_IsExpandCollapsePatternAvailablePropertyId);
    e.has_scroll = bool_property(el, UIA_IsScrollPatternAvailablePropertyId);
    RECT r{};
    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r)))
        e.rect = rect_from(r);
    if (e.has_value || e.has_text || ct == UIA_EditControlTypeId || ct == UIA_DocumentControlTypeId || ct == UIA_TextControlTypeId)
        e.value = read_value(el);
    return e;
}

std::wstring element_key(const ElementInfo& e) {
    std::wostringstream os;
    os << e.control_type << L'|' << e.name << L'|' << e.value << L'|'
       << e.rect.x << L',' << e.rect.y << L',' << e.rect.w << L',' << e.rect.h;
    return os.str();
}

void append_point_probe_elements(IUIAutomation* uia, WindowDump& out, const Query& query) {
    if (!uia || out.elements.empty()) return;

    std::set<std::wstring> seen;
    for (const auto& e : out.elements) seen.insert(element_key(e));

    std::vector<Rect> probe_rects;
    std::set<std::wstring> seen_probe_rects;
    for (const auto& e : out.elements) {
        const bool chrome_webview_pane =
            e.control_type == L"Pane" &&
            (e.class_name == L"Chrome_RenderWidgetHostHWND" ||
             e.class_name == L"Chrome_WidgetWin_1" ||
             e.framework_id == L"Chrome" ||
             e.name.find(L"Chrome") != std::wstring::npos);
        const bool docked_search_pane =
            e.control_type == L"Pane" &&
            e.class_name == L"Win32++ Docker" &&
            lower(e.name) == L"search";
        const bool candidate = e.control_type == L"Document" || e.control_type == L"Table" ||
                               e.control_type == L"DataGrid" ||
                               chrome_webview_pane ||
                               docked_search_pane ||
                               (e.control_type == L"Unknown" && e.keyboard_focusable &&
                                e.rect.w > 200 && e.rect.h > 120);
        if (candidate && e.rect.w > 80 && e.rect.h > 40) {
            std::wostringstream key;
            key << e.rect.x << L',' << e.rect.y << L',' << e.rect.w << L',' << e.rect.h;
            if (!seen_probe_rects.insert(key.str()).second) continue;
            probe_rects.push_back(e.rect);
        }
    }

    const int limit = query.limit > 0 ? query.limit : 500;
    for (const Rect& r : probe_rects) {
        if (static_cast<int>(out.elements.size()) >= limit) break;
        const int x_step = 80;
        const int y_step = 17;
        for (int y = r.y + y_step / 2; y < r.y + r.h && static_cast<int>(out.elements.size()) < limit; y += y_step) {
            for (int x = r.x + x_step / 2; x < r.x + r.w && static_cast<int>(out.elements.size()) < limit; x += x_step) {
                POINT pt{x, y};
                ComPtr<IUIAutomationElement> el;
                if (FAILED(uia->ElementFromPoint(pt, el.addr())) || !el) continue;
                ElementInfo info = read_element(el.p, out.hwnd, out.pid, out.process, out.title,
                                                static_cast<int>(out.elements.size()));
                if (!useful_element(info)) continue;
                // WebView2/Chromium content is often provided by a renderer process,
                // so UIA_ProcessIdPropertyId can differ from the native pm-image
                // frame pid even though ElementFromPoint landed inside a child HWND
                // owned by this window. The probe rects already come from this
                // window's subtree, so keep renderer-owned descendants here.
                if (info.hwnd != out.hwnd && info.pid != out.pid) {
                    const bool inside_window =
                        info.rect.w > 0 && info.rect.h > 0 &&
                        info.rect.x >= out.rect.x && info.rect.y >= out.rect.y &&
                        info.rect.x + info.rect.w <= out.rect.x + out.rect.w &&
                        info.rect.y + info.rect.h <= out.rect.y + out.rect.h;
                    if (!inside_window) continue;
                }
                const std::wstring key = element_key(info);
                if (!seen.insert(key).second) continue;
                out.elements.push_back(std::move(info));
            }
        }
    }
}

void append_child_hwnd_elements(IUIAutomation* uia, WindowDump& out, const Query& query) {
    if (!uia || !out.hwnd) return;

    std::set<std::wstring> seen;
    for (const auto& e : out.elements) seen.insert(element_key(e));

    struct EnumCtx {
        IUIAutomation* uia = nullptr;
        WindowDump* out = nullptr;
        const Query* query = nullptr;
        std::set<std::wstring>* seen = nullptr;
    } ctx{uia, &out, &query, &seen};

    ::EnumChildWindows(out.hwnd, [](HWND child, LPARAM lp) -> BOOL {
        auto* ctx = reinterpret_cast<EnumCtx*>(lp);
        if (!ctx || !ctx->uia || !ctx->out || !ctx->seen) return FALSE;
        const int limit = ctx->query && ctx->query->limit > 0 ? ctx->query->limit : 500;
        if (static_cast<int>(ctx->out->elements.size()) >= limit) return FALSE;
        if (!::IsWindowVisible(child)) return TRUE;

        ComPtr<IUIAutomationElement> el;
        if (FAILED(ctx->uia->ElementFromHandle(child, el.addr())) || !el) return TRUE;
        ElementInfo info = read_element(el.p, ctx->out->hwnd, ctx->out->pid,
                                        ctx->out->process, ctx->out->title,
                                        static_cast<int>(ctx->out->elements.size()));
        if (!useful_element(info)) return TRUE;
        const std::wstring key = element_key(info);
        if (!ctx->seen->insert(key).second) return TRUE;
        ctx->out->elements.push_back(std::move(info));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

std::vector<HWND> candidate_windows(const Query& query) {
    if (query.foreground) {
        HWND fg = ::GetForegroundWindow();
        return fg ? std::vector<HWND>{fg} : std::vector<HWND>{};
    }
    if (query.hwnd)
        return {query.hwnd};

    // When a PID filter is active we want ALL visible windows for that process,
    // including untitled popup/dropdown windows (WS_POPUP, no caption text).
    // Without a PID filter we skip untitled windows to avoid noise.
    const bool has_pid = query.pid != 0;

    struct Ctx { std::vector<HWND>* out; bool any_pid; };
    std::vector<HWND> out;
    Ctx ctx{&out, has_pid};

    ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        if (!::IsWindowVisible(hwnd)) return TRUE;
        RECT r{};
        if (!::GetWindowRect(hwnd, &r) || r.right <= r.left || r.bottom <= r.top) return TRUE;
        if (!c->any_pid) {
            // General search: skip titleless windows to reduce noise.
            wchar_t title[512] = {};
            ::GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
            if (title[0] == L'\0') return TRUE;
        }
        c->out->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return out;
}

bool matches_query(HWND hwnd, const Query& query, DWORD& pid, std::wstring& title, std::wstring& process) {
    pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (query.pid && pid != query.pid) return false;
    wchar_t title_buf[1024] = {};
    ::GetWindowTextW(hwnd, title_buf, static_cast<int>(std::size(title_buf)));
    title = title_buf;
    process = process_name_from_pid(pid);
    if (!contains_ci(process, query.process_contains)) return false;
    if (!contains_ci(title, query.title_contains)) return false;
    return true;
}

bool dump_one_window(IUIAutomation* uia, HWND hwnd, const Query& query, WindowDump& out) {
    DWORD pid = 0;
    std::wstring title, process;
    if (!matches_query(hwnd, query, pid, title, process)) return false;
    const std::string window_log =
        " pid=" + std::to_string(pid)
        + " hwnd=" + std::to_string(reinterpret_cast<std::uintptr_t>(hwnd));
    logger::info("app-inspect phase=window_start" + window_log);

    out.pid = pid;
    out.hwnd = hwnd;
    out.process = process;
    out.title = title;
    RECT wr{};
    if (::GetWindowRect(hwnd, &wr)) out.rect = rect_from(wr);

    ComPtr<IUIAutomationElement> root;
    logger::info("app-inspect phase=element_from_handle_start" + window_log);
    if (FAILED(uia->ElementFromHandle(hwnd, root.addr())) || !root)
        return true;
    logger::info("app-inspect phase=element_from_handle_done" + window_log);

    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreateTrueCondition(cond.addr())) || !cond)
        return true;

    ComPtr<IUIAutomationElementArray> arr;
    logger::info("app-inspect phase=find_all_start" + window_log);
    if (FAILED(root->FindAll(TreeScope_Subtree, cond.p, arr.addr())) || !arr)
        return true;

    int n = 0;
    arr->get_Length(&n);
    logger::info("app-inspect phase=find_all_done" + window_log
                 + " elements=" + std::to_string(n));
    const int limit = query.limit > 0 ? query.limit : 500;
    for (int i = 0; i < n && static_cast<int>(out.elements.size()) < limit; ++i) {
        if (i == 0 || i % 25 == 0)
            logger::info("app-inspect phase=read_elements" + window_log
                         + " index=" + std::to_string(i)
                         + " kept=" + std::to_string(out.elements.size()));
        ComPtr<IUIAutomationElement> el;
        if (FAILED(arr->GetElement(i, el.addr())) || !el) continue;
        ElementInfo info = read_element(el.p, hwnd, pid, process, title, i);
        if (useful_element(info))
            out.elements.push_back(std::move(info));
    }
    append_child_hwnd_elements(uia, out, query);
    if (query.probe_cells)
        append_point_probe_elements(uia, out, query);
    logger::info("app-inspect phase=window_done" + window_log
                 + " kept=" + std::to_string(out.elements.size()));
    return true;
}

std::wstring trim_copy(std::wstring s) {
    const auto not_space = [](wchar_t c) { return !std::iswspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::wstring> split_csv_lower(std::wstring csv) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto normalize = [](std::wstring s) {
        s = lower(trim_copy(std::move(s)));
        s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) {
            return c == L'-' || c == L'_';
        }), s.end());
        if (s.size() > 1 && s.back() == L's') s.pop_back();
        return s;
    };
    for (wchar_t c : csv) {
        if (c == L',' || c == L';' || std::iswspace(c)) {
            cur = normalize(std::move(cur));
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    cur = normalize(std::move(cur));
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string compact_text(const std::wstring& value, size_t max_chars) {
    std::wstring out;
    out.reserve(value.size());
    // Preserve newline / tab structure as the literal escape sequences `\n` `\r` `\t`
    // (two characters each) so multi-line UIA values stay legible in the single-line
    // Markdown dump. Agents reading the dump can faithfully reproduce the original
    // structure when they type it back; the typing layer already converts JSON "\n"
    // (one char) back into VK_RETURN.
    for (wchar_t c : value) {
        if (c == L'\n')      { out.push_back(L'\\'); out.push_back(L'n'); }
        else if (c == L'\r') { out.push_back(L'\\'); out.push_back(L'r'); }
        else if (c == L'\t') { out.push_back(L'\\'); out.push_back(L't'); }
        else if (c >= 32 && c < 127) out.push_back(c);
        else if (c >= 127) out.push_back(L'?');
    }
    out = trim_copy(out);
    if (out.size() > max_chars)
        out = out.substr(0, max_chars) + L"...";
    return media::process::narrow_utf8(out);
}

std::string quote_md(const std::wstring& value, size_t max_chars) {
    std::string s = compact_text(value, max_chars);
    if (s.empty()) return {};
    for (char& c : s) {
        if (c == '"') c = '\'';
        else if (c == '`') c = '\'';
    }
    return "\"" + s + "\"";
}

bool control_allowed(const ElementInfo& e, const std::vector<std::wstring>& controls) {
    std::wstring ct = lower(e.control_type);
    ct.erase(std::remove_if(ct.begin(), ct.end(), [](wchar_t c) {
        return c == L'-' || c == L'_';
    }), ct.end());
    const bool is_menu = ct == L"menu" || ct == L"menubar" || ct == L"menuitem";
    const bool is_input = ct == L"edit" || ct == L"document" || ct == L"combobox" ||
                          ((e.has_value || e.has_text) && e.keyboard_focusable);

    if (controls.empty())
        return e.has_invoke || is_menu || is_input;

    for (const auto& wanted : controls) {
        if (wanted == L"all") return true;
        if (wanted == ct) return true;
        if (wanted == L"button" && ct == L"splitbutton") return true;
        if ((wanted == L"cell" || wanted == L"dataitem") && ct == L"dataitem") return true;
        if ((wanted == L"input" || wanted == L"textfield" || wanted == L"textbox" || wanted == L"editable") && is_input)
            return true;
        if (wanted == L"text" && (ct == L"text" || ct == L"document" || e.has_text)) return true;
        if (wanted == L"menu" && is_menu) return true;
    }
    return false;
}

bool wants_all_controls(const std::vector<std::wstring>& controls) {
    return std::find(controls.begin(), controls.end(), L"all") != controls.end();
}

bool markdown_should_skip_empty_cell(const ElementInfo& e, const std::vector<std::wstring>& controls) {
    if (wants_all_controls(controls)) return false;
    return e.control_type == L"DataItem" && trim_copy(e.value).empty();
}

std::string pattern_flags(const ElementInfo& e) {
    std::string flags;
    if (e.has_invoke) flags += " invoke";
    if (e.has_value) flags += " value";
    if (e.has_text) flags += " text";
    if (e.has_selection_item) flags += " select";
    if (e.has_expand_collapse) flags += " expand";
    if (e.has_scroll) flags += " scroll";
    if (e.keyboard_focusable) flags += " kbd";
    if (e.focused) flags += " focus";
    return flags;
}

bool write_hbitmap_jpeg(HBITMAP bmp, const std::wstring& out_path, int quality, std::string& err) {
    if (!bmp) {
        err = "bitmap is null";
        return false;
    }
    ComInit co;
    if (!co.ok()) { err = hresult_error("CoInitializeEx", co.hr); return false; }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.addr()));
    if (FAILED(hr) || !factory) {
        err = hresult_error("CoCreateInstance(WICImagingFactory)", hr);
        return false;
    }
    ComPtr<IWICBitmap> wic_bitmap;
    hr = factory->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapIgnoreAlpha, wic_bitmap.addr());
    if (FAILED(hr) || !wic_bitmap) { err = hresult_error("CreateBitmapFromHBITMAP", hr); return false; }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.addr());
    if (FAILED(hr) || !stream) { err = hresult_error("CreateStream", hr); return false; }
    hr = stream->InitializeFromFilename(out_path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) { err = hresult_error("InitializeFromFilename", hr); return false; }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.addr());
    if (FAILED(hr) || !encoder) { err = hresult_error("CreateEncoder(JPEG)", hr); return false; }
    hr = encoder->Initialize(stream.p, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { err = hresult_error("Encoder::Initialize", hr); return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(frame.addr(), props.addr());
    if (FAILED(hr) || !frame) { err = hresult_error("CreateNewFrame", hr); return false; }
    if (props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v{};
        v.vt = VT_R4;
        const int q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
        v.fltVal = static_cast<float>(q) / 100.0f;
        props->Write(1, &opt, &v);
    }
    hr = frame->Initialize(props.p);
    if (FAILED(hr)) { err = hresult_error("Frame::Initialize", hr); return false; }
    hr = frame->WriteSource(wic_bitmap.p, nullptr);
    if (FAILED(hr)) { err = hresult_error("Frame::WriteSource", hr); return false; }
    hr = frame->Commit();
    if (FAILED(hr)) { err = hresult_error("Frame::Commit", hr); return false; }
    hr = encoder->Commit();
    if (FAILED(hr)) { err = hresult_error("Encoder::Commit", hr); return false; }
    return true;
}

} // namespace

bool dump_windows(const Query& query, std::vector<WindowDump>& out, std::string& err) {
    ComInit co;
    if (!co.ok()) { err = hresult_error("CoInitializeEx", co.hr); return false; }
    ComPtr<IUIAutomation> uia;
    HRESULT hr = ::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(uia.addr()));
    if (FAILED(hr) || !uia) { err = hresult_error("CoCreateInstance(CUIAutomation)", hr); return false; }

    const auto candidates = candidate_windows(query);
    logger::info("app-inspect phase=dump_start candidates="
                 + std::to_string(candidates.size()));
    for (HWND hwnd : candidates) {
        WindowDump wd;
        if (dump_one_window(uia.p, hwnd, query, wd) && wd.hwnd)
            out.push_back(std::move(wd));
    }
    logger::info("app-inspect phase=dump_done windows=" + std::to_string(out.size()));
    return true;
}

bool find_element_rect(const Query& query, int element_index, Rect& out, std::string& err) {
    std::vector<WindowDump> windows;
    if (!dump_windows(query, windows, err)) return false;
    for (const auto& w : windows) {
        for (const auto& e : w.elements) {
            if (e.index == element_index) {
                out = e.rect;
                return true;
            }
        }
    }
    err = "element index not found in current dump";
    return false;
}

std::string dump_windows_markdown(const std::vector<WindowDump>& windows, const MarkdownOptions& opts) {
    const auto controls = split_csv_lower(opts.control_types_csv);
    const size_t text_max = opts.text_max_chars > 0 ? static_cast<size_t>(opts.text_max_chars) : 80;

    std::ostringstream os;
    os << "# App Inspect\n\n";
    os << "windows: " << windows.size();
    if (!controls.empty()) os << " | controls: " << compact_text(opts.control_types_csv, 120);
    os << "\n\n";

    for (size_t wi = 0; wi < windows.size(); ++wi) {
        const auto& w = windows[wi];
        os << "- win[" << wi << "] ";
        const std::string title = quote_md(w.title, 100);
        os << (title.empty() ? "\"\"" : title);
        os << " process=" << compact_text(w.process, 64)
           << " pid=" << w.pid
           << " hwnd=0x" << std::hex << reinterpret_cast<std::uintptr_t>(w.hwnd) << std::dec
           << " rect=" << w.rect.x << "," << w.rect.y << "," << w.rect.w << "x" << w.rect.h
           << "\n";

        int shown = 0;
        for (const auto& e : w.elements) {
            if (!control_allowed(e, controls)) continue;
            if (markdown_should_skip_empty_cell(e, controls)) continue;
            ++shown;
            os << "  - #" << e.index
               << " " << compact_text(e.control_type, 32);
            const std::string name = quote_md(e.name, text_max);
            const std::string value = quote_md(e.value, text_max);
            if (!name.empty()) os << " name=" << name;
            if (!value.empty() && value != name) os << " value=" << value;
            if (!e.automation_id.empty()) os << " id=" << quote_md(e.automation_id, 48);
            const int cx = e.rect.x + e.rect.w / 2;
            const int cy = e.rect.y + e.rect.h / 2;
            const int xw = cx - w.rect.x;
            const int yw = cy - w.rect.y;
            os << " at=" << e.rect.x << "," << e.rect.y << "," << e.rect.w << "x" << e.rect.h
               << " center=" << cx << "," << cy
               << " win_center=xw:" << xw << ",yw:" << yw
               << pattern_flags(e)
               << "\n";
        }
        if (shown == 0)
            os << "  - (no matching elements)\n";
    }
    return os.str();
}

bool save_window_jpeg(HWND hwnd, Rect& out_rect, const std::wstring& out_path, int quality, std::string& err) {
    if (!hwnd || !::IsWindow(hwnd)) {
        err = "invalid window handle";
        return false;
    }
    RECT wr{};
    if (!::GetWindowRect(hwnd, &wr)) {
        err = "GetWindowRect failed";
        return false;
    }
    out_rect = rect_from(wr);
    if (out_rect.w <= 0 || out_rect.h <= 0) {
        err = "window rect must have positive width and height";
        return false;
    }

    HDC screen = ::GetDC(nullptr);
    if (!screen) { err = "GetDC(nullptr) failed"; return false; }
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, out_rect.w, out_rect.h);
    HGDIOBJ old = mem && bmp ? ::SelectObject(mem, bmp) : nullptr;
    BOOL painted = FALSE;
    if (mem && bmp) {
        constexpr UINT kPrintFullContent = 0x00000002; // PW_RENDERFULLCONTENT, for newer composited windows.
        painted = ::PrintWindow(hwnd, mem, kPrintFullContent);
        if (!painted)
            painted = ::PrintWindow(hwnd, mem, 0);
    }
    if (old) ::SelectObject(mem, old);
    if (mem) ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    if (!painted || !bmp) {
        if (bmp) ::DeleteObject(bmp);
        err = "PrintWindow capture failed";
        return false;
    }

    const bool ok = write_hbitmap_jpeg(bmp, out_path, quality, err);
    ::DeleteObject(bmp);
    return ok;
}

bool save_screen_rect_jpeg(const Rect& rect, const std::wstring& out_path, int quality, std::string& err) {
    if (rect.w <= 0 || rect.h <= 0) {
        err = "screenshot rect must have positive width and height";
        return false;
    }
    HDC screen = ::GetDC(nullptr);
    if (!screen) { err = "GetDC(nullptr) failed"; return false; }
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP bmp = ::CreateCompatibleBitmap(screen, rect.w, rect.h);
    HGDIOBJ old = mem && bmp ? ::SelectObject(mem, bmp) : nullptr;
    BOOL copied = FALSE;
    if (mem && bmp)
        copied = ::BitBlt(mem, 0, 0, rect.w, rect.h, screen, rect.x, rect.y, SRCCOPY | CAPTUREBLT);
    if (old) ::SelectObject(mem, old);
    if (mem) ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    if (!copied || !bmp) {
        if (bmp) ::DeleteObject(bmp);
        err = "BitBlt screen capture failed";
        return false;
    }

    const bool ok = write_hbitmap_jpeg(bmp, out_path, quality, err);
    ::DeleteObject(bmp);
    return ok;
}

// ── ExpandCollapse + subtree helpers ─────────────────────────────────────────

namespace {

// Walk ALL top-level windows (or just the matching ones from the query) looking
// for an IUIAutomationElement that (a) belongs to the target process, (b) has a
// name containing @p name_needle (case-insensitive), and (c) optionally matches
// control_type_needle.  Returns the first match via @p out_el; @p out_uia keeps
// the IUIAutomation instance alive for the caller.
bool find_uia_element(const Query& query,
                      const std::wstring& name_needle,
                      const std::wstring& control_type_needle,
                      const std::wstring& automation_id_needle,
                      IUIAutomation** out_uia,
                      IUIAutomationElement** out_el,
                      HWND& out_hwnd,
                      std::string& err)
{
    ComInit ci;
    if (!ci.ok()) { err = "CoInitialize failed"; return false; }

    ComPtr<IUIAutomation> uia;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                  reinterpret_cast<void**>(uia.addr()))) || !uia)
    { err = "CoCreateInstance IUIAutomation failed"; return false; }

    const std::vector<HWND> hwnds = candidate_windows(query);
    for (HWND hwnd : hwnds) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (query.pid && pid != query.pid) continue;
        std::wstring title, process;
        if (!matches_query(hwnd, query, pid, title, process)) continue;

        ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(hwnd, root.addr())) || !root) continue;

        ComPtr<IUIAutomationCondition> cond;
        if (FAILED(uia->CreateTrueCondition(cond.addr())) || !cond) continue;

        ComPtr<IUIAutomationElementArray> arr;
        if (FAILED(root->FindAll(TreeScope_Subtree, cond.p, arr.addr())) || !arr) continue;

        int n = 0;
        arr->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            ComPtr<IUIAutomationElement> el;
            if (FAILED(arr->GetElement(i, el.addr())) || !el) continue;

            if (!bool_property(el.p, UIA_IsExpandCollapsePatternAvailablePropertyId)) continue;

            Bstr ename;
            el->get_CurrentName(ename.addr());
            const std::wstring name_lc = lower(ename.str());
            if (!name_needle.empty() && name_lc.find(lower(name_needle)) == std::wstring::npos) continue;

            if (!control_type_needle.empty()) {
                CONTROLTYPEID ct = 0;
                el->get_CurrentControlType(&ct);
                const std::wstring ct_name = lower(control_type_name(ct));
                if (ct_name.find(lower(control_type_needle)) == std::wstring::npos) continue;
            }

            if (!automation_id_needle.empty()) {
                Bstr aid;
                el->get_CurrentAutomationId(aid.addr());
                if (lower(aid.str()).find(lower(automation_id_needle)) == std::wstring::npos) continue;
            }

            // Found a match — transfer ownership to caller.
            *out_el  = el.p; el.p = nullptr;
            *out_uia = uia.p; uia.p = nullptr;
            out_hwnd = hwnd;
            return true;
        }
    }
    err = "no expandable element matching name='" +
          media::process::narrow_utf8(name_needle) + "' found";
    return false;
}

const wchar_t* expand_state_name(ExpandCollapseState s) noexcept {
    switch (s) {
    case ExpandCollapseState_Collapsed:       return L"Collapsed";
    case ExpandCollapseState_Expanded:        return L"Expanded";
    case ExpandCollapseState_PartiallyExpanded: return L"PartiallyExpanded";
    case ExpandCollapseState_LeafNode:        return L"LeafNode";
    default:                                  return L"Unknown";
    }
}

// ── Generic element finder by AutomationId ───────────────────────────────────
// No pattern-availability requirement; searches the full UIA subtree.
// Transfers ownership of the raw COM pointers to the caller.
bool find_element_by_aid(const Query&       query,
                          const std::wstring& automation_id,
                          IUIAutomation**     out_uia,
                          IUIAutomationElement** out_el,
                          std::string&        err)
{
    ComInit ci;
    if (!ci.ok()) { err = "CoInitialize failed"; return false; }

    ComPtr<IUIAutomation> uia;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                  reinterpret_cast<void**>(uia.addr()))) || !uia)
    { err = "CoCreateInstance IUIAutomation failed"; return false; }

    VARIANT v{};
    v.vt      = VT_BSTR;
    v.bstrVal = ::SysAllocString(automation_id.c_str());
    ComPtr<IUIAutomationCondition> cond;
    HRESULT hr = uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, cond.addr());
    ::SysFreeString(v.bstrVal);
    if (FAILED(hr) || !cond) { err = "CreatePropertyCondition failed"; return false; }

    for (HWND hwnd : candidate_windows(query)) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (query.pid && pid != query.pid) continue;
        std::wstring title, process;
        if (!matches_query(hwnd, query, pid, title, process)) continue;

        ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(hwnd, root.addr())) || !root) continue;

        ComPtr<IUIAutomationElement> el;
        if (FAILED(root->FindFirst(TreeScope_Subtree, cond.p, el.addr())) || !el) continue;

        *out_el  = el.p;  el.p  = nullptr;
        *out_uia = uia.p; uia.p = nullptr;
        return true;

        // Unreachable; kept structurally close to the fast path above.
    }

    if (query.probe_cells) {
        int probe_rect_count = 0;
        int probe_point_count = 0;
        int probe_nonempty_aids = 0;
        std::wstring first_probe_aid;
        for (HWND hwnd : candidate_windows(query)) {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);
            if (query.pid && pid != query.pid) continue;
            std::wstring title, process;
            if (!matches_query(hwnd, query, pid, title, process)) continue;

            RECT wr{};
            if (!::GetWindowRect(hwnd, &wr) || wr.right <= wr.left || wr.bottom <= wr.top) continue;
            const Rect window_rect = rect_from(wr);

            ComPtr<IUIAutomationElement> root;
            if (FAILED(uia->ElementFromHandle(hwnd, root.addr())) || !root) continue;

            ComPtr<IUIAutomationCondition> true_cond;
            if (FAILED(uia->CreateTrueCondition(true_cond.addr())) || !true_cond) continue;

            ComPtr<IUIAutomationElementArray> arr;
            if (FAILED(root->FindAll(TreeScope_Subtree, true_cond.p, arr.addr())) || !arr) continue;

            std::vector<Rect> probe_rects;
            std::set<std::wstring> seen_probe_rects;
            auto maybe_add_probe_rect = [&](const ElementInfo& info) {
                const bool chrome_webview_pane =
                    info.control_type == L"Pane" &&
                    (info.class_name == L"Chrome_RenderWidgetHostHWND" ||
                     info.class_name == L"Chrome_WidgetWin_1" ||
                     info.framework_id == L"Chrome" ||
                     info.name.find(L"Chrome") != std::wstring::npos);
                const bool docked_search_pane =
                    info.control_type == L"Pane" &&
                    info.class_name == L"Win32++ Docker" &&
                    lower(info.name) == L"search";
                const bool candidate = info.control_type == L"Document" || info.control_type == L"Table" ||
                                       info.control_type == L"DataGrid" ||
                                       chrome_webview_pane ||
                                       docked_search_pane ||
                                       (info.control_type == L"Unknown" && info.keyboard_focusable &&
                                        info.rect.w > 200 && info.rect.h > 120);
                if (!candidate || info.rect.w <= 80 || info.rect.h <= 40) return;
                std::wostringstream key;
                key << info.rect.x << L',' << info.rect.y << L',' << info.rect.w << L',' << info.rect.h;
                if (!seen_probe_rects.insert(key.str()).second) return;
                probe_rects.push_back(info.rect);
            };
            int n = 0;
            arr->get_Length(&n);
            for (int i = 0; i < n; ++i) {
                ComPtr<IUIAutomationElement> probe_el;
                if (FAILED(arr->GetElement(i, probe_el.addr())) || !probe_el) continue;
                ElementInfo info = read_element(probe_el.p, hwnd, pid, process, title, i);
                maybe_add_probe_rect(info);
            }
            struct ChildCtx {
                IUIAutomation* uia = nullptr;
                HWND hwnd = nullptr;
                DWORD pid = 0;
                const std::wstring* process = nullptr;
                const std::wstring* title = nullptr;
                decltype(maybe_add_probe_rect)* add = nullptr;
            } child_ctx{uia.p, hwnd, pid, &process, &title, &maybe_add_probe_rect};
            ::EnumChildWindows(hwnd, [](HWND child, LPARAM lp) -> BOOL {
                auto* ctx = reinterpret_cast<ChildCtx*>(lp);
                if (!ctx || !ctx->uia || !ctx->add) return FALSE;
                if (!::IsWindowVisible(child)) return TRUE;
                ComPtr<IUIAutomationElement> child_el;
                if (FAILED(ctx->uia->ElementFromHandle(child, child_el.addr())) || !child_el) return TRUE;
                ElementInfo info = read_element(child_el.p, ctx->hwnd, ctx->pid,
                                                *ctx->process, *ctx->title, 0);
                (*ctx->add)(info);
                return TRUE;
            }, reinterpret_cast<LPARAM>(&child_ctx));
            probe_rect_count += static_cast<int>(probe_rects.size());

            for (const Rect& r : probe_rects) {
                const int x_step = 40;
                const int y_step = 16;
                for (int y = r.y + y_step / 2; y < r.y + r.h; y += y_step) {
                    for (int x = r.x + x_step / 2; x < r.x + r.w; x += x_step) {
                        POINT pt{x, y};
                        ++probe_point_count;
                        ComPtr<IUIAutomationElement> point_el;
                        if (FAILED(uia->ElementFromPoint(pt, point_el.addr())) || !point_el) continue;
                        Bstr aid;
                        if (FAILED(point_el->get_CurrentAutomationId(aid.addr()))) continue;
                        const std::wstring aid_value = aid.str();
                        if (!aid_value.empty()) {
                            ++probe_nonempty_aids;
                            if (first_probe_aid.empty()) first_probe_aid = aid_value;
                        }
                        if (aid_value != automation_id) continue;

                        ElementInfo info = read_element(point_el.p, hwnd, pid, process, title, 0);
                        const bool inside_window =
                            info.rect.w > 0 && info.rect.h > 0 &&
                            info.rect.x >= window_rect.x && info.rect.y >= window_rect.y &&
                            info.rect.x + info.rect.w <= window_rect.x + window_rect.w &&
                            info.rect.y + info.rect.h <= window_rect.y + window_rect.h;
                        if (!inside_window) continue;

                        *out_el  = point_el.p; point_el.p = nullptr;
                        *out_uia = uia.p;      uia.p = nullptr;
                        return true;
                    }
                }
            }
        }
        err = "element not found: automationId='" + media::process::narrow_utf8(automation_id) +
              "' (probe enabled; rects=" + std::to_string(probe_rect_count) +
              ", points=" + std::to_string(probe_point_count) +
              ", nonemptyAutomationIds=" + std::to_string(probe_nonempty_aids) +
              (first_probe_aid.empty() ? std::string{} : ", firstAutomationId='" + media::process::narrow_utf8(first_probe_aid) + "'") +
              ")";
        return false;
    }
    err = "element not found: automationId='" + media::process::narrow_utf8(automation_id) + "'";
    return false;
}

} // anonymous namespace

bool set_element_value(const Query&        query,
                       const std::wstring& automation_id,
                       const std::wstring& value,
                       std::string&        err)
{
    ComInit ci;   // must outlive the COM pointers below
    if (!ci.ok()) { err = "CoInitialize failed"; return false; }

    IUIAutomation*        raw_uia = nullptr;
    IUIAutomationElement* raw_el  = nullptr;
    if (!find_element_by_aid(query, automation_id, &raw_uia, &raw_el, err)) return false;
    ComPtr<IUIAutomation>        uia(raw_uia);
    ComPtr<IUIAutomationElement> el(raw_el);

    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(el->GetCurrentPatternAs(UIA_ValuePatternId,
                                       __uuidof(IUIAutomationValuePattern),
                                       reinterpret_cast<void**>(vp.addr()))) || !vp)
    { err = "element '" + media::process::narrow_utf8(automation_id) + "' does not support Value pattern"; return false; }

    BSTR bv = ::SysAllocString(value.c_str());
    HRESULT hr = vp->SetValue(bv);
    ::SysFreeString(bv);
    if (FAILED(hr)) { err = "IValuePattern::SetValue failed (HRESULT=" + std::to_string(hr) + ")"; return false; }
    return true;
}

bool set_element_checked(const Query&        query,
                         const std::wstring& automation_id,
                         bool                desired_checked,
                         std::string&        err)
{
    ComInit ci;   // must outlive the COM pointers below
    if (!ci.ok()) { err = "CoInitialize failed"; return false; }

    IUIAutomation*        raw_uia = nullptr;
    IUIAutomationElement* raw_el  = nullptr;
    if (!find_element_by_aid(query, automation_id, &raw_uia, &raw_el, err)) return false;
    ComPtr<IUIAutomation>        uia(raw_uia);
    ComPtr<IUIAutomationElement> el(raw_el);

    ComPtr<IUIAutomationTogglePattern> tp;
    if (FAILED(el->GetCurrentPatternAs(UIA_TogglePatternId,
                                       __uuidof(IUIAutomationTogglePattern),
                                       reinterpret_cast<void**>(tp.addr()))) || !tp)
    { err = "element '" + media::process::narrow_utf8(automation_id) + "' does not support Toggle pattern"; return false; }

    // Toggle until the desired state is reached (max 2 iterations to avoid infinite loop).
    for (int attempt = 0; attempt < 2; ++attempt) {
        ToggleState ts = ToggleState_Off;
        tp->get_CurrentToggleState(&ts);
        const bool is_checked = (ts == ToggleState_On);
        if (is_checked == desired_checked) return true;
        tp->Toggle();
        ::Sleep(60);
    }
    return true; // best effort
}

bool select_list_item(const Query&        query,
                      const std::wstring& item_name,
                      std::string&        err)
{
    ComInit ci;
    if (!ci.ok()) { err = "CoInitialize failed"; return false; }

    ComPtr<IUIAutomation> uia;
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr,
                                  CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                  reinterpret_cast<void**>(uia.addr()))) || !uia)
    { err = "CoCreateInstance IUIAutomation failed"; return false; }

    // Condition: ControlType == ListItem AND Name == item_name
    VARIANT vCT{};
    vCT.vt   = VT_I4;
    vCT.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> condCT;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, vCT, condCT.addr());

    VARIANT vName{};
    vName.vt      = VT_BSTR;
    vName.bstrVal = ::SysAllocString(item_name.c_str());
    ComPtr<IUIAutomationCondition> condName;
    uia->CreatePropertyCondition(UIA_NamePropertyId, vName, condName.addr());
    ::SysFreeString(vName.bstrVal);

    ComPtr<IUIAutomationCondition> condAnd;
    if (!condCT || !condName ||
        FAILED(uia->CreateAndCondition(condCT.p, condName.p, condAnd.addr())) || !condAnd)
    { err = "failed to build ListItem condition"; return false; }

    for (HWND hwnd : candidate_windows(query)) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (query.pid && pid != query.pid) continue;
        std::wstring title, process;
        if (!matches_query(hwnd, query, pid, title, process)) continue;

        ComPtr<IUIAutomationElement> root;
        if (FAILED(uia->ElementFromHandle(hwnd, root.addr())) || !root) continue;

        ComPtr<IUIAutomationElement> el;
        if (FAILED(root->FindFirst(TreeScope_Subtree, condAnd.p, el.addr())) || !el) continue;

        ComPtr<IUIAutomationSelectionItemPattern> sip;
        if (FAILED(el->GetCurrentPatternAs(UIA_SelectionItemPatternId,
                                           __uuidof(IUIAutomationSelectionItemPattern),
                                           reinterpret_cast<void**>(sip.addr()))) || !sip)
        { err = "ListItem '" + media::process::narrow_utf8(item_name) + "' does not support SelectionItem pattern"; return false; }

        if (FAILED(sip->Select()))
        { err = "ISelectionItemPattern::Select failed"; return false; }
        return true;
    }
    err = "ListItem not found: '" + media::process::narrow_utf8(item_name) + "'";
    return false;
}

bool expand_collapse_element(const Query& query,
                             const std::wstring& element_name,
                             const std::wstring& element_control_type,
                             const std::wstring& element_automation_id,
                             bool expand,
                             std::string& expanded_state, std::string& err)
{
    ComInit ci;
    if (!ci.ok()) { err = "CoInitialize failed"; return false; }

    IUIAutomation* raw_uia = nullptr;
    IUIAutomationElement* raw_el = nullptr;
    HWND hwnd = nullptr;

    if (!find_uia_element(query,
                          element_name,
                          element_control_type,
                          element_automation_id,
                          &raw_uia, &raw_el, hwnd, err))
        return false;

    ComPtr<IUIAutomation> uia(raw_uia);
    ComPtr<IUIAutomationElement> el(raw_el);

    ComPtr<IUIAutomationExpandCollapsePattern> ecp;
    if (FAILED(el->GetCurrentPatternAs(UIA_ExpandCollapsePatternId,
                                       __uuidof(IUIAutomationExpandCollapsePattern),
                                       reinterpret_cast<void**>(ecp.addr()))) || !ecp)
    {
        err = "element does not support ExpandCollapse pattern";
        return false;
    }

    const HRESULT hr = expand ? ecp->Expand() : ecp->Collapse();
    if (FAILED(hr)) {
        err = "ExpandCollapse::" + std::string(expand ? "Expand" : "Collapse") +
              " failed: 0x" + [&]{ char buf[16]; sprintf_s(buf, "%08lX", hr); return std::string(buf); }();
        return false;
    }

    // Read back the resulting state.
    ExpandCollapseState state = ExpandCollapseState_Collapsed;
    ecp->get_CurrentExpandCollapseState(&state);
    expanded_state = media::process::narrow_utf8(expand_state_name(state));
    return true;
}

bool dump_element_subtree(const Query& query,
                          const std::wstring& element_name,
                          const std::wstring& element_control_type,
                          const std::wstring& element_automation_id,
                          std::vector<WindowDump>& out, std::string& err)
{
    ComInit ci;
    if (!ci.ok()) { err = "CoInitialize failed"; return false; }

    IUIAutomation* raw_uia = nullptr;
    IUIAutomationElement* raw_el = nullptr;
    HWND hwnd = nullptr;

    if (!find_uia_element(query, element_name, element_control_type, element_automation_id,
                          &raw_uia, &raw_el, hwnd, err))
    {
        // Fall back to full dump.
        err.clear();
        return dump_windows(query, out, err);
    }

    ComPtr<IUIAutomation> uia(raw_uia);
    ComPtr<IUIAutomationElement> root_el(raw_el);

    // Populate window metadata.
    WindowDump wd;
    wd.hwnd = hwnd;
    ::GetWindowThreadProcessId(hwnd, &wd.pid);
    wd.process = process_name_from_pid(wd.pid);
    wchar_t title_buf[1024] = {};
    ::GetWindowTextW(hwnd, title_buf, static_cast<int>(std::size(title_buf)));
    wd.title = title_buf;
    RECT wr{};
    if (::GetWindowRect(hwnd, &wr)) wd.rect = rect_from(wr);

    // Walk the subtree of the matched element.
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreateTrueCondition(cond.addr())) || !cond)
        return dump_windows(query, out, err);

    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(root_el->FindAll(TreeScope_Subtree, cond.p, arr.addr())) || !arr)
        return dump_windows(query, out, err);

    int n = 0;
    arr->get_Length(&n);
    for (int i = 0; i < n; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(arr->GetElement(i, el.addr())) || !el) continue;
        ElementInfo info = read_element(el.p, hwnd, wd.pid, wd.process, wd.title, i);
        if (useful_element(info))
            wd.elements.push_back(std::move(info));
    }
    out.push_back(std::move(wd));
    return true;
}

} // namespace app_inspect
} // namespace assistant
} // namespace media
