#include "context_shell.hpp"

#if defined(_WIN32)

#include <ole2.h>
#include <exdisp.h>
#include <shldisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <servprov.h>
#include <oleauto.h>
#include <psapi.h>
#include <dwmapi.h>
#include <propidl.h>
#include <propkey.h>
#include <propsys.h>
#include <appmodel.h>
#include <commctrl.h>
#include <shellapi.h>
#include <wchar.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "propsys.lib")

namespace media::win::context::shell {

ContextItem item_from_shell_item(::IShellItem* item, bool selected, bool focused)
{
    ContextItem ci;
    if (!item)
        return ci;
    ci.selected = selected;
    ci.focused = focused;

    PWSTR name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
        ci.displayName = name;
        ::CoTaskMemFree(name);
    }
    name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
        ci.parsingName = name;
        ::CoTaskMemFree(name);
    }
    name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
        ci.path = name;
        ci.filesystem = true;
        ::CoTaskMemFree(name);
    }

    SFGAOF attrs = 0;
    if (SUCCEEDED(item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM | SFGAO_STREAM, &attrs))) {
        if (attrs & SFGAO_FILESYSTEM)
            ci.filesystem = true;
        if ((attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM))
            ci.kind = ContextItem::Kind::Directory;
        else if (ci.filesystem)
            ci.kind = ContextItem::Kind::File;
        else
            ci.kind = ContextItem::Kind::ShellItem;
    } else if (ci.filesystem) {
        ci.kind = ContextItem::Kind::File;
    } else {
        ci.kind = ContextItem::Kind::ShellItem;
    }

    if (ci.displayName.empty() && ci.path)
        ci.displayName = ci.path->substr(ci.path->find_last_of(L"\\/") + 1);
    return ci;
}

std::vector<ContextItem> items_from_data_object(::IDataObject* obj)
{
    std::vector<ContextItem> out;
    if (!obj)
        return out;

    IShellItemArray* arr = nullptr;
    if (SUCCEEDED(::SHCreateShellItemArrayFromDataObject(obj, IID_PPV_ARGS(&arr))) && arr) {
        DWORD n = 0;
        (void)arr->GetCount(&n);
        const DWORD cap = (std::min)(n, static_cast<DWORD>(kMaxSnapshotItems));
        out.reserve(cap);
        for (DWORD i = 0; i < cap; ++i) {
            IShellItem* it = nullptr;
            if (FAILED(arr->GetItemAt(i, &it)) || !it)
                continue;
            out.push_back(item_from_shell_item(it, true, i == 0));
            it->Release();
        }
        arr->Release();
        if (!out.empty())
            return out;
    }

    FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM st{};
    if (FAILED(obj->GetData(&fe, &st)) || !st.hGlobal)
        return out;
    auto add_path = [&](const std::wstring& path) {
        if (path.empty() || out.size() >= kMaxSnapshotItems)
            return;
        IShellItem* si = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&si)))
            && si) {
            out.push_back(item_from_shell_item(si, true, out.empty()));
            si->Release();
            return;
        }
        out.push_back(FileItem(path, true, out.empty()));
        if (out.back().kind == ContextItem::Kind::File) {
            const DWORD attr = ::GetFileAttributesW(path.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                out.back().kind = ContextItem::Kind::Directory;
        }
    };
    if (st.tymed == TYMED_HGLOBAL) {
        const auto drop = static_cast<HDROP>(st.hGlobal);
        const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT chars = ::DragQueryFileW(drop, i, nullptr, 0);
            if (chars == 0)
                continue;
            std::wstring path(static_cast<size_t>(chars) + 1, L'\0');
            if (::DragQueryFileW(drop, i, path.data(), chars + 1)) {
                path.resize(wcsnlen(path.c_str(), path.size()));
                add_path(path);
            }
        }
    }
    ::ReleaseStgMedium(&st);
    return out;
}

namespace {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** operator&() { reset(); return &p_; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void attach(T* p) { reset(); p_ = p; }
    HRESULT query_from(IUnknown* from)
    {
        reset();
        if (!from)
            return E_POINTER;
        return from->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&p_));
    }
    void** put_void() { reset(); return reinterpret_cast<void**>(&p_); }
    void reset()
    {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
private:
    T* p_ = nullptr;
};

bool iequals(const std::wstring& a, const wchar_t* b)
{
    if (!b) return false;
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i]))
            return false;
    }
    return i == a.size() && b[i] == 0;
}

std::wstring bstr_to_w(BSTR b)
{
    if (!b) return {};
    return std::wstring(b, SysStringLen(b));
}

void ensure_com()
{
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)hr;
}

ContextFolder folder_from_shell_item(IShellItem* item)
{
    ContextFolder f;
    PWSTR name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && name) {
        f.displayName = name;
        ::CoTaskMemFree(name);
    }
    name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name)) && name) {
        f.parsingName = name;
        ::CoTaskMemFree(name);
    }
    name = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
        f.path = name;
        ::CoTaskMemFree(name);
    }
    return f;
}

ViewObservation observe_folder_view(IFolderView2* fv2, WindowContext win, bool desktop,
                                    bool wantSelection)
{
    ViewObservation out;
    out.window = win;
    out.desktop = desktop;
    out.status = ProviderStatus::Unavailable;

    ComPtr<IShellItem> folder;
    if (SUCCEEDED(fv2->GetFolder(IID_IShellItem, folder.put_void())) && folder)
        out.folder = folder_from_shell_item(folder.get());

    if (!wantSelection) {
        out.status = ProviderStatus::Ok;
        return out;
    }

    ComPtr<IShellItemArray> sel;
    HRESULT hr = fv2->GetSelection(FALSE, &sel);
    if (hr == S_FALSE || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || !sel) {
        out.status = ProviderStatus::KnownEmpty;
        return out;
    }
    if (FAILED(hr)) {
        out.status = ProviderStatus::Unavailable;
        out.error = "GetSelection failed";
        return out;
    }

    DWORD count = 0;
    if (FAILED(sel->GetCount(&count))) {
        out.status = ProviderStatus::Unavailable;
        out.error = "selection count failed";
        return out;
    }
    out.totalCount = count;
    if (count == 0) {
        out.status = ProviderStatus::KnownEmpty;
        return out;
    }

    int focused = -1;
    (void)fv2->GetFocusedItem(&focused);

    const DWORD cap = (std::min)(count, static_cast<DWORD>(kMaxSnapshotItems));
    out.truncated = cap < count;
    out.selection.reserve(cap);
    for (DWORD i = 0; i < cap; ++i) {
        ComPtr<IShellItem> it;
        if (FAILED(sel->GetItemAt(i, &it)) || !it)
            continue;
        const bool is_focused = (static_cast<int>(i) == focused);
        out.selection.push_back(item_from_shell_item(it.get(), true, is_focused));
    }
    out.status = out.selection.empty() ? ProviderStatus::KnownEmpty : ProviderStatus::Ok;
    return out;
}

HWND hwnd_of_dispatch(IDispatch* disp)
{
    if (!disp)
        return nullptr;
    ComPtr<IWebBrowserApp> app;
    if (FAILED(app.query_from(disp)) || !app)
        return nullptr;
    SHANDLE_PTR hwndRaw = 0;
    if (FAILED(app->get_HWND(&hwndRaw)) || !hwndRaw)
        return nullptr;
    return reinterpret_cast<HWND>(hwndRaw);
}

IDispatch* find_dispatch_for_hwnd(IShellWindows* windows, HWND hwnd, ComPtr<IDispatch>& holder)
{
    if (!windows || !hwnd)
        return nullptr;

    VARIANT loc;
    VariantInit(&loc);
    loc.vt = VT_I4;
    loc.lVal = static_cast<LONG>(HandleToLong(hwnd));
    VARIANT empty;
    VariantInit(&empty);
    long found = 0;
    const int classes[] = { SWC_BROWSER, SWC_EXPLORER };
    for (int c : classes) {
        holder.reset();
        if (SUCCEEDED(windows->FindWindowSW(&loc, &empty, c, &found,
                                            SWFO_NEEDDISPATCH, &holder)) && holder)
            return holder.get();
    }

    long count = 0;
    if (FAILED(windows->get_Count(&count)) || count <= 0)
        return nullptr;
    HWND root = ::GetAncestor(hwnd, GA_ROOT);
    for (long i = 0; i < count; ++i) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt = VT_I4;
        idx.lVal = i;
        holder.reset();
        if (FAILED(windows->Item(idx, &holder)) || !holder)
            continue;
        const HWND wh = hwnd_of_dispatch(holder.get());
        if (wh == hwnd || (root && wh == root))
            return holder.get();
    }
    holder.reset();
    return nullptr;
}

ViewObservation observe_dispatch(IDispatch* disp, uint64_t timestampMs, bool wantSelection)
{
    ViewObservation miss;
    miss.status = ProviderStatus::Unavailable;
    if (!disp)
        return miss;

    ComPtr<IWebBrowserApp> app;
    if (FAILED(app.query_from(disp)) || !app) {
        miss.error = "IWebBrowserApp unavailable";
        return miss;
    }

    SHANDLE_PTR hwndRaw = 0;
    if (FAILED(app->get_HWND(&hwndRaw)) || !hwndRaw) {
        miss.error = "view hwnd unavailable";
        return miss;
    }
    const HWND hwnd = reinterpret_cast<HWND>(hwndRaw);
    WindowContext win = describe_hwnd(hwnd, timestampMs);

    ComPtr<IServiceProvider> sp;
    if (FAILED(sp.query_from(app.get())) || !sp) {
        miss.window = win;
        miss.error = "IServiceProvider unavailable";
        return miss;
    }

    ComPtr<IShellBrowser> browser;
    if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, browser.put_void())) || !browser) {
        miss.window = win;
        miss.error = "IShellBrowser unavailable";
        return miss;
    }

    ComPtr<IShellView> view;
    IShellView* rawView = nullptr;
    if (FAILED(browser->QueryActiveShellView(&rawView)) || !rawView) {
        miss.window = win;
        miss.error = "IShellView unavailable";
        return miss;
    }
    view.attach(rawView);

    ComPtr<IFolderView2> fv2;
    if (FAILED(fv2.query_from(view.get())) || !fv2) {
        miss.window = win;
        miss.error = "IFolderView2 unavailable";
        return miss;
    }

    const bool desktop = is_desktop_window(hwnd) || win.desktop;
    return observe_folder_view(fv2.get(), win, desktop, wantSelection);
}

} // namespace

namespace {
std::mutex g_proc_mu;
struct ProcCacheEnt {
    std::wstring name;
    std::wstring exePath;
    std::wstring pfn;
    uint64_t at = 0;
};
std::unordered_map<uint32_t, ProcCacheEnt> g_proc_cache;
std::mutex g_aumid_mu;
std::unordered_map<uintptr_t, std::pair<std::wstring, uint64_t>> g_aumid_cache;
} // namespace

static ProcCacheEnt process_identity_of(uint32_t pid)
{
    ProcCacheEnt miss;
    if (!pid)
        return miss;
    const uint64_t t = now_ms();
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        auto it = g_proc_cache.find(pid);
        if (it != g_proc_cache.end() && t >= it->second.at
            && (t - it->second.at) < 30'000)
            return it->second;
    }
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    ProcCacheEnt out;
    if (h) {
        wchar_t path[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        if (::QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
            out.exePath.assign(path, n);
            const auto slash = out.exePath.find_last_of(L"\\/");
            out.name = slash == std::wstring::npos ? out.exePath
                                                   : out.exePath.substr(slash + 1);
        }
        UINT32 pfnLen = 0;
        const LONG rc = ::GetPackageFamilyName(h, &pfnLen, nullptr);
        if (rc == ERROR_INSUFFICIENT_BUFFER && pfnLen > 1) {
            std::wstring pfn(pfnLen, L'\0');
            if (::GetPackageFamilyName(h, &pfnLen, pfn.data()) == ERROR_SUCCESS) {
                if (!pfn.empty() && pfn.back() == L'\0')
                    pfn.pop_back();
                out.pfn = std::move(pfn);
            }
        }
        ::CloseHandle(h);
    }
    out.at = t;
    {
        std::lock_guard<std::mutex> lk(g_proc_mu);
        g_proc_cache[pid] = out;
        if (g_proc_cache.size() > 256)
            g_proc_cache.clear();
    }
    return out;
}

std::wstring process_name_of(uint32_t pid)
{
    return process_identity_of(pid).name;
}

std::wstring class_name_of(HWND hwnd)
{
    wchar_t buf[256] = {};
    ::GetClassNameW(hwnd, buf, 256);
    return buf;
}

std::wstring title_of(HWND hwnd)
{
    wchar_t buf[1024] = {};
    ::GetWindowTextW(hwnd, buf, 1024);
    return buf;
}

bool is_explorer_window(HWND hwnd)
{
    const std::wstring cls = class_name_of(hwnd);
    return iequals(cls, L"CabinetWClass") || iequals(cls, L"ExploreWClass");
}

bool is_desktop_window(HWND hwnd)
{
    if (!hwnd)
        return false;
    if (hwnd == ::GetShellWindow())
        return true;
    const std::wstring cls = class_name_of(hwnd);
    if (iequals(cls, L"Progman") || iequals(cls, L"WorkerW"))
        return true;
    HWND shell = ::GetShellWindow();
    if (shell && ::GetAncestor(hwnd, GA_ROOT) == shell)
        return true;
    return false;
}

bool is_our_ui_window(HWND hwnd)
{
    if (!hwnd || !::IsWindow(hwnd))
        return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ::GetCurrentProcessId())
        return true;
    const std::wstring cls = class_name_of(hwnd);
    return iequals(cls, L"PmLauncherPopup_v1")
        || iequals(cls, L"PmAssistantBar")
        || iequals(cls, L"PmOverlayHud")
        || iequals(cls, L"PmTranscriptionHud");
}

bool is_our_helper_window(HWND hwnd)
{
    if (!hwnd || !::IsWindow(hwnd))
        return false;
    const std::wstring cls = class_name_of(hwnd);
    return iequals(cls, L"PmLauncherPopup_v1")
        || iequals(cls, L"PmAssistantBar")
        || iequals(cls, L"PmOverlayHud")
        || iequals(cls, L"PmTranscriptionHud")
        || iequals(cls, L"PmCommandPalette")
        || iequals(cls, L"PmHighlightOverlay")
        || iequals(cls, L"PmPickHud")
        || iequals(cls, L"PmPickLoupe");
}

bool is_shell_infrastructure(HWND hwnd)
{
    if (!hwnd || !::IsWindow(hwnd))
        return false;
    if (is_desktop_window(hwnd))
        return true;
    const std::wstring cls = class_name_of(hwnd);
    return iequals(cls, L"Shell_TrayWnd")
        || iequals(cls, L"Shell_SecondaryTrayWnd")
        || iequals(cls, L"NotifyIconOverflowWindow")
        || iequals(cls, L"NotifyIconOverflowWindowClass")
        || iequals(cls, L"Shell_CharmWindow")
        || iequals(cls, L"ForegroundStaging")
        || iequals(cls, L"DummyDWMWnd")
        || iequals(cls, L"TaskListThumbnailWnd")
        || iequals(cls, L"TaskListOverlayWnd")
        || iequals(cls, L"MultitaskingViewFrame")
        || iequals(cls, L"Windows.UI.Core.CoreWindow")
        || iequals(cls, L"ApplicationManager_DesktopShellWindow")
        || iequals(cls, L"ImmersiveLauncher")
        || iequals(cls, L"SearchPane")
        || iequals(cls, L"#32769");
}

bool is_window_cloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    if (FAILED(::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return false;
    return cloaked != 0;
}

bool IsTaskbarCandidate(HWND hwnd)
{
    if (!hwnd || !::IsWindow(hwnd))
        return false;
    if (!::IsWindowVisible(hwnd))
        return false;
    if (is_our_helper_window(hwnd) || is_shell_infrastructure(hwnd))
        return false;
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_CHILD)
        return false;
    if (is_window_cloaked(hwnd))
        return false;
    if (exStyle & WS_EX_TOOLWINDOW)
        return false;
    HWND owner = ::GetWindow(hwnd, GW_OWNER);
    if (owner && !(exStyle & WS_EX_APPWINDOW))
        return false;
    return true;
}

bool IsAltTabCandidate(HWND hwnd)
{
    if (!IsTaskbarCandidate(hwnd))
        return false;
    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_NOACTIVATE)
        return false;
    return true;
}

HWND representative_hwnd(HWND hwnd)
{
    if (!hwnd)
        return nullptr;
    HWND root = ::GetAncestor(hwnd, GA_ROOTOWNER);
    if (root && root != hwnd && IsTaskbarCandidate(root))
        return root;
    HWND owner = ::GetWindow(hwnd, GW_OWNER);
    if (owner && IsTaskbarCandidate(owner))
        return owner;
    return hwnd;
}

std::wstring app_user_model_id_of(HWND hwnd)
{
    if (!hwnd || !::IsWindow(hwnd))
        return {};
    const uint64_t t = now_ms();
    const uintptr_t key = reinterpret_cast<uintptr_t>(hwnd);
    {
        std::lock_guard<std::mutex> lk(g_aumid_mu);
        auto it = g_aumid_cache.find(key);
        if (it != g_aumid_cache.end() && t >= it->second.second
            && (t - it->second.second) < 30'000)
            return it->second.first;
    }
    ensure_com();
    std::wstring id;
    IPropertyStore* store = nullptr;
    if (SUCCEEDED(::SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore,
                                                reinterpret_cast<void**>(&store))) && store) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv))) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal)
                id = pv.pwszVal;
            else if (pv.vt == VT_BSTR && pv.bstrVal)
                id = bstr_to_w(pv.bstrVal);
        }
        PropVariantClear(&pv);
        store->Release();
    }
    {
        std::lock_guard<std::mutex> lk(g_aumid_mu);
        g_aumid_cache[key] = {id, t};
        if (g_aumid_cache.size() > 256)
            g_aumid_cache.clear();
    }
    return id;
}

void fill_app_identity(HWND hwnd, uint32_t pid, AppIdentity& id)
{
    const auto proc = process_identity_of(pid);
    id.pid = pid;
    id.exePath = proc.exePath;
    id.packageFamilyName = proc.pfn;
    id.appUserModelId = app_user_model_id_of(hwnd);
}

WindowContext describe_hwnd(HWND hwnd, uint64_t timestampMs)
{
    WindowContext w;
    w.hwnd = hwnd;
    w.timestampMs = timestampMs;
    if (!hwnd || !::IsWindow(hwnd))
        return w;
    w.tid = ::GetWindowThreadProcessId(hwnd, reinterpret_cast<DWORD*>(&w.pid));
    w.title = title_of(hwnd);
    w.className = class_name_of(hwnd);
    fill_app_identity(hwnd, w.pid, w.app);
    w.processName = process_identity_of(w.pid).name;
    RECT r{};
    if (::GetWindowRect(hwnd, &r)) {
        w.left = r.left;
        w.top = r.top;
        w.right = r.right;
        w.bottom = r.bottom;
    }
    w.explorer = is_explorer_window(hwnd);
    w.desktop = is_desktop_window(hwnd);
    w.ourProcess = is_our_ui_window(hwnd);
    return w;
}

std::vector<ViewObservation> enumerate_shell_views(uint64_t timestampMs)
{
    ensure_com();
    std::vector<ViewObservation> out;
    ComPtr<IShellWindows> windows;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_IShellWindows, windows.put_void())) || !windows) {
        ViewObservation miss;
        miss.status = ProviderStatus::Unavailable;
        miss.error = "IShellWindows unavailable";
        out.push_back(std::move(miss));
        return out;
    }
    long count = 0;
    if (FAILED(windows->get_Count(&count)) || count <= 0)
        return out;
    for (long i = 0; i < count; ++i) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt = VT_I4;
        idx.lVal = i;
        ComPtr<IDispatch> disp;
        if (FAILED(windows->Item(idx, &disp)) || !disp)
            continue;
        out.push_back(observe_dispatch(disp.get(), timestampMs, true));
    }
    return out;
}

ViewObservation query_desktop_view(uint64_t timestampMs, bool wantSelection)
{
    ensure_com();
    ComPtr<IShellWindows> windows;
    if (SUCCEEDED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                     IID_IShellWindows, windows.put_void())) && windows) {
        VARIANT loc;
        VariantInit(&loc);
        loc.vt = VT_VARIANT | VT_BYREF;
        VARIANT empty;
        VariantInit(&empty);
        long hwnd = 0;
        ComPtr<IDispatch> disp;
        if (SUCCEEDED(windows->FindWindowSW(&empty, &empty, SWC_DESKTOP, &hwnd,
                                            SWFO_NEEDDISPATCH, &disp)) && disp) {
            auto obs = observe_dispatch(disp.get(), timestampMs, wantSelection);
            obs.desktop = true;
            return obs;
        }
    }
    ViewObservation miss;
    miss.window = describe_hwnd(::GetShellWindow(), timestampMs);
    miss.desktop = true;
    miss.status = ProviderStatus::Unavailable;
    miss.error = "desktop view unavailable";
    return miss;
}

ViewObservation query_view_for_hwnd(HWND hwnd, uint64_t timestampMs, bool wantSelection)
{
    ensure_com();
    ComPtr<IShellWindows> windows;
    if (FAILED(::CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL,
                                  IID_IShellWindows, windows.put_void())) || !windows) {
        ViewObservation miss;
        miss.window = describe_hwnd(hwnd, timestampMs);
        miss.status = ProviderStatus::Unavailable;
        miss.error = "IShellWindows unavailable";
        return miss;
    }
    ComPtr<IDispatch> disp;
    if (find_dispatch_for_hwnd(windows.get(), hwnd, disp) && disp)
        return observe_dispatch(disp.get(), timestampMs, wantSelection);
    ViewObservation miss;
    miss.window = describe_hwnd(hwnd, timestampMs);
    miss.status = ProviderStatus::Unavailable;
    miss.error = "no shell view for hwnd";
    return miss;
}

OpenAppWindow describe_open_app(HWND hwnd, uint64_t timestampMs, HWND foreground,
                                HWND previousExternal)
{
    OpenAppWindow o;
    o.hwnd = hwnd;
    o.representative = representative_hwnd(hwnd);
    o.visible = hwnd && ::IsWindowVisible(hwnd) != FALSE;
    o.cloaked = hwnd && is_window_cloaked(hwnd);
    o.minimized = hwnd && ::IsIconic(hwnd) != FALSE;
    o.taskbarCandidate = IsTaskbarCandidate(hwnd);
    o.altTabCandidate = IsAltTabCandidate(hwnd);
    o.foreground = hwnd && hwnd == foreground;
    o.previousForeground = hwnd && previousExternal && hwnd == previousExternal;
    if (!hwnd || !::IsWindow(hwnd))
        return o;
    o.tid = ::GetWindowThreadProcessId(hwnd, reinterpret_cast<DWORD*>(&o.pid));
    o.title = title_of(hwnd);
    o.className = class_name_of(hwnd);
    fill_app_identity(hwnd, o.pid, o.app);
    o.processName = process_identity_of(o.pid).name;
    (void)timestampMs;
    return o;
}

std::vector<OpenAppWindow> enumerate_open_apps(uint64_t timestampMs, HWND foreground,
                                               HWND previousExternal)
{
    struct Acc {
        uint64_t t = 0;
        HWND fg = nullptr;
        HWND prev = nullptr;
        std::vector<OpenAppWindow> raw;
    } acc;
    acc.t = timestampMs;
    acc.fg = foreground;
    acc.prev = previousExternal;
    ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* a = reinterpret_cast<Acc*>(lp);
        if (!IsTaskbarCandidate(hwnd) && !IsAltTabCandidate(hwnd))
            return TRUE;
        a->raw.push_back(describe_open_app(hwnd, a->t, a->fg, a->prev));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&acc));

    std::vector<OpenAppWindow> out;
    out.reserve(acc.raw.size());
    for (auto& w : acc.raw) {
        HWND key = w.representative ? w.representative : w.hwnd;
        auto it = std::find_if(out.begin(), out.end(), [&](const OpenAppWindow& e) {
            HWND ek = e.representative ? e.representative : e.hwnd;
            return ek == key;
        });
        if (it == out.end()) {
            out.push_back(std::move(w));
            continue;
        }
        it->foreground = it->foreground || w.foreground;
        it->previousForeground = it->previousForeground || w.previousForeground;
        if (w.foreground)
            it->hwnd = w.hwnd;
        if (w.lastForegroundAt > it->lastForegroundAt)
            it->lastForegroundAt = w.lastForegroundAt;
    }
    if (out.size() > kMaxOpenApps)
        out.resize(kMaxOpenApps);
    return out;
}

static ContextItem item_from_link(IShellLinkW* link)
{
    ContextItem ci;
    ci.kind = ContextItem::Kind::File;
    wchar_t path[MAX_PATH] = {};
    // SLGP_RAWPATH: do not resolve the shortcut (no disk hit).
    if (SUCCEEDED(link->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH)) && path[0]) {
        ci.path = path;
        ci.parsingName = path;
        ci.filesystem = true;
        const auto slash = ci.path->find_last_of(L"\\/");
        ci.displayName = slash == std::wstring::npos ? *ci.path : ci.path->substr(slash + 1);
    }
    wchar_t desc[1024] = {};
    if (SUCCEEDED(link->GetDescription(desc, 1024)) && desc[0] && ci.displayName.empty())
        ci.displayName = desc;
    return ci;
}

static void append_doc_list(IApplicationDocumentLists* lists, APPDOCLISTTYPE kind,
                            size_t maxItems, std::vector<ContextItem>& out)
{
    IObjectArray* arr = nullptr;
    if (FAILED(lists->GetList(kind, static_cast<UINT>(maxItems), IID_IObjectArray,
                              reinterpret_cast<void**>(&arr))) || !arr)
        return;
    UINT n = 0;
    (void)arr->GetCount(&n);
    if (n > maxItems)
        n = static_cast<UINT>(maxItems);
    for (UINT i = 0; i < n; ++i) {
        IUnknown* unk = nullptr;
        if (FAILED(arr->GetAt(i, IID_IUnknown, reinterpret_cast<void**>(&unk))) || !unk)
            continue;
        IShellItem* si = nullptr;
        if (SUCCEEDED(unk->QueryInterface(IID_IShellItem, reinterpret_cast<void**>(&si))) && si) {
            out.push_back(item_from_shell_item(si, false, false));
            si->Release();
        } else {
            IShellLinkW* link = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_IShellLinkW, reinterpret_cast<void**>(&link))) && link) {
                auto it = item_from_link(link);
                if (it.path || !it.displayName.empty())
                    out.push_back(std::move(it));
                link->Release();
            }
        }
        unk->Release();
    }
    arr->Release();
}

AppRecentContext query_app_document_lists(const std::wstring& appUserModelId, size_t maxItems)
{
    AppRecentContext out;
    out.app.appUserModelId = appUserModelId;
    if (appUserModelId.empty() || maxItems == 0)
        return out;
    ensure_com();
    IApplicationDocumentLists* lists = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_ApplicationDocumentLists, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IApplicationDocumentLists, reinterpret_cast<void**>(&lists)))
        || !lists)
        return out;
    (void)lists->SetAppID(appUserModelId.c_str());
    append_doc_list(lists, ADLT_RECENT, maxItems, out.recent);
    append_doc_list(lists, ADLT_FREQUENT, maxItems, out.frequent);
    lists->Release();
    return out;
}

static bool recent_target_skip(const ContextItem& it)
{
    if (it.kind == ContextItem::Kind::Directory)
        return true;
    std::wstring path;
    if (it.path && !it.path->empty())
        path = *it.path;
    else if (it.parsingName && !it.parsingName->empty())
        path = *it.parsingName;
    if (path.empty())
        return true;
    auto dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        std::wstring ext = path.substr(dot);
        for (auto& c : ext)
            c = static_cast<wchar_t>(std::towlower(c));
        if (ext == L".exe" || ext == L".lnk" || ext == L".bat" || ext == L".cmd"
            || ext == L".com" || ext == L".url")
            return true;
    }
    return false;
}

std::vector<ContextItem> query_windows_recent(size_t maxItems)
{
    std::vector<ContextItem> out;
    if (maxItems == 0)
        return out;
    ensure_com();
    PWSTR folder = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_Recent, 0, nullptr, &folder)) || !folder)
        return out;
    const std::wstring dir = folder;
    ::CoTaskMemFree(folder);

    struct Ent {
        FILETIME ft{};
        std::wstring name;
    };
    std::vector<Ent> ents;
    WIN32_FIND_DATAW fd{};
    const std::wstring glob = dir + L"\\*.lnk";
    HANDLE h = ::FindFirstFileW(glob.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            ents.push_back({fd.ftLastWriteTime, fd.cFileName});
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
        return ::CompareFileTime(&a.ft, &b.ft) > 0;
    });
    constexpr size_t kScanCap = 48;
    if (ents.size() > kScanCap)
        ents.resize(kScanCap);

    for (const auto& e : ents) {
        if (out.size() >= maxItems)
            break;
        const std::wstring lnk = dir + L"\\" + e.name;
        if (::GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        // Raw IShellLink only. BHID_LinkTargetItem / IShellLink::Resolve pop a
        // "Problem with Shortcut" dialog when the .lnk target is gone.
        ContextItem ci;
        IShellLinkW* link = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_IShellLinkW, reinterpret_cast<void**>(&link)))
            && link) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf)))
                && pf) {
                if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ)))
                    ci = item_from_link(link);
                pf->Release();
            }
            link->Release();
        }
        if (recent_target_skip(ci))
            continue;
        if (!ci.path || ci.path->empty()
            || ::GetFileAttributesW(ci.path->c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        out.push_back(std::move(ci));
    }
    return out;
}

std::vector<ContextItem> items_from_paths(const std::vector<std::wstring>& paths)
{
    std::vector<ContextItem> out;
    out.reserve(paths.size());
    for (const auto& p : paths) {
        ContextItem it;
        it.path = p;
        it.parsingName = p;
        it.filesystem = true;
        it.selected = true;
        it.kind = ContextItem::Kind::File;
        const auto slash = p.find_last_of(L"\\/");
        it.displayName = slash == std::wstring::npos ? p : p.substr(slash + 1);
        out.push_back(std::move(it));
    }
    return out;
}

} // namespace media::win::context::shell

#endif
