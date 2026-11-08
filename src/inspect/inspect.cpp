#include "inspect.hpp"

#if defined(_WIN32)

#include "win/display_affinity.hpp"
#include "win/highlight_overlay.hpp"
#include "pick_hud.hpp"
#include "pick_loupe.hpp"
#include "logging/logger.h"
#include "process/process_helpers.hpp"
#include "win/window_bounds.hpp"

#include <objbase.h>
#ifndef interface
#  define interface struct __declspec(novtable)
#endif
#include <uiautomation.h>
#include <oleauto.h>
#include <winternl.h>

#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include <algorithm>
#include <thread>

#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
#include "win/chrome_inspect/picker_enrich.hpp"
#endif

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

namespace media::win::inspect {
namespace {

// Picker geometry must stay in physical pixels: LL mouse-hook/UIA coordinates
// are physical, while window APIs can virtualize coordinates for a DPI-unaware
// thread. Per-Monitor V2 is unavailable on Windows 10 before 1703, so fall back
// to V1 there. Resolve dynamically because the API itself is absent pre-1607.
struct ThreadDpiPerMonitorScope {
    using SetContextFn = DPI_AWARENESS_CONTEXT (WINAPI*)(DPI_AWARENESS_CONTEXT);

    SetContextFn          set_context = nullptr;
    DPI_AWARENESS_CONTEXT previous    = nullptr;

    ThreadDpiPerMonitorScope() noexcept {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (!user32) return;
        set_context = reinterpret_cast<SetContextFn>(
            ::GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (!set_context) return;

        previous = set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!previous)
            previous = set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    }

    ~ThreadDpiPerMonitorScope() {
        if (set_context && previous)
            set_context(previous);
    }

    ThreadDpiPerMonitorScope(const ThreadDpiPerMonitorScope&) = delete;
    ThreadDpiPerMonitorScope& operator=(const ThreadDpiPerMonitorScope&) = delete;
};

// ─────────────────────────────────────────────────────────────────────────────
// WindowFromPointEx — ported from ref/winspy/WindowFromPointEx.c
// ─────────────────────────────────────────────────────────────────────────────

struct FindBestChildCtx {
    POINT pt;
    bool  show_hidden;
    DWORD best_area;
    HWND  best;
};

BOOL CALLBACK find_best_child_proc(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<FindBestChildCtx*>(lp);
    RECT r;
    if (!::GetWindowRect(hwnd, &r)) return TRUE;
    if (!::PtInRect(&r, c->pt)) return TRUE;
    const DWORD area = static_cast<DWORD>(r.right - r.left) *
                       static_cast<DWORD>(r.bottom - r.top);
    if (area < c->best_area && (::IsWindowVisible(hwnd) || c->show_hidden)) {
        c->best_area = area;
        c->best      = hwnd;
    }
    return TRUE;
}

HWND find_best_child(HWND found, POINT pt, bool show_hidden) {
    FindBestChildCtx c{pt, show_hidden, 0xFFFFFFFFu, nullptr};
    HWND parent = ::GetParent(found);
    const LONG style = ::GetWindowLong(found, GWL_STYLE);
    if (parent == nullptr || (style & WS_POPUP)) parent = found;
    ::EnumChildWindows(parent, find_best_child_proc, reinterpret_cast<LPARAM>(&c));
    return c.best ? c.best : parent;
}

} // namespace

HWND window_from_point_ex(POINT pt, bool show_hidden) noexcept {
    HWND p = ::WindowFromPoint(pt);
    if (!p) return nullptr;
    p = find_best_child(p, pt, show_hidden);
    if (!show_hidden) {
        while (p && !::IsWindowVisible(p)) p = ::GetParent(p);
    }
    return p;
}

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Small helpers
// ─────────────────────────────────────────────────────────────────────────────

std::wstring class_of(HWND hwnd) {
    wchar_t buf[256] = {};
    ::GetClassNameW(hwnd, buf, ARRAYSIZE(buf));
    return std::wstring(buf);
}

std::wstring title_of(HWND hwnd) {
    wchar_t buf[1024] = {};
    ::GetWindowTextW(hwnd, buf, ARRAYSIZE(buf));
    return std::wstring(buf);
}

// Eyedropper. Picker windows sit beside the cursor (HUD) or as a thin
// outline, so GetPixel under the cursor still hits the target. Same-process
// auto/exclude captures hide them via WDA for the take.
COLORREF sample_screen_color(POINT pt, bool& ok) {
    ok = false;
    HDC screen = ::GetDC(nullptr);
    if (!screen) return 0;
    const COLORREF c = ::GetPixel(screen, pt.x, pt.y);
    ::ReleaseDC(nullptr, screen);
    if (c == CLR_INVALID) return 0;
    ok = true;
    return c;
}

bool copy_text_to_clipboard(const std::wstring& text) {
    if (!::OpenClipboard(nullptr)) return false;
    bool ok = false;
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* dst = ::GlobalLock(mem)) {
            std::memcpy(dst, text.c_str(), bytes);
            ::GlobalUnlock(mem);
            ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) ::GlobalFree(mem);
    }
    ::CloseClipboard();
    return ok;
}

int monitor_index_of(POINT pt) {
    struct Ctx { POINT pt; int idx; int found; int cur; } ctx{pt, 0, -1, 0};
    ::EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            if (::GetMonitorInfoW(hmon, &mi) && ::PtInRect(&mi.rcMonitor, c->pt))
                c->found = c->cur;
            ++c->cur;
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found < 0 ? 0 : ctx.found;
}

void fill_process_info(PickResult& out, bool resolve_cmdline) {
    if (!out.pid) return;
    HANDLE proc = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | (resolve_cmdline ? PROCESS_VM_READ : 0),
        FALSE, out.pid);
    if (!proc) {
        // Retry without VM_READ — exe path still works under limited rights.
        proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, out.pid);
        resolve_cmdline = false;
    }
    if (!proc) return;

    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = ARRAYSIZE(path);
    if (::QueryFullProcessImageNameW(proc, 0, path, &n) && n > 0)
        out.exe.assign(path, n);

    if (resolve_cmdline) {
        PROCESS_BASIC_INFORMATION pbi{};
        ULONG ret = 0;
        const NTSTATUS st = ::NtQueryInformationProcess(
            proc, ProcessBasicInformation, &pbi, sizeof(pbi), &ret);
        if (st == 0 && pbi.PebBaseAddress) {
            PEB peb{};
            SIZE_T got = 0;
            if (::ReadProcessMemory(proc, pbi.PebBaseAddress, &peb, sizeof(peb), &got) && got >= sizeof(peb)) {
                RTL_USER_PROCESS_PARAMETERS upp{};
                if (::ReadProcessMemory(proc, peb.ProcessParameters, &upp, sizeof(upp), &got) && got >= sizeof(upp)) {
                    const USHORT len = upp.CommandLine.Length; // bytes
                    if (upp.CommandLine.Buffer && len > 0 && static_cast<size_t>(len) < 64u * 1024u) {
                        std::wstring cl(len / sizeof(wchar_t), L'\0');
                        if (::ReadProcessMemory(proc, upp.CommandLine.Buffer, &cl[0], len, &got))
                            out.cmdline = std::move(cl);
                    }
                }
            }
        }
    }
    ::CloseHandle(proc);
}

std::wstring bstr_to_w(BSTR b) {
    if (!b) return {};
    std::wstring s(b, ::SysStringLen(b));
    return s;
}

LONG rect_area(const RECT& r) {
    const long long w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return 0;
    const long long a = w * h;
    return a > 0x7FFFFFFFLL ? 0x7FFFFFFF : static_cast<LONG>(a);
}

// Drill from @p start down the raw UIA tree to the deepest element whose bounding
// rectangle still contains @p pt. Chrome's ElementFromPoint hit-test returns a
// coarse container (e.g. the page "main"); descending the children reaches the
// real leaf (the video player / a DOM node). Caller releases the returned element.
IUIAutomationElement* descend_to_point(IUIAutomation* uia, IUIAutomationElement* start, POINT pt) {
    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(uia->get_RawViewWalker(&walker)) || !walker) {
        start->AddRef();
        return start;
    }
    IUIAutomationElement* cur = start;
    cur->AddRef();
    for (int depth = 0; depth < 256; ++depth) {
        IUIAutomationElement* child = nullptr;
        if (FAILED(walker->GetFirstChildElement(cur, &child)) || !child) break;

        IUIAutomationElement* best = nullptr;
        LONG best_area = 0x7FFFFFFF;
        while (child) {
            RECT rr{};
            if (SUCCEEDED(child->get_CurrentBoundingRectangle(&rr)) && ::PtInRect(&rr, pt)) {
                const LONG a = rect_area(rr);
                if (a > 0 && a < best_area) {
                    if (best) best->Release();
                    best = child;
                    best->AddRef();
                    best_area = a;
                }
            }
            IUIAutomationElement* next = nullptr;
            walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = next;
        }
        if (!best) break;          // no child contains the point → cur is the leaf
        cur->Release();
        cur = best;                // descend into the smallest child under the point
    }
    walker->Release();
    return cur;
}

void read_element(IUIAutomationElement* el, PickResult& out) {
    out.uia_ok = true;
    RECT r{};
    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&r))) out.uia_rect = r;
    BSTR b = nullptr;
    if (SUCCEEDED(el->get_CurrentName(&b)))                 { out.uia_name = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentAutomationId(&b)))         { out.uia_automation_id = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentLocalizedControlType(&b))) { out.uia_control_type = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentFrameworkId(&b)))          { out.uia_framework_id = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentClassName(&b)))            { out.uia_class_name = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    POINT cp{};
    BOOL got = FALSE;
    if (SUCCEEDED(el->GetClickablePoint(&cp, &got)) && got) {
        out.uia_has_clickable = true;
        out.uia_clickable = cp;
    }
}

// ElementFromPoint + deepest raw-tree descent, plus a focused-element preference.
// Returns the leaf element (caller releases), or nullptr. @p ignore_hwnd lets us
// skip our own highlight window if UIA ever reports it instead of passing through.
//
// Chrome is one HWND + a deep UIA tree, so WindowFromPoint can't reach DOM nodes;
// ElementFromPoint lands on a coarse container (e.g. the page "main") and we have
// to descend. Separately, Inspect reports the YouTube player via "How found:
// Focus", so when the focused element contains the point and is tighter than the
// point-descended leaf, we prefer it (that's how we land on AutomationId
// "shorts-player").
IUIAutomationElement* uia_element_at(IUIAutomation* uia, POINT pt, HWND ignore_hwnd) {
    if (!uia) return nullptr;

    IUIAutomationElement* leaf = nullptr;
    IUIAutomationElement* hit = nullptr;
    if (SUCCEEDED(uia->ElementFromPoint(pt, &hit)) && hit) {
        bool is_ours = false;
        if (ignore_hwnd) {
            UIA_HWND nh = nullptr;
            if (SUCCEEDED(hit->get_CurrentNativeWindowHandle(&nh)) &&
                reinterpret_cast<HWND>(nh) == ignore_hwnd)
                is_ours = true;
        }
        if (is_ours) { hit->Release(); return nullptr; }
        leaf = descend_to_point(uia, hit, pt);
        hit->Release();
    }

    // Focused-element preference: pick the smaller (more specific) of {leaf, focus}
    // when the focused element's rect contains the point.
    IUIAutomationElement* foc = nullptr;
    if (SUCCEEDED(uia->GetFocusedElement(&foc)) && foc) {
        RECT fr{};
        if (SUCCEEDED(foc->get_CurrentBoundingRectangle(&fr)) && ::PtInRect(&fr, pt) &&
            rect_area(fr) > 0) {
            if (!leaf) {
                leaf = foc; foc = nullptr;
            } else {
                RECT lr{};
                if (SUCCEEDED(leaf->get_CurrentBoundingRectangle(&lr)) &&
                    rect_area(fr) < rect_area(lr)) {
                    leaf->Release();
                    leaf = foc; foc = nullptr;
                }
            }
        }
        if (foc) foc->Release();
    }
    return leaf;
}

// ─────────────────────────────────────────────────────────────────────────────
// Pick session state — drives the highlight + hooks via a single g_ctx pointer
// ─────────────────────────────────────────────────────────────────────────────

struct StackEntry {
    RECT         rect{};
    std::wstring name;
    std::wstring automation_id;
    std::wstring control_type;
    std::wstring framework_id;
    std::wstring class_name;
    bool         from_uia = false;
};

struct PidExeCache {
    DWORD        pid = 0;
    std::wstring exe;
};

struct PointHwndCache {
    HWND         hwnd = nullptr;
    HWND         raw = nullptr;      ///< WindowFromPoint before EnumChildWindows
    HWND         root = nullptr;
    HWND         webview_host = nullptr;
    POINT        pt{};
    RECT         rect{};
    std::wstring class_name;
    std::wstring root_class;
    std::wstring title;
    DWORD        pid = 0;
    std::wstring exe;
    bool         chrome = false;
    bool         valid = false;
};

struct ChromeHoverHit {
    HWND         host = nullptr;
    RECT         bounds{};
    bool         bounds_ok = false;
    int          qx = 0;
    int          qy = 0;
    std::wstring css;
    std::wstring tag;
    bool         ok = false;
    DWORD        tick = 0;
};

struct UiaHoverCache {
    int          qx = 0;
    int          qy = 0;
    HWND         hwnd = nullptr;
    bool         deep = false;
    bool         uia_resolved = false;
    int          stack_index = 0;
    std::vector<StackEntry> stack;
};

struct PickContext {
    PickOptions       opts;
    POINT             cur_pt{};
    DWORD             cursor_move_tick = 0; ///< defer blocking providers until idle
    POINT             drag_start{};
    POINT             stack_pt{};       ///< point used to build stack_
    RECT              custom_rect{};
    RECT              committed_rect{}; ///< resolve_target_rect at click — WYSIWYG with highlight
    HighlightOverlay  overlay;          ///< reusable rect painter (src/win/highlight_overlay)
    HighlightOverlay  measure_dim;      ///< semi-transparent scrim after anchor A
    MeasureOverlay    measure_stage;    ///< span rect + H/V guides + target rects
    PickHud           hud;              ///< stack depth / name chip
    PickLoupe         loupe;            ///< magnifier (Z toggles in region mode)
    bool              loupe_on = false;
    POINT             sample_pt{};        ///< last hover sample position
    bool              sample_pt_valid = false;
    DWORD             sample_tick = 0;  ///< throttle eyedropper / loupe updates
    std::wstring      chrome_preview_css;
    std::wstring      chrome_preview_tag;
    bool              chrome_preview_valid = false;
    POINT             chrome_probe_pt{};
    bool              chrome_probe_pt_valid = false;
    DWORD             chrome_probe_tick = 0;
    HHOOK             mouse_hook = nullptr;
    HHOOK             kbd_hook = nullptr;
    IUIAutomation*    uia = nullptr;    ///< owned for the whole pick (UIA hover + commit)
    std::vector<StackEntry> stack;      ///< leaf → siblings → ∪children → parent → …
    int               stack_index = 0;  ///< 0 = leaf
    bool              stack_deep = false; ///< true when stack includes ancestors / siblings
    bool              stack_uia_resolved = false;
    StackEntry        committed_entry{};
    COLORREF          cur_color = 0;        ///< pixel under cursor (sample_color)
    bool              cur_color_ok = false;
    COLORREF          committed_color = 0;
    bool              committed_color_ok = false;
    bool              committed = false;
    bool              cancelled = false;
    bool              waiting_up = false;
    bool              custom_dragging = false;
    bool              custom_committed = false;
    bool              measure_has_a = false;
    POINT             measure_a{};
    POINT             measure_b{};
    POINT             measure_probe{};    ///< snapped hover / commit point B
    RECT              measure_rect_a{};
    RECT              measure_rect_b{};
    bool              measure_rect_a_valid = false;
    bool              measure_rect_b_valid = false;
    RECT              measure_snap_rect{};
    bool              measure_snap_valid = false;
    bool              measure_committed = false;
    bool              committed_rect_valid = false;
    bool              committed_entry_valid = false;
    /// Session caches — UIA / OpenProcess / DOM IPC stay sticky while picking.
    std::vector<PidExeCache>    pid_exe;
    PointHwndCache              hwnd_at;
    std::vector<ChromeHoverHit> chrome_hover;
    std::vector<UiaHoverCache>  uia_hover;
};

PickContext* g_ctx = nullptr;

constexpr UINT kColorCommitMessage = WM_APP + 0x431;
constexpr UINT kColorFinishMessage = WM_APP + 0x432;
constexpr UINT kColorKeyMessage    = WM_APP + 0x433;

std::atomic<DWORD> g_color_ui_thread{0};
std::atomic<LONG>  g_color_click_x{0};
std::atomic<LONG>  g_color_click_y{0};

struct ColorHookThreadState {
    HANDLE ready = nullptr;
    DWORD thread_id = 0;
    DWORD error = ERROR_SUCCESS;
    HHOOK mouse = nullptr;
    HHOOK keyboard = nullptr;
    bool ok = false;
};

bool eyedropper_mode() {
    return g_ctx && g_ctx->opts.eyedropper;
}

bool measure_mode() {
    return g_ctx && g_ctx->opts.measure;
}

bool region_mode() {
    return g_ctx && !g_ctx->opts.eyedropper && !g_ctx->opts.measure;
}

bool same_rect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool monitor_rect_at(POINT pt, RECT& out)
{
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(mon, &mi))
        return false;
    out = mi.rcMonitor;
    return out.right > out.left && out.bottom > out.top;
}

bool rect_covers_virtual_desktop(const RECT& r)
{
    const RECT vs = media::win::virtual_screen_rect();
    const LONG va = rect_area(vs);
    const LONG ra = rect_area(r);
    if (va <= 0 || ra <= 0)
        return false;
    return static_cast<long long>(ra) * 5 >= static_cast<long long>(va) * 4;
}

struct MonitorCoverCount {
    const RECT* r = nullptr;
    int n = 0;
};

BOOL CALLBACK count_fully_covered_monitors(HMONITOR hmon, HDC, LPRECT, LPARAM lp)
{
    auto* c = reinterpret_cast<MonitorCoverCount*>(lp);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!c || !c->r || !::GetMonitorInfoW(hmon, &mi))
        return TRUE;
    RECT inter{};
    if (!::IntersectRect(&inter, c->r, &mi.rcMonitor))
        return TRUE;
    const LONG ia = rect_area(inter);
    const LONG ma = rect_area(mi.rcMonitor);
    if (ma > 0 && static_cast<long long>(ia) * 5 >= static_cast<long long>(ma) * 4)
        ++c->n;
    return TRUE;
}

bool rect_covers_multiple_monitors(const RECT& r)
{
    if (r.right <= r.left || r.bottom <= r.top)
        return false;
    MonitorCoverCount c{&r, 0};
    RECT probe = r;
    ::EnumDisplayMonitors(nullptr, &probe, count_fully_covered_monitors,
                          reinterpret_cast<LPARAM>(&c));
    return c.n >= 2;
}

bool is_desktop_shell_hwnd(HWND hwnd)
{
    return media::win::hwnd_is_desktop_shell(hwnd);
}

void clip_pick_rect_to_point_monitor(RECT& r, POINT pt, HWND hwnd = nullptr)
{
    const bool shell = is_desktop_shell_hwnd(hwnd);
    if (!shell && !rect_covers_virtual_desktop(r) && !rect_covers_multiple_monitors(r))
        return;
    RECT mon{};
    if (!monitor_rect_at(pt, mon))
        return;
    RECT inter{};
    if (!::IntersectRect(&inter, &r, &mon))
        return;
    r = inter;
}

// Buttons / list rows stay sticky; page-sized panes and the desktop do not —
// reusing those as "still on the same control" froze the highlight after caching.
bool pick_rect_is_tight(const RECT& r, POINT pt)
{
    if (r.right <= r.left || r.bottom <= r.top || rect_covers_virtual_desktop(r)
        || rect_covers_multiple_monitors(r))
        return false;
    RECT mon{};
    if (monitor_rect_at(pt, mon)) {
        const LONG ma = rect_area(mon);
        const LONG ra = rect_area(r);
        if (ma > 0 && static_cast<long long>(ra) * 5 >= static_cast<long long>(ma) * 2)
            return false;
    }
    if (g_ctx && g_ctx->hwnd_at.valid) {
        const LONG wa = rect_area(g_ctx->hwnd_at.rect);
        const LONG ra = rect_area(r);
        if (wa > 0 && static_cast<long long>(ra) * 5 >= static_cast<long long>(wa) * 3)
            return false;
    }
    return true;
}

bool is_picker_chrome(HWND hwnd) {
    if (!g_ctx || !hwnd) return false;
    return hwnd == g_ctx->overlay.hwnd() || hwnd == g_ctx->hud.hwnd() ||
           hwnd == g_ctx->loupe.hwnd() || hwnd == g_ctx->measure_dim.hwnd() ||
           hwnd == g_ctx->measure_stage.hwnd();
}

HWND ignore_hwnd_for_uia() {
    if (!g_ctx) return nullptr;
    if (g_ctx->overlay.hwnd()) return g_ctx->overlay.hwnd();
    return g_ctx->hud.hwnd();
}

constexpr int kChromeHoverCell = 8;
constexpr std::size_t kUiaHoverCap = 96;
#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
constexpr std::size_t kChromeHoverCap = 192;
constexpr DWORD kChromeMissRetryMs = 500;
#endif

bool exe_is_chrome(const std::wstring& exe)
{
    auto ends = [&](const wchar_t* suf) {
        const size_t n = std::wcslen(suf);
        return exe.size() >= n
            && _wcsnicmp(exe.c_str() + exe.size() - n, suf, n) == 0;
    };
    return ends(L"chrome.exe") || ends(L"chromium.exe");
}

const std::wstring& exe_for_pid(DWORD pid)
{
    static const std::wstring empty;
    if (!g_ctx || !pid)
        return empty;
    for (const auto& e : g_ctx->pid_exe) {
        if (e.pid == pid)
            return e.exe;
    }
    PidExeCache row;
    row.pid = pid;
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD n = ARRAYSIZE(path);
        if (::QueryFullProcessImageNameW(proc, 0, path, &n) && n > 0)
            row.exe.assign(path, n);
        ::CloseHandle(proc);
    }
    g_ctx->pid_exe.push_back(std::move(row));
    return g_ctx->pid_exe.back().exe;
}

int chrome_cell(int v)
{
    return v >= 0 ? v / kChromeHoverCell
                  : -((-v + kChromeHoverCell - 1) / kChromeHoverCell);
}

bool hover_leaf_covers(POINT pt)
{
    if (!g_ctx || g_ctx->stack.empty())
        return false;
    const RECT& r = g_ctx->stack.front().rect;
    if (!pick_rect_is_tight(r, pt))
        return false;
    return ::PtInRect(&r, pt);
}

void remember_point_hwnd(HWND hwnd, POINT pt, HWND raw)
{
    if (!g_ctx)
        return;
    auto& c = g_ctx->hwnd_at;
    if (c.valid && c.hwnd == hwnd) {
        c.pt = pt;
        if (raw)
            c.raw = raw;
        if (c.rect.right <= c.rect.left || !::PtInRect(&c.rect, pt)) {
            visible_window_bounds(hwnd, c.rect);
            clip_pick_rect_to_point_monitor(c.rect, pt, hwnd);
        }
        return;
    }
    c = {};
    c.hwnd = hwnd;
    c.raw = raw ? raw : (hwnd ? ::WindowFromPoint(pt) : nullptr);
    c.pt = pt;
    c.valid = hwnd != nullptr;
    if (!hwnd)
        return;
    visible_window_bounds(hwnd, c.rect);
    clip_pick_rect_to_point_monitor(c.rect, pt, hwnd);
    c.root = ::GetAncestor(hwnd, GA_ROOT);
    c.class_name = class_of(hwnd);
    if (c.root) {
        c.root_class = class_of(c.root);
        c.title = title_of(c.root);
    } else {
        c.title = title_of(hwnd);
    }
    ::GetWindowThreadProcessId(hwnd, &c.pid);
    c.exe = exe_for_pid(c.pid);
    c.chrome = exe_is_chrome(c.exe);
#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
    c.webview_host = media::win::chrome_inspect::webview_dom_host_at(hwnd, pt);
#endif
}

HWND hwnd_at_point(POINT pt)
{
    if (!g_ctx)
        return window_from_point_ex(pt, true);
    auto& c = g_ctx->hwnd_at;
    HWND raw = ::WindowFromPoint(pt);
    if (is_picker_chrome(raw))
        raw = nullptr;
    // Same raw HWND can span both monitors (Progman/WorkerW). Reuse only while
    // the cursor is still inside the resolved child's clipped rect.
    if (c.valid && raw && c.raw == raw && c.hwnd && ::IsWindow(c.hwnd)
        && c.rect.right > c.rect.left && ::PtInRect(&c.rect, pt)
        && !rect_covers_virtual_desktop(c.rect)
        && !rect_covers_multiple_monitors(c.rect)) {
        c.pt = pt;
        return c.hwnd;
    }
    HWND hwnd = window_from_point_ex(pt, g_ctx->opts.show_hidden);
    remember_point_hwnd(hwnd, pt, raw);
    return hwnd;
}

HWND cached_point_hwnd()
{
    return g_ctx ? hwnd_at_point(g_ctx->cur_pt) : nullptr;
}

bool restore_uia_hover(POINT pt, bool deep)
{
    if (!g_ctx)
        return false;
    const HWND hwnd = hwnd_at_point(pt);
    const int qx = chrome_cell(pt.x);
    const int qy = chrome_cell(pt.y);
    for (auto it = g_ctx->uia_hover.rbegin(); it != g_ctx->uia_hover.rend(); ++it) {
        if (it->hwnd != hwnd || it->deep != deep)
            continue;
        const bool cell = it->qx == qx && it->qy == qy;
        const bool in_leaf = !it->stack.empty()
            && pick_rect_is_tight(it->stack.front().rect, pt)
            && ::PtInRect(&it->stack.front().rect, pt);
        if (!cell && !in_leaf)
            continue;
        g_ctx->stack = it->stack;
        for (auto& e : g_ctx->stack)
            clip_pick_rect_to_point_monitor(e.rect, pt, hwnd);
        g_ctx->stack_index = 0;
        g_ctx->stack_pt = pt;
        g_ctx->stack_deep = it->deep;
        g_ctx->stack_uia_resolved = it->uia_resolved;
        return true;
    }
    return false;
}

void store_uia_hover(POINT pt, HWND hwnd)
{
    if (!g_ctx || !hwnd)
        return;
    UiaHoverCache row;
    row.qx = chrome_cell(pt.x);
    row.qy = chrome_cell(pt.y);
    row.hwnd = hwnd;
    row.deep = g_ctx->stack_deep;
    row.uia_resolved = g_ctx->stack_uia_resolved;
    row.stack_index = 0;
    row.stack = g_ctx->stack;
    for (auto& old : g_ctx->uia_hover) {
        if (old.hwnd == hwnd && old.deep == row.deep && old.qx == row.qx && old.qy == row.qy) {
            old = std::move(row);
            return;
        }
    }
    if (g_ctx->uia_hover.size() >= kUiaHoverCap)
        g_ctx->uia_hover.erase(g_ctx->uia_hover.begin());
    g_ctx->uia_hover.push_back(std::move(row));
}

void rebuild_stack(POINT pt, bool deep, bool probe_uia);
const StackEntry* current_stack_entry();

void refresh_hover_stack(POINT pt, bool deep)
{
    if (!g_ctx)
        return;
    if (deep) {
        if (!g_ctx->stack_deep || g_ctx->stack.empty()) {
            if (!restore_uia_hover(pt, true))
                rebuild_stack(pt, true, true);
        }
        return;
    }
    if (g_ctx->stack_deep && !g_ctx->stack.empty()) {
        if (const StackEntry* e = current_stack_entry()) {
            if (e->rect.right > e->rect.left && ::PtInRect(&e->rect, pt))
                return;
        }
    }
    const bool idle = (::GetTickCount() - g_ctx->cursor_move_tick) >= 120;
    const bool in_leaf = hover_leaf_covers(pt);
    if (in_leaf && (g_ctx->stack_uia_resolved || !idle) && !g_ctx->stack_deep)
        return;
    if (restore_uia_hover(pt, false) && (g_ctx->stack_uia_resolved || !idle))
        return;
    rebuild_stack(pt, false, idle || !in_leaf);
}

StackEntry entry_from_uia(IUIAutomationElement* el, const RECT& r) {
    StackEntry e;
    e.rect = r;
    e.from_uia = true;
    BSTR b = nullptr;
    if (SUCCEEDED(el->get_CurrentName(&b)))                 { e.name = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentAutomationId(&b)))         { e.automation_id = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentLocalizedControlType(&b))) { e.control_type = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentFrameworkId(&b)))          { e.framework_id = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    if (SUCCEEDED(el->get_CurrentClassName(&b)))            { e.class_name = bstr_to_w(b); ::SysFreeString(b); b = nullptr; }
    return e;
}

void append_unique(std::vector<StackEntry>& stack, StackEntry e) {
    if (g_ctx)
        clip_pick_rect_to_point_monitor(e.rect, g_ctx->cur_pt);
    if (e.rect.right <= e.rect.left || e.rect.bottom <= e.rect.top) return;
    for (const auto& existing : stack) {
        if (same_rect(existing.rect, e.rect) && existing.name == e.name &&
            existing.automation_id == e.automation_id)
            return;
    }
    stack.push_back(std::move(e));
}

void inflate_union(RECT& u, const RECT& r, bool& init) {
    if (!init) {
        u = r;
        init = true;
        return;
    }
    if (r.left < u.left) u.left = r.left;
    if (r.top < u.top) u.top = r.top;
    if (r.right > u.right) u.right = r.right;
    if (r.bottom > u.bottom) u.bottom = r.bottom;
}

// Drop splitter grips / tiny chrome from the sibling cycle. Absolute floor plus
// optional share of the parent so a 6px sash never shows next to Explorer/Center.
bool sibling_rect_significant(const RECT& r, const RECT* parent_r) {
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w < 40 || h < 40) return false;
    const long long area = static_cast<long long>(w) * h;
    if (area < 80LL * 80LL) return false;
    if (parent_r) {
        const long long pw = parent_r->right - parent_r->left;
        const long long ph = parent_r->bottom - parent_r->top;
        const long long parent_area = pw * ph;
        // Require at least ~4% of the parent (dock panes); ignore noise.
        if (parent_area > 0 && area * 100 < parent_area * 4) return false;
    }
    return true;
}

// After the current node: siblings under the same parent, then union-of-children
// (Explorer | Center View without moving the cursor). Caps keep the cycle usable.
void append_uia_siblings_and_union(IUIAutomation* uia,
                                   IUIAutomationTreeWalker* walker,
                                   IUIAutomationElement* cur,
                                   IUIAutomationElement* parent) {
    if (!uia || !walker || !cur || !parent || !g_ctx) return;

    IUIAutomationElement* child = nullptr;
    if (FAILED(walker->GetFirstChildElement(parent, &child)) || !child) return;

    RECT parent_r{};
    const RECT* parent_rp = nullptr;
    if (SUCCEEDED(parent->get_CurrentBoundingRectangle(&parent_r)) &&
        parent_r.right > parent_r.left && parent_r.bottom > parent_r.top) {
        parent_rp = &parent_r;
    }

    RECT union_r{};
    bool union_init = false;
    int significant_count = 0;
    int sibling_added = 0;
    constexpr int kMaxSiblings = 16;

    while (child) {
        UIA_HWND nh = nullptr;
        if (SUCCEEDED(child->get_CurrentNativeWindowHandle(&nh)) &&
            is_picker_chrome(reinterpret_cast<HWND>(nh))) {
            IUIAutomationElement* next = nullptr;
            const HRESULT hr = walker->GetNextSiblingElement(child, &next);
            child->Release();
            child = SUCCEEDED(hr) ? next : nullptr;
            continue;
        }

        RECT r{};
        if (SUCCEEDED(child->get_CurrentBoundingRectangle(&r)) &&
            r.right > r.left && r.bottom > r.top) {
            BOOL same = FALSE;
            const bool is_cur =
                SUCCEEDED(uia->CompareElements(cur, child, &same)) && same;
            // Keep the hovered branch in the union even if tiny; siblings must
            // clear the size floor.
            if (is_cur || sibling_rect_significant(r, parent_rp)) {
                inflate_union(union_r, r, union_init);
                ++significant_count;
                if (!is_cur && sibling_added < kMaxSiblings) {
                    append_unique(g_ctx->stack, entry_from_uia(child, r));
                    ++sibling_added;
                }
            }
        }

        IUIAutomationElement* next = nullptr;
        const HRESULT hr = walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = SUCCEEDED(hr) ? next : nullptr;
    }

    if (significant_count >= 2 && union_init) {
        StackEntry e;
        e.rect = union_r;
        e.name = L"siblings (" + std::to_wstring(significant_count) + L")";
        e.control_type = L"siblings";
        e.from_uia = true;
        append_unique(g_ctx->stack, std::move(e));
    }
}

void append_hwnd_siblings_and_union(HWND self, HWND parent) {
    if (!g_ctx || !self || !parent) return;

    RECT parent_r{};
    const RECT* parent_rp = nullptr;
    if (visible_window_bounds(parent, parent_r))
        parent_rp = &parent_r;

    // Direct children only (EnumChildWindows recurses and floods the stack).
    RECT union_r{};
    bool union_init = false;
    int significant_count = 0;
    int sibling_added = 0;
    constexpr int kMaxSiblings = 16;

    for (HWND child = ::GetWindow(parent, GW_CHILD); child;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
        if (is_picker_chrome(child)) continue;
        if (!::IsWindowVisible(child) && !g_ctx->opts.show_hidden) continue;

        RECT r{};
        if (!visible_window_bounds(child, r) || r.right <= r.left || r.bottom <= r.top)
            continue;

        const bool is_cur = (child == self);
        if (!is_cur && !sibling_rect_significant(r, parent_rp)) continue;

        inflate_union(union_r, r, union_init);
        ++significant_count;

        if (!is_cur && sibling_added < kMaxSiblings) {
            StackEntry e;
            e.rect = r;
            e.name = title_of(child);
            if (e.name.empty()) e.name = class_of(child);
            e.class_name = class_of(child);
            e.control_type = L"window";
            append_unique(g_ctx->stack, std::move(e));
            ++sibling_added;
        }
    }

    if (significant_count >= 2 && union_init) {
        StackEntry e;
        e.rect = union_r;
        e.name = L"siblings (" + std::to_wstring(significant_count) + L")";
        e.control_type = L"siblings";
        append_unique(g_ctx->stack, std::move(e));
    }
}

void rebuild_stack(POINT pt, bool deep, bool probe_uia) {
    if (!g_ctx) return;
    g_ctx->stack.clear();
    g_ctx->stack_index = 0;
    g_ctx->stack_pt = pt;

    const bool uia_deep = deep;
    g_ctx->stack_deep = uia_deep;
    g_ctx->stack_uia_resolved = !g_ctx->opts.resolve_uia || probe_uia;

    if (probe_uia && g_ctx->uia && g_ctx->opts.resolve_uia) {
        IUIAutomationElement* cur = uia_element_at(g_ctx->uia, pt, ignore_hwnd_for_uia());
        if (!uia_deep) {
            if (cur) {
                RECT r{};
                if (SUCCEEDED(cur->get_CurrentBoundingRectangle(&r)) &&
                    r.right > r.left && r.bottom > r.top && ::PtInRect(&r, pt)) {
                    append_unique(g_ctx->stack, entry_from_uia(cur, r));
                }
                cur->Release();
            }
        } else {
            IUIAutomationTreeWalker* walker = nullptr;
            if (cur)
                (void)g_ctx->uia->get_RawViewWalker(&walker);
            for (int depth = 0; cur && depth < 16; ++depth) {
                UIA_HWND nh = nullptr;
                if (SUCCEEDED(cur->get_CurrentNativeWindowHandle(&nh)) &&
                    is_picker_chrome(reinterpret_cast<HWND>(nh))) {
                    IUIAutomationElement* parent = nullptr;
                    if (walker && SUCCEEDED(walker->GetParentElement(cur, &parent)) && parent) {
                        cur->Release();
                        cur = parent;
                        continue;
                    }
                    break;
                }
                RECT r{};
                if (SUCCEEDED(cur->get_CurrentBoundingRectangle(&r)) &&
                    r.right > r.left && r.bottom > r.top && ::PtInRect(&r, pt)) {
                    append_unique(g_ctx->stack, entry_from_uia(cur, r));
                }
                IUIAutomationElement* parent = nullptr;
                if (!walker || FAILED(walker->GetParentElement(cur, &parent)) || !parent) {
                    cur->Release();
                    cur = nullptr;
                    break;
                }
                if (g_ctx->opts.siblings)
                    append_uia_siblings_and_union(g_ctx->uia, walker, cur, parent);
                cur->Release();
                cur = parent;
            }
            if (walker) walker->Release();
            if (cur) cur->Release();
        }
    }

    HWND hwnd = hwnd_at_point(pt);
    const HWND leaf_hwnd = hwnd;
    if (!uia_deep) {
        if (hwnd && !is_picker_chrome(hwnd)) {
            RECT r{};
            if (visible_window_bounds(hwnd, r) && ::PtInRect(&r, pt)) {
                StackEntry e;
                e.rect = r;
                e.name = title_of(hwnd);
                if (e.name.empty()) e.name = class_of(hwnd);
                e.class_name = class_of(hwnd);
                e.control_type = L"window";
                append_unique(g_ctx->stack, std::move(e));
            }
        }
        store_uia_hover(pt, leaf_hwnd);
        return;
    }

    for (int depth = 0; hwnd && depth < 12; ++depth) {
        if (!is_picker_chrome(hwnd)) {
            RECT r{};
            if (visible_window_bounds(hwnd, r) && ::PtInRect(&r, pt)) {
                StackEntry e;
                e.rect = r;
                e.name = title_of(hwnd);
                if (e.name.empty()) e.name = class_of(hwnd);
                e.class_name = class_of(hwnd);
                e.control_type = L"window";
                append_unique(g_ctx->stack, std::move(e));
            }
        }
        HWND next = ::GetParent(hwnd);
        if (!next) next = ::GetWindow(hwnd, GW_OWNER);
        if (!next || next == hwnd) break;
        if (g_ctx->opts.siblings)
            append_hwnd_siblings_and_union(hwnd, next);
        hwnd = next;
    }
    store_uia_hover(pt, leaf_hwnd);
}

const StackEntry* current_stack_entry() {
    if (!g_ctx || g_ctx->stack.empty()) return nullptr;
    if (g_ctx->stack_index < 0) g_ctx->stack_index = 0;
    if (g_ctx->stack_index >= static_cast<int>(g_ctx->stack.size()))
        g_ctx->stack_index = static_cast<int>(g_ctx->stack.size()) - 1;
    return &g_ctx->stack[static_cast<std::size_t>(g_ctx->stack_index)];
}

void cycle_stack(int delta) {
    if (!g_ctx || g_ctx->stack.empty()) return;
    const int n = static_cast<int>(g_ctx->stack.size());
    g_ctx->stack_index = (g_ctx->stack_index + delta % n + n) % n;
}

// Resolve the rectangle to highlight under the cursor: selected stack entry when
// available, else deepest UIA / HWND rect.
bool resolve_target_rect(POINT pt, RECT& out) {
    const HWND clip_hwnd = (g_ctx && g_ctx->hwnd_at.valid) ? g_ctx->hwnd_at.hwnd : nullptr;
    if (g_ctx && !g_ctx->stack.empty()) {
        if (const StackEntry* e = current_stack_entry()) {
            out = e->rect;
            clip_pick_rect_to_point_monitor(out, pt, clip_hwnd);
            return true;
        }
    }
    if (g_ctx && g_ctx->uia && g_ctx->opts.resolve_uia) {
        IUIAutomationElement* el = uia_element_at(g_ctx->uia, pt, ignore_hwnd_for_uia());
        if (el) {
            RECT r{};
            const bool ok = SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) &&
                            r.right > r.left && r.bottom > r.top;
            el->Release();
            if (ok) {
                out = r;
                clip_pick_rect_to_point_monitor(out, pt, clip_hwnd);
                return true;
            }
        }
    }
    const HWND hwnd = hwnd_at_point(pt);
    if (hwnd && !is_picker_chrome(hwnd) && visible_window_bounds(hwnd, out)) {
        clip_pick_rect_to_point_monitor(out, pt, hwnd);
        return true;
    }
    return false;
}

POINT nearest_on_rect_boundary(POINT p, const RECT& r) {
    if (r.right <= r.left || r.bottom <= r.top) return p;

    auto clampi = [](int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };

    const int left = r.left, right = r.right, top = r.top, bottom = r.bottom;
    if (p.x >= left && p.x <= right && p.y >= top && p.y <= bottom) {
        const int dl = p.x - left, dr = right - p.x;
        const int dt = p.y - top, db = bottom - p.y;
        int min_d = dl;
        POINT out{left, p.y};
        if (dr < min_d) { min_d = dr; out = POINT{right, p.y}; }
        if (dt < min_d) { min_d = dt; out = POINT{p.x, top}; }
        if (db < min_d) out = POINT{p.x, bottom};
        return out;
    }

    return POINT{clampi(p.x, left, right), clampi(p.y, top, bottom)};
}

POINT measure_snap_point(POINT p) {
    if (!g_ctx || !g_ctx->measure_snap_valid) return p;
    return nearest_on_rect_boundary(p, g_ctx->measure_snap_rect);
}

void refresh_measure_snap_target() {
    if (!g_ctx || !measure_mode()) return;
    g_ctx->measure_snap_valid = false;
    RECT r{};
    if (resolve_target_rect(g_ctx->cur_pt, r)) {
        g_ctx->measure_snap_rect = r;
        g_ctx->measure_snap_valid = true;
    }
}

RECT rect_from_points(POINT a, POINT b) {
    RECT r{
        a.x < b.x ? a.x : b.x,
        a.y < b.y ? a.y : b.y,
        a.x > b.x ? a.x : b.x,
        a.y > b.y ? a.y : b.y,
    };
    if (r.right == r.left) ++r.right;
    if (r.bottom == r.top) ++r.bottom;
    return r;
}

// Click vs drag: a press that stays inside this slop is a window/UIA pick;
// beyond it, the same gesture becomes a free rectangle (Shift/Ctrl still
// start the rect immediately).
constexpr int kCustomRectSlopPx = 6;

bool beyond_custom_rect_slop(POINT a, POINT b) noexcept {
    const int dx = a.x - b.x;
    const int dy = a.y - b.y;
    return dx * dx + dy * dy >= kCustomRectSlopPx * kCustomRectSlopPx;
}

bool custom_rect_too_small(const RECT& r) noexcept {
    return (r.right - r.left) < kCustomRectSlopPx &&
           (r.bottom - r.top) < kCustomRectSlopPx;
}

void begin_custom_rect_drag() {
    if (!g_ctx) return;
    g_ctx->custom_rect = rect_from_points(g_ctx->drag_start, g_ctx->cur_pt);
    g_ctx->custom_dragging = true;
    if (g_ctx->hud.valid())
        g_ctx->hud.hide();
}

POINT rect_center(const RECT& r) {
    return POINT{r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2};
}

void measure_metrics(POINT a, POINT b, int& dx, int& dy, double& dist) {
    dx = b.x - a.x;
    dy = b.y - a.y;
    dist = std::hypot(static_cast<double>(dx), static_cast<double>(dy));
}

std::wstring measure_hud_line(POINT a, POINT b) {
    int dx = 0, dy = 0;
    double dist = 0;
    measure_metrics(a, b, dx, dy, dist);
    return L"Δ" + std::to_wstring(dx) + L"×" + std::to_wstring(dy) +
           L"  ·  dist " + std::to_wstring(static_cast<long long>(std::llround(dist))) +
           L" px";
}

std::wstring truncate_label(std::wstring s, std::size_t max_chars) {
    if (s.size() <= max_chars) return s;
    if (max_chars <= 1) return L"…";
    s.resize(max_chars - 1);
    s.push_back(L'…');
    return s;
}

#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
bool hud_stack_covers_host(const RECT& stack, HWND host) {
    if (!host) return false;
    RECT hr{};
    if (!visible_window_bounds(host, hr) || hr.right <= hr.left || hr.bottom <= hr.top)
        return false;
    constexpr int kPad = 8;
    return stack.left <= hr.left + kPad && stack.top <= hr.top + kPad
        && stack.right >= hr.right - kPad && stack.bottom >= hr.bottom - kPad;
}

// Cheap HUD source. Prefer the session HWND cache so we do not OpenProcess /
// EnumChildWindows on every timer tick.
std::wstring hud_dom_prefix(HWND hwnd, const RECT* stack_rect) {
    if (!g_ctx || !g_ctx->opts.chrome_dom)
        return {};
    HWND host = nullptr;
    bool chrome = false;
    if (g_ctx->hwnd_at.valid && g_ctx->hwnd_at.hwnd == hwnd) {
        host = g_ctx->hwnd_at.webview_host;
        chrome = g_ctx->hwnd_at.chrome;
    }
    if (host) {
        if (stack_rect && hud_stack_covers_host(*stack_rect, host))
            return L"web view : page";
        return L"web view :";
    }
    if (chrome)
        return L"chrome :";
    return {};
}

std::wstring with_hud_prefix(std::wstring prefix, std::wstring rest) {
    if (prefix.empty())
        return rest;
    if (rest.empty())
        return prefix.back() == L':' ? prefix.substr(0, prefix.size() - 1) : prefix;
    if (prefix.back() == L':')
        return prefix + L" " + rest;
    return prefix + L"  ·  " + rest;
}
#endif

void update_measure_hud() {
    if (!g_ctx || !g_ctx->hud.valid() || !measure_mode()) return;

    std::wstring secondary;
    if (!g_ctx->measure_has_a) {
        secondary = L"snap boundary A  ·  Alt+wheel / [ ] stack  ·  Esc";
    } else {
        const POINT probe =
            g_ctx->measure_committed ? g_ctx->measure_b : g_ctx->measure_probe;
        secondary = measure_hud_line(g_ctx->measure_a, probe);
        if (!g_ctx->measure_committed)
            secondary += L"  ·  snap B  ·  Backspace undo A";
        else
            secondary += L"  ·  C copy";
    }
    if (g_ctx->opts.magnifier)
        secondary += L"  ·  Z loupe";

    const StackEntry* e = current_stack_entry();
    if (e) {
        const int n = static_cast<int>(g_ctx->stack.size());
        const int idx = g_ctx->stack_index + 1;
        std::wstring label = e->name.empty()
            ? (e->control_type.empty() ? e->class_name : e->control_type)
            : e->name;
        if (label.empty()) label = L"(unnamed)";
        label = truncate_label(std::move(label), 48);
        std::wstring primary =
            L"measure  ·  " + std::to_wstring(idx) + L"/" + std::to_wstring(n) +
            L"  ·  " + label;
        const RECT& r = e->rect;
        primary += L"  ·  " + std::to_wstring(r.right - r.left) + L"×" +
                   std::to_wstring(r.bottom - r.top);
#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
        const HWND hwnd = cached_point_hwnd();
        primary = with_hud_prefix(hud_dom_prefix(hwnd, &r), std::move(primary));
        if (g_ctx->opts.chrome_dom && !g_ctx->chrome_preview_css.empty()) {
            secondary = (g_ctx->chrome_preview_valid ? L"css " : L"") +
                        g_ctx->chrome_preview_css + L"  ·  " + secondary;
        }
#endif
        g_ctx->hud.show_near(g_ctx->cur_pt, primary, secondary);
        return;
    }
#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
    {
        const HWND hwnd = cached_point_hwnd();
        const std::wstring prefix = hud_dom_prefix(hwnd, nullptr);
        if (!prefix.empty() || !g_ctx->chrome_preview_css.empty()) {
            std::wstring sec = secondary;
            if (g_ctx->opts.chrome_dom && !g_ctx->chrome_preview_css.empty())
                sec = g_ctx->chrome_preview_css + L"  ·  " + sec;
            g_ctx->hud.show_near(g_ctx->cur_pt,
                                 with_hud_prefix(prefix, L"measure"),
                                 sec);
            return;
        }
    }
#endif
    g_ctx->hud.show_near(g_ctx->cur_pt, L"measure", secondary);
}

std::wstring measure_clipboard_text(POINT a, POINT b) {
    int dx = 0, dy = 0;
    double dist = 0;
    measure_metrics(a, b, dx, dy, dist);
    return std::to_wstring(a.x) + L"," + std::to_wstring(a.y) + L"→" +
           std::to_wstring(b.x) + L"," + std::to_wstring(b.y) + L" Δ" +
           std::to_wstring(dx) + L"×" + std::to_wstring(dy) + L" dist=" +
           std::to_wstring(static_cast<long long>(std::llround(dist)));
}

std::wstring trim_w(std::wstring value) {
    auto not_space = [](wchar_t c) {
        return c != L' ' && c != L'\t' && c != L'\r' && c != L'\n';
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

constexpr int kUiaTextHardCap = 256 * 1024;

int uia_text_fetch_limit(int max_chars) {
    if (max_chars <= 0) return kUiaTextHardCap;
    return max_chars > kUiaTextHardCap ? kUiaTextHardCap : max_chars;
}

bool seh_uia_text_range_get_text(IUIAutomationTextRange* range, int limit, BSTR* out) {
    if (!range || !out) return false;
    *out = nullptr;
    __try {
        return SUCCEEDED(range->GetText(limit, out)) && *out;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

bool seh_uia_value_get_current(IUIAutomationValuePattern* vp, BSTR* out) {
    if (!vp || !out) return false;
    *out = nullptr;
    __try {
        return SUCCEEDED(vp->get_CurrentValue(out)) && *out;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

bool seh_uia_legacy_get_current(IUIAutomationLegacyIAccessiblePattern* lp, BSTR* out) {
    if (!lp || !out) return false;
    *out = nullptr;
    __try {
        return SUCCEEDED(lp->get_CurrentValue(out)) && *out;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

bool seh_uia_get_name(IUIAutomationElement* el, BSTR* out) {
    if (!el || !out) return false;
    *out = nullptr;
    __try {
        return SUCCEEDED(el->get_CurrentName(out)) && *out;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

std::wstring bstr_to_trimmed_w(BSTR value) {
    if (!value) return {};
    std::wstring out(value, ::SysStringLen(value));
    ::SysFreeString(value);
    return trim_w(std::move(out));
}

std::wstring uia_element_spoken_text(IUIAutomationElement* el, int max_chars) {
    if (!el) return {};

    const int text_limit = uia_text_fetch_limit(max_chars);

    IUIAutomationTextPattern* rawtp = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void**>(&rawtp))) &&
        rawtp) {
        IUIAutomationTextRange* rawdoc = nullptr;
        if (SUCCEEDED(rawtp->get_DocumentRange(&rawdoc)) && rawdoc) {
            BSTR text = nullptr;
            std::wstring out;
            if (seh_uia_text_range_get_text(rawdoc, text_limit, &text)) {
                out = bstr_to_trimmed_w(text);
            }
            rawdoc->Release();
            if (!out.empty()) {
                rawtp->Release();
                return out;
            }
        }
        rawtp->Release();
    }

    IUIAutomationValuePattern* rawvp = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern),
            reinterpret_cast<void**>(&rawvp))) &&
        rawvp) {
        BSTR value = nullptr;
        std::wstring out;
        if (seh_uia_value_get_current(rawvp, &value)) {
            out = bstr_to_trimmed_w(value);
        }
        rawvp->Release();
        if (!out.empty()) return out;
    }

    IUIAutomationLegacyIAccessiblePattern* rawlp = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_LegacyIAccessiblePatternId,
            __uuidof(IUIAutomationLegacyIAccessiblePattern),
            reinterpret_cast<void**>(&rawlp))) &&
        rawlp) {
        BSTR value = nullptr;
        std::wstring out;
        if (seh_uia_legacy_get_current(rawlp, &value)) {
            out = bstr_to_trimmed_w(value);
        }
        rawlp->Release();
        if (!out.empty()) return out;
    }

    BSTR name = nullptr;
    if (seh_uia_get_name(el, &name))
        return bstr_to_trimmed_w(name);
    return {};
}

bool spoken_text_from_pick_impl(const PickResult& pick,
                                std::wstring& out,
                                std::string& err,
                                int max_chars) {
    out.clear();
    if (!pick.uia_name.empty())
        out = trim_w(pick.uia_name);

    // Re-query UIA only when we need bulk text (max_chars==0) or have no name yet.
    const bool need_uia_rich = out.empty() || max_chars == 0;
    if (need_uia_rich) {
        IUIAutomation* uia = nullptr;
        if (SUCCEEDED(::CoCreateInstance(
                __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia))) &&
            uia) {
            IUIAutomationElement* el = uia_element_at(uia, pick.point, nullptr);
            if (el) {
                std::wstring richer = uia_element_spoken_text(el, max_chars);
                el->Release();
                if (!richer.empty()) out = std::move(richer);
            }
            uia->Release();
        }
    }

    if (out.empty() && !pick.title.empty())
        out = trim_w(pick.title);
    if (out.empty()) {
        err = "no speakable text at pick target";
        return false;
    }
    return true;
}

std::wstring hex_w(COLORREF c) {
    static const wchar_t* digits = L"0123456789ABCDEF";
    const int comp[3] = {GetRValue(c), GetGValue(c), GetBValue(c)};
    std::wstring out = L"#";
    for (int v : comp) {
        out.push_back(digits[(v >> 4) & 0xF]);
        out.push_back(digits[v & 0xF]);
    }
    return out;
}

#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
std::wstring utf8_to_wide(const std::string& value) {
    return media::process::widen_utf8(value);
}

std::wstring chrome_element_preview_label(const nlohmann::json& element) {
    const nlohmann::json& sel =
        element.value("selectors", nlohmann::json::object());
    const std::string css = sel.value("css", std::string{});
    if (!css.empty())
        return truncate_label(utf8_to_wide(css), 56);
    std::string tag = element.value("tag", std::string{});
    const std::string id = element.value("id", std::string{});
    if (!id.empty()) tag += "#" + id;
    if (tag.empty()) tag = element.value("role", std::string{"element"});
    return truncate_label(utf8_to_wide(tag), 48);
}

int json_px(const nlohmann::json& o, const char* a, const char* b = nullptr)
{
    auto num = [](const nlohmann::json& v) -> int {
        if (v.is_number_integer())
            return v.get<int>();
        if (v.is_number_float())
            return static_cast<int>(std::lround(v.get<double>()));
        return 0;
    };
    if (o.contains(a) && o[a].is_number())
        return num(o[a]);
    if (b && o.contains(b) && o[b].is_number())
        return num(o[b]);
    return 0;
}

bool chrome_bounds_on_screen(const nlohmann::json& element, HWND host, RECT& out)
{
    if (!host || !element.contains("bounds") || !element["bounds"].is_object())
        return false;
    const auto& b = element["bounds"];
    const int left = json_px(b, "left", "x");
    const int top = json_px(b, "top", "y");
    int right = json_px(b, "right");
    int bottom = json_px(b, "bottom");
    if (right <= left)
        right = left + json_px(b, "width");
    if (bottom <= top)
        bottom = top + json_px(b, "height");
    if (right <= left || bottom <= top)
        return false;
    POINT tl{left, top};
    POINT br{right, bottom};
    if (!::ClientToScreen(host, &tl) || !::ClientToScreen(host, &br))
        return false;
    out = RECT{tl.x, tl.y, br.x, br.y};
    return out.right > out.left && out.bottom > out.top;
}

void apply_chrome_hover_hit(const ChromeHoverHit& hit)
{
    g_ctx->chrome_preview_css = hit.css;
    g_ctx->chrome_preview_tag = hit.tag;
    g_ctx->chrome_preview_valid = hit.ok;
    g_ctx->chrome_probe_pt = g_ctx->cur_pt;
    g_ctx->chrome_probe_pt_valid = true;
}

const ChromeHoverHit* find_chrome_hover_hit(HWND host, POINT pt)
{
    if (!g_ctx || !host)
        return nullptr;
    const int qx = chrome_cell(pt.x);
    const int qy = chrome_cell(pt.y);
    const DWORD now = ::GetTickCount();
    auto usable = [&](const ChromeHoverHit& h) {
        return h.ok || (now - h.tick) < kChromeMissRetryMs;
    };
    const ChromeHoverHit* cell = nullptr;
    for (auto it = g_ctx->chrome_hover.rbegin(); it != g_ctx->chrome_hover.rend(); ++it) {
        if (it->host != host)
            continue;
        if (it->bounds_ok && pick_rect_is_tight(it->bounds, pt)
            && ::PtInRect(&it->bounds, pt)) {
            if (usable(*it))
                return &*it;
            continue;
        }
        if (!cell && it->qx == qx && it->qy == qy && usable(*it))
            cell = &*it;
    }
    return cell;
}

void store_chrome_hover_hit(ChromeHoverHit hit)
{
    if (!g_ctx)
        return;
    hit.tick = ::GetTickCount();
    if (hit.bounds_ok) {
        for (auto& old : g_ctx->chrome_hover) {
            if (old.host == hit.host && old.bounds_ok && same_rect(old.bounds, hit.bounds)) {
                old = std::move(hit);
                return;
            }
        }
    }
    for (auto& old : g_ctx->chrome_hover) {
        if (old.host == hit.host && old.qx == hit.qx && old.qy == hit.qy) {
            old = std::move(hit);
            return;
        }
    }
    if (g_ctx->chrome_hover.size() >= kChromeHoverCap)
        g_ctx->chrome_hover.erase(g_ctx->chrome_hover.begin());
    g_ctx->chrome_hover.push_back(std::move(hit));
}

void clear_chrome_preview() {
    if (!g_ctx) return;
    g_ctx->chrome_preview_valid = false;
    g_ctx->chrome_preview_css.clear();
    g_ctx->chrome_preview_tag.clear();
}

void maybe_update_chrome_preview() {
    if (!g_ctx || !g_ctx->opts.chrome_dom || !g_ctx->opts.chrome_preview || eyedropper_mode() ||
        !g_ctx->opts.hud || g_ctx->custom_dragging)
        return;

    const HWND hwnd = cached_point_hwnd();
    HWND host = g_ctx->hwnd_at.valid ? g_ctx->hwnd_at.webview_host : nullptr;
    const bool chrome = g_ctx->hwnd_at.valid && g_ctx->hwnd_at.chrome;
    if (!host && !chrome) {
        clear_chrome_preview();
        return;
    }
    if (!host)
        host = hwnd;

    if (const ChromeHoverHit* hit = find_chrome_hover_hit(host, g_ctx->cur_pt)) {
        apply_chrome_hover_hit(*hit);
        return;
    }

    const DWORD now = ::GetTickCount();
    if ((now - g_ctx->cursor_move_tick) < 150)
        return;

    g_ctx->chrome_probe_tick = now;
    g_ctx->chrome_probe_pt = g_ctx->cur_pt;
    g_ctx->chrome_probe_pt_valid = true;

    media::win::inspect::PickResult stub;
    stub.point = g_ctx->cur_pt;
    stub.hwnd = hwnd;
    stub.root = g_ctx->hwnd_at.root ? g_ctx->hwnd_at.root
                                    : (hwnd ? ::GetAncestor(hwnd, GA_ROOT) : nullptr);
    stub.class_name = g_ctx->hwnd_at.class_name;
    stub.root_class = g_ctx->hwnd_at.root_class;
    stub.title = g_ctx->hwnd_at.title;
    stub.pid = g_ctx->hwnd_at.pid;
    stub.exe = g_ctx->hwnd_at.exe;

    g_ctx->chrome_preview_valid = false;
    g_ctx->chrome_preview_css = L"dom…";
    g_ctx->chrome_preview_tag.clear();

    media::win::chrome_inspect::DomInspectOpts preview_opts;
    preview_opts.scope = "element";
    preview_opts.html_max_chars = 64;
    preview_opts.markdown_max_chars = 64;
    preview_opts.text_max_chars = 256;

    media::win::chrome_inspect::ChromePickEnrichResult dom;
    std::string err;
    ChromeHoverHit hit;
    hit.host = host;
    hit.qx = chrome_cell(g_ctx->cur_pt.x);
    hit.qy = chrome_cell(g_ctx->cur_pt.y);
    if (!media::win::chrome_inspect::enrich_pick_with_chrome_dom(
            stub, 200, dom, err, preview_opts) ||
        !dom.ok) {
        hit.css = err.empty() ? L"dom…" : truncate_label(utf8_to_wide(err), 40);
        hit.ok = false;
        store_chrome_hover_hit(hit);
        apply_chrome_hover_hit(hit);
        return;
    }

    hit.css = chrome_element_preview_label(dom.element);
    const std::string tag = dom.element.value("tag", std::string{});
    hit.tag = tag.empty() ? L"" : utf8_to_wide(tag);
    hit.ok = true;
    hit.bounds_ok = chrome_bounds_on_screen(dom.element, host, hit.bounds);
    store_chrome_hover_hit(hit);
    apply_chrome_hover_hit(hit);
}
#else
void clear_chrome_preview() {}
void maybe_update_chrome_preview() {}
#endif

void update_hud_for_stack() {
    if (!g_ctx || !g_ctx->hud.valid()) return;
    if (g_ctx->custom_dragging) {
        g_ctx->hud.hide();
        return;
    }
    const StackEntry* e = current_stack_entry();
    const wchar_t* hints = e && static_cast<int>(g_ctx->stack.size()) > 1
        ? L"Alt+wheel / [ ]  leaf·siblings·parent  ·  click commit  ·  drag rect"
        : L"click commit  ·  drag rect  ·  Esc cancel";
    if (!e) {
#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
        const HWND hwnd = cached_point_hwnd();
        const std::wstring prefix = hud_dom_prefix(hwnd, nullptr);
        if (!prefix.empty() || !g_ctx->chrome_preview_css.empty()) {
            std::wstring secondary;
            if (g_ctx->opts.chrome_dom && !g_ctx->chrome_preview_css.empty()) {
                secondary = g_ctx->chrome_preview_valid ? L"css " : L"";
                secondary += g_ctx->chrome_preview_css;
                secondary += L"  ·  ";
            }
            secondary += hints;
            g_ctx->hud.show_near(g_ctx->cur_pt,
                                 prefix.empty() ? std::wstring(L"DOM")
                                                : with_hud_prefix(prefix, {}),
                                 secondary);
            return;
        }
#endif
        g_ctx->hud.hide();
        return;
    }
    const int n = static_cast<int>(g_ctx->stack.size());
    const int idx = g_ctx->stack_index + 1;
    std::wstring label = e->name.empty()
        ? (e->control_type.empty() ? e->class_name : e->control_type)
        : e->name;
    if (label.empty()) label = L"(unnamed)";
    label = truncate_label(std::move(label), 48);
    std::wstring primary =
        std::to_wstring(idx) + L"/" + std::to_wstring(n) + L"  ·  " + label;

    const RECT& r = e->rect;
    primary += L"  ·  " + std::to_wstring(r.right - r.left) + L"×" +
               std::to_wstring(r.bottom - r.top);

    std::wstring secondary;
#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
    const HWND hwnd = cached_point_hwnd();
    primary = with_hud_prefix(hud_dom_prefix(hwnd, &r), std::move(primary));
    if (g_ctx->opts.chrome_dom && !g_ctx->chrome_preview_css.empty()) {
        secondary = g_ctx->chrome_preview_valid ? L"css " : L"";
        secondary += g_ctx->chrome_preview_css;
        if (g_ctx->chrome_preview_valid && !g_ctx->chrome_preview_tag.empty())
            secondary += L"  ·  " + g_ctx->chrome_preview_tag;
    }
#endif
    if (!secondary.empty()) {
        secondary += L"  ·  ";
        secondary += hints;
    } else {
        secondary = hints;
    }
    g_ctx->hud.show_near(g_ctx->cur_pt, primary, secondary);
}

// Live pixel read + loupe repaint. Throttled (~30 Hz) and skipped when the cursor
// has not moved — StretchBlt + GetPixel every mouse hook tick was the main drag.
void sample_update(bool force = false) {
    if (!g_ctx) return;
    const bool live = g_ctx->opts.eyedropper ||
                      (g_ctx->loupe_on && g_ctx->loupe.valid());
    if (!live && !g_ctx->opts.sample_color) return;

    if (!force && g_ctx->sample_pt_valid &&
        g_ctx->sample_pt.x == g_ctx->cur_pt.x &&
        g_ctx->sample_pt.y == g_ctx->cur_pt.y)
        return;

    const DWORD now = ::GetTickCount();
    if (!force && g_ctx->sample_tick != 0 && (now - g_ctx->sample_tick) < 33)
        return;
    g_ctx->sample_tick = now;
    g_ctx->sample_pt = g_ctx->cur_pt;
    g_ctx->sample_pt_valid = true;

    if (g_ctx->opts.eyedropper || g_ctx->opts.sample_color)
        g_ctx->cur_color = sample_screen_color(g_ctx->cur_pt, g_ctx->cur_color_ok);
    if (g_ctx->loupe_on && g_ctx->loupe.valid()) {
        const wchar_t* hint = g_ctx->opts.eyedropper ? L"click · Esc" : nullptr;
        g_ctx->loupe.update(g_ctx->cur_pt, g_ctx->cur_color, g_ctx->cur_color_ok, hint);
    }
}

void measure_update() {
    if (!g_ctx || !measure_mode()) return;

    refresh_hover_stack(g_ctx->cur_pt, false);

    refresh_measure_snap_target();
    g_ctx->measure_probe = measure_snap_point(g_ctx->cur_pt);

    if (!g_ctx->measure_has_a) {
        g_ctx->measure_dim.hide();
        g_ctx->measure_stage.hide();
        RECT r{};
        if (g_ctx->overlay.valid() && resolve_target_rect(g_ctx->cur_pt, r))
            g_ctx->overlay.move_to(r);
    } else {
        if (g_ctx->overlay.valid())
            g_ctx->overlay.hide();
        if (g_ctx->measure_dim.valid())
            g_ctx->measure_dim.move_to(media::win::virtual_screen_rect());

        const POINT probe =
            g_ctx->measure_committed ? g_ctx->measure_b : g_ctx->measure_probe;
        RECT rect_b{};
        const RECT* rect_b_ptr = nullptr;
        if (g_ctx->measure_committed && g_ctx->measure_rect_b_valid) {
            rect_b = g_ctx->measure_rect_b;
            rect_b_ptr = &rect_b;
        } else if (!g_ctx->measure_committed &&
                   resolve_target_rect(g_ctx->cur_pt, rect_b)) {
            rect_b_ptr = &rect_b;
        }

        const RECT* rect_a_ptr =
            g_ctx->measure_rect_a_valid ? &g_ctx->measure_rect_a : nullptr;
        if (g_ctx->measure_stage.valid()) {
            g_ctx->measure_stage.update(true,
                                         g_ctx->measure_a,
                                         probe,
                                         rect_a_ptr,
                                         rect_b_ptr,
                                         !g_ctx->measure_committed);
        }
    }

    maybe_update_chrome_preview();
    update_measure_hud();
}

void eyedropper_update() {
    if (!g_ctx) return;
    // Color hook thread intentionally ignores movement; polling keeps the hook
    // callback constant-time even when GDI rendering is slow.
    ::GetCursorPos(&g_ctx->cur_pt);
    sample_update();
}

void hover_update() {
    if (!g_ctx) return;
    if (eyedropper_mode()) {
        eyedropper_update();
        return;
    }
    if (measure_mode()) {
        measure_update();
        return;
    }
    if (g_ctx->custom_dragging) {
        // Paint here, not in WH_MOUSE_LL — SetWindowPos in the hook stalls
        // all mouse input (very visible over NoMachine).
        if (g_ctx->overlay.valid())
            g_ctx->overlay.move_to(g_ctx->custom_rect);
        g_ctx->hud.hide();
        return;
    }

    refresh_hover_stack(g_ctx->cur_pt, false);

    RECT r{};
    if (g_ctx->overlay.valid() && resolve_target_rect(g_ctx->cur_pt, r))
        g_ctx->overlay.move_to(r);
    maybe_update_chrome_preview();
    update_hud_for_stack();
}

void CALLBACK hover_timer_proc(HWND, UINT, UINT_PTR, DWORD) { hover_update(); }

// ─────────────────────────────────────────────────────────────────────────────
// Low-level hooks
// ─────────────────────────────────────────────────────────────────────────────

void commit_eyedropper(POINT pt) {
    if (!g_ctx) return;
    g_ctx->cur_pt = pt;
    g_ctx->committed_color = sample_screen_color(pt, g_ctx->committed_color_ok);
    g_ctx->committed_rect = RECT{pt.x, pt.y, pt.x + 1, pt.y + 1};
    g_ctx->committed_rect_valid = true;
    g_ctx->committed = true;
}

void commit_current_target(POINT pt) {
    if (!g_ctx) return;
    if (eyedropper_mode()) {
        commit_eyedropper(pt);
        return;
    }
    g_ctx->cur_pt = pt;
    if (g_ctx->stack.empty() ||
        (pt.x - g_ctx->stack_pt.x) * (pt.x - g_ctx->stack_pt.x) +
            (pt.y - g_ctx->stack_pt.y) * (pt.y - g_ctx->stack_pt.y) >= 16 ||
        (!g_ctx->stack_deep && !g_ctx->stack_uia_resolved)) {
        rebuild_stack(pt, false, true);
    }
    if (g_ctx->opts.sample_color && !g_ctx->opts.eyedropper) {
        g_ctx->committed_color = sample_screen_color(pt, g_ctx->committed_color_ok);
    }
    if (const StackEntry* e = current_stack_entry()) {
        g_ctx->committed_rect = e->rect;
        clip_pick_rect_to_point_monitor(g_ctx->committed_rect, pt,
                                        g_ctx->hwnd_at.valid ? g_ctx->hwnd_at.hwnd : nullptr);
        g_ctx->committed_rect_valid = true;
        g_ctx->committed_entry = *e;
        g_ctx->committed_entry.rect = g_ctx->committed_rect;
        g_ctx->committed_entry_valid = true;
    } else {
        RECT r{};
        if (resolve_target_rect(pt, r)) {
            clip_pick_rect_to_point_monitor(r, pt,
                                            g_ctx->hwnd_at.valid ? g_ctx->hwnd_at.hwnd : nullptr);
            g_ctx->committed_rect = r;
            g_ctx->committed_rect_valid = true;
        }
    }
    g_ctx->committed = true;
}

// Color mode keeps its low-level hooks on a dedicated thread. A slow GDI
// magnifier paint can then delay only the picker UI, never global mouse input.
LRESULT CALLBACK color_mouse_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const DWORD target = g_color_ui_thread.load(std::memory_order_acquire);
        if (target != 0) {
            const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            if (wp == WM_LBUTTONDOWN) {
                g_color_click_x.store(ms->pt.x, std::memory_order_relaxed);
                g_color_click_y.store(ms->pt.y, std::memory_order_relaxed);
                ::PostThreadMessageW(target, kColorCommitMessage, 0, 0);
                return 1;
            }
            if (wp == WM_LBUTTONUP) {
                ::PostThreadMessageW(target, kColorFinishMessage, 0, 0);
                return 1;
            }
            if (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP)
                return 1;
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK color_keyboard_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        const DWORD target = g_color_ui_thread.load(std::memory_order_acquire);
        if (target != 0) {
            const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
            if (kb->vkCode == VK_ESCAPE || kb->vkCode == 'Z' || kb->vkCode == 'C') {
                ::PostThreadMessageW(target, kColorKeyMessage, kb->vkCode, 0);
                return 1;
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK mouse_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_ctx) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        switch (wp) {
        case WM_MOUSEMOVE:
            g_ctx->cur_pt = ms->pt;
            g_ctx->cursor_move_tick = ::GetTickCount();
            if (!eyedropper_mode() && !measure_mode()) {
                if (g_ctx->custom_dragging) {
                    g_ctx->custom_rect = rect_from_points(g_ctx->drag_start, ms->pt);
                } else if (g_ctx->waiting_up && !g_ctx->committed &&
                           beyond_custom_rect_slop(g_ctx->drag_start, ms->pt)) {
                    begin_custom_rect_drag();
                }
            }
            break;
        case WM_MOUSEWHEEL:
            if (eyedropper_mode()) break;
            if (!g_ctx->custom_dragging && (::GetAsyncKeyState(VK_MENU) & 0x8000)) {
                g_ctx->cur_pt = ms->pt;
                if (!g_ctx->stack_deep)
                    refresh_hover_stack(ms->pt, true);
                const short delta = static_cast<short>(HIWORD(ms->mouseData));
                cycle_stack(delta > 0 ? -1 : 1);
                if (measure_mode()) {
                    measure_update();
                } else {
                    RECT r{};
                    if (resolve_target_rect(ms->pt, r) && g_ctx->overlay.valid())
                        g_ctx->overlay.move_to(r);
                    update_hud_for_stack();
                }
                return 1;
            }
            break;
        case WM_LBUTTONDOWN:
            g_ctx->cur_pt = ms->pt;
            if (eyedropper_mode()) {
                commit_eyedropper(ms->pt);
                g_ctx->waiting_up = true;
                return 1;
            }
            if (measure_mode()) {
                if (!g_ctx->stack_deep && !g_ctx->stack_uia_resolved)
                    rebuild_stack(ms->pt, false, true);
                refresh_measure_snap_target();
                const POINT snap = measure_snap_point(ms->pt);
                RECT target{};
                const bool have_target = resolve_target_rect(ms->pt, target);
                if (!g_ctx->measure_has_a) {
                    g_ctx->measure_a = snap;
                    g_ctx->measure_has_a = true;
                    if (have_target) {
                        g_ctx->measure_rect_a = target;
                        g_ctx->measure_rect_a_valid = true;
                    } else {
                        g_ctx->measure_rect_a_valid = false;
                    }
                    measure_update();
                } else {
                    g_ctx->measure_b = snap;
                    g_ctx->measure_committed = true;
                    g_ctx->committed = true;
                    g_ctx->waiting_up = true;
                    if (have_target) {
                        g_ctx->measure_rect_b = target;
                        g_ctx->measure_rect_b_valid = true;
                    } else {
                        g_ctx->measure_rect_b_valid = false;
                    }
                    measure_update();
                }
                return 1;
            }
            // Press: wait for up. Drag past slop → free rect. Shift/Ctrl start
            // the rect immediately (screen:N or rrect=).
            g_ctx->drag_start = ms->pt;
            g_ctx->waiting_up = true;
            if ((::GetAsyncKeyState(VK_SHIFT) & 0x8000) ||
                (::GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
                begin_custom_rect_drag();
            }
            return 1;  // swallow the click so the target app is not actually clicked
        case WM_LBUTTONUP:
            if (g_ctx->custom_dragging) {
                g_ctx->cur_pt = ms->pt;
                g_ctx->custom_rect = rect_from_points(g_ctx->drag_start, ms->pt);
                g_ctx->custom_dragging = false;
                if (custom_rect_too_small(g_ctx->custom_rect)) {
                    commit_current_target(g_ctx->drag_start);
                } else {
                    g_ctx->custom_committed = true;
                    g_ctx->committed = true;
                }
                g_ctx->waiting_up = false;
                ::PostQuitMessage(0);
                return 1;
            }
            if (g_ctx->waiting_up) {
                if (!g_ctx->committed)
                    commit_current_target(ms->pt);
                g_ctx->waiting_up = false;
                ::PostQuitMessage(0);
                return 1;
            }
            break;
        case WM_RBUTTONDOWN:
            if (eyedropper_mode() || measure_mode())
                return 1;
            // Right-click also commits at the current stack selection (swallowed).
            commit_current_target(ms->pt);
            ::PostQuitMessage(0);
            return 1;
        default:
            break;
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

void toggle_picker_loupe() {
    if (!g_ctx) return;
    g_ctx->loupe_on = !g_ctx->loupe_on;
    if (g_ctx->loupe_on) {
        if (!g_ctx->loupe.valid() && !g_ctx->loupe.create())
            g_ctx->loupe_on = false;
        else
            g_ctx->loupe.update(g_ctx->cur_pt, g_ctx->cur_color, g_ctx->cur_color_ok);
    } else {
        g_ctx->loupe.hide();
    }
}

LRESULT CALLBACK kbd_hook_proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_ctx && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (kb->vkCode == VK_ESCAPE) {
            g_ctx->cancelled = true;
            ::PostQuitMessage(0);
            return 1;
        }
        // Z toggles the magnifier; C copies colour or measure.
        if (kb->vkCode == 'Z' &&
            (region_mode() || measure_mode() || eyedropper_mode())) {
            toggle_picker_loupe();
            return 1;
        }
        if (kb->vkCode == 'C' && measure_mode() && g_ctx->measure_has_a) {
            const POINT b =
                g_ctx->measure_committed ? g_ctx->measure_b : g_ctx->measure_probe;
            if (!copy_text_to_clipboard(measure_clipboard_text(g_ctx->measure_a, b)))
                logger::warn("[picker] clipboard copy failed for measure");
            return 1;
        }
        if (kb->vkCode == 'C' && g_ctx->cur_color_ok && !measure_mode()) {
            if (!copy_text_to_clipboard(hex_w(g_ctx->cur_color)))
                logger::warn("[picker] clipboard copy failed for sampled color");
            return 1;
        }
        if (kb->vkCode == VK_BACK && measure_mode() && g_ctx->measure_has_a &&
            !g_ctx->measure_committed) {
            g_ctx->measure_has_a = false;
            g_ctx->measure_rect_a_valid = false;
            g_ctx->measure_dim.hide();
            g_ctx->measure_stage.hide();
            measure_update();
            return 1;
        }
        if (eyedropper_mode()) return 0;
        // [ / ] cycle stack without Alt (handy on laptops).
        if (!g_ctx->custom_dragging &&
            (kb->vkCode == VK_OEM_4 || kb->vkCode == VK_OEM_6)) {
            if (!g_ctx->stack_deep)
                refresh_hover_stack(g_ctx->cur_pt, true);
            cycle_stack(kb->vkCode == VK_OEM_4 ? -1 : 1);
            if (measure_mode()) {
                measure_update();
            } else {
                RECT r{};
                if (resolve_target_rect(g_ctx->cur_pt, r) && g_ctx->overlay.valid())
                    g_ctx->overlay.move_to(r);
                update_hud_for_stack();
            }
            return 1;
        }
        if (measure_mode()) return 0;
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

} // namespace

std::string color_to_hex(COLORREF color, bool ok) {
    if (!ok) return {};
    static const char* digits = "0123456789ABCDEF";
    const int comp[3] = {GetRValue(color), GetGValue(color), GetBValue(color)};
    std::string out = "#";
    for (int v : comp) {
        out.push_back(digits[(v >> 4) & 0xF]);
        out.push_back(digits[v & 0xF]);
    }
    return out;
}

std::string measure_to_string(const PickResult& pick) {
    if (!pick.measure_ok) return {};
    const int dx = pick.measure_b.x - pick.measure_a.x;
    const int dy = pick.measure_b.y - pick.measure_a.y;
    const double dist =
        std::hypot(static_cast<double>(dx), static_cast<double>(dy));
    return std::to_string(pick.measure_a.x) + "," +
           std::to_string(pick.measure_a.y) + "->" +
           std::to_string(pick.measure_b.x) + "," +
           std::to_string(pick.measure_b.y) + " d=" +
           std::to_string(static_cast<long long>(std::llround(dist))) +
           " dx=" + std::to_string(dx) + " dy=" + std::to_string(dy);
}

bool spoken_text_from_pick(const PickResult& pick,
                           std::wstring& out,
                           std::string& err,
                           int max_chars) {
    const HRESULT coi = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool need_uninit = (coi == S_OK || coi == S_FALSE);
    const bool ok = spoken_text_from_pick_impl(pick, out, err, max_chars);
    if (need_uninit) ::CoUninitialize();
    return ok;
}

namespace {

std::wstring process_basename(const std::wstring& exe) {
    const auto pos = exe.find_last_of(L"\\/");
    return pos == std::wstring::npos ? exe : exe.substr(pos + 1);
}

std::string w_to_u8(const std::wstring& ws) {
    return media::process::narrow_utf8(ws);
}

IUIAutomation* create_uia() {
    IUIAutomation* uia = nullptr;
    HRESULT hr = ::CoCreateInstance(
        __uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    if (FAILED(hr) || !uia) {
        hr = ::CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
    }
    return SUCCEEDED(hr) ? uia : nullptr;
}

std::wstring text_from_pattern(IUIAutomationTextPattern* tp, int max_chars) {
    if (!tp) return {};
    IUIAutomationTextRangeArray* rawsel = nullptr;
    if (FAILED(tp->GetSelection(&rawsel)) || !rawsel)
        return {};
    int count = 0;
    rawsel->get_Length(&count);
    std::wstring result;
    const int limit = uia_text_fetch_limit(max_chars);
    for (int i = 0; i < count; ++i) {
        IUIAutomationTextRange* rawrange = nullptr;
        if (FAILED(rawsel->GetElement(i, &rawrange)) || !rawrange)
            continue;
        BSTR text = nullptr;
        if (seh_uia_text_range_get_text(rawrange, limit, &text) && text) {
            std::wstring piece(text, ::SysStringLen(text));
            ::SysFreeString(text);
            if (!piece.empty()) {
                if (!result.empty())
                    result += L"\n";
                result += piece;
            }
        }
        rawrange->Release();
    }
    rawsel->Release();
    return result;
}

IUIAutomationTextPattern* text_pattern_of(IUIAutomationElement* el) {
    if (!el) return nullptr;
    IUIAutomationTextPattern* tp = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_TextPatternId, __uuidof(IUIAutomationTextPattern),
            reinterpret_cast<void**>(&tp))) &&
        tp)
        return tp;
#ifdef UIA_TextPattern2Id
    IUIAutomationTextPattern2* tp2 = nullptr;
    if (SUCCEEDED(el->GetCurrentPatternAs(
            UIA_TextPattern2Id, __uuidof(IUIAutomationTextPattern2),
            reinterpret_cast<void**>(&tp2))) &&
        tp2) {
        IUIAutomationTextPattern* as_tp = nullptr;
        if (SUCCEEDED(tp2->QueryInterface(__uuidof(IUIAutomationTextPattern),
                                          reinterpret_cast<void**>(&as_tp))) &&
            as_tp) {
            tp2->Release();
            return as_tp;
        }
        tp2->Release();
    }
#endif
    return nullptr;
}

IUIAutomationTextPattern* text_pattern_walk(IUIAutomation* uia, IUIAutomationElement* start) {
    if (!uia || !start) return nullptr;
    if (IUIAutomationTextPattern* tp = text_pattern_of(start))
        return tp;

    IUIAutomationTreeWalker* walker = nullptr;
    if (FAILED(uia->get_RawViewWalker(&walker)) || !walker)
        return nullptr;

    IUIAutomationElement* cur = start;
    cur->AddRef();
    IUIAutomationTextPattern* found = nullptr;
    for (int depth = 0; depth < 12 && cur; ++depth) {
        IUIAutomationElement* parent = nullptr;
        if (FAILED(walker->GetParentElement(cur, &parent)) || !parent)
            break;
        cur->Release();
        cur = parent;
        found = text_pattern_of(cur);
        if (found)
            break;
    }
    if (cur) cur->Release();
    walker->Release();
    return found;
}

DWORD hwnd_pid(HWND hwnd) {
    DWORD pid = 0;
    if (hwnd)
        ::GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

bool is_our_pid(DWORD pid) {
    return pid != 0 && pid == ::GetCurrentProcessId();
}

bool is_our_element(IUIAutomationElement* el) {
    if (!el) return false;
    int pid = 0;
    el->get_CurrentProcessId(&pid);
    return is_our_pid(static_cast<DWORD>(pid));
}

bool is_password_element(IUIAutomationElement* el) {
    if (!el) return false;
    BOOL is_password = FALSE;
    return SUCCEEDED(el->get_CurrentIsPassword(&is_password)) && is_password;
}

bool is_foreign_top_window(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd) || !::IsWindowVisible(hwnd))
        return false;
    if (::GetWindow(hwnd, GW_OWNER))
        return false;
    if (is_our_pid(hwnd_pid(hwnd)))
        return false;
    const LONG ex = ::GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW)
        return false;
    RECT r{};
    if (!::GetWindowRect(hwnd, &r) || r.right - r.left < 32 || r.bottom - r.top < 32)
        return false;
    return true;
}

std::vector<HWND> collect_foreign_top_windows(int max_windows) {
    std::vector<HWND> out;
    ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* v = reinterpret_cast<std::vector<HWND>*>(lp);
        if (is_foreign_top_window(hwnd))
            v->push_back(hwnd);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&out));
    if (max_windows > 0 && static_cast<int>(out.size()) > max_windows)
        out.resize(static_cast<size_t>(max_windows));
    return out;
}

std::atomic<std::uintptr_t> g_last_foreign_hwnd{0};

void remember_foreign_hwnd(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd))
        return;
    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    if (!root)
        root = hwnd;
    if (!::IsWindow(root) || is_our_pid(hwnd_pid(root)))
        return;
    g_last_foreign_hwnd.store(reinterpret_cast<std::uintptr_t>(root),
                              std::memory_order_relaxed);
}

void CALLBACK last_focus_win_event(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                   LONG, LONG, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND || !hwnd)
        return;
    remember_foreign_hwnd(hwnd);
}

void ensure_last_focus_hook() {
    static std::once_flag once;
    std::call_once(once, [] {
        ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                          nullptr, last_focus_win_event, 0, 0,
                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        HWND fg = ::GetForegroundWindow();
        if (fg)
            remember_foreign_hwnd(fg);
    });
}

HWND last_foreign_hwnd() {
    const HWND h = reinterpret_cast<HWND>(
        g_last_foreign_hwnd.load(std::memory_order_relaxed));
    if (h && ::IsWindow(h) && !is_our_pid(hwnd_pid(h)))
        return h;
    return nullptr;
}

HWND resolve_foreground_hwnd(std::string& source) {
    ensure_last_focus_hook();
    HWND fg = ::GetForegroundWindow();
    if (fg) {
        HWND root = ::GetAncestor(fg, GA_ROOT);
        if (!root) root = fg;
        if (!is_our_pid(hwnd_pid(root))) {
            source = "foreground";
            return root;
        }
    }
    if (HWND last = last_foreign_hwnd()) {
        source = "lastFocus";
        return last;
    }
    const std::vector<HWND> wins = collect_foreign_top_windows(1);
    if (!wins.empty()) {
        source = "zorder";
        return wins.front();
    }
    source = "none";
    return nullptr;
}

void fill_foreground_window(HWND hwnd, ForegroundWindow& out) {
    out.hwnd = hwnd;
    out.root = hwnd ? ::GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (!out.root)
        out.root = hwnd;
    out.pid = hwnd_pid(out.root ? out.root : hwnd);
    out.title = title_of(out.root ? out.root : hwnd);
    out.class_name = class_of(out.root ? out.root : hwnd);
    if (out.pid) {
        PickResult proc;
        proc.pid = out.pid;
        fill_process_info(proc, false);
        out.process = process_basename(proc.exe);
    }
    ::GetCursorPos(&out.cursor);
    out.cursor_hwnd = window_from_point_ex(out.cursor, true);
    if (out.cursor_hwnd) {
        out.cursor_parent = ::GetParent(out.cursor_hwnd);
        if (!out.cursor_parent)
            out.cursor_parent = ::GetAncestor(out.cursor_hwnd, GA_PARENT);
        out.cursor_root = ::GetAncestor(out.cursor_hwnd, GA_ROOT);
        if (!out.cursor_parent)
            out.cursor_parent = out.cursor_root ? out.cursor_root : out.cursor_hwnd;
        if (!out.cursor_root)
            out.cursor_root = out.cursor_hwnd;
    }
}

bool get_foreground_window_impl(ForegroundWindow& out, std::string& err) {
    out = {};
    out.hwnd = resolve_foreground_hwnd(out.source);
    if (!out.hwnd) {
        err = "no foreground window";
        return false;
    }
    fill_foreground_window(out.hwnd, out);
    return true;
}

HWND focus_hwnd_of(HWND root) {
    GUITHREADINFO gti{};
    gti.cbSize = sizeof(gti);
    const DWORD tid = ::GetWindowThreadProcessId(root, nullptr);
    if (tid && ::GetGUIThreadInfo(tid, &gti)) {
        if (gti.hwndFocus && ::IsWindow(gti.hwndFocus))
            return gti.hwndFocus;
        if (gti.hwndCaret && ::IsWindow(gti.hwndCaret))
            return gti.hwndCaret;
        if (gti.hwndActive && ::IsWindow(gti.hwndActive))
            return gti.hwndActive;
    }
    return root;
}

IUIAutomationElement* element_from_hwnd(IUIAutomation* uia, HWND hwnd) {
    if (!uia || !hwnd)
        return nullptr;
    IUIAutomationElement* el = nullptr;
    if (FAILED(uia->ElementFromHandle(reinterpret_cast<UIA_HWND>(hwnd), &el)) || !el)
        return nullptr;
    return el;
}

IUIAutomationCondition* text_pattern_condition(IUIAutomation* uia) {
    if (!uia) return nullptr;
    VARIANT vt;
    ::VariantInit(&vt);
    vt.vt = VT_BOOL;
    vt.boolVal = VARIANT_TRUE;
    IUIAutomationCondition* cond = nullptr;
    if (FAILED(uia->CreatePropertyCondition(UIA_IsTextPatternAvailablePropertyId, vt, &cond)))
        return nullptr;
    return cond;
}

bool try_selection_on_element(IUIAutomation* uia,
                              IUIAutomationElement* el,
                              int max_chars,
                              std::wstring& text,
                              IUIAutomationElement** hit) {
    if (!el) return false;
    if (is_password_element(el)) return false;
    IUIAutomationTextPattern* tp = text_pattern_walk(uia, el);
    if (!tp) return false;
    text = text_from_pattern(tp, max_chars);
    tp->Release();
    if (text.empty()) return false;
    if (hit) {
        el->AddRef();
        *hit = el;
    }
    return true;
}

bool try_selection_in_tree(IUIAutomation* uia,
                           IUIAutomationElement* root,
                           int max_chars,
                           std::wstring& text,
                           IUIAutomationElement** hit) {
    if (!uia || !root) return false;
    if (try_selection_on_element(uia, root, max_chars, text, hit))
        return true;

    IUIAutomationCondition* cond = text_pattern_condition(uia);
    if (!cond) return false;
    IUIAutomationElementArray* arr = nullptr;
    const HRESULT hr = root->FindAll(TreeScope_Descendants, cond, &arr);
    cond->Release();
    if (FAILED(hr) || !arr)
        return false;

    int n = 0;
    arr->get_Length(&n);
    if (n > 48) n = 48;
    bool found = false;
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el)
            continue;
        if (try_selection_on_element(uia, el, max_chars, text, hit)) {
            el->Release();
            found = true;
            break;
        }
        el->Release();
    }
    arr->Release();
    return found;
}

bool try_selection_in_hwnd(IUIAutomation* uia,
                           HWND hwnd,
                           int max_chars,
                           std::wstring& text,
                           IUIAutomationElement** hit) {
    if (!uia || !hwnd) return false;

    const HWND focus = focus_hwnd_of(hwnd);
    if (focus) {
        IUIAutomationElement* el = element_from_hwnd(uia, focus);
        if (el) {
            const bool ok = try_selection_on_element(uia, el, max_chars, text, hit)
                         || try_selection_in_tree(uia, el, max_chars, text, hit);
            el->Release();
            if (ok) return true;
        }
    }

    if (focus != hwnd) {
        IUIAutomationElement* root = element_from_hwnd(uia, hwnd);
        if (root) {
            const bool ok = try_selection_in_tree(uia, root, max_chars, text, hit);
            root->Release();
            if (ok) return true;
        }
    }
    return false;
}

void fill_meta_from_hwnd(HWND hwnd, SelectedText& out) {
    if (!hwnd) return;
    out.hwnd = hwnd;
    const DWORD pid = hwnd_pid(hwnd);
    if (!pid) return;
    PickResult proc;
    proc.pid = pid;
    fill_process_info(proc, false);
    out.process = process_basename(proc.exe);
}

void fill_selected_meta(IUIAutomationElement* el, SelectedText& out) {
    if (!el) return;

    UIA_HWND native = nullptr;
    if (SUCCEEDED(el->get_CurrentNativeWindowHandle(&native)) && native)
        out.hwnd = reinterpret_cast<HWND>(native);
    if (!out.hwnd || !::IsWindow(out.hwnd))
        out.hwnd = ::GetForegroundWindow();

    int pid = 0;
    el->get_CurrentProcessId(&pid);
    if (pid) {
        PickResult proc;
        proc.pid = static_cast<DWORD>(pid);
        fill_process_info(proc, false);
        out.process = process_basename(proc.exe);
    }

    BSTR b = nullptr;
    if (SUCCEEDED(el->get_CurrentLocalizedControlType(&b)) && b) {
        out.control_type = bstr_to_w(b);
        ::SysFreeString(b);
        b = nullptr;
    }
    if (SUCCEEDED(el->get_CurrentFrameworkId(&b)) && b) {
        out.framework_id = bstr_to_w(b);
        ::SysFreeString(b);
    }

    BOOL is_password = FALSE;
    if (SUCCEEDED(el->get_CurrentIsPassword(&is_password)) && is_password)
        out.password = true;
}

#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
bool chrome_selected_text(const SelectedText& meta,
                          const SelectedTextOptions& opts,
                          std::wstring& text,
                          std::string& err) {
    HWND hwnd = meta.hwnd;
    HWND root = hwnd ? ::GetAncestor(hwnd, GA_ROOT) : ::GetForegroundWindow();
    if (!hwnd)
        hwnd = root;
    if (!hwnd)
        return false;

    RECT wr{};
    POINT pt{};
    if (::GetWindowRect(hwnd, &wr)) {
        pt.x = wr.left + (wr.right - wr.left) / 2;
        pt.y = wr.top + (wr.bottom - wr.top) / 2;
    }

    std::wstring title;
    if (root) {
        wchar_t buf[1024] = {};
        ::GetWindowTextW(root, buf, ARRAYSIZE(buf));
        title.assign(buf);
    }
    return media::win::chrome_inspect::get_selected_text_from_dom(
        hwnd, root, title, pt, opts.timeout_ms, text, err);
}
#endif

bool apply_selection_hit(IUIAutomationElement* hit,
                         std::wstring text,
                         SelectedText::Source source,
                         const SelectedTextOptions& opts,
                         SelectedText& out) {
    if (opts.max_chars > 0 && static_cast<int>(text.size()) > opts.max_chars)
        text.resize(static_cast<size_t>(opts.max_chars));
    out.text = std::move(text);
    out.source = source;
    if (hit)
        fill_selected_meta(hit, out);
    return !out.text.empty();
}

#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
bool try_chrome_for_hwnd(HWND hwnd, const SelectedTextOptions& opts, SelectedText& out) {
    if (!opts.chrome_dom || !hwnd)
        return false;
    SelectedText meta = out;
    fill_meta_from_hwnd(hwnd, meta);
    std::wstring chrome_text;
    std::string chrome_err;
    if (!chrome_selected_text(meta, opts, chrome_text, chrome_err) || chrome_text.empty()) {
        if (!chrome_err.empty())
            logger::trace(std::string("[selection] chrome miss hwnd=0x")
                          + std::to_string(reinterpret_cast<uintptr_t>(hwnd))
                          + " " + chrome_err);
        return false;
    }
    out = std::move(meta);
    return apply_selection_hit(nullptr, std::move(chrome_text),
                               SelectedText::Source::ChromeCDP, opts, out);
}
#endif

bool get_selected_text_impl(const SelectedTextOptions& opts,
                            SelectedText& out,
                            std::string& err) {
    out = {};
    ensure_last_focus_hook();
    IUIAutomation* uia = create_uia();
    if (!uia) {
        err = "CoCreateInstance IUIAutomation failed";
        return false;
    }

    IUIAutomationElement* focused = nullptr;
    if (SUCCEEDED(uia->GetFocusedElement(&focused)) && focused) {
        if (is_password_element(focused) && !is_our_element(focused)) {
            fill_selected_meta(focused, out);
            focused->Release();
            uia->Release();
            err = "password field (not captured)";
            return false;
        }
        if (!is_our_element(focused)) {
            std::wstring text;
            IUIAutomationElement* hit = nullptr;
            if (try_selection_on_element(uia, focused, opts.max_chars, text, &hit)
                || try_selection_in_tree(uia, focused, opts.max_chars, text, &hit)) {
                apply_selection_hit(hit, std::move(text),
                                    SelectedText::Source::UIAutomation, opts, out);
                if (hit) hit->Release();
                focused->Release();
                uia->Release();
                logger::info("[selection] focused UIA chars=" + std::to_string(out.text.size())
                             + " process=" + w_to_u8(out.process));
                return true;
            }
            if (hit) hit->Release();
            fill_selected_meta(focused, out);
#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
            if (try_chrome_for_hwnd(out.hwnd, opts, out)) {
                focused->Release();
                uia->Release();
                return true;
            }
#endif
        } else {
            logger::trace("[selection] focused element is this process — scanning other apps");
        }
        focused->Release();
    }

    const std::vector<HWND> windows = collect_foreign_top_windows(12);
    logger::info("[selection] scanning " + std::to_string(windows.size()) + " foreign window(s)");
    for (HWND hwnd : windows) {
        fill_meta_from_hwnd(hwnd, out);
        logger::trace(std::string("[selection] try hwnd=0x")
                      + std::to_string(reinterpret_cast<uintptr_t>(hwnd))
                      + " process=" + w_to_u8(out.process));

        std::wstring text;
        IUIAutomationElement* hit = nullptr;
        if (try_selection_in_hwnd(uia, hwnd, opts.max_chars, text, &hit)) {
            apply_selection_hit(hit, std::move(text),
                                SelectedText::Source::UIAutomation, opts, out);
            if (hit) hit->Release();
            uia->Release();
            logger::info("[selection] window UIA chars=" + std::to_string(out.text.size())
                         + " process=" + w_to_u8(out.process));
            return true;
        }
        if (hit) hit->Release();

#if defined(FEATURE_ASSISTANT) && FEATURE_ASSISTANT
        if (try_chrome_for_hwnd(hwnd, opts, out)) {
            uia->Release();
            logger::info("[selection] window chrome chars=" + std::to_string(out.text.size())
                         + " process=" + w_to_u8(out.process));
            return true;
        }
#endif
    }

    uia->Release();
    out.text.clear();
    out.source = SelectedText::Source::None;
    err.clear();
    return true;
}

} // namespace

bool get_selected_text(const SelectedTextOptions& opts,
                       SelectedText& out,
                       std::string& err) {
    const HRESULT coi = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool need_uninit = (coi == S_OK || coi == S_FALSE);
    const bool ok = get_selected_text_impl(opts, out, err);
    if (need_uninit) ::CoUninitialize();
    return ok;
}

bool get_foreground_window(ForegroundWindow& out, std::string& err) {
    return get_foreground_window_impl(out, err);
}

bool pick_window(const PickOptions& opts, PickResult& out, std::string& err) {
    // Keep hook, UIA, window bounds, and overlay placement in the same coordinate space.
    ThreadDpiPerMonitorScope dpi_scope;

    const HRESULT coi = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool need_uninit = (coi == S_OK || coi == S_FALSE);

    PickContext ctx;
    ctx.opts = opts;
    if (ctx.opts.eyedropper) {
        ctx.opts.sample_color = true;
        ctx.opts.highlight   = false;
        ctx.opts.siblings    = false;
        ctx.opts.resolve_uia = false;
    }
    if (ctx.opts.measure) {
        ctx.opts.highlight = true;
    }
    ::GetCursorPos(&ctx.cur_pt);
    ctx.cursor_move_tick = ::GetTickCount();

    if (ctx.opts.resolve_uia && !ctx.opts.eyedropper) {
        ::CoCreateInstance(
            __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&ctx.uia));
    }

    if (ctx.opts.magnifier) {
        ctx.loupe_on = ctx.loupe.create();
        if (!ctx.loupe_on)
            logger::warn("[picker] loupe create failed — continuing without magnifier");
    }

    if ((ctx.opts.highlight || ctx.opts.measure) && !ctx.opts.eyedropper) {
        HighlightOverlay::Options ho;
        if (ctx.opts.measure) {
            ho.color = RGB(0xFF, 0xA5, 0x33);
            ho.alpha = ctx.opts.highlight_alpha;
        } else {
            ho.color = ctx.opts.highlight_color;
            ho.alpha = ctx.opts.highlight_alpha;
        }
        if (!ctx.overlay.create(ho)) {
            err = "failed to create highlight window";
            logger::warn("[picker] overlay create failed err=" + std::to_string(::GetLastError()));
            ctx.loupe.destroy();
            if (ctx.uia) ctx.uia->Release();
            if (need_uninit) ::CoUninitialize();
            return false;
        }
    }

    if (ctx.opts.measure) {
        MeasureOverlay::Options mo;
        mo.accent = RGB(0xFF, 0xA5, 0x33);
        mo.thickness = 2;
        if (!ctx.measure_stage.create(mo))
            logger::warn("[picker] measure stage overlay create failed");

        HighlightOverlay::Options dim;
        dim.color     = RGB(0, 0, 0);
        dim.filled    = true;
        dim.alpha     = 120;
        dim.thickness = 1;
        if (!ctx.measure_dim.create(dim))
            logger::warn("[picker] measure dim overlay create failed");
    }

    const bool want_hud = ctx.opts.hud && !ctx.opts.eyedropper &&
        (ctx.opts.highlight || ctx.opts.chrome_preview || ctx.opts.measure);
    if (want_hud && !ctx.hud.create()) {
        logger::warn("[picker] hud create failed — continuing without stack chip");
    }

    g_ctx = &ctx;
    std::thread color_hook_thread;
    ColorHookThreadState color_hooks;

    if (ctx.opts.eyedropper) {
        // Ensure this thread has a queue before the hook thread can post to it.
        MSG seed{};
        ::PeekMessageW(&seed, nullptr, 0, 0, PM_NOREMOVE);
        g_color_ui_thread.store(::GetCurrentThreadId(), std::memory_order_release);
        color_hooks.ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (color_hooks.ready) {
            try {
                color_hook_thread = std::thread([&color_hooks]() {
                    color_hooks.thread_id = ::GetCurrentThreadId();
                    MSG seed_msg{};
                    ::PeekMessageW(&seed_msg, nullptr, 0, 0, PM_NOREMOVE);
                    const HMODULE module = ::GetModuleHandleW(nullptr);
                    color_hooks.mouse = ::SetWindowsHookExW(
                        WH_MOUSE_LL, color_mouse_hook_proc, module, 0);
                    color_hooks.keyboard = ::SetWindowsHookExW(
                        WH_KEYBOARD_LL, color_keyboard_hook_proc, module, 0);
                    color_hooks.ok = color_hooks.mouse && color_hooks.keyboard;
                    if (!color_hooks.ok)
                        color_hooks.error = ::GetLastError();
                    ::SetEvent(color_hooks.ready);

                    if (color_hooks.ok) {
                        MSG hook_msg{};
                        while (::GetMessageW(&hook_msg, nullptr, 0, 0) > 0) {
                            ::TranslateMessage(&hook_msg);
                            ::DispatchMessageW(&hook_msg);
                        }
                    }
                    if (color_hooks.mouse)
                        ::UnhookWindowsHookEx(color_hooks.mouse);
                    if (color_hooks.keyboard)
                        ::UnhookWindowsHookEx(color_hooks.keyboard);
                });
                ::WaitForSingleObject(color_hooks.ready, INFINITE);
            } catch (const std::system_error&) {
                color_hooks.error = ERROR_NOT_ENOUGH_MEMORY;
            }
            ::CloseHandle(color_hooks.ready);
            color_hooks.ready = nullptr;
        }
    } else {
        ctx.mouse_hook = ::SetWindowsHookExW(
            WH_MOUSE_LL, mouse_hook_proc, ::GetModuleHandleW(nullptr), 0);
        ctx.kbd_hook = ::SetWindowsHookExW(
            WH_KEYBOARD_LL, kbd_hook_proc, ::GetModuleHandleW(nullptr), 0);
    }

    const bool hooks_ok = ctx.opts.eyedropper
        ? color_hooks.ok
        : (ctx.mouse_hook && ctx.kbd_hook);
    if (!hooks_ok) {
        const DWORD hook_error = ctx.opts.eyedropper
            ? (color_hooks.error ? color_hooks.error : ::GetLastError())
            : ::GetLastError();
        err = "failed to install input hooks (err=" + std::to_string(hook_error) + ")";
        logger::warn("[picker] " + err);
        g_color_ui_thread.store(0, std::memory_order_release);
        if (color_hook_thread.joinable()) {
            if (color_hooks.thread_id)
                ::PostThreadMessageW(color_hooks.thread_id, WM_QUIT, 0, 0);
            color_hook_thread.join();
        }
        if (ctx.mouse_hook) ::UnhookWindowsHookEx(ctx.mouse_hook);
        if (ctx.kbd_hook)   ::UnhookWindowsHookEx(ctx.kbd_hook);
        ctx.hud.destroy();
        ctx.measure_stage.destroy();
        ctx.measure_dim.destroy();
        ctx.overlay.destroy();
        g_ctx = nullptr;
        if (ctx.uia) ctx.uia->Release();
        if (need_uninit) ::CoUninitialize();
        return false;
    }

    UINT_PTR timer = 0;
    if (ctx.opts.eyedropper) {
        eyedropper_update();
        // 20 Hz keeps the software GDI loupe responsive without monopolising
        // DWM/CPU-only machines; commit still samples the exact clicked pixel.
        timer = ::SetTimer(nullptr, 0, 50, hover_timer_proc);
    } else if (ctx.opts.measure) {
        measure_update();
        timer = ::SetTimer(nullptr, 0, 33, hover_timer_proc);
    } else if (ctx.overlay.valid() || ctx.hud.valid()) {
        hover_update();
        timer = ::SetTimer(nullptr, 0, 33, hover_timer_proc);
    }

    // Message loop — LL hooks are dispatched while this thread pumps messages.
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (ctx.opts.eyedropper) {
            if (msg.message == kColorCommitMessage) {
                const POINT pt{
                    g_color_click_x.load(std::memory_order_relaxed),
                    g_color_click_y.load(std::memory_order_relaxed),
                };
                ctx.cur_pt = pt;
                commit_eyedropper(pt);
                ctx.waiting_up = true;
                continue;
            }
            if (msg.message == kColorFinishMessage) {
                if (ctx.waiting_up) {
                    ctx.waiting_up = false;
                    ::PostQuitMessage(0);
                }
                continue;
            }
            if (msg.message == kColorKeyMessage) {
                const DWORD vk = static_cast<DWORD>(msg.wParam);
                if (vk == VK_ESCAPE) {
                    ctx.cancelled = true;
                    ::PostQuitMessage(0);
                } else if (vk == 'Z') {
                    toggle_picker_loupe();
                } else if (vk == 'C' && ctx.cur_color_ok) {
                    if (!copy_text_to_clipboard(hex_w(ctx.cur_color)))
                        logger::warn("[picker] clipboard copy failed for sampled color");
                }
                continue;
            }
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    if (timer) ::KillTimer(nullptr, timer);
    g_color_ui_thread.store(0, std::memory_order_release);
    if (color_hook_thread.joinable()) {
        if (color_hooks.thread_id)
            ::PostThreadMessageW(color_hooks.thread_id, WM_QUIT, 0, 0);
        color_hook_thread.join();
    }
    if (ctx.mouse_hook) {
        ::UnhookWindowsHookEx(ctx.mouse_hook);
        ctx.mouse_hook = nullptr;
    }
    if (ctx.kbd_hook) {
        ::UnhookWindowsHookEx(ctx.kbd_hook);
        ctx.kbd_hook = nullptr;
    }

    const bool cancelled = ctx.cancelled || !ctx.committed;
    if (!cancelled) {
        out.cancelled = false;

        if (ctx.measure_committed) {
            out.measure_ok = true;
            out.measure_a = ctx.measure_a;
            out.measure_b = ctx.measure_b;
            if (ctx.measure_rect_a_valid)
                out.measure_rect_a = ctx.measure_rect_a;
            if (ctx.measure_rect_b_valid)
                out.measure_rect_b = ctx.measure_rect_b;
            out.measure_span = rect_from_points(ctx.measure_a, ctx.measure_b);
            out.custom_rect = true;
            out.target_rect = out.measure_span;
            out.point = ctx.measure_b;
            out.monitor_idx = monitor_index_of(out.point);
            out.hwnd = (ctx.hwnd_at.valid && ctx.hwnd_at.hwnd
                        && ::IsWindow(ctx.hwnd_at.hwnd)
                        && ctx.hwnd_at.rect.right > ctx.hwnd_at.rect.left
                        && ::PtInRect(&ctx.hwnd_at.rect, out.point))
                ? ctx.hwnd_at.hwnd
                : window_from_point_ex(out.point, opts.show_hidden);
            if (out.hwnd) {
                out.root = ::GetAncestor(out.hwnd, GA_ROOT);
                (void)visible_window_bounds(out.hwnd, out.hwnd_rect);
                if (out.root) (void)visible_window_bounds(out.root, out.root_rect);
                out.class_name = class_of(out.hwnd);
                out.root_class = out.root ? class_of(out.root) : out.class_name;
                out.title = title_of(out.root ? out.root : out.hwnd);
                ::GetWindowThreadProcessId(out.hwnd, &out.pid);
            }
            fill_process_info(out, opts.resolve_cmdline);
        } else {
        // ── Resolve the committed target ──────────────────────────────────────
        out.custom_rect = ctx.custom_committed;
        out.point       = ctx.custom_committed ? rect_center(ctx.custom_rect) : ctx.cur_pt;
        out.monitor_idx = monitor_index_of(out.point);
        out.color_ok    = ctx.committed_color_ok;
        out.color       = ctx.committed_color;

        // HWND selectors (the capturable frame + process). For Chrome this is only
        // the render-widget HWND; the page element comes from UIA below.
        out.hwnd = (ctx.hwnd_at.valid && ctx.hwnd_at.hwnd
                    && ::IsWindow(ctx.hwnd_at.hwnd)
                    && ctx.hwnd_at.rect.right > ctx.hwnd_at.rect.left
                    && ::PtInRect(&ctx.hwnd_at.rect, out.point))
            ? ctx.hwnd_at.hwnd
            : window_from_point_ex(out.point, opts.show_hidden);
        if (out.hwnd) {
            out.root = ::GetAncestor(out.hwnd, GA_ROOT);
            (void)visible_window_bounds(out.hwnd, out.hwnd_rect);
            if (out.root) (void)visible_window_bounds(out.root, out.root_rect);
            out.class_name = class_of(out.hwnd);
            out.root_class = out.root ? class_of(out.root) : out.class_name;
            out.title      = title_of(out.root ? out.root : out.hwnd);
            ::GetWindowThreadProcessId(out.hwnd, &out.pid);
        }

        fill_process_info(out, opts.resolve_cmdline);

        // Prefer the stack entry the user highlighted (may be a parent container).
        if (!ctx.custom_committed && ctx.committed_entry_valid &&
            ctx.committed_entry.from_uia) {
            out.uia_ok = true;
            out.uia_rect = ctx.committed_entry.rect;
            out.uia_name = ctx.committed_entry.name;
            out.uia_automation_id = ctx.committed_entry.automation_id;
            out.uia_control_type = ctx.committed_entry.control_type;
            out.uia_framework_id = ctx.committed_entry.framework_id;
            out.uia_class_name = ctx.committed_entry.class_name;
        } else if (ctx.uia && !ctx.custom_committed) {
            // Fallback: deepest element under the point.
            IUIAutomationElement* el = uia_element_at(ctx.uia, ctx.cur_pt, nullptr);
            if (el) { read_element(el, out); el->Release(); }
        }

        // ── Choose the target rect: use the rect resolved at commit (same as hover) ──
        if (ctx.custom_committed) {
            out.target_rect = ctx.custom_rect;
        } else if (ctx.committed_rect_valid) {
            out.target_rect = ctx.committed_rect;
        } else {
            out.target_rect = out.root ? out.root_rect : out.hwnd_rect;
            if (out.uia_ok) {
                const LONG ua = rect_area(out.uia_rect);
                if (ua > 0 && ua < rect_area(out.target_rect))
                    out.target_rect = out.uia_rect;
            }
            if (out.hwnd) {
                const LONG ha = rect_area(out.hwnd_rect);
                if (ha > 0 && ha < rect_area(out.target_rect))
                    out.target_rect = out.hwnd_rect;
            }
        }
        }
        if (opts.before_teardown)
            opts.before_teardown(out);
    } else {
        out.cancelled = true;
    }

    ctx.loupe.destroy();
    ctx.hud.destroy();
    ctx.measure_stage.destroy();
    ctx.measure_dim.destroy();
    ctx.overlay.destroy();
    g_ctx = nullptr;

    if (ctx.uia) ctx.uia->Release();
    if (need_uninit) ::CoUninitialize();
    return true;
}

bool live_picker_active() noexcept
{
    return g_ctx != nullptr;
}

std::vector<HWND> live_picker_capture_hwnds()
{
    std::vector<HWND> out;
    if (!g_ctx)
        return out;
    auto add = [&](HWND h) {
        if (h && ::IsWindow(h) && ::IsWindowVisible(h))
            out.push_back(h);
    };
    add(g_ctx->hud.hwnd());
    add(g_ctx->overlay.hwnd());
    add(g_ctx->measure_stage.hwnd());
    add(g_ctx->measure_dim.hwnd());
    return out;
}

} // namespace media::win::inspect

namespace {
struct RegisterPickerCaptureHwnds {
    RegisterPickerCaptureHwnds()
    {
        media::win::set_live_picker_capture_hwnds_fn(
            &media::win::inspect::live_picker_capture_hwnds);
    }
} g_register_picker_capture_hwnds;
} // namespace

#endif // _WIN32
