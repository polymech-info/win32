#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>

#include <nlohmann/json.hpp>

namespace media::win::inspect {
struct PickResult;
}

namespace media::win::chrome_inspect {

struct ChromePickEnrichResult {
    bool ok = false;
    std::string source = "uia";
    std::string html;
    std::string markdown;
    /// DOM selectors for the picked node: css, xpath, id, dataTestId, name, aria.
    /// Usable directly by `assistant app-use` / `app-inspect` and app_click.
    nlohmann::json selectors = nlohmann::json::object();
    std::string css_selector;
    std::string xpath;
    nlohmann::json element = nlohmann::json::object();
};

/// Caps match `apps/shared/inspect/domTraverse.ts` DEFAULT_OPTS (0 = unlimited).
struct DomInspectOpts {
    /// `element` = leaf only; `document` = always `documentSelector`;
    /// `auto` = leaf, then document on miss / edge / ancestor pick.
    std::string scope = "auto";
    /// `html`, `body`, or `main`.
    std::string document_selector = "html";
    int text_max_chars = 0;
    int html_max_chars = 8 * 1024 * 1024;
    int markdown_max_chars = 2 * 1024 * 1024;
};

DomInspectOpts normalize_dom_inspect_opts(DomInspectOpts opts);

bool is_chrome_pick(const media::win::inspect::PickResult& pick);
bool is_chrome_hwnd(HWND hwnd, HWND root = nullptr);

/// In-app WebView2 host with DOM probe (browser / viewer / chat / CWebView).
bool is_webview_dom_pick(const media::win::inspect::PickResult& pick);
bool is_webview_dom_hwnd(HWND hwnd);
bool is_webview_dom_at(HWND hwnd, POINT screen_pt);
/// Registered WebView2 host for this HWND / point, or nullptr.
HWND webview_dom_host_at(HWND hwnd, POINT screen_pt);

/// Chrome extension bridge and/or in-app WebView2 script probe.
bool is_dom_enrichable_pick(const media::win::inspect::PickResult& pick);
bool is_dom_enrichable_hwnd(HWND hwnd, HWND root = nullptr);
/// Point-aware: parent dock / main-frame HWNDs that contain an in-app WebView.
bool is_dom_enrichable_at(HWND hwnd, POINT screen_pt, HWND root = nullptr);

bool inspect_dom_at_client_point(const std::string& tab_title_hint,
                                 int client_x,
                                 int client_y,
                                 int screen_x,
                                 int screen_y,
                                 int timeout_ms,
                                 ChromePickEnrichResult& out,
                                 std::string& err,
                                 const DomInspectOpts& opts = {});

/// Prefer in-app WebView2 `ExecuteScript` probe; fall back to Chrome native host.
bool enrich_pick_with_chrome_dom(const media::win::inspect::PickResult& pick,
                                 int timeout_ms,
                                 ChromePickEnrichResult& out,
                                 std::string& err,
                                 const DomInspectOpts& opts = {});

std::string markdown_from_dom_result(const ChromePickEnrichResult& dom,
                                     int markdown_max_chars = 0);

/// Speakable plain text: speakText (cleaned) → innerText → text → value → aria → placeholder.
std::string text_from_dom_result(const ChromePickEnrichResult& dom);

/// Focused-document selection via in-app WebView2 and/or the Chrome extension.
/// Does not fall back to UIA — caller tries UI Automation first.
bool get_selected_text_from_dom(HWND hwnd,
                                HWND root,
                                const std::wstring& title,
                                POINT screen_pt,
                                int timeout_ms,
                                std::wstring& out,
                                std::string& err);

} // namespace media::win::chrome_inspect
