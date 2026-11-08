#include "agent_serve_client.hpp"
#include "json/json_utils.hpp"
#include "fs/path_sanitizer.hpp"
#include "chrome_tool_server.hpp"
#include "cmd_ipc_client.hpp"
#include "core/app_storage.hpp"
#include "audio/session/audio_events.hpp"
#include "pm_branding_config.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "string/string_utils.hpp"

namespace media::win::chrome_inspect {
namespace {

std::wstring chrome_work_dir();
std::string utf8_from_wide(const std::wstring& text);

constexpr int k_base_port = 8090;
constexpr int k_port_tries = 5;
constexpr DWORD k_idle_ms = 60 * 1000;

struct AgentProcess {
    std::mutex mu;
    HANDLE process = nullptr;
    bool owned = false;
    int port = 0;
    ULONGLONG activity = 0;
    int inflight = 0;
    std::atomic<bool> cancel{false};
    HINTERNET stream_request = nullptr;
    HINTERNET realtime_request = nullptr;
    HINTERNET chat_events_request = nullptr;
    std::vector<nlohmann::json> events;
    struct DebugLine {
        std::string level;
        std::string text;
    };
    std::vector<DebugLine> debug_lines;
};

AgentProcess g_agent;
std::once_flag g_idle_once;
std::atomic<int> g_rt_gen{0};
std::atomic<int> g_chat_ev_gen{0};
std::mutex g_chat_sub_mu;
std::string g_chat_sender;
std::uint64_t g_chat_after = 0;
std::atomic<int> g_chat_ev_port{0};

void touch_activity()
{
    std::lock_guard lock(g_agent.mu);
    g_agent.activity = ::GetTickCount64();
}

nlohmann::json drain_debug_locked(int max)
{
    nlohmann::json events = nlohmann::json::array();
    int moved = 0;
    while (!g_agent.debug_lines.empty() && moved < max) {
        AgentProcess::DebugLine line = std::move(g_agent.debug_lines.front());
        events.push_back({
            {"method", "appendDebugLog"},
            {"args", nlohmann::json::array({nlohmann::json{
                {"source", "agent"},
                {"level", line.level.empty() ? "trace" : line.level},
                {"text", std::move(line.text)},
            }})},
        });
        g_agent.debug_lines.erase(g_agent.debug_lines.begin());
        ++moved;
    }
    return events;
}

void push_event(const char* method, const nlohmann::json& args)
{
    if (!method || !args.is_array()) return;
    nlohmann::json events;
    {
        std::lock_guard lock(g_agent.mu);
        events = drain_debug_locked(40);
        events.push_back({{"method", method}, {"args", args}});
    }
    publish_chrome_chat_events(events);
}

void push_event_row(nlohmann::json row)
{
    if (!row.value("method", std::string{}).size() || !row.contains("args") || !row["args"].is_array())
        return;
    nlohmann::json events;
    {
        std::lock_guard lock(g_agent.mu);
        events = drain_debug_locked(40);
        events.push_back(std::move(row));
    }
    publish_chrome_chat_events(events);
}

void push_debug(std::string line, const char* level = "trace", bool flush = false)
{
    if (line.empty()) return;
    if (line.size() > 1024) line.resize(1024);
    nlohmann::json events;
    {
        std::lock_guard lock(g_agent.mu);
        if (g_agent.debug_lines.size() >= 800)
            g_agent.debug_lines.erase(g_agent.debug_lines.begin(), g_agent.debug_lines.begin() + 200);
        g_agent.debug_lines.push_back({level && level[0] ? level : "trace", std::move(line)});
        if (flush || g_agent.debug_lines.size() >= 20)
            events = drain_debug_locked(40);
    }
    publish_chrome_chat_events(events);
}

nlohmann::json take_events()
{
    std::lock_guard lock(g_agent.mu);
    nlohmann::json events = drain_debug_locked(40);
    for (auto& event : g_agent.events)
        events.push_back(std::move(event));
    g_agent.events.clear();
    return events;
}

void start_log_reader(HANDLE read_pipe)
{
    std::thread([read_pipe] {
        std::string pending;
        char buf[4096];
        for (;;) {
            DWORD n = 0;
            if (!::ReadFile(read_pipe, buf, sizeof(buf), &n, nullptr) || n == 0)
                break;
            pending.append(buf, buf + n);
            for (;;) {
                const auto cut = pending.find('\n');
                if (cut == std::string::npos) break;
                std::string line = pending.substr(0, cut);
                pending.erase(0, cut + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                push_debug(std::move(line));
            }
        }
        if (!pending.empty()) push_debug(std::move(pending));
        ::CloseHandle(read_pipe);
    }).detach();
}

nlohmann::json with_request(nlohmann::json out, const nlohmann::json& message)
{
    out["source"] = "tanit-chat";
    if (message.contains("chromeRequestId"))
        out["chromeRequestId"] = message["chromeRequestId"];
    return out;
}

struct HttpReply {
    int status = 0;
    std::string body;
    std::string error;
};

bool read_http(HINTERNET request, std::string& body, const std::function<void(std::string_view)>& on_chunk,
               bool honor_cancel)
{
    for (;;) {
        if (honor_cancel && g_agent.cancel.load()) {
            return false;
        }
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(request, &avail))
            return false;
        if (avail == 0)
            return true;
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!::WinHttpReadData(request, chunk.data(), avail, &read))
            return false;
        chunk.resize(read);
        if (on_chunk)
            on_chunk(chunk);
        else
            body += chunk;
    }
}

HttpReply http_exchange(int port, const wchar_t* method, const wchar_t* path,
                        const std::string& payload, bool stream,
                        const std::function<void(std::string_view)>& on_chunk,
                        HINTERNET* hold = nullptr)
{
    HttpReply reply;
    HINTERNET session = ::WinHttpOpen(L"tanit-chrome-host",
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        reply.error = "winhttp open failed";
        return reply;
    }
    const bool long_read = stream || hold != nullptr;
    ::WinHttpSetTimeouts(session, 4000, 4000, 15000, long_read ? 0 : 15000);
    HINTERNET connect = ::WinHttpConnect(session, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        ::WinHttpCloseHandle(session);
        reply.error = "winhttp connect failed";
        return reply;
    }
    HINTERNET request = ::WinHttpOpenRequest(connect, method, path, nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        reply.error = "winhttp request failed";
        return reply;
    }
    if (hold || stream) {
        std::lock_guard lock(g_agent.mu);
        if (hold)
            *hold = request;
        else
            g_agent.stream_request = request;
    }
    const wchar_t* headers = payload.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                             : L"Content-Type: application/json\r\n";
    const DWORD header_len = payload.empty() ? 0 : static_cast<DWORD>(-1);
    LPVOID body = payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data());
    const DWORD body_len = static_cast<DWORD>(payload.size());
    const BOOL sent = ::WinHttpSendRequest(request, headers, header_len, body, body_len, body_len, 0)
        && ::WinHttpReceiveResponse(request, nullptr);
    if (!sent) {
        reply.error = g_agent.cancel.load() ? "cancelled" : "agent request failed";
    } else {
        DWORD status = 0;
        DWORD status_len = sizeof(status);
        ::WinHttpQueryHeaders(request,
                              WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_len,
                              WINHTTP_NO_HEADER_INDEX);
        reply.status = static_cast<int>(status);
        if (!read_http(request, reply.body, on_chunk, stream && hold == nullptr) && reply.error.empty())
            reply.error = (stream && g_agent.cancel.load()) ? "cancelled" : "agent response truncated";
    }
    bool close_request = true;
    if (hold || stream) {
        std::lock_guard lock(g_agent.mu);
        HINTERNET& slot = hold ? *hold : g_agent.stream_request;
        if (slot == request)
            slot = nullptr;
        else
            close_request = false;
    }
    if (close_request)
        ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    return reply;
}

bool probe_ready(int port)
{
    const HttpReply reply = http_exchange(port, L"GET", L"/ready", {}, false, {});
    if (reply.status != 200) return false;
    try {
        return nlohmann::json::parse(reply.body).value("ready", true);
    } catch (...) {
        return false;
    }
}

// Named value on the native-messaging host key. Chrome reads only the default
// value (the host manifest). This one is ours: a full path to tanit-cli.exe.
constexpr wchar_t k_cli_override_key[] =
    L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.polymech.tanit.chrome";
constexpr wchar_t k_cli_override_value[] = L"cli";

std::wstring trim_cli_override(std::wstring path)
{
    while (!path.empty() && (path.front() == L' ' || path.front() == L'\t' || path.front() == L'"'))
        path.erase(path.begin());
    while (!path.empty() && (path.back() == L' ' || path.back() == L'\t' || path.back() == L'"'))
        path.pop_back();
    if (path.empty())
        return {};
    wchar_t expanded[32768]{};
    const DWORD n = ::ExpandEnvironmentStringsW(path.c_str(), expanded, 32768);
    if (n == 0 || n > 32768)
        return path;
    return std::wstring(expanded);
}

std::wstring registry_cli_override()
{
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, k_cli_override_key, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[32768]{};
    DWORD type = 0;
    DWORD size = sizeof(buf);
    const LSTATUS st = ::RegQueryValueExW(key, k_cli_override_value, nullptr, &type,
                                          reinterpret_cast<LPBYTE>(buf), &size);
    ::RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ)
        return {};
    return trim_cli_override(buf);
}

bool is_existing_file(const std::wstring& path)
{
    if (path.empty())
        return false;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool is_existing_directory(const std::wstring& path)
{
    if (path.empty())
        return false;
    const DWORD attr = ::GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool path_under_windowsapps(std::wstring path)
{
    for (wchar_t& c : path) {
        if (c == L'/')
            c = L'\\';
        else if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return path.find(L"\\windowsapps\\") != std::wstring::npos;
}

// The extension keeps the folder in chrome.storage. The host only remembers
// the last path it was given, for this process. Nothing is written beside the
// host or under the Tanit config directory.
std::mutex g_options_cli_mu;
std::wstring g_options_cli_dir;

std::wstring options_cli_dir()
{
    std::lock_guard lock(g_options_cli_mu);
    return g_options_cli_dir;
}

bool set_options_cli_dir(const std::wstring& dir, std::string& err)
{
    if (!dir.empty()) {
        if (path_under_windowsapps(dir)) {
            err = "that folder is inside WindowsApps";
            return false;
        }
        if (!is_existing_directory(dir)) {
            err = "folder does not exist";
            return false;
        }
    }
    std::lock_guard lock(g_options_cli_mu);
    g_options_cli_dir = dir;
    return true;
}

bool remember_options_cli_dir(const std::string& raw, std::wstring& stored, std::string& err)
{
    std::wstring wide;
    if (!raw.empty()) {
        if (raw.size() > 1024 || raw.find('"') != std::string::npos
            || raw.find('\n') != std::string::npos || raw.find('\r') != std::string::npos) {
            err = "invalid folder";
            return false;
        }
        const int chars = ::MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()),
                                                 nullptr, 0);
        if (chars <= 0) {
            err = "invalid folder";
            return false;
        }
        wide.assign(static_cast<size_t>(chars), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, raw.data(), static_cast<int>(raw.size()),
                              wide.data(), chars);
    }
    if (!set_options_cli_dir(wide, err))
        return false;
    stored = options_cli_dir();
    return true;
}

struct FolderPick {
    std::wstring path;
    std::string err;
};

void run_folder_picker(FolderPick* out)
{
    // IFileDialog needs OLE, and a null owner. A zero-size window owned by
    // this host never appears in front of Chrome.
    const HRESULT init = ::OleInitialize(nullptr);
    const bool uninit = SUCCEEDED(init);

    IFileOpenDialog* dlg = nullptr;
    const HRESULT created = ::CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(created) || !dlg) {
        out->err = "folder picker did not open";
    } else {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_PICKFOLDERS);
        dlg->SetTitle(L"Folder that contains tanit-cli.exe");
        const HRESULT shown = dlg->Show(nullptr);
        if (SUCCEEDED(shown)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR name = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                    out->path = name;
                    ::CoTaskMemFree(name);
                }
                item->Release();
            }
        } else if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            char text[80];
            std::snprintf(text, sizeof(text), "folder picker did not open (%08lx)",
                          static_cast<unsigned long>(shown));
            out->err = text;
        }
        dlg->Release();
    }

    if (uninit)
        ::OleUninitialize();
}

struct ProbeState {
    std::mutex mu;
    std::string value;
    bool done = false;
};

// Suspended launch, then stop before main. DETACHED_PROCESS so a console
// tanit-cli does not attach to this host's native-messaging pipes and stall
// the reply. The wait is capped so Save still returns.
std::string probe_cli_launch(const std::wstring& exe)
{
    if (exe.empty() || !is_existing_file(exe))
        return "not found";
    auto state = std::make_shared<ProbeState>();
    std::thread([exe, state] {
        std::wstring cmd = L"\"" + exe + L"\"";
        std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
        mutable_cmd.push_back(L'\0');
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::string value = "ok";
        if (!::CreateProcessW(exe.c_str(), mutable_cmd.data(), nullptr, nullptr, FALSE,
                              CREATE_SUSPENDED | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
                              nullptr, nullptr, &si, &pi)) {
            value = "win32=" + std::to_string(::GetLastError());
        } else {
            ::TerminateProcess(pi.hProcess, 0);
            ::WaitForSingleObject(pi.hProcess, 2000);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
        std::lock_guard lock(state->mu);
        state->value = std::move(value);
        state->done = true;
    }).detach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        {
            std::lock_guard lock(state->mu);
            if (state->done)
                return state->value;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return "timed out";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

std::wstring browse_options_cli_dir(std::string& err)
{
    FolderPick picked;
    std::thread worker(run_folder_picker, &picked);
    worker.join();
    err = std::move(picked.err);
    return picked.path;
}

std::wstring find_tanit_cli()
{
    // Brand-driven CLI twin name (tanit-cli.exe / tanit-viewer-cli.exe / ...).
    const std::wstring exe = pm::brand::k_exe_basename_cli_w;
    const std::string exe_u8 = utf8_from_wide(exe);
    // Options-page folder wins when it names a real tanit-cli.exe.
    const std::wstring opted = options_cli_dir();
    if (opted.empty()) {
        push_debug("cli options (none)", "trace", true);
    } else if (path_under_windowsapps(opted)) {
        push_debug("cli options inside WindowsApps " + utf8_from_wide(opted), "warn", true);
    } else {
        const std::wstring candidate = (std::filesystem::path(opted) / exe).wstring();
        if (is_existing_file(candidate)) {
            push_debug("cli options " + utf8_from_wide(candidate), "trace", true);
            return candidate;
        }
        push_debug("cli options missing " + utf8_from_wide(candidate), "warn", true);
    }
    const std::wstring override_path = registry_cli_override();
    if (override_path.empty()) {
        push_debug("cli registry (none)", "trace", true);
    } else if (is_existing_file(override_path)) {
        push_debug("cli registry " + utf8_from_wide(override_path), "trace", true);
        return override_path;
    } else {
        push_debug("cli registry missing " + utf8_from_wide(override_path), "warn", true);
    }
    // Store alias. SearchPath hits a desktop CLI that is still on PATH
    // or beside this process, and that process is the wrong profile.
    wchar_t local[MAX_PATH]{};
    if (!SUCCEEDED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
        push_debug("cli alias LocalAppData unavailable", "warn", true);
    } else {
        const std::wstring alias = std::wstring(local) + L"\\Microsoft\\WindowsApps\\" + exe;
        if (is_existing_file(alias)) {
            push_debug("cli alias " + utf8_from_wide(alias), "trace", true);
            return alias;
        }
        push_debug("cli alias missing " + utf8_from_wide(alias), "warn", true);
    }
    wchar_t buf[MAX_PATH];
    const DWORD n = ::SearchPathW(nullptr, exe.c_str(), nullptr, MAX_PATH, buf, nullptr);
    if (n > 0 && n < MAX_PATH) {
        push_debug("cli SearchPath " + utf8_from_wide(buf), "trace", true);
        return buf;
    }
    push_debug("cli SearchPath miss " + exe_u8, "error", true);
    return {};
}

void close_owned_locked()
{
    if (!g_agent.owned || !g_agent.process)
        return;
    ::TerminateProcess(g_agent.process, 0);
    ::WaitForSingleObject(g_agent.process, 2000);
    ::CloseHandle(g_agent.process);
    g_agent.process = nullptr;
    g_agent.owned = false;
    g_agent.port = 0;
    g_agent.inflight = 0;
}

bool spawn_on_port(const std::wstring& exe, int port)
{
    std::wstring cmd = L"\"" + exe + L"\" --no-gui --log-level trace llm agent --serve --host 127.0.0.1 --port "
        + std::to_wstring(port) + L" --consent-ui stream";
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return false;
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = write_pipe;
    si.StartupInfo.hStdError = write_pipe;

    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    si.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        ::HeapAlloc(::GetProcessHeap(), 0, attr_size));
    HANDLE inherit_list[2] = { write_pipe, nul ? nul : write_pipe };
    const DWORD inherit_bytes = nul ? sizeof(inherit_list) : sizeof(HANDLE);
    const BOOL list_inited = si.lpAttributeList
        && ::InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size);
    const BOOL attrs_ok = list_inited
        && ::UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       inherit_list, inherit_bytes, nullptr, nullptr);

    PROCESS_INFORMATION pi{};
    const std::wstring work = chrome_work_dir();
    const BOOL created = attrs_ok && ::CreateProcessW(
        nullptr, mutable_cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        work.empty() ? nullptr : work.c_str(),
        &si.StartupInfo, &pi);
    if (list_inited)
        ::DeleteProcThreadAttributeList(si.lpAttributeList);
    if (si.lpAttributeList)
        ::HeapFree(::GetProcessHeap(), 0, si.lpAttributeList);
    ::CloseHandle(write_pipe);
    if (nul) ::CloseHandle(nul);
    if (!created) {
        const DWORD winerr = ::GetLastError();
        ::CloseHandle(read_pipe);
        const std::string why = attrs_ok
            ? ("win32=" + std::to_string(winerr))
            : "attribute list";
        push_debug(std::string("spawn failed ") + why + " " + utf8_from_wide(exe), "error", true);
        return false;
    }
    push_debug(std::string("spawn ") + pm::brand::k_exe_stem_cli_u8
               + " " + utf8_from_wide(exe)
               + " --log-level trace port " + std::to_string(port), "trace", true);
    start_log_reader(read_pipe);
    ::CloseHandle(pi.hThread);
    {
        std::lock_guard lock(g_agent.mu);
        close_owned_locked();
        g_agent.process = pi.hProcess;
        g_agent.owned = true;
        g_agent.port = port;
        g_agent.activity = ::GetTickCount64();
    }
    const ULONGLONG deadline = ::GetTickCount64() + 12000;
    while (::GetTickCount64() < deadline) {
        if (::WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD code = 1;
            ::GetExitCodeProcess(pi.hProcess, &code);
            push_debug("spawn exited " + std::to_string(code)
                       + " before ready port " + std::to_string(port), "error", true);
            return false;
        }
        if (probe_ready(port))
            return true;
        ::Sleep(400);
    }
    if (probe_ready(port))
        return true;
    push_debug("spawn port " + std::to_string(port) + " not ready", "warn", true);
    return false;
}

// Optional extra provider from TANIT_CHROME_HOST_TOOL=id=http://127.0.0.1:port/path.
// The Chrome browser catalog is registered after it.
void attach_host_tool_provider(int port)
{
    wchar_t spec[512];
    const DWORD n = ::GetEnvironmentVariableW(L"TANIT_CHROME_HOST_TOOL", spec, 512);
    if (n == 0 || n >= 512) return;
    const std::wstring wide(spec, n);
    const auto eq = wide.find(L'=');
    if (eq == std::wstring::npos || eq == 0 || eq + 1 >= wide.size()) return;
    auto narrow = [](const std::wstring& w) {
        if (w.empty()) return std::string{};
        const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                                 nullptr, 0, nullptr, nullptr);
        std::string out(bytes, '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                              out.data(), bytes, nullptr, nullptr);
        return out;
    };
    const nlohmann::json body = {
        {"id", narrow(wide.substr(0, eq))},
        {"url", narrow(wide.substr(eq + 1))},
    };
    (void)http_exchange(port, L"POST", L"/v1/host-tool-providers", body.dump(), false, {});
}

void attach_providers(int port)
{
    attach_host_tool_provider(port);
    const std::string url = chrome_tool_provider_url();
    if (url.empty()) {
        push_debug("chrome browser tools did not bind");
        return;
    }
    const nlohmann::json body = {
        {"id", "chrome"},
        {"url", url},
        {"token", chrome_tool_session_token()},
    };
    (void)http_exchange(port, L"POST", L"/v1/host-tool-providers", body.dump(), false, {});
    const auto qmark = url.find('?');
    push_debug("chrome browser tools " + (qmark == std::string::npos ? url : url.substr(0, qmark)));
}

void ensure_chat_events_pump();
void stop_chat_events_pump();
void note_chat_sender(const std::string& sender, std::uint64_t after);

bool ensure_agent(std::string& err)
{
    touch_activity();
    int existing = 0;
    {
        std::lock_guard lock(g_agent.mu);
        if (g_agent.process && ::WaitForSingleObject(g_agent.process, 0) != WAIT_TIMEOUT)
            close_owned_locked();
        if (g_agent.port != 0 && (!g_agent.owned || g_agent.process))
            existing = g_agent.port;
    }
    if (existing != 0 && probe_ready(existing)) {
        push_debug("reusing llm agent on port " + std::to_string(existing), "trace", true);
        attach_providers(existing);
        publish_chrome_agent_status("connected");
        ensure_chat_events_pump();
        return true;
    }
    for (int i = 0; i < k_port_tries; ++i) {
        const int port = k_base_port + i;
        if (!probe_ready(port))
            continue;
        push_debug("reusing llm agent on port " + std::to_string(port), "trace", true);
        {
            std::lock_guard lock(g_agent.mu);
            if (!g_agent.owned)
                g_agent.port = port;
        }
        attach_providers(port);
        publish_chrome_agent_status("connected");
        ensure_chat_events_pump();
        return true;
    }
    const std::wstring exe = find_tanit_cli();
    if (exe.empty()) {
        err = std::string(pm::brand::k_exe_basename_cli_u8) + " is not on PATH";
        push_debug(err, "error", true);
        return false;
    }
    for (int i = 0; i < k_port_tries; ++i) {
        const int port = k_base_port + i;
        if (spawn_on_port(exe, port)) {
            attach_providers(port);
            publish_chrome_agent_status("connected");
            ensure_chat_events_pump();
            return true;
        }
        std::lock_guard lock(g_agent.mu);
        close_owned_locked();
    }
    err = "llm agent --serve did not become ready";
    push_debug(err, "error", true);
    return false;
}

int current_port()
{
    std::lock_guard lock(g_agent.mu);
    return g_agent.port;
}

void idle_loop()
{
    for (;;) {
        ::Sleep(5000);
        bool stopped = false;
        {
            std::lock_guard lock(g_agent.mu);
            if (!g_agent.owned || !g_agent.process || g_agent.inflight > 0)
                continue;
            if (g_agent.activity != 0 && ::GetTickCount64() - g_agent.activity >= k_idle_ms) {
                close_owned_locked();
                stopped = true;
            }
        }
        if (stopped) {
            stop_chat_events_pump();
            push_debug("idle: owned llm agent stopped after 60s quiet", "warn", true);
            publish_chrome_agent_status("disconnected");
        }
    }
}

void ensure_idle_thread()
{
    std::call_once(g_idle_once, [] { std::thread(idle_loop).detach(); });
}

nlohmann::json presets_from_models(const nlohmann::json& models)
{
    nlohmann::json presets = nlohmann::json::array();
    if (!models.contains("data") || !models["data"].is_array())
        return presets;
    for (const auto& row : models["data"]) {
        if (!row.is_object()) continue;
        const std::string name = row.value("id", std::string{});
        if (name.empty()) continue;
        const std::string id = row.value("tanit_preset_id", name);
        const bool is_default = row.value("tanit_is_default", false);
        presets.push_back({
            {"id", id},
            {"name", name},
            {"is_default", is_default},
            {"isDefault", is_default},
        });
    }
    return presets;
}

nlohmann::json provider_reply(const nlohmann::json& message, bool ok,
                              const nlohmann::json& data, const std::string& error)
{
    nlohmann::json out{{"kind", "hostProviderRpc"}, {"ok", ok}};
    if (ok) out["data"] = data;
    else out["error"] = error;
    if (message.contains("rpcId")) out["id"] = message["rpcId"];
    else if (message.contains("id")) out["id"] = message["id"];
    return with_request(std::move(out), message);
}

void handle_sse_block(const std::string& block, const std::string& run_id,
                      const std::string& chat_session, std::string& full, int& seq, bool& finished,
                      nlohmann::json& usage)
{
    std::string data;
    std::string event_name;
    size_t i = 0;
    while (i < block.size()) {
        const size_t end = block.find('\n', i);
        const std::string line = block.substr(i, end == std::string::npos ? std::string::npos : end - i);
        i = end == std::string::npos ? block.size() : end + 1;
        if (line.rfind("data:", 0) == 0) {
            std::string piece = line.substr(5);
            if (!piece.empty() && piece[0] == ' ') piece.erase(piece.begin());
            if (!data.empty()) data.push_back('\n');
            data += piece;
        } else if (line.rfind("event:", 0) == 0) {
            event_name = line.substr(6);
            if (!event_name.empty() && event_name[0] == ' ') event_name.erase(event_name.begin());
        }
    }
    if (data.empty() || data == "[DONE]") {
        finished = data == "[DONE]";
        return;
    }
    nlohmann::json chunk;
    try {
        chunk = nlohmann::json::parse(data);
    } catch (...) {
        return;
    }
    if (chunk.contains("error")) {
        const auto& err = chunk["error"];
        const std::string message = err.is_object() ? err.value("message", std::string{"agent failed"})
                                                     : err.dump();
        push_event("appendText", nlohmann::json::array({nlohmann::json{{"role", "error"}, {"text", message}}}));
        finished = true;
        return;
    }
    if (chunk.value("object", std::string{}) == "tanit.event") {
        const std::string type = chunk.value("type", event_name);
        if (type == "thinking") {
            push_event("appendThinking", nlohmann::json::array({nlohmann::json{
                {"runId", run_id},
                {"chatSessionId", chat_session},
                {"text", chunk.value("text", std::string{})},
            }}));
        } else if (type == "tool_call" || type == "tool_result") {
            const std::string name = chunk.value("name", std::string{"tool"});
            const std::string text = chunk.value("text", std::string{});
            nlohmann::json row{{"role", "tool"}, {"text", text.empty() ? name : text}};
            if (chunk.contains("id") && chunk["id"].is_string())
                row["toolCallId"] = chunk["id"];
            if (chunk.contains("duration_ms") && chunk["duration_ms"].is_number())
                row["durationMs"] = chunk["duration_ms"];
            if (type == "tool_call")
                row["toolRunning"] = true;
            const std::string produced = chunk.value("path", std::string{});
            push_event("appendText", nlohmann::json::array({std::move(row)}));
            if (type == "tool_result" && !produced.empty()) {
                std::string ext;
                const auto dot = produced.rfind('.');
                if (dot != std::string::npos)
                    ext = produced.substr(dot + 1);
                ext = media::strutil::lower_ascii(std::move(ext));
                const bool image = ext == "jpg" || ext == "jpeg" || ext == "png"
                    || ext == "webp" || ext == "gif" || ext == "avif";
                push_event("appendText", nlohmann::json::array({nlohmann::json{
                    {"role", image ? "image" : "file"},
                    {"text", produced},
                }}));
            }
        } else if (type == "tool_file_progress") {
            const std::string id = chunk.value("id", std::string{});
            if (!id.empty()) {
                const std::string evt = chunk.value("event", std::string{});
                if (evt == "start") {
                    push_event("startRun", nlohmann::json::array({
                        id, chunk.value("command", std::string{}),
                    }));
                } else if (evt == "done") {
                    push_event("finishRun", nlohmann::json::array({
                        id,
                        chunk.value("exit_code", -1),
                        chunk.value("duration_ms", static_cast<int64_t>(0)),
                    }));
                } else {
                    const std::string piece = chunk.value("chunk", std::string{});
                    if (!piece.empty()) {
                        push_event("appendRunChunk", nlohmann::json::array({
                            id, piece, chunk.value("stream", std::string{"stdout"}),
                        }));
                    }
                }
            }
        } else if (type == "consent" || type == "consent.dismiss") {
            nlohmann::json payload = chunk;
            payload.erase("object");
            payload.erase("type");
            payload.erase("model");
            const char* method = type == "consent"
                ? pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::ConsentRequest)
                : pm::audio::session::audio_method_name(pm::audio::session::AudioMethod::ConsentDismiss);
            push_event(method, nlohmann::json::array({std::move(payload)}));
        }
        return;
    }
    if (!chunk.contains("choices") || !chunk["choices"].is_array() || chunk["choices"].empty())
        return;
    const auto& choice = chunk["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) {
        const std::string piece = choice["delta"].value("content", std::string{});
        if (!piece.empty()) {
            full += piece;
            push_event("appendAssistantDelta", nlohmann::json::array({nlohmann::json{
                {"runId", run_id},
                {"chatSessionId", chat_session},
                {"seq", ++seq},
                {"delta", piece},
            }}));
        }
    }
    if (chunk.contains("usage") && chunk["usage"].is_object()
        && (chunk["usage"].contains("prompt_tokens") || chunk["usage"].contains("completion_tokens")
            || chunk["usage"].contains("total_tokens")))
        usage = chunk["usage"];
    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null())
        finished = true;
}

void finish_turn(const std::string& run_id, const std::string& chat_session,
                 const std::string& full, bool ok, const std::string& error,
                 const nlohmann::json& usage = {})
{
    if (ok) {
        push_event("completeAssistantText", nlohmann::json::array({nlohmann::json{
            {"runId", run_id},
            {"chatSessionId", chat_session},
            {"text", full},
        }}));
        if (usage.is_object() && !usage.empty())
            push_event("setTurnLlmUsage", nlohmann::json::array({usage}));
    }
    push_event("agentDone", nlohmann::json::array({run_id, ok, error}));
    std::lock_guard lock(g_agent.mu);
    if (g_agent.inflight > 0)
        --g_agent.inflight;
}

void run_turn(nlohmann::json message)
{
    std::string prompt = message.value("prompt", std::string{});
    if (message.contains("browser") && message["browser"].is_object()) {
        note_browser_snapshot(message["browser"]);
        const std::string url = message["browser"].value("currentUrl", std::string{});
        const std::string title = message["browser"].value("currentTitle", std::string{});
        if (!url.empty())
            prompt = "[chrome tab] " + title + " — " + url + "\n\n" + prompt;
    }
    std::string chat_session = message.value("chat_session_id", std::string{});
    if (chat_session.empty())
        chat_session = "chrome";
    std::string preset = message.value("preset", std::string{});
    const std::string run_id = "chrome-" + std::to_string(::GetTickCount64());
    push_event("setBusy", nlohmann::json::array({true}));
    push_event("agentStarted", nlohmann::json::array({nlohmann::json{
        {"id", run_id},
        {"name", preset.empty() ? "agent" : preset},
        {"chat_session_id", chat_session},
    }}));

    std::string err;
    if (!ensure_agent(err)) {
        finish_turn(run_id, chat_session, {}, false, err);
        return;
    }
    if (preset.empty()) {
        const HttpReply models = http_exchange(current_port(), L"GET", L"/v1/models", {}, false, {});
        try {
            const auto parsed = nlohmann::json::parse(models.body);
            const auto rows = presets_from_models(parsed);
            for (const auto& row : rows) {
                if (row.value("is_default", false)) {
                    preset = row.value("id", std::string{});
                    break;
                }
            }
            if (preset.empty() && !rows.empty())
                preset = rows.front().value("id", std::string{});
        } catch (...) {
        }
    }
    if (preset.empty()) {
        finish_turn(run_id, chat_session, {}, false, "no chat preset is selected");
        return;
    }
    nlohmann::json body = {
        {"model", preset},
        {"stream", true},
        {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}}})},
        {"session_id", chat_session},
    };
    {
        nlohmann::json tanit = nlohmann::json::object();
        if (message.contains("browser") && message["browser"].is_object())
            tanit["browser"] = message["browser"];
        if (message.contains("context_paths") && message["context_paths"].is_array())
            tanit["selection"] = message["context_paths"];
        const std::string work = utf8_from_wide(chrome_work_dir());
        if (!work.empty())
            tanit["cwd"] = work;
        if (!tanit.empty())
            body["tanit"] = std::move(tanit);
    }
    g_agent.cancel.store(false);
    std::string pending;
    std::string full;
    int seq = 0;
    bool finished = false;
    nlohmann::json usage;
    const int port = current_port();
    const HttpReply reply = http_exchange(port, L"POST", L"/v1/chat/completions", body.dump(), true,
        [&](std::string_view chunk) {
            pending.append(chunk.data(), chunk.size());
            for (;;) {
                const auto cut = pending.find("\n\n");
                if (cut == std::string::npos) break;
                handle_sse_block(pending.substr(0, cut), run_id, chat_session, full, seq, finished, usage);
                pending.erase(0, cut + 2);
            }
        });
    if (!finished && !pending.empty())
        handle_sse_block(pending, run_id, chat_session, full, seq, finished, usage);
    if (g_agent.cancel.load()) {
        finish_turn(run_id, chat_session, full, false, "cancelled");
        return;
    }
    if ((reply.status == 0 || reply.status >= 400) && full.empty()) {
        finish_turn(run_id, chat_session, {}, false,
                    reply.error.empty() ? "agent request failed" : reply.error);
        return;
    }
    finish_turn(run_id, chat_session, full, true, {}, usage);
}

nlohmann::json forward_agent_json(const nlohmann::json& message, const wchar_t* method,
                                  const wchar_t* path, const std::string& payload)
{
    std::string err;
    if (!ensure_agent(err))
        return provider_reply(message, false, {}, err);
    const HttpReply reply = http_exchange(current_port(), method, path, payload, false, {});
    if (reply.status != 200)
        return provider_reply(message, false, {},
                              reply.error.empty() ? "agent request failed" : reply.error);
    try {
        const auto body = nlohmann::json::parse(reply.body.empty() ? "{}" : reply.body);
        if (!body.value("ok", false))
            return provider_reply(message, false, {},
                                  body.value("error", std::string{"agent request failed"}));
        return provider_reply(message, true,
                              body.contains("data") ? body["data"] : nlohmann::json::object(), {});
    } catch (...) {
        return provider_reply(message, false, {}, "agent reply was not JSON");
    }
}

std::wstring wide_from_utf8(const std::string& text)
{
    if (text.empty())
        return {};
    const int bytes = static_cast<int>(text.size());
    const int chars = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, nullptr, 0);
    if (chars <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), bytes, out.data(), chars);
    return out;
}

bool is_web_url(const std::string& path)
{
    return path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0;
}

std::wstring chrome_work_dir()
{
    static std::wstring cached;
    static bool ready = false;
    if (ready)
        return cached;
    ready = true;
    // Store MSIX keeps the chrome-extension work dir under package LocalState
    // (== ApplicationData.Current.LocalFolder) so it is not subject to AppData
    // virtualisation. Portable / sparse Explorer11 keep
    // %APPDATA%\<vendor>\<config-slug>\chrome-extension.
    std::filesystem::path dir = pm::appdata::uses_package_appdata()
        ? pm::appdata::local_state() / L"chrome-extension"
        : [&] {
              wchar_t roaming[MAX_PATH]{};
              if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, roaming)))
                  return std::filesystem::path{};
              return std::filesystem::path(roaming)
                  / pm::brand::k_vendor_w / pm::brand::k_config_slug_w / L"chrome-extension";
          }();
    if (dir.empty())
        return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return {};
    cached = dir.wstring();
    return cached;
}

std::string utf8_from_wide(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int chars = static_cast<int>(text.size());
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), chars, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string out(static_cast<std::size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), chars, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring find_file_named(const std::filesystem::path& root, const std::wstring& name)
{
    if (name.empty())
        return {};
    std::error_code ec;
    if (!std::filesystem::exists(root, ec))
        return {};
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
        return {};
    int seen = 0;
    for (; it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (++seen > 4000)
            break;
        if (it.depth() > 3) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec))
            continue;
        if (it->path().filename().wstring() == name)
            return it->path().wstring();
    }
    return {};
}

std::wstring resolve_open_path(const std::string& raw)
{
    if (raw.empty() || is_web_url(raw))
        return wide_from_utf8(raw);
    const std::wstring root = chrome_work_dir();
    if (root.empty())
        return {};
    std::filesystem::path path = media::path::path_from_utf8(raw);
    if (!path.is_absolute())
        path = std::filesystem::path(root) / path;
    path = path.lexically_normal();
    std::error_code ec;
    if (std::filesystem::exists(path, ec))
        return path.wstring();
    const std::wstring name = media::path::path_from_utf8(raw).filename().wstring();
    return find_file_named(root, name);
}

void shell_open_target(const std::wstring& target, const wchar_t* verb)
{
    if (target.empty())
        return;
    const std::wstring dir = std::filesystem::path(target).parent_path().wstring();
    ::ShellExecuteW(nullptr, verb, target.c_str(), nullptr,
                    dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
}

void shell_select_target(const std::wstring& target)
{
    if (target.empty())
        return;
    const std::wstring args = L"/select,\"" + target + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace

namespace {

void close_chat_events_request()
{
    HINTERNET request = nullptr;
    {
        std::lock_guard lock(g_agent.mu);
        request = g_agent.chat_events_request;
        g_agent.chat_events_request = nullptr;
    }
    if (request)
        ::WinHttpCloseHandle(request);
}

void stop_chat_events_pump()
{
    ++g_chat_ev_gen;
    g_chat_ev_port.store(0);
    close_chat_events_request();
}

void apply_chat_events_block(const std::string& block)
{
    std::string data;
    std::size_t i = 0;
    while (i < block.size()) {
        const std::size_t end = block.find('\n', i);
        std::string line = block.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("data:", 0) == 0) {
            std::string rest = line.substr(5);
            if (!rest.empty() && rest.front() == ' ')
                rest.erase(rest.begin());
            data += rest;
        }
        if (end == std::string::npos)
            break;
        i = end + 1;
    }
    if (data.empty())
        return;
    const auto ev = media::json::try_parse(data);
    if (!ev.is_object())
        return;
    const std::string method = ev.value("method", std::string{});
    if (method.empty() || !ev.contains("args") || !ev["args"].is_array())
        return;
    nlohmann::json row{{"method", method}, {"args", ev["args"]}};
    if (ev.contains("id"))
        row["id"] = ev["id"];
    if (ev.contains("sender") && ev["sender"].is_string())
        row["sender"] = ev["sender"];
    push_event_row(std::move(row));
}

void pump_chat_events(int gen, int port, std::string sender, std::uint64_t after)
{
    std::wstring path = L"/v1/chat/events?after=" + std::to_wstring(after);
    if (!sender.empty())
        path += L"&sender=" + wide_from_utf8(sender);
    std::string pending;
    const HttpReply reply = http_exchange(
        port, L"GET", path.c_str(), {}, false,
        [&](std::string_view chunk) {
            if (g_chat_ev_gen.load() != gen)
                return;
            pending.append(chunk.data(), chunk.size());
            for (;;) {
                const auto cut = pending.find("\n\n");
                if (cut == std::string::npos)
                    break;
                apply_chat_events_block(pending.substr(0, cut));
                pending.erase(0, cut + 2);
            }
        },
        &g_agent.chat_events_request);
    if (!pending.empty() && g_chat_ev_gen.load() == gen)
        apply_chat_events_block(pending);
    if (g_chat_ev_gen.load() == gen && g_chat_ev_port.load() == port)
        g_chat_ev_port.store(0);
    (void)reply;
}

void ensure_chat_events_pump()
{
    const int port = current_port();
    if (port <= 0)
        return;
    int cur = g_chat_ev_port.load();
    if (cur == port)
        return;
    if (!g_chat_ev_port.compare_exchange_strong(cur, port))
        return;
    const int gen = ++g_chat_ev_gen;
    close_chat_events_request();
    std::string sender;
    std::uint64_t after = 0;
    {
        std::lock_guard lock(g_chat_sub_mu);
        sender = g_chat_sender;
        after = g_chat_after;
    }
    std::thread([gen, port, sender = std::move(sender), after] {
        pump_chat_events(gen, port, sender, after);
    }).detach();
}

void note_chat_sender(const std::string& raw, std::uint64_t after)
{
    std::string sender;
    sender.reserve(raw.size());
    for (unsigned char c : raw) {
        if (sender.size() >= 80)
            break;
        if (std::isalnum(c) || c == '-' || c == '_')
            sender.push_back(static_cast<char>(c));
    }
    bool restart = false;
    {
        std::lock_guard lock(g_chat_sub_mu);
        restart = g_chat_ev_port.load() != 0
            && (sender != g_chat_sender || after != g_chat_after);
        g_chat_sender = std::move(sender);
        g_chat_after = after;
    }
    if (!restart)
        return;
    stop_chat_events_pump();
    ensure_chat_events_pump();
}

void close_realtime_request()
{
    HINTERNET request = nullptr;
    {
        std::lock_guard lock(g_agent.mu);
        request = g_agent.realtime_request;
        g_agent.realtime_request = nullptr;
    }
    if (request)
        ::WinHttpCloseHandle(request);
}

void apply_realtime_block(const std::string& block, bool& saw_done)
{
    std::string data;
    std::size_t i = 0;
    while (i < block.size()) {
        const std::size_t end = block.find('\n', i);
        std::string line = block.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("data:", 0) == 0) {
            std::string rest = line.substr(5);
            if (!rest.empty() && rest.front() == ' ')
                rest.erase(rest.begin());
            data += rest;
        }
        if (end == std::string::npos)
            break;
        i = end + 1;
    }
    if (data.empty())
        return;
    const auto ev = media::json::try_parse(data);
    if (!ev.is_object())
        return;
    const std::string method = ev.value("method", std::string{});
    if (method.empty() || !ev.contains("args") || !ev["args"].is_array())
        return;
    push_event(method.c_str(), ev["args"]);
    if (method == "realtimeDone")
        saw_done = true;
}

void pump_realtime(int gen)
{
    if (g_rt_gen.load() != gen)
        return;
    std::string pending;
    bool saw_done = false;
    const HttpReply reply = http_exchange(
        current_port(), L"GET", L"/v1/realtime/events", {}, false,
        [&](std::string_view chunk) {
            if (g_rt_gen.load() != gen)
                return;
            touch_activity();
            pending.append(chunk.data(), chunk.size());
            for (;;) {
                const auto cut = pending.find("\n\n");
                if (cut == std::string::npos)
                    break;
                apply_realtime_block(pending.substr(0, cut), saw_done);
                pending.erase(0, cut + 2);
            }
        },
        &g_agent.realtime_request);
    if (!pending.empty() && g_rt_gen.load() == gen)
        apply_realtime_block(pending, saw_done);
    if (g_rt_gen.load() != gen || saw_done)
        return;
    if (reply.status != 200) {
        const std::string err = reply.error.empty() ? "realtime session failed" : reply.error;
        push_event("realtimeError", nlohmann::json::array({err}));
    }
    push_event("realtimeDone", nlohmann::json::array());
}

void push_realtime_failure(const std::string& err)
{
    push_event("realtimeError", nlohmann::json::array({err.empty() ? "realtime session failed" : err}));
    push_event("realtimeDone", nlohmann::json::array());
}

std::atomic<bool> g_picker_open{false};

void start_context_picker(bool folder)
{
    if (g_picker_open.exchange(true))
        return;
    std::thread([folder] {
        struct Clear {
            ~Clear() { g_picker_open.store(false); }
        } clear;
        const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool uninit = SUCCEEDED(init);
        IFileOpenDialog* dlg = nullptr;
        const HRESULT created = ::CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
        if (FAILED(created) || !dlg) {
            push_event("appendText", nlohmann::json::array({
                nlohmann::json{{"role", "error"}, {"text", "File picker did not open"}},
            }));
            if (uninit)
                ::CoUninitialize();
            return;
        }
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT;
        if (folder)
            opts |= FOS_PICKFOLDERS;
        else
            opts |= FOS_FILEMUSTEXIST;
        dlg->SetOptions(opts);
        dlg->SetTitle(folder ? L"Select a folder to add to chat" : L"Add files to chat");
        HWND owner = ::GetForegroundWindow();
        HRESULT shown = dlg->Show(owner);
        if (FAILED(shown) && owner)
            shown = dlg->Show(nullptr);
        nlohmann::json paths = nlohmann::json::array();
        if (SUCCEEDED(shown)) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dlg->GetResults(&items)) && items) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD i = 0; i < count; ++i) {
                    IShellItem* item = nullptr;
                    if (FAILED(items->GetItemAt(i, &item)) || !item)
                        continue;
                    PWSTR name = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
                        const std::string utf8 = utf8_from_wide(name);
                        if (!utf8.empty())
                            paths.push_back(utf8);
                        ::CoTaskMemFree(name);
                    }
                    item->Release();
                }
                items->Release();
            }
        }
        dlg->Release();
        if (uninit)
            ::CoUninitialize();
        if (!paths.empty())
            push_event("appendContextPaths", nlohmann::json::array({std::move(paths)}));
    }).detach();
}

void start_speak(std::string text)
{
    if (text.empty())
        return;
    push_event("ttsStarted", nlohmann::json::array());
    std::thread([text = std::move(text)] {
        std::string err;
        if (!ensure_agent(err)) {
            push_event("appendText", nlohmann::json::array({
                nlohmann::json{{"role", "error"}, {"text", err.empty() ? "speak did not start" : err}},
            }));
            push_event("ttsDone", nlohmann::json::array());
            return;
        }
        const nlohmann::json body = {{"text", text}};
        const HttpReply reply = http_exchange(current_port(), L"POST", L"/v1/speak", body.dump(), false, {});
        if (reply.status != 200) {
            push_event("appendText", nlohmann::json::array({
                nlohmann::json{{"role", "error"},
                               {"text", reply.error.empty() ? "speak failed" : reply.error}},
            }));
        } else {
            try {
                const auto parsed = nlohmann::json::parse(reply.body.empty() ? "{}" : reply.body);
                if (!parsed.value("ok", false)) {
                    push_event("appendText", nlohmann::json::array({
                        nlohmann::json{{"role", "error"},
                                       {"text", parsed.value("error", std::string{"speak failed"})}},
                    }));
                }
            } catch (...) {
                push_event("appendText", nlohmann::json::array({
                    nlohmann::json{{"role", "error"}, {"text", "speak failed"}},
                }));
            }
        }
        push_event("ttsDone", nlohmann::json::array());
    }).detach();
}

void release_owned_agent()
{
    ++g_rt_gen;
    close_realtime_request();
    stop_chat_events_pump();
    g_agent.cancel.store(true);
    HINTERNET stream = nullptr;
    {
        std::lock_guard lock(g_agent.mu);
        stream = g_agent.stream_request;
        g_agent.stream_request = nullptr;
        g_agent.inflight = 0;
        close_owned_locked();
    }
    if (stream)
        ::WinHttpCloseHandle(stream);
}

void stop_speak()
{
    std::thread([] {
        const int port = current_port();
        if (port > 0)
            http_exchange(port, L"POST", L"/v1/speak/stop", "{}", false, {});
        push_event("ttsDone", nlohmann::json::array());
    }).detach();
}

void pause_speak()
{
    std::thread([] {
        const int port = current_port();
        if (port > 0)
            http_exchange(port, L"POST", L"/v1/speak/pause", "{}", false, {});
        push_event("ttsPaused", nlohmann::json::array());
    }).detach();
}

void resume_speak()
{
    std::thread([] {
        const int port = current_port();
        if (port > 0)
            http_exchange(port, L"POST", L"/v1/speak/resume", "{}", false, {});
        push_event("ttsResumed", nlohmann::json::array());
    }).detach();
}

} // namespace

std::wstring chrome_extension_work_dir()
{
    return chrome_work_dir();
}

nlohmann::json handle_chrome_chat(const nlohmann::json& message)
{
    ensure_idle_thread();
    const std::string kind = message.value("kind", std::string{});
    if (kind == "browserSnapshot") {
        if (message.contains("browser") && message["browser"].is_object())
            note_browser_snapshot(message["browser"]);
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }
    if (kind == "panelClosed") {
        release_owned_agent();
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }
    // Session sync and status do not count as the user still working.
    if (kind != "poll" && kind != "ready" && kind != "chatWebState"
        && kind != "requestContext" && kind != "hostLog" && kind != "getStatus")
        touch_activity();

    if (kind == "cliDirGet") {
        const std::wstring dir = options_cli_dir();
        return with_request({
            {"kind", "cliDir"},
            {"ok", true},
            {"dir", utf8_from_wide(dir)},
        }, message);
    }

    if (kind == "chatSender") {
        const auto after = message.value("after", std::uint64_t{0});
        note_chat_sender(message.value("sender", std::string{}), after);
        return with_request({{"kind", "chatSender"}, {"ok", true}}, message);
    }

    if (kind == "cliDirApply" || kind == "cliDirSet") {
        std::string err;
        std::wstring stored;
        if (!remember_options_cli_dir(message.value("dir", std::string{}), stored, err))
            return with_request({{"kind", "cliDir"}, {"ok", false}, {"error", err}}, message);
        if (kind == "cliDirApply")
            return with_request({{"kind", "cliDirApply"}, {"ok", true}, {"dir", utf8_from_wide(stored)}}, message);
        const std::wstring resolved = find_tanit_cli();
        const std::string resolved_u8 = utf8_from_wide(resolved);
        const std::string probe = probe_cli_launch(resolved);
        const std::wstring wanted = stored.empty()
            ? std::wstring{}
            : (std::filesystem::path(stored) / pm::brand::k_exe_basename_cli_w).wstring();
        const bool wanted_here = !wanted.empty() && is_existing_file(wanted);
        std::string feedback;
        if (stored.empty()) {
            if (resolved.empty())
                feedback = "Cleared. No tanit-cli.exe was found.";
            else if (probe == "ok")
                feedback = "Cleared. Next start uses " + resolved_u8 + ".";
            else
                feedback = "Cleared. " + resolved_u8 + " did not start (" + probe + ").";
        } else if (!wanted_here) {
            feedback = utf8_from_wide(pm::brand::k_exe_basename_cli_w) + " is not in that folder.";
            if (!resolved.empty() && probe == "ok")
                feedback += " Next start uses " + resolved_u8 + ".";
            else if (!resolved.empty())
                feedback += " Next start " + resolved_u8 + " did not start (" + probe + ").";
        } else if (probe == "ok") {
            feedback = "Saved. " + resolved_u8 + " starts.";
        } else {
            feedback = resolved_u8 + " did not start (" + probe + ").";
        }
        nlohmann::json ack = {
            {"kind", "cliDir"},
            {"ok", true},
            {"dir", utf8_from_wide(stored)},
            {"cli", resolved_u8},
            {"probe", probe},
            {"feedback", feedback},
        };
        if (probe != "ok")
            ack["warning"] = feedback;
        return with_request(std::move(ack), message);
    }

    if (kind == "cliDirBrowse") {
        std::string err;
        const std::wstring chosen = browse_options_cli_dir(err);
        if (!err.empty())
            return with_request({{"kind", "cliDir"}, {"ok", false}, {"error", err}}, message);
        return with_request({
            {"kind", "cliDir"},
            {"ok", true},
            {"dir", utf8_from_wide(chosen)},
        }, message);
    }

    if (kind == "providerRpc") {
        const std::string method = message.value("method", std::string{});
        if (method == "settingsChatPresetsGet" || method == "getChatPresets") {
            std::string err;
            if (!ensure_agent(err))
                return provider_reply(message, false, {}, err);
            const HttpReply reply = http_exchange(current_port(), L"GET", L"/v1/models", {}, false, {});
            if (reply.status != 200)
                return provider_reply(message, false, {}, reply.error.empty() ? "preset list failed" : reply.error);
            try {
                const auto models = nlohmann::json::parse(reply.body);
                return provider_reply(message, true, {{"presets", presets_from_models(models)}}, {});
            } catch (...) {
                return provider_reply(message, false, {}, "preset list was not JSON");
            }
        }
        if (method == "getCreditInfo")
            return forward_agent_json(message, L"GET", L"/v1/credit", {});
        if (method == "listChatSessions")
            return forward_agent_json(message, L"GET", L"/v1/chat/sessions", {});
        if (method == "loadChatSession") {
            const std::string id = message.value("id", std::string{});
            const std::wstring path = L"/v1/chat/session?id=" + wide_from_utf8(id);
            return forward_agent_json(message, L"GET", path.c_str(), {});
        }
        if (method == "saveChatSession") {
            const auto session = message.contains("session") ? message["session"] : nlohmann::json{};
            return forward_agent_json(message, L"POST", L"/v1/chat/sessions", session.dump());
        }
        if (method == "deleteChatSession") {
            const std::string id = message.value("id", std::string{});
            const std::wstring path = L"/v1/chat/session?id=" + wide_from_utf8(id);
            return forward_agent_json(message, L"DELETE", path.c_str(), {});
        }
        if (method == "commandEventsSubscribe") {
            std::string err;
            if (!ensure_agent(err))
                return provider_reply(message, false, {}, err);
            return provider_reply(message, true, nlohmann::json::object(), {});
        }
        if (method == "commandRun") {
            nlohmann::json body{{"id", message.value("id", std::string{})}};
            {
                std::lock_guard lock(g_chat_sub_mu);
                if (!g_chat_sender.empty())
                    body["sender"] = g_chat_sender;
            }
            std::string agent_err;
            const bool agent_up = ensure_agent(agent_err);
            HttpReply http;
            if (agent_up)
                http = http_exchange(current_port(), L"POST", L"/v1/chat/commands/run", body.dump(), false, {});
            nlohmann::json parsed = nlohmann::json::object();
            bool parsed_ok = false;
            if (agent_up && http.status == 200) {
                try {
                    parsed = nlohmann::json::parse(http.body.empty() ? "{}" : http.body);
                    parsed_ok = true;
                } catch (...) {
                }
            }
            const bool missing_route = !agent_up || http.status == 404 || !parsed_ok;
            if (parsed_ok && parsed.value("ok", false))
                return provider_reply(message, true,
                                      parsed.contains("data") ? parsed["data"] : nlohmann::json::object(), {});
            if (parsed_ok && !parsed.value("needsUi", false) && !missing_route)
                return provider_reply(message, false, {},
                                      parsed.value("error", std::string{"commandRun failed"}));
            nlohmann::json reply;
            std::string err;
            if (!command_run_via_cmd_ipc(message, reply, err))
                return provider_reply(message, false, {}, err.empty() ? "tanit is not running" : err);
            if (!reply.value("ok", false))
                return provider_reply(message, false, {},
                                      reply.value("error", std::string{"commandRun failed"}));
            return provider_reply(message, true,
                                  reply.contains("data") ? reply["data"] : nlohmann::json::object(), {});
        }
        if (method == "chatCommandsList")
            return forward_agent_json(message, L"GET", L"/v1/chat/commands", {});
        if (method == "chatSkillsList") {
            const std::string cwd = message.value("cwd", std::string{});
            std::string query;
            for (unsigned char c : cwd) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    query.push_back(static_cast<char>(c));
                else {
                    char hex[4];
                    std::snprintf(hex, sizeof(hex), "%%%02X", c);
                    query += hex;
                }
            }
            const std::wstring path = query.empty()
                ? std::wstring(L"/v1/chat/skills")
                : (L"/v1/chat/skills?cwd=" + wide_from_utf8(query));
            return forward_agent_json(message, L"GET", path.c_str(), {});
        }
        if (method == "homeRecentPathsGet")
            return forward_agent_json(message, L"GET", L"/v1/chat/recent-paths", {});
        if (method == "typeaheadPresetsGet")
            return forward_agent_json(message, L"GET", L"/v1/chat/typeahead-presets", {});
        if (method == "searchCancel" || method == "searchCancelAll")
            return provider_reply(message, true, nlohmann::json::object(), {});
        if (method == "searchStart") {
            nlohmann::json body = {
                {"query", message.value("query", std::string{})},
                {"headLimit", message.value("headLimit", 40)},
                {"inputs", message.contains("inputs") ? message["inputs"] : nlohmann::json::array()},
            };
            if (message.contains("user") && message["user"].is_object()
                && message["user"].contains("scopes"))
                body["scopes"] = message["user"]["scopes"];
            return forward_agent_json(message, L"POST", L"/v1/chat/search-names", body.dump());
        }
        if (method == "rankPageModel") {
            nlohmann::json body = nlohmann::json::object();
            if (message.contains("query") && message["query"].is_string())
                body["query"] = message["query"];
            if (message.contains("model") && message["model"].is_object())
                body["model"] = message["model"];
            if (message.contains("topK") && message["topK"].is_number())
                body["topK"] = message["topK"];
            return forward_agent_json(message, L"POST", L"/v1/browser/page-model/rank", body.dump());
        }
        if (method == "xbloxArtifactsGet") {
            const int limit = message.value("limit", 0);
            const std::wstring path = limit > 0
                ? (L"/v1/chat/xblox-artifacts?limit=" + std::to_wstring(limit))
                : std::wstring(L"/v1/chat/xblox-artifacts");
            return forward_agent_json(message, L"GET", path.c_str(), {});
        }
        return provider_reply(message, false, {}, "chrome host does not handle " + method);
    }

    if (kind == "openPathInternal" || kind == "openPathFullscreen") {
        const std::string path = message.value("path", std::string{});
        std::string err;
        if (!ensure_agent(err))
            return with_request({{"kind", "ack"}, {"ok", false}, {"error", err}}, message);
        nlohmann::json body = {
            {"path", path},
            {"fullscreen", kind == "openPathFullscreen"},
        };
        if (message.contains("paths") && message["paths"].is_array())
            body["paths"] = message["paths"];
        const HttpReply reply = http_exchange(current_port(), L"POST", L"/v1/ui/open-file",
                                               body.dump(), false, {});
        bool ok = reply.status == 200;
        std::string open_err = reply.error;
        if (ok) {
            try {
                const auto parsed = nlohmann::json::parse(reply.body.empty() ? "{}" : reply.body);
                ok = parsed.value("ok", false);
                if (!ok)
                    open_err = parsed.value("error", std::string("file did not open"));
            } catch (...) {
                ok = false;
                open_err = "file did not open";
            }
        }
        nlohmann::json ack = {{"kind", "ack"}, {"ok", ok}};
        if (!ok)
            ack["error"] = open_err.empty() ? std::string("file did not open") : open_err;
        return with_request(std::move(ack), message);
    }

    if (kind == "openPathDefault" || kind == "openUrlDefault"
        || kind == "revealPathInExplorer" || kind == "selectPathInExplorer"
        || kind == "openFolderInExplorer") {
        std::string raw = message.value("path", std::string{});
        if (raw.empty())
            raw = message.value("url", std::string{});
        if (kind == "openUrlDefault" || is_web_url(raw)) {
            const std::wstring url = wide_from_utf8(raw);
            if (!url.empty())
                shell_open_target(url, L"open");
            return with_request({{"kind", "ack"}, {"ok", !url.empty()}}, message);
        }
        const std::wstring path = resolve_open_path(raw);
        if (path.empty()) {
            return with_request({
                {"kind", "ack"},
                {"ok", false},
                {"error", "file not found"},
            }, message);
        }
        if (kind == "revealPathInExplorer" || kind == "selectPathInExplorer")
            shell_select_target(path);
        else if (kind == "openFolderInExplorer")
            shell_open_target(path, L"explore");
        else
            shell_open_target(path, L"open");
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "realtimeStart") {
        if (message.contains("browser") && message["browser"].is_object())
            note_browser_snapshot(message["browser"]);
        std::string err;
        if (!ensure_agent(err)) {
            push_realtime_failure(err);
            return with_request({{"kind", "ack"}, {"ok", true}}, message);
        }
        const int gen = ++g_rt_gen;
        close_realtime_request();
        nlohmann::json body{
            {"preset", message.value("preset", std::string{})},
            {"chat_session_id", message.value("chat_session_id", std::string{})},
            {"mic_send", true},
            {"acquire_timeout_ms", 1500},
        };
        const HttpReply reply = http_exchange(current_port(), L"POST", L"/v1/realtime/start",
                                               body.dump(), false, {});
        if (reply.status != 200) {
            push_realtime_failure(reply.error.empty() ? "realtime start failed" : reply.error);
            return with_request({{"kind", "ack"}, {"ok", true}}, message);
        }
        const auto parsed = media::json::try_parse(reply.body.empty() ? "{}" : reply.body);
        if (!parsed.is_object() || !parsed.value("ok", false)) {
            const std::string start_err = parsed.is_object()
                ? parsed.value("error", std::string{"realtime start failed"})
                : "realtime start failed";
            push_realtime_failure(start_err);
            return with_request({{"kind", "ack"}, {"ok", true}}, message);
        }
        std::thread(pump_realtime, gen).detach();
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "realtimeStop") {
        ++g_rt_gen;
        close_realtime_request();
        std::string err;
        if (ensure_agent(err)) {
            http_exchange(current_port(), L"POST", L"/v1/realtime/stop", "{}", false, {});
        }
        push_event("realtimeDone", nlohmann::json::array());
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "realtimePushToTalk") {
        std::string err;
        if (ensure_agent(err)) {
            nlohmann::json body{
                {"held", message.value("held", true)},
                {"latched", message.value("latched", false)},
                {"finishInputTurn", message.value("finishInputTurn", false)},
            };
            http_exchange(current_port(), L"POST", L"/v1/realtime/ptt", body.dump(), false, {});
        }
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "poll") {
        return with_request({
            {"kind", "hostChatEvents"},
            {"ok", true},
            {"events", take_events()},
        }, message);
    }

    if (kind == "consentReply") {
        nlohmann::json body = message;
        body.erase("kind");
        std::string err;
        if (!ensure_agent(err)) {
            return with_request({{"kind", "ack"}, {"ok", false}, {"error", err}}, message);
        }
        const HttpReply reply = http_exchange(current_port(), L"POST", L"/v1/chat/consent",
                                               body.dump(), false, {});
        return with_request({
            {"kind", "ack"},
            {"ok", reply.status == 200},
            {"error", reply.status == 200 ? std::string{} : reply.error},
        }, message);
    }

    if (kind == "speak") {
        start_speak(message.value("text", std::string{}));
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "speakStop") {
        stop_speak();
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "speakPause") {
        pause_speak();
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "speakResume") {
        resume_speak();
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "settings") {
        const std::string section = message.value("section", std::string("chat"));
        std::string err;
        if (!ensure_agent(err))
            return with_request({{"kind", "ack"}, {"ok", false}, {"error", err}}, message);
        const nlohmann::json body = {{"section", section}};
        const HttpReply reply = http_exchange(current_port(), L"POST", L"/v1/ui/open",
                                               body.dump(), false, {});
        std::string open_err = reply.error;
        bool ok = reply.status == 200;
        if (ok) {
            try {
                const auto parsed = nlohmann::json::parse(reply.body.empty() ? "{}" : reply.body);
                ok = parsed.value("ok", false);
                if (!ok)
                    open_err = parsed.value("error", std::string("settings did not open"));
            } catch (...) {
                ok = false;
                open_err = "settings did not open";
            }
        }
        nlohmann::json ack = {{"kind", "ack"}, {"ok", ok}};
        if (!ok)
            ack["error"] = open_err.empty() ? std::string("settings did not open") : open_err;
        return with_request(std::move(ack), message);
    }

    if (kind == "attachContextPaths") {
        nlohmann::json paths = nlohmann::json::array();
        if (message.contains("paths") && message["paths"].is_array())
            paths = message["paths"];
        if (!paths.empty())
            push_event("appendContextPaths", nlohmann::json::array({std::move(paths)}));
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "pickContextPaths") {
        start_context_picker(message.value("mode", std::string{}) == "folder");
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "contextImportUrl") {
        const std::string url = chrome_context_import_url();
        nlohmann::json ack = {
            {"kind", "ack"},
            {"ok", !url.empty()},
            {"url", url},
            {"token", chrome_tool_session_token()},
        };
        if (url.empty())
            ack["error"] = "file import is not ready";
        return with_request(std::move(ack), message);
    }

    if (kind == "agentReload") {
        const int port = current_port();
        if (port > 0)
            http_exchange(port, L"POST", L"/v1/speak/stop", "{}", false, {});
        release_owned_agent();
        {
            std::lock_guard lock(g_agent.mu);
            g_agent.port = 0;
        }
        std::string err;
        const bool ok = ensure_agent(err);
        nlohmann::json ack = {{"kind", "ack"}, {"ok", ok}};
        if (!ok)
            ack["error"] = err.empty() ? std::string("llm agent did not reconnect") : err;
        return with_request(std::move(ack), message);
    }

    if (kind == "stop") {
        g_agent.cancel.store(true);
        HINTERNET request = nullptr;
        {
            std::lock_guard lock(g_agent.mu);
            request = g_agent.stream_request;
            g_agent.stream_request = nullptr;
        }
        if (request)
            ::WinHttpCloseHandle(request);
        if (current_port() > 0)
            http_exchange(current_port(), L"POST", L"/v1/chat/consent",
                          "{\"cancel\":true}", false, {});
        stop_speak();
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    if (kind == "send") {
        if (message.value("prompt", std::string{}).empty()) {
            return with_request({
                {"kind", "error"},
                {"ok", false},
                {"error", "empty prompt"},
            }, message);
        }
        {
            std::lock_guard lock(g_agent.mu);
            ++g_agent.inflight;
        }
        std::thread(run_turn, message).detach();
        return with_request({{"kind", "ack"}, {"ok", true}}, message);
    }

    return with_request({
        {"kind", "ack"},
        {"ok", true},
    }, message);
}

void stop_owned_agent_server()
{
    release_owned_agent();
}

} // namespace media::win::chrome_inspect
