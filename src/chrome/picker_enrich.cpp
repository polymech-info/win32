#include "picker_enrich.hpp"
#include "fs/path_sanitizer.hpp"

#include "bridge.hpp"
#include "inspect/inspect.hpp"
#include "win/web/webview_dom_inspect.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "html/html.h"
#include "logging/logger.h"

#include <cwchar>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace media::win::chrome_inspect {
namespace {

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
constexpr bool kFeatureBrowser = true;
#else
constexpr bool kFeatureBrowser = false;
#endif

bool ends_with_icase(const std::wstring& value, const wchar_t* suffix)
{
    const size_t suffix_len = std::wcslen(suffix);
    if (suffix_len > value.size()) return false;
    return _wcsnicmp(value.c_str() + value.size() - suffix_len, suffix, suffix_len) == 0;
}

std::wstring strip_chrome_window_suffix(std::wstring title)
{
    static const wchar_t* suffixes[] = {
        L" - Google Chrome",
        L" - Chromium",
    };
    for (const wchar_t* suffix : suffixes) {
        if (ends_with_icase(title, suffix)) {
            title.resize(title.size() - std::wcslen(suffix));
            break;
        }
    }
    while (!title.empty() && (title.back() == L' ' || title.back() == L'\t'))
        title.pop_back();
    return title;
}

std::string wide_to_utf8(const std::wstring& value)
{
    if (value.empty()) return {};
    std::string out(value.size(), '\0');
    const int n = ::WideCharToMultiByte(CP_UTF8, 0,
                                        value.c_str(), static_cast<int>(value.size()),
                                        out.data(), static_cast<int>(out.size()),
                                        nullptr, nullptr);
    if (n <= 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
                                        static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                          out.data(), n);
    return out;
}

std::string hwnd_hex(HWND hwnd)
{
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "0x%llX",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(hwnd)));
    return buf;
}

bool is_webview2_class_name(const wchar_t* cls)
{
    if (!cls || !cls[0])
        return false;
    return std::wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0
        || std::wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0
        || std::wcscmp(cls, L"Intermediate D3D Window") == 0;
}

bool is_webview2_hwnd(HWND hwnd)
{
    if (!hwnd)
        return false;
    wchar_t cls[256] = {};
    ::GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return is_webview2_class_name(cls);
}

bool has_dom_host_prop(HWND hwnd)
{
    return hwnd && ::GetPropW(hwnd, media::win::webview_dom::kHostPropName) != nullptr;
}

long long rect_area(const RECT& r)
{
    const long long w = static_cast<long long>(r.right) - r.left;
    const long long h = static_cast<long long>(r.bottom) - r.top;
    if (w <= 0 || h <= 0)
        return 0;
    return w * h;
}

POINT client_point_for_pick(const media::win::inspect::PickResult& pick)
{
    POINT client = pick.point;
    const HWND targets[] = {pick.hwnd, pick.root};
    for (HWND hwnd : targets) {
        if (!hwnd) continue;
        POINT probe = pick.point;
        if (::ScreenToClient(hwnd, &probe))
            return probe;
    }
    return client;
}

void trace_hwnd(const char* step, HWND hwnd)
{
    if (!hwnd) {
        logger::trace(std::string("[picker-enrich] ") + step + " hwnd=null");
        return;
    }
    wchar_t cls[256] = {};
    ::GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    logger::trace(std::string("[picker-enrich] ") + step
                  + " hwnd=" + hwnd_hex(hwnd)
                  + " class=" + wide_to_utf8(cls)
                  + " prop=" + (has_dom_host_prop(hwnd) ? "1" : "0")
                  + " wv2=" + (is_webview2_class_name(cls) ? "1" : "0"));
}

HWND first_non_webview2_ancestor(HWND chrome_hwnd)
{
    HWND h = chrome_hwnd ? ::GetParent(chrome_hwnd) : nullptr;
    while (h && is_webview2_hwnd(h)) {
        HWND parent = ::GetParent(h);
        if (!parent || parent == h)
            break;
        h = parent;
    }
    return h;
}

HWND hwnd_from_env()
{
    const char* env = std::getenv("PM_IMAGE_HOST_HWND");
    if (!env || !env[0])
        return nullptr;
    try {
        const auto raw = static_cast<uintptr_t>(std::stoull(env));
        HWND hwnd = reinterpret_cast<HWND>(raw);
        if (hwnd && ::IsWindow(hwnd))
            return hwnd;
    } catch (...) {
    }
    return nullptr;
}

struct HostEnumCtx {
    POINT pt{};
    HWND  best_prop = nullptr;
    long long best_prop_area = (std::numeric_limits<long long>::max)();
    HWND  best_chrome = nullptr;
    long long best_chrome_area = (std::numeric_limits<long long>::max)();
    int   visits = 0;
};

BOOL CALLBACK enum_dom_hosts(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<HostEnumCtx*>(lp);
    if (!hwnd || !::IsWindow(hwnd))
        return TRUE;
    ++ctx->visits;
    RECT wr{};
    if (!::GetWindowRect(hwnd, &wr))
        return TRUE;
    const bool contains = ::PtInRect(&wr, ctx->pt) != FALSE;
    const long long area = rect_area(wr);
    if (has_dom_host_prop(hwnd) && contains && area > 0 && area < ctx->best_prop_area) {
        ctx->best_prop = hwnd;
        ctx->best_prop_area = area;
    }
    if (is_webview2_hwnd(hwnd) && contains && area > 0 && area < ctx->best_chrome_area) {
        ctx->best_chrome = hwnd;
        ctx->best_chrome_area = area;
    }
    return TRUE;
}

HWND pick_from_enum(HWND start, POINT pt, const char* label)
{
    if (!start || !::IsWindow(start))
        return nullptr;
    HostEnumCtx ctx;
    ctx.pt = pt;
    ::EnumChildWindows(start, enum_dom_hosts, reinterpret_cast<LPARAM>(&ctx));
    logger::trace(std::string("[picker-enrich] host-search enum ") + label
                  + " start=" + hwnd_hex(start)
                  + " visits=" + std::to_string(ctx.visits)
                  + " prop=" + hwnd_hex(ctx.best_prop)
                  + " chrome=" + hwnd_hex(ctx.best_chrome));
    if (ctx.best_prop)
        return ctx.best_prop;
    if (ctx.best_chrome) {
        if (HWND inferred = first_non_webview2_ancestor(ctx.best_chrome)) {
            trace_hwnd("host-search inferred", inferred);
            return inferred;
        }
        return ctx.best_chrome;
    }
    return nullptr;
}

HWND find_cross_process_dom_host(HWND start, POINT screen_pt)
{
    if (!start)
        return nullptr;

    for (HWND h = start; h; ) {
        if (has_dom_host_prop(h)) {
            logger::trace(std::string("[picker-enrich] host-search found prop ") + hwnd_hex(h));
            return h;
        }
        HWND parent = ::GetParent(h);
        if (!parent || parent == h)
            break;
        h = parent;
    }

    if (is_webview2_hwnd(start)) {
        if (HWND inferred = first_non_webview2_ancestor(start)) {
            trace_hwnd("host-search inferred-from-leaf", inferred);
            return inferred;
        }
    }

    if (HWND found = pick_from_enum(start, screen_pt, "start"))
        return found;

    const HWND root = ::GetAncestor(start, GA_ROOT);
    if (root && root != start) {
        if (HWND found = pick_from_enum(root, screen_pt, "root"))
            return found;
    }
    return nullptr;
}

HWND find_cross_process_dom_host(const media::win::inspect::PickResult& pick)
{
    if (HWND host = find_cross_process_dom_host(pick.hwnd, pick.point))
        return host;
    if (pick.root && pick.root != pick.hwnd) {
        if (HWND host = find_cross_process_dom_host(pick.root, pick.point))
            return host;
    }
    if (HWND env = hwnd_from_env()) {
        logger::trace(std::string("[picker-enrich] host-search PM_IMAGE_HOST_HWND=")
                      + hwnd_hex(env));
        if (HWND host = find_cross_process_dom_host(env, pick.point))
            return host;
        if (HWND found = pick_from_enum(env, pick.point, "env"))
            return found;
    }
    logger::trace("[picker-enrich] host-search miss");
    return nullptr;
}

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
media::win::webview_dom::DomSerializeOpts to_serialize(const DomInspectOpts& opts)
{
    media::win::webview_dom::DomSerializeOpts s;
    s.text_max_chars = opts.text_max_chars;
    s.html_max_chars = opts.html_max_chars;
    s.markdown_max_chars = opts.markdown_max_chars;
    return s;
}

std::wstring document_selector_w(const DomInspectOpts& opts)
{
    return utf8_to_wide(opts.document_selector);
}
#endif

void apply_serialize_caps(nlohmann::json& payload, const DomInspectOpts& opts)
{
    payload["textMaxChars"] = opts.text_max_chars >= 0 ? opts.text_max_chars : 0;
    payload["htmlMaxChars"] = opts.html_max_chars >= 0 ? opts.html_max_chars : 0;
    payload["markdownMaxChars"] = opts.markdown_max_chars >= 0 ? opts.markdown_max_chars : 0;
}

void clamp_text_cap(std::string& value, int max_chars)
{
    if (max_chars <= 0 || value.size() <= static_cast<size_t>(max_chars))
        return;
    value.resize(static_cast<size_t>(max_chars));
    value += "\xE2\x80\xA6";
}

void log_pick(const media::win::inspect::PickResult& pick)
{
    logger::trace(std::string("[picker-enrich] pick hwnd=") + hwnd_hex(pick.hwnd)
                  + " class=" + wide_to_utf8(pick.class_name)
                  + " root=" + hwnd_hex(pick.root)
                  + " root_class=" + wide_to_utf8(pick.root_class)
                  + " exe=" + wide_to_utf8(pick.exe)
                  + " pid=" + std::to_string(pick.pid)
                  + " pt=" + std::to_string(pick.point.x) + "," + std::to_string(pick.point.y)
                  + " FEATURE_BROWSER=" + (kFeatureBrowser ? "1" : "0"));
}

std::wstring make_copydata_out_path()
{
    wchar_t temp[MAX_PATH] = {};
    const DWORD n = ::GetTempPathW(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH)
        return {};
    wchar_t path[MAX_PATH] = {};
    if (_snwprintf_s(path, ARRAYSIZE(path), _TRUNCATE, L"%stanit-dom-%lu-%lu.json",
                     temp, ::GetCurrentProcessId(), ::GetTickCount()) < 0)
        return {};
    return path;
}

bool fill_from_page_json(const nlohmann::json& page, ChromePickEnrichResult& out, std::string& err)
{
    nlohmann::json resolved = page;
    if (resolved.is_string()) {
        try {
            resolved = nlohmann::json::parse(resolved.get<std::string>());
        } catch (...) {
        }
    }
    if (resolved.is_null() || !resolved.is_object()) {
        err = "DOM probe returned no element";
        return false;
    }
    const nlohmann::json& element = resolved.contains("element") && resolved["element"].is_object()
        ? resolved["element"]
        : resolved;
    if (!element.is_object() || element.empty()) {
        err = "DOM probe returned empty element";
        return false;
    }
    out.ok = true;
    out.source = "webview-dom";
    out.element = element;
    out.html = element.value("html", std::string{});
    out.markdown = element.value("markdown", std::string{});
    if (element.contains("selectors") && element["selectors"].is_object()) {
        out.selectors = element["selectors"];
        out.css_selector = out.selectors.value("css", std::string{});
        out.xpath = out.selectors.value("xpath", std::string{});
    }
    return true;
}

bool parse_probe_json(const std::string& result_json, ChromePickEnrichResult& out, std::string& err)
{
    nlohmann::json page;
    try {
        page = nlohmann::json::parse(result_json);
    } catch (const std::exception& ex) {
        err = std::string("DOM probe JSON parse failed: ") + ex.what();
        return false;
    }
    return fill_from_page_json(page, out, err);
}

bool send_copydata_inspect(HWND host,
                           const nlohmann::json& req,
                           int timeout_ms,
                           ChromePickEnrichResult& out,
                           std::string& err)
{
    const std::string body = req.dump();
    COPYDATASTRUCT cds{};
    cds.dwData = media::win::webview_dom::kCopyDataMagic;
    cds.cbData = static_cast<DWORD>(body.size());
    cds.lpData = const_cast<char*>(body.data());

    const DWORD wait = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 15000u;
    logger::trace(std::string("[picker-enrich] copydata send host=") + hwnd_hex(host)
                  + " op=" + req.value("op", std::string{})
                  + " bytes=" + std::to_string(body.size())
                  + " timeout_ms=" + std::to_string(wait));

    DWORD_PTR send_result = 0;
    const LRESULT sent = ::SendMessageTimeoutW(
        host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG, wait, &send_result);
    if (!sent) {
        err = "in-app WebView inspect: WM_COPYDATA timed out or host hung";
        logger::trace(std::string("[picker-enrich] copydata SendMessageTimeout failed err=")
                      + std::to_string(::GetLastError()));
        return false;
    }
    if (send_result == 0) {
        err = "in-app WebView inspect: host ignored WM_COPYDATA (rebuild Tanit GUI)";
        logger::trace("[picker-enrich] copydata not handled (DefWindowProc)");
        return false;
    }

    const std::wstring out_path = utf8_to_wide(req.value("out", std::string{}));
    std::ifstream in(std::filesystem::path(out_path), std::ios::binary);
    if (!in) {
        err = "in-app WebView inspect: GUI wrote no result file (rebuild Tanit GUI)";
        logger::trace("[picker-enrich] copydata result file missing");
        return false;
    }
    const std::string reply_utf8((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    in.close();
    ::DeleteFileW(out_path.c_str());

    nlohmann::json reply;
    try {
        reply = nlohmann::json::parse(reply_utf8);
    } catch (const std::exception& ex) {
        err = std::string("in-app WebView inspect: result JSON: ") + ex.what();
        return false;
    }
    if (!reply.value("ok", false)) {
        err = reply.value("error", "in-app WebView inspect failed");
        logger::trace(std::string("[picker-enrich] copydata reply error: ") + err);
        return false;
    }
    logger::trace(std::string("[picker-enrich] copydata reply ok bytes=")
                  + std::to_string(reply_utf8.size()));
    return fill_from_page_json(reply.value("page", nlohmann::json{}), out, err);
}

bool enrich_pick_with_webview_dom_copydata(const media::win::inspect::PickResult& pick,
                                           HWND host,
                                           int timeout_ms,
                                           ChromePickEnrichResult& out,
                                           std::string& err,
                                           const DomInspectOpts& opts)
{
    if (!host || !::IsWindow(host)) {
        err = "no in-app WebView2 DOM probe host for pick";
        return false;
    }

    const std::wstring out_path = make_copydata_out_path();
    if (out_path.empty()) {
        err = "could not create temp path for WebView inspect";
        return false;
    }

    POINT client = pick.point;
    if (!::ScreenToClient(host, &client)) {
        err = "ScreenToClient failed for in-app browser host";
        return false;
    }

    RECT crc{};
    const bool have_client = ::GetClientRect(host, &crc) != FALSE;
    const bool inside = have_client && ::PtInRect(&crc, client);
    const HWND leaf = pick.hwnd ? pick.hwnd : pick.root;
    const bool ancestor_pick = leaf
        && host != leaf
        && !::IsChild(host, leaf);

    const DWORD wait = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 15000u;
    nlohmann::json base{
        {"v", 1},
        {"out", wide_to_utf8(out_path)},
        {"timeoutMs", static_cast<int>(wait)},
    };
    apply_serialize_caps(base, opts);

    auto send = [&](nlohmann::json req) {
        return send_copydata_inspect(host, req, static_cast<int>(wait), out, err);
    };
    auto send_document = [&]() {
        nlohmann::json req = base;
        req["op"] = "inspectSelector";
        req["selector"] = opts.document_selector;
        return send(std::move(req));
    };

    if (opts.scope == "document") {
        logger::trace(std::string("[picker-enrich] copydata scope=document sel=")
                      + opts.document_selector + " host=" + hwnd_hex(host));
        return send_document();
    }

    const bool edge_or_ancestor = ancestor_pick || !inside;
    if (opts.scope == "auto" && edge_or_ancestor) {
        logger::trace(std::string("[picker-enrich] copydata document inspect host=")
                      + hwnd_hex(host) + (inside ? " ancestor" : " edge")
                      + " sel=" + opts.document_selector
                      + " client=" + std::to_string(client.x) + "," + std::to_string(client.y));
        if (send_document())
            return true;
        if (!inside)
            return false;
        err.clear();
    }

    {
        nlohmann::json req = base;
        req["op"] = "inspectAtPoint";
        req["x"] = client.x;
        req["y"] = client.y;
        if (send(std::move(req)))
            return true;
    }

    if (opts.scope == "element") {
        logger::trace("[picker-enrich] copydata scope=element miss (no document fallback)");
        return false;
    }

    std::string point_err = err;
    err.clear();
    if (send_document()) {
        logger::trace(std::string("[picker-enrich] copydata point miss → ")
                      + opts.document_selector);
        return true;
    }
    if (err.empty())
        err = point_err;
    return false;
}

} // namespace

std::wstring class_of_hwnd(HWND hwnd)
{
    wchar_t buf[256] = {};
    if (hwnd) ::GetClassNameW(hwnd, buf, ARRAYSIZE(buf));
    return std::wstring(buf);
}

bool is_chrome_pick(const media::win::inspect::PickResult& pick)
{
    // Require a Chrome process — WebView2 also uses Chrome_* window classes.
    return ends_with_icase(pick.exe, L"chrome.exe")
        || ends_with_icase(pick.exe, L"chromium.exe");
}

bool is_chrome_hwnd(HWND hwnd, HWND root)
{
    if (!hwnd) return false;
    media::win::inspect::PickResult stub;
    stub.hwnd = hwnd;
    stub.root = root ? root : ::GetAncestor(hwnd, GA_ROOT);
    stub.class_name = class_of_hwnd(hwnd);
    if (stub.root)
        stub.root_class = class_of_hwnd(stub.root);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(stub.hwnd, &pid);
    stub.pid = pid;
    wchar_t path[MAX_PATH] = {};
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc) {
        DWORD size = MAX_PATH;
        if (::QueryFullProcessImageNameW(proc, 0, path, &size))
            stub.exe.assign(path);
        ::CloseHandle(proc);
    }
    return is_chrome_pick(stub);
}

bool looks_like_in_app_webview(HWND hwnd)
{
    if (!hwnd || is_chrome_hwnd(hwnd, nullptr))
        return false;
    if (has_dom_host_prop(hwnd) || is_webview2_hwnd(hwnd))
        return true;
    for (HWND h = ::GetParent(hwnd); h; ) {
        if (has_dom_host_prop(h) || is_webview2_hwnd(h))
            return !is_chrome_hwnd(h, nullptr);
        HWND parent = ::GetParent(h);
        if (!parent || parent == h)
            break;
        h = parent;
    }
    return false;
}

bool is_webview_dom_hwnd(HWND hwnd)
{
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    if (media::win::webview_dom::is_registered_pick(hwnd))
        return true;
#endif
    return looks_like_in_app_webview(hwnd);
}

HWND webview_dom_host_at(HWND hwnd, POINT screen_pt)
{
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    if (HWND host = media::win::webview_dom::find_host_at(hwnd, screen_pt))
        return host;
    if (HWND host = media::win::webview_dom::find_host(hwnd))
        return host;
#endif
    return find_cross_process_dom_host(hwnd, screen_pt);
}

bool is_webview_dom_at(HWND hwnd, POINT screen_pt)
{
    return webview_dom_host_at(hwnd, screen_pt) != nullptr;
}

bool is_webview_dom_pick(const media::win::inspect::PickResult& pick)
{
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    if (media::win::webview_dom::find_host_at(pick.hwnd, pick.point))
        return true;
    if (pick.root && pick.root != pick.hwnd
        && media::win::webview_dom::find_host_at(pick.root, pick.point))
        return true;
#endif
    if (is_chrome_pick(pick))
        return false;
    if (find_cross_process_dom_host(pick))
        return true;
    return is_webview_dom_hwnd(pick.hwnd) || is_webview_dom_hwnd(pick.root);
}

bool is_dom_enrichable_pick(const media::win::inspect::PickResult& pick)
{
    return is_webview_dom_pick(pick) || is_chrome_pick(pick);
}

bool is_dom_enrichable_hwnd(HWND hwnd, HWND root)
{
    if (is_webview_dom_hwnd(hwnd))
        return true;
    if (root && is_webview_dom_hwnd(root))
        return true;
    return is_chrome_hwnd(hwnd, root);
}

bool is_dom_enrichable_at(HWND hwnd, POINT screen_pt, HWND root)
{
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    if (media::win::webview_dom::find_host_at(hwnd, screen_pt))
        return true;
    if (root && media::win::webview_dom::find_host_at(root, screen_pt))
        return true;
#endif
    if (find_cross_process_dom_host(hwnd, screen_pt))
        return true;
    if (root && root != hwnd && find_cross_process_dom_host(root, screen_pt))
        return true;
    return is_dom_enrichable_hwnd(hwnd, root);
}

bool fill_from_bridge_response(const nlohmann::json& response,
                               ChromePickEnrichResult& out,
                               std::string& err)
{
    if (!response.value("ok", false) || response.value("type", std::string{}) != "elementInspected") {
        if (response.contains("error") && response["error"].is_object())
            err = response["error"].value("message", err.empty() ? "DOM inspection failed" : err);
        else if (err.empty())
            err = "DOM inspection failed";
        return false;
    }

    const nlohmann::json& data = response["data"];
    const nlohmann::json& element = data.value("element", nlohmann::json::object());
    out.ok = true;
    out.source = "dom";
    out.element = element;
    out.html = element.value("html", std::string{});
    out.markdown = element.value("markdown", std::string{});
    if (element.contains("selectors") && element["selectors"].is_object()) {
        out.selectors = element["selectors"];
        out.css_selector = out.selectors.value("css", std::string{});
        out.xpath = out.selectors.value("xpath", std::string{});
    }
    return true;
}

bool inspect_dom_selector(const std::string& tab_title_hint,
                          const std::string& selector,
                          int screen_x,
                          int screen_y,
                          int timeout_ms,
                          ChromePickEnrichResult& out,
                          std::string& err,
                          const DomInspectOpts& opts)
{
    nlohmann::json payload = nlohmann::json{
        {"selector", selector},
        {"screenX", screen_x},
        {"screenY", screen_y},
    };
    if (!tab_title_hint.empty())
        payload["tabTitleIncludes"] = tab_title_hint;
    apply_serialize_caps(payload, opts);

    nlohmann::json response;
    if (!send_bridge_request(make_bridge_request("inspectSelector", std::move(payload)),
                             response,
                             timeout_ms > 0 ? timeout_ms : 15000,
                             err))
        return false;
    return fill_from_bridge_response(response, out, err);
}

bool inspect_dom_at_client_point(const std::string& tab_title_hint,
                                 int client_x,
                                 int client_y,
                                 int screen_x,
                                 int screen_y,
                                 int timeout_ms,
                                 ChromePickEnrichResult& out,
                                 std::string& err,
                                 const DomInspectOpts& opts)
{
    nlohmann::json payload = nlohmann::json{
        {"x", client_x},
        {"y", client_y},
        {"screenX", screen_x},
        {"screenY", screen_y},
        // Win32 ScreenToClient on the render HWND yields device pixels.
        {"coordinateSpace", "physical"},
    };
    if (!tab_title_hint.empty())
        payload["tabTitleIncludes"] = tab_title_hint;
    apply_serialize_caps(payload, opts);

    nlohmann::json response;
    if (!send_bridge_request(make_bridge_request("inspectAtPoint", std::move(payload)),
                             response,
                             timeout_ms > 0 ? timeout_ms : 15000,
                             err))
        return false;
    return fill_from_bridge_response(response, out, err);
}

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
HWND resolve_webview_host(const media::win::inspect::PickResult& pick)
{
    HWND host = media::win::webview_dom::find_host_at(pick.hwnd, pick.point);
    if (host)
        return host;
    if (pick.root && pick.root != pick.hwnd)
        host = media::win::webview_dom::find_host_at(pick.root, pick.point);
    if (host)
        return host;
    host = media::win::webview_dom::find_host(pick.hwnd);
    if (host)
        return host;
    return media::win::webview_dom::find_host(pick.root);
}

bool enrich_pick_with_webview_dom(const media::win::inspect::PickResult& pick,
                                  int timeout_ms,
                                  ChromePickEnrichResult& out,
                                  std::string& err,
                                  const DomInspectOpts& opts)
{
    const HWND host = resolve_webview_host(pick);
    if (!host) {
        err = "no in-app WebView2 DOM probe host for pick";
        return false;
    }

    POINT client = pick.point;
    if (!::ScreenToClient(host, &client)) {
        err = "ScreenToClient failed for in-app browser host";
        return false;
    }

    RECT crc{};
    const bool have_client = ::GetClientRect(host, &crc) != FALSE;
    const bool inside = have_client && ::PtInRect(&crc, client);
    const HWND leaf = pick.hwnd ? pick.hwnd : pick.root;
    const bool ancestor_pick = leaf
        && host != leaf
        && !::IsChild(host, leaf);

    const DWORD wait = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 15000u;
    const auto serialize = to_serialize(opts);
    const std::wstring doc_sel = document_selector_w(opts);
    std::string result_json;

    auto dump_document = [&]() {
        return media::win::webview_dom::inspect_selector(
            host, doc_sel, serialize, wait, result_json, err)
            && parse_probe_json(result_json, out, err);
    };

    if (opts.scope == "document") {
        logger::info(std::string("[picker-enrich] webview scope=document sel=")
                     + opts.document_selector);
        return dump_document();
    }

    // Parent dock / main-frame picks, or a click just outside the WebView
    // (document edge), dump the document instead of elementFromPoint.
    if (opts.scope == "auto" && (ancestor_pick || !inside)) {
        logger::info(std::string("[picker-enrich] webview document inspect host=0x")
                     + std::to_string(reinterpret_cast<uintptr_t>(host))
                     + (inside ? " ancestor" : " edge")
                     + " sel=" + opts.document_selector);
        if (dump_document())
            return true;
        if (!inside)
            return false;
        err.clear();
    }

    if (media::win::webview_dom::inspect_at_physical_point(
            host, client.x, client.y, serialize, wait, result_json, err))
        return parse_probe_json(result_json, out, err);

    if (opts.scope == "element") {
        logger::info("[picker-enrich] webview-dom scope=element miss");
        return false;
    }

    std::string point_err = err;
    err.clear();
    if (dump_document()) {
        logger::info(std::string("[picker-enrich] webview-dom point miss → ")
                     + opts.document_selector);
        return true;
    }
    if (err.empty())
        err = point_err;
    return false;
}
#endif // FEATURE_BROWSER

bool enrich_pick_with_chrome_dom(const media::win::inspect::PickResult& pick,
                                 int timeout_ms,
                                 ChromePickEnrichResult& out,
                                 std::string& err,
                                 const DomInspectOpts& raw_opts)
{
    const DomInspectOpts opts = normalize_dom_inspect_opts(raw_opts);
    log_pick(pick);
    logger::trace(std::string("[picker-enrich] opts scope=") + opts.scope
                  + " documentSelector=" + opts.document_selector
                  + " textMax=" + std::to_string(opts.text_max_chars)
                  + " htmlMax=" + std::to_string(opts.html_max_chars)
                  + " mdMax=" + std::to_string(opts.markdown_max_chars));

    HWND inproc_host = nullptr;
#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    inproc_host = resolve_webview_host(pick);
    logger::trace(std::string("[picker-enrich] in-process host=") + hwnd_hex(inproc_host));
    if (inproc_host) {
        logger::info("[picker-enrich] trying webview-dom");
        if (enrich_pick_with_webview_dom(pick, timeout_ms, out, err, opts)) {
            logger::info("[picker-enrich] webview-dom ok");
            return true;
        }
        logger::warn(std::string("[picker-enrich] webview-dom failed: ") + err);
    }
#endif

    const HWND xp_host = find_cross_process_dom_host(pick);
    logger::trace(std::string("[picker-enrich] cross-process host=") + hwnd_hex(xp_host));
    if (xp_host && xp_host != inproc_host) {
        logger::info("[picker-enrich] trying webview-dom copydata");
        if (enrich_pick_with_webview_dom_copydata(
                pick, xp_host, timeout_ms, out, err, opts)) {
            logger::info("[picker-enrich] webview-dom copydata ok");
            return true;
        }
        logger::warn(std::string("[picker-enrich] webview-dom copydata failed: ") + err);
        if (!is_chrome_pick(pick))
            return false;
        err.clear();
    } else if (xp_host && xp_host == inproc_host) {
        logger::trace("[picker-enrich] skip copydata (same host as in-process)");
        if (!is_chrome_pick(pick))
            return false;
        err.clear();
    }

    if (!is_chrome_pick(pick)) {
        err = err.empty() ? "picked target is not Chrome or in-app browser" : err;
        logger::warn(std::string("[picker-enrich] skip: ") + err);
        return false;
    }

    const POINT client = client_point_for_pick(pick);
    std::string title_hint;
    if (!pick.title.empty())
        title_hint = wide_to_utf8(strip_chrome_window_suffix(pick.title));

    auto chrome_document = [&]() {
        logger::trace(std::string("[picker-enrich] chrome inspectSelector ")
                      + opts.document_selector);
        return inspect_dom_selector(title_hint,
                                    opts.document_selector,
                                    pick.point.x,
                                    pick.point.y,
                                    timeout_ms,
                                    out,
                                    err,
                                    opts);
    };

    if (opts.scope == "document")
        return chrome_document();

    logger::trace(std::string("[picker-enrich] chrome bridge client=")
                  + std::to_string(client.x) + "," + std::to_string(client.y));
    if (inspect_dom_at_client_point(title_hint,
                                    client.x,
                                    client.y,
                                    pick.point.x,
                                    pick.point.y,
                                    timeout_ms,
                                    out,
                                    err,
                                    opts))
        return true;

    if (opts.scope == "element")
        return false;

    std::string point_err = err;
    err.clear();
    if (chrome_document()) {
        logger::trace(std::string("[picker-enrich] chrome point miss → ")
                      + opts.document_selector);
        return true;
    }
    if (err.empty())
        err = point_err;
    return false;
}

std::string dom_field_as_text(const nlohmann::json& element, const char* key)
{
    if (!element.contains(key) || !element[key].is_string()) return {};
    std::string value = element[key].get<std::string>();
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string text_from_dom_result(const ChromePickEnrichResult& dom)
{
    if (!dom.ok) return {};
    const nlohmann::json& element = dom.element;
    static const char* keys[] = {
        "speakText", "innerText", "text", "value", "ariaLabel", "placeholder",
    };
    for (const char* key : keys) {
        const std::string value = dom_field_as_text(element, key);
        if (!value.empty() && value != "[redacted]")
            return value;
    }
    return {};
}

DomInspectOpts normalize_dom_inspect_opts(DomInspectOpts opts)
{
    if (opts.scope != "element" && opts.scope != "document")
        opts.scope = "auto";
    if (opts.document_selector != "body" && opts.document_selector != "main")
        opts.document_selector = "html";
    if (opts.text_max_chars < 0)
        opts.text_max_chars = 0;
    if (opts.html_max_chars < 0)
        opts.html_max_chars = 0;
    if (opts.markdown_max_chars < 0)
        opts.markdown_max_chars = 0;
    return opts;
}

std::string markdown_from_dom_result(const ChromePickEnrichResult& dom,
                                     int markdown_max_chars)
{
    std::string md = !dom.html.empty()
        ? html::to_markdown(html::clean_for_markdown(dom.html))
        : dom.markdown;
    clamp_text_cap(md, markdown_max_chars);
    return md;
}

namespace {

bool send_copydata_selected_text(HWND host, int timeout_ms, std::wstring& out, std::string& err)
{
    const std::wstring out_path = make_copydata_out_path();
    if (out_path.empty()) {
        err = "could not create temp path for WebView selection";
        return false;
    }
    const DWORD wait = timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 8000u;
    const nlohmann::json req{
        {"v", 1},
        {"op", "getSelectedText"},
        {"out", wide_to_utf8(out_path)},
        {"timeoutMs", static_cast<int>(wait)},
    };
    const std::string body = req.dump();
    COPYDATASTRUCT cds{};
    cds.dwData = media::win::webview_dom::kCopyDataMagic;
    cds.cbData = static_cast<DWORD>(body.size());
    cds.lpData = const_cast<char*>(body.data());

    DWORD_PTR send_result = 0;
    if (!::SendMessageTimeoutW(host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                               SMTO_ABORTIFHUNG, wait, &send_result) ||
        send_result == 0) {
        err = "in-app WebView selection: WM_COPYDATA failed";
        return false;
    }

    std::ifstream in(std::filesystem::path(out_path), std::ios::binary);
    if (!in) {
        err = "in-app WebView selection: no result file";
        return false;
    }
    const std::string reply_utf8((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    in.close();
    ::DeleteFileW(out_path.c_str());

    nlohmann::json reply;
    try {
        reply = nlohmann::json::parse(reply_utf8);
    } catch (const std::exception& ex) {
        err = std::string("in-app WebView selection JSON: ") + ex.what();
        return false;
    }
    if (!reply.value("ok", false)) {
        err = reply.value("error", "in-app WebView selection failed");
        return false;
    }
    out = utf8_to_wide(reply.value("text", std::string{}));
    return !out.empty();
}

bool chrome_bridge_selected_text(const std::wstring& title,
                                 POINT screen_pt,
                                 int timeout_ms,
                                 std::wstring& out,
                                 std::string& err)
{
    nlohmann::json payload = nlohmann::json::object();
    if (!title.empty())
        payload["tabTitleIncludes"] = wide_to_utf8(strip_chrome_window_suffix(title));
    if (screen_pt.x || screen_pt.y) {
        payload["screenX"] = screen_pt.x;
        payload["screenY"] = screen_pt.y;
    }

    nlohmann::json response;
    if (!send_bridge_request(make_bridge_request("getSelectedText", std::move(payload)),
                             response,
                             timeout_ms > 0 ? timeout_ms : 8000,
                             err))
        return false;
    if (!response.value("ok", false) || response.value("type", std::string{}) != "selectedText") {
        if (response.contains("error") && response["error"].is_object())
            err = response["error"].value("message", "Chrome selection failed");
        else if (err.empty())
            err = "Chrome selection failed";
        return false;
    }
    out = utf8_to_wide(response.value("data", nlohmann::json::object())
                           .value("text", std::string{}));
    return !out.empty();
}

} // namespace

bool get_selected_text_from_dom(HWND hwnd,
                                HWND root,
                                const std::wstring& title,
                                POINT screen_pt,
                                int timeout_ms,
                                std::wstring& out,
                                std::string& err)
{
    out.clear();
    err.clear();

#if defined(FEATURE_BROWSER) && FEATURE_BROWSER
    HWND inproc = media::win::webview_dom::find_host_at(hwnd, screen_pt);
    if (!inproc && root && root != hwnd)
        inproc = media::win::webview_dom::find_host_at(root, screen_pt);
    if (!inproc)
        inproc = media::win::webview_dom::find_host(hwnd);
    if (!inproc && root)
        inproc = media::win::webview_dom::find_host(root);
    if (inproc) {
        if (media::win::webview_dom::inspect_selected_text(
                inproc, timeout_ms > 0 ? static_cast<DWORD>(timeout_ms) : 8000u, out, err) &&
            !out.empty())
            return true;
        err.clear();
    }
#endif

    HWND xp = find_cross_process_dom_host(hwnd, screen_pt);
    if (!xp && root && root != hwnd)
        xp = find_cross_process_dom_host(root, screen_pt);
    if (xp) {
        if (send_copydata_selected_text(xp, timeout_ms, out, err) && !out.empty())
            return true;
        if (!is_chrome_hwnd(hwnd, root))
            return false;
        err.clear();
    }

    if (!is_chrome_hwnd(hwnd, root) && !is_chrome_hwnd(root, nullptr)) {
        if (err.empty())
            err = "focused target is not Chrome or in-app browser";
        return false;
    }

    return chrome_bridge_selected_text(title, screen_pt, timeout_ms, out, err);
}

} // namespace media::win::chrome_inspect
