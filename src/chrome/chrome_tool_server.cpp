#include "chrome_tool_server.hpp"
#include "json/json_utils.hpp"
#include "fs/path_sanitizer.hpp"
#include "agent_serve_client.hpp"
#include "extension_channel.hpp"
#include "llm/tools/browser_use/browser_use_compact.hpp"
#include "llm/tools/browser_use/browser_use_inject.hpp"
#include "llm/tools/browser_use/browser_use_page_model.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cwctype>
#include <charconv>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include "string/string_utils.hpp"

namespace media::win::chrome_inspect {
namespace {

std::atomic<int> g_port{0};
std::atomic<int> g_clients{0};
std::string g_token;
std::mutex g_snap_mu;
nlohmann::json g_snapshot = nlohmann::json::object();

constexpr const char* k_extension_origin = "chrome-extension://bhpeejlpjkdlbibojmbdhdcondflmban";

std::string cors_headers(const std::string& origin)
{
    if (origin != k_extension_origin)
        return {};
    return std::string("Access-Control-Allow-Origin: ") + origin +
           "\r\nVary: Origin"
           "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS"
           "\r\nAccess-Control-Allow-Headers: Content-Type, X-Tanit-Session\r\n";
}

bool send_all(SOCKET sock, const char* data, int size)
{
    int sent = 0;
    while (sent < size) {
        const int n = ::send(sock, data + sent, size - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

std::string http_response(int status, const char* reason, const std::string& body, const std::string& origin)
{
    return "HTTP/1.1 " + std::to_string(status) + " " + reason +
           "\r\nContent-Type: application/json\r\n" + cors_headers(origin) +
           "Content-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

std::string http_file_response(const std::string& content_type, const std::string& body, const std::string& origin)
{
    return "HTTP/1.1 200 OK\r\nContent-Type: " + content_type + "\r\n" + cors_headers(origin) +
           "Cache-Control: no-store\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + body;
}

bool header_name_is(const std::string& line, const char* name)
{
    const std::size_t n = std::char_traits<char>::length(name);
    if (line.size() < n + 1 || line[n] != ':')
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        char c = line[i];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != name[i])
            return false;
    }
    return true;
}

int content_length_of(const std::string& headers, bool& ok)
{
    ok = true;
    int seen = 0;
    int value = 0;
    std::size_t at = 0;
    while (at < headers.size()) {
        const auto end = headers.find("\r\n", at);
        const std::string line = headers.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.find('\0') != std::string::npos || (!line.empty() && (line[0] == ' ' || line[0] == '\t'))) {
            ok = false;
            return -1;
        }
        if (header_name_is(line, "transfer-encoding")) {
            ok = false;
            return -1;
        }
        if (header_name_is(line, "content-length")) {
            ++seen;
            std::size_t value_at = std::string("content-length").size() + 1;
            while (value_at < line.size() && (line[value_at] == ' ' || line[value_at] == '\t'))
                ++value_at;
            if (value_at >= line.size() || line[value_at] == '+' || line[value_at] == '-') {
                ok = false;
                return -1;
            }
            int parsed = 0;
            const char* first = line.data() + value_at;
            const char* last = line.data() + line.size();
            const auto result = std::from_chars(first, last, parsed);
            if (result.ec != std::errc{} || result.ptr != last || parsed < 0) {
                ok = false;
                return -1;
            }
            value = parsed;
        }
        if (end == std::string::npos)
            break;
        at = end + 2;
    }
    if (seen > 1) {
        ok = false;
        return -1;
    }
    return seen == 0 ? 0 : value;
}

std::string header_value(const std::string& headers, const char* name)
{
    const std::string key = std::string(name) + ":";
    std::size_t at = 0;
    while (at < headers.size()) {
        const auto end = headers.find("\r\n", at);
        const std::string line = headers.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.size() >= key.size()) {
            bool match = true;
            for (std::size_t i = 0; i < key.size(); ++i) {
                char c = line[i];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != key[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::size_t value = key.size();
                while (value < line.size() && (line[value] == ' ' || line[value] == '\t'))
                    ++value;
                return line.substr(value);
            }
        }
        if (end == std::string::npos)
            break;
        at = end + 2;
    }
    return {};
}

bool read_http(SOCKET sock, std::string& method, std::string& path, std::string& query, std::string& body,
               std::string& origin, std::string& session, std::string& host, std::string& fetch_dest)
{
    std::string buf;
    char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        const int n = ::recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, tmp + n);
        if (buf.size() > 64 * 1024)
            return false;
    }
    const auto header_end = buf.find("\r\n\r\n");
    if (buf.find('\0') < header_end)
        return false;
    const std::string headers = buf.substr(0, header_end);
    origin = header_value(headers, "origin");
    session = header_value(headers, "x-tanit-session");
    host = header_value(headers, "host");
    fetch_dest = header_value(headers, "sec-fetch-dest");
    fetch_dest = media::strutil::lower_ascii(std::move(fetch_dest));
    body = buf.substr(header_end + 4);
    const auto line_end = headers.find("\r\n");
    const std::string request_line = line_end == std::string::npos ? headers : headers.substr(0, line_end);
    const auto sp1 = request_line.find(' ');
    const auto sp2 = request_line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    method = request_line.substr(0, sp1);
    if (method != "GET" && method != "POST" && method != "OPTIONS")
        return false;
    if (request_line.substr(sp2 + 1) != "HTTP/1.1")
        return false;
    path = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (path.empty() || path[0] != '/' || path.rfind("//", 0) == 0 || path.find("://") != std::string::npos)
        return false;
    const auto q = path.find('?');
    if (q != std::string::npos) {
        query = path.substr(q + 1);
        path.resize(q);
    } else {
        query.clear();
    }
    if (path.empty() || path.find('\0') != std::string::npos)
        return false;
    const int cap = path == "/api/context-file" ? (32 * 1024 * 1024) : (1 << 20);
    bool length_ok = false;
    const int need = content_length_of(headers, length_ok);
    if (!length_ok || need < 0 || need > cap) return false;
    while (static_cast<int>(body.size()) < need) {
        const int n = ::recv(sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        body.append(tmp, tmp + n);
        if (static_cast<int>(body.size()) > cap)
            return false;
    }
    if (static_cast<int>(body.size()) > need) body.resize(static_cast<std::size_t>(need));
    return true;
}

nlohmann::json tool_entry(const char* name, const char* description)
{
    return {
        {"name", name},
        {"description", description},
        {"parameters", {{"type", "object"}, {"additionalProperties", true}}},
    };
}

nlohmann::json catalog()
{
    return {
        {"id", "chrome"},
        {"name", "tanit chat"},
        {"tools", nlohmann::json::array({
            tool_entry("browser_read",
                       "Read the active Chrome tab. Returns refs for click, type, and select. "
                       "files[] lists visible image URLs and filename-like links. "
                       "This is the user's Chrome window, not an in-app WebView."),
            tool_entry("browser_find", "Find elements in the active Chrome tab."),
            tool_entry("browser_scope", "Inspect or pin a zone in the active Chrome tab."),
            tool_entry("browser_click", "Click an element ref in the active Chrome tab."),
            tool_entry("browser_type", "Type into an element ref in the active Chrome tab."),
            tool_entry("browser_select", "Choose a select option in the active Chrome tab."),
            tool_entry("browser_batch",
                       "Run read, find, click, type, and select steps on the active Chrome tab."),
            tool_entry("browser_navigate",
                       "Navigate the Chrome window: goto, back, reload, tabs, switch, new, close."),
        })},
    };
}

std::string context_prompt()
{
    nlohmann::json snap;
    {
        std::lock_guard lock(g_snap_mu);
        snap = g_snapshot;
    }
    if (!snap.is_object())
        return "No Chrome tab is attached to this turn.";
    const std::string url = snap.value("currentUrl", std::string{});
    const std::string title = snap.value("currentTitle", std::string{});
    std::string text =
        "The browser for this turn is the user's Chrome window. "
        "browser_read, browser_find, browser_scope, browser_click, browser_type, "
        "browser_select, browser_batch, and browser_navigate act on that window. "
        "Do not claim the browser connection is unavailable.";
    if (url.empty()) {
        text += " No http(s) tab is active.";
        return text;
    }
    text += " Active tab (the page the user is looking at now): " + title + " " + url;
    text += " browser_read with no tab argument reads this tab, not an earlier one.";
    if (snap.contains("tabs") && snap["tabs"].is_array()) {
        text += " Open tabs:";
        for (const auto& tab : snap["tabs"]) {
            if (!tab.is_object()) continue;
            text += " [" + std::to_string(tab.value("id", 0)) + "] " +
                    tab.value("title", std::string{}) + " " + tab.value("url", std::string{});
        }
    }
    return text;
}

bool tool_call_allowed(const std::string& name, const nlohmann::json& args, int depth);

// Chrome domUse wraps batch steps and the page model under data. The shared
// compactor reads those fields on the envelope, matching the in-app batch report.
void hoist_batch_payload(nlohmann::json& envelope)
{
    if (envelope.value("tool", std::string{}) != "browser_batch")
        return;
    if (!envelope.contains("data") || !envelope["data"].is_object())
        return;
    const nlohmann::json& data = envelope["data"];
    static constexpr const char* k_keys[] = {
        "steps", "all_ok", "model", "first_failure", "batch_error",
    };
    for (const char* key : k_keys) {
        if (!envelope.contains(key) && data.contains(key))
            envelope[key] = data[key];
    }
}

nlohmann::json kept_chrome_fields(const nlohmann::json& envelope)
{
    nlohmann::json kept = nlohmann::json::object();
    static constexpr const char* k_keys[] = {
        "tabId", "code", "action", "tabs", "closed", "reused",
    };
    for (const char* key : k_keys) {
        if (envelope.contains(key))
            kept[key] = envelope[key];
    }
    return kept;
}

void restore_fields(nlohmann::json& envelope, const nlohmann::json& kept)
{
    if (!envelope.is_object() || !kept.is_object())
        return;
    for (auto it = kept.begin(); it != kept.end(); ++it) {
        if (!envelope.contains(it.key()))
            envelope[it.key()] = it.value();
    }
}

nlohmann::json finish_chrome_browser_result(nlohmann::json envelope, const nlohmann::json& args)
{
    if (!envelope.is_object())
        return envelope;
    hoist_batch_payload(envelope);
#if defined(FEATURE_BROWSER_PAGE_MODEL) && FEATURE_BROWSER_PAGE_MODEL
    if (args.contains("query") && args["query"].is_string()) {
        const std::string query = args["query"].get<std::string>();
        if (!query.empty())
            media::llm::browser_use::enrich_envelope_page_model(envelope, query);
    }
    if (envelope.value("tool", std::string{}) == "browser_batch"
        && envelope.contains("data") && envelope["data"].is_object()
        && envelope["data"].contains("model") && envelope["data"]["model"].is_object()) {
        envelope["model"] = envelope["data"]["model"];
    }
#else
    (void)args;
#endif
    if (auto hit = media::llm::browser_use::detect_prompt_injection_in_envelope(envelope))
        media::llm::browser_use::apply_prompt_injection_guard(envelope, *hit);
    // Navigate carries the tab list at the top level. The action compactor
    // rebuilds the object and would drop it.
    if (envelope.value("tool", std::string{}) == "browser_navigate")
        return envelope;
    const nlohmann::json kept = kept_chrome_fields(envelope);
    envelope = media::llm::browser_use::compact_tool_envelope_for_history(std::move(envelope));
    restore_fields(envelope, kept);
    return envelope;
}

nlohmann::json call_tool(const nlohmann::json& body)
{
    const std::string name = body.value("name", std::string{});
    nlohmann::json args = body.contains("arguments") && body["arguments"].is_object()
                              ? body["arguments"]
                              : nlohmann::json::object();
    if (name.empty())
        return {{"ok", false}, {"error", "missing tool name"}};
    if (!tool_call_allowed(name, args, 0))
        return {{"ok", false}, {"tool", name}, {"error", "tool is not allowed"}};

    const std::string request_id = "cht-" + std::to_string(::GetTickCount64());
    const nlohmann::json request = {
        {"protocolVersion", 1},
        {"requestId", request_id},
        {"command", "browserUse"},
        {"payload", {{"name", name}, {"arguments", args}}},
    };
    nlohmann::json response;
    if (!ask_extension(request, response, 25000)) {
        return {{"ok", false}, {"tool", name},
                {"error", "Chrome extension did not answer the browser tool."}};
    }
    if (!response.value("ok", false)) {
        std::string message = "browser tool failed";
        if (response.contains("error") && response["error"].is_object())
            message = response["error"].value("message", message);
        return {{"ok", false}, {"tool", name}, {"error", message}};
    }
    if (response.contains("data") && response["data"].is_object())
        return finish_chrome_browser_result(response["data"], args);
    return {{"ok", false}, {"tool", name}, {"error", "Chrome returned an empty browser result."}};
}

std::string query_value(const std::string& query, const char* key)
{
    const std::string prefix = std::string(key) + "=";
    std::size_t at = 0;
    while (at < query.size()) {
        const auto amp = query.find('&', at);
        const std::string part = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        if (part.rfind(prefix, 0) == 0)
            return part.substr(prefix.size());
        if (amp == std::string::npos)
            break;
        at = amp + 1;
    }
    return {};
}

bool token_matches(const std::string& got)
{
    if (g_token.empty() || got.size() != g_token.size())
        return false;
    unsigned diff = 0;
    for (std::size_t i = 0; i < g_token.size(); ++i)
        diff |= static_cast<unsigned char>(got[i]) ^ static_cast<unsigned char>(g_token[i]);
    return diff == 0;
}

bool host_is_this_server(const std::string& host)
{
    std::string lower = host;
    lower = media::strutil::lower_ascii(std::move(lower));
    const int port = g_port.load();
    if (port <= 0)
        return false;
    const std::string port_s = std::to_string(port);
    return lower == "127.0.0.1:" + port_s || lower == "localhost:" + port_s;
}

bool origin_allowed(const std::string& method, const std::string& path, const std::string& origin,
                    const std::string& fetch_dest)
{
    if (origin == k_extension_origin)
        return true;
    if (!origin.empty())
        return false;
    if (path.rfind("/api/agent-tools", 0) == 0)
        return true;
    return method == "GET" && path == "/api/local-file" &&
           (fetch_dest == "image" || fetch_dest == "media");
}

bool batch_steps_allowed(const nlohmann::json& args, int depth);

bool tool_call_allowed(const std::string& name, const nlohmann::json& args, int depth)
{
    if (depth > 4)
        return false;
    static constexpr const char* k_names[] = {
        "browser_read", "browser_find", "browser_scope", "browser_click",
        "browser_type", "browser_select", "browser_batch", "browser_navigate",
    };
    bool known = false;
    for (const char* allowed : k_names) {
        if (name == allowed)
            known = true;
    }
    if (!known)
        return false;
    if (name == "browser_navigate") {
        std::string action = args.value("action", std::string{});
        if (action.empty())
            action = args.contains("url") && args["url"].is_string() && !args["url"].get<std::string>().empty()
                         ? "goto"
                         : "tabs";
        return action == "tabs" || action == "new" || action == "switch" || action == "close" ||
               action == "reload" || action == "back" || action == "goto";
    }
    if (name != "browser_batch")
        return true;
    return batch_steps_allowed(args, depth);
}

bool batch_steps_allowed(const nlohmann::json& args, int depth)
{
    if (depth > 4 || !args.contains("steps") || !args["steps"].is_array() || args["steps"].empty() ||
        args["steps"].size() > 32)
        return false;
    for (const auto& step : args["steps"]) {
        if (!step.is_object())
            return false;
        std::string action = step.value("action", std::string{});
        if (action.empty())
            action = step.value("tool", step.value("name", std::string{}));
        if (action == "browser_read") action = "read";
        else if (action == "browser_find") action = "find";
        else if (action == "browser_click") action = "click";
        else if (action == "browser_type") action = "type";
        else if (action == "browser_select") action = "select";
        else if (action == "browser_scope") action = "scope";
        if (action == "browser_batch" || action == "batch") {
            const nlohmann::json& inner =
                step.contains("arguments") && step["arguments"].is_object() ? step["arguments"] : step;
            if (!batch_steps_allowed(inner, depth + 1))
                return false;
            continue;
        }
        if (action != "read" && action != "find" && action != "click" && action != "type" &&
            action != "select" && action != "wait")
            return false;
        if (step.contains("steps") || step.contains("arguments"))
            return false;
    }
    return true;
}

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex_nibble(in[i + 1]);
            const int lo = hex_nibble(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i] == '+' ? ' ' : in[i]);
    }
    return out;
}

std::string safe_file_leaf(const std::string& raw)
{
    std::string leaf = raw;
    const auto slash = leaf.find_last_of("/\\");
    if (slash != std::string::npos)
        leaf = leaf.substr(slash + 1);
    std::string clean;
    for (unsigned char c : leaf) {
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*')
            clean.push_back('_');
        else
            clean.push_back(static_cast<char>(c));
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.'))
        clean.pop_back();
    if (clean.empty() || clean == "." || clean == "..")
        clean = "file";
    if (clean.size() > 120)
        clean.resize(120);
    return clean;
}

nlohmann::json save_context_file(const std::string& name, const std::string& bytes)
{
    if (bytes.empty())
        return {{"ok", false}, {"error", "empty file"}};
    const std::wstring root = chrome_extension_work_dir();
    if (root.empty())
        return {{"ok", false}, {"error", "chrome work folder is unavailable"}};
    const std::filesystem::path dir = std::filesystem::path(root).parent_path() / "chat-paste";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return {{"ok", false}, {"error", "could not create the drops folder"}};
    const auto cutoff = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    std::size_t drops = 0;
    for (const auto& ent : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
            break;
        std::error_code fec;
        if (!ent.is_regular_file(fec))
            continue;
        const auto written = ent.last_write_time(fec);
        if (!fec && written < cutoff) {
            std::filesystem::remove(ent.path(), fec);
            continue;
        }
        ++drops;
        if (drops >= 200)
            return {{"ok", false}, {"error", "drops folder is full"}};
    }
    static std::atomic<unsigned> seq{0};
    const std::string leaf = std::to_string(::GetTickCount64()) + "-" +
                             std::to_string(seq.fetch_add(1)) + "-" + safe_file_leaf(name);
    const int chars = ::MultiByteToWideChar(CP_UTF8, 0, leaf.data(), static_cast<int>(leaf.size()), nullptr, 0);
    if (chars <= 0)
        return {{"ok", false}, {"error", "could not name the file"}};
    std::wstring wide_leaf(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, leaf.data(), static_cast<int>(leaf.size()), wide_leaf.data(), chars);
    const std::filesystem::path dest = dir / wide_leaf;
    std::ofstream out(dest, std::ios::binary);
    if (!out)
        return {{"ok", false}, {"error", "could not write the file"}};
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out)
        return {{"ok", false}, {"error", "could not write the file"}};
    const std::wstring written = dest.wstring();
    const int bytes_out = ::WideCharToMultiByte(CP_UTF8, 0, written.data(), static_cast<int>(written.size()), nullptr, 0, nullptr, nullptr);
    if (bytes_out <= 0)
        return {{"ok", false}, {"error", "could not write the file"}};
    std::string path_utf8(static_cast<std::size_t>(bytes_out), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, written.data(), static_cast<int>(written.size()), path_utf8.data(), bytes_out, nullptr, nullptr);
    return {{"ok", true}, {"path", path_utf8}};
}

std::wstring final_path_name(const std::filesystem::path& path, bool directory)
{
    const DWORD flags = directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    const HANDLE handle = ::CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return {};
    wchar_t buf[32768];
    const DWORD n = ::GetFinalPathNameByHandleW(handle, buf, 32768, FILE_NAME_NORMALIZED);
    ::CloseHandle(handle);
    if (n == 0 || n >= 32768)
        return {};
    std::wstring out(buf, n);
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0)
        return {};
    if (out.rfind(L"\\\\?\\", 0) == 0)
        out.erase(0, 4);
    return out;
}

bool opened_under_tanit(const std::filesystem::path& path, std::filesystem::path& opened)
{
    opened.clear();
    const std::wstring root_w = chrome_extension_work_dir();
    if (root_w.empty())
        return false;
    const std::wstring base = final_path_name(std::filesystem::path(root_w).parent_path(), true);
    const std::wstring file = final_path_name(path, false);
    if (base.empty() || file.empty() || file.size() <= base.size())
        return false;
    if (::_wcsnicmp(file.c_str(), base.c_str(), base.size()) != 0)
        return false;
    const wchar_t sep = file[base.size()];
    if (sep != L'\\' && sep != L'/')
        return false;
    opened = std::filesystem::path(file);
    return true;
}

const char* content_type_for(const std::filesystem::path& path)
{
    std::wstring ext = path.extension().wstring();
    for (wchar_t& c : ext)
        c = static_cast<wchar_t>(towlower(c));
    if (ext == L".png") return "image/png";
    if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
    if (ext == L".webp") return "image/webp";
    if (ext == L".gif") return "image/gif";
    if (ext == L".bmp") return "image/bmp";
    return "application/octet-stream";
}

std::string read_local_file(const std::string& raw, std::string& content_type, std::string& error)
{
    content_type.clear();
    error.clear();
    if (raw.empty() || raw.find('\0') != std::string::npos || raw.rfind("\\\\", 0) == 0) {
        error = "file is not available";
        return {};
    }
    if (raw.size() >= 2 && raw[1] == ':' && raw.find(':', 2) != std::string::npos) {
        error = "file is not available";
        return {};
    }
    std::filesystem::path path = media::path::path_from_utf8(raw).lexically_normal();
    std::filesystem::path opened;
    std::error_code ec;
    if (!opened_under_tanit(path, opened) || !std::filesystem::is_regular_file(opened, ec)) {
        error = "file is not available";
        return {};
    }
    path = opened;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 16 * 1024 * 1024) {
        error = "file is not available";
        return {};
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "file is not available";
        return {};
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        error = "file is not available";
        return {};
    }
    content_type = content_type_for(path);
    return bytes;
}

void handle_client(SOCKET client)
{
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::string origin;
    std::string session;
    std::string host;
    std::string fetch_dest;
    std::string payload;
    if (!read_http(client, method, path, query, body, origin, session, host, fetch_dest) ||
        !host_is_this_server(host)) {
        payload = http_response(400, "Bad Request", "{\"ok\":false}", {});
    } else if (method == "OPTIONS") {
        payload = origin == k_extension_origin
                      ? http_response(204, "No Content", "", origin)
                      : http_response(403, "Forbidden", "{\"ok\":false}", {});
    } else if (!token_matches(session)) {
        payload = http_response(401, "Unauthorized", "{\"ok\":false}", origin);
    } else if (!origin_allowed(method, path, origin, fetch_dest)) {
        payload = http_response(403, "Forbidden", "{\"ok\":false}", {});
    } else if (method == "POST" && path == "/api/context-file") {
        payload = http_response(200, "OK", save_context_file(url_decode(query_value(query, "name")), body).dump(), origin);
    } else if (method == "GET" && path == "/api/local-file") {
        std::string content_type;
        std::string error;
        const std::string bytes = read_local_file(url_decode(query_value(query, "path")), content_type, error);
        if (!error.empty())
            payload = http_response(404, "Not Found", nlohmann::json{{"ok", false}, {"error", error}}.dump(), origin);
        else
            payload = http_file_response(content_type, bytes, origin);
    } else if (method == "GET" && path == "/api/agent-tools") {
        payload = http_response(200, "OK", catalog().dump(), origin);
    } else if (method == "GET" && path == "/api/agent-tools/context") {
        const nlohmann::json ctx = {{"prompt", context_prompt()}};
        payload = http_response(200, "OK", ctx.dump(), origin);
    } else if (method == "POST" && path == "/api/agent-tools/call") {
        const nlohmann::json parsed = media::json::parse_or(body, nlohmann::json::object());
        payload = http_response(200, "OK", call_tool(parsed).dump(), origin);
    } else {
        payload = http_response(404, "Not Found", "{\"ok\":false}", origin);
    }
    (void)send_all(client, payload.data(), static_cast<int>(payload.size()));
    ::closesocket(client);
}

} // namespace

void note_browser_snapshot(const nlohmann::json& browser)
{
    std::lock_guard lock(g_snap_mu);
    g_snapshot = browser.is_object() ? browser : nlohmann::json::object();
}

std::string chrome_tool_provider_url()
{
    const int port = g_port.load();
    if (port <= 0 || g_token.empty()) return {};
    return "http://127.0.0.1:" + std::to_string(port) + "/api/agent-tools";
}

std::string chrome_context_import_url()
{
    const int port = g_port.load();
    if (port <= 0 || g_token.empty()) return {};
    return "http://127.0.0.1:" + std::to_string(port) + "/api/context-file";
}

std::string chrome_tool_session_token()
{
    return g_token;
}

void start_chrome_tool_server()
{
    if (g_token.empty()) {
        unsigned char raw[32] = {};
        static constexpr char hex[] = "0123456789abcdef";
        std::string token(64, '0');
        if (::BCryptGenRandom(nullptr, raw, sizeof(raw), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) {
            for (int i = 0; i < 32; ++i) {
                token[static_cast<std::size_t>(i * 2)] = hex[raw[i] >> 4];
                token[static_cast<std::size_t>(i * 2 + 1)] = hex[raw[i] & 0xfu];
            }
            g_token = std::move(token);
        }
    }
    std::mutex ready_mu;
    std::condition_variable ready_cv;
    bool ready = false;
    std::thread([&] {
        WSADATA wsa{};
        if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::lock_guard lock(ready_mu);
            ready = true;
            ready_cv.notify_one();
            return;
        }
        const SOCKET listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock == INVALID_SOCKET) {
            std::lock_guard lock(ready_mu);
            ready = true;
            ready_cv.notify_one();
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        for (int port = 39270; port < 39280; ++port) {
            addr.sin_port = htons(static_cast<u_short>(port));
            if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                g_port.store(port);
                break;
            }
        }
        if (g_port.load() > 0)
            ::listen(listen_sock, 8);
        {
            std::lock_guard lock(ready_mu);
            ready = true;
            ready_cv.notify_one();
        }
        if (g_port.load() <= 0) {
            ::closesocket(listen_sock);
            return;
        }
        for (;;) {
            const SOCKET client = ::accept(listen_sock, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            if (g_clients.fetch_add(1) >= 16) {
                g_clients.fetch_sub(1);
                ::closesocket(client);
                continue;
            }
            const DWORD timeout_ms = 5000;
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            std::thread([client] {
                struct Guard {
                    ~Guard() { g_clients.fetch_sub(1); }
                } guard;
                handle_client(client);
            }).detach();
        }
    }).detach();

    std::unique_lock lock(ready_mu);
    ready_cv.wait_for(lock, std::chrono::seconds(2), [&] { return ready; });
}

} // namespace media::win::chrome_inspect
