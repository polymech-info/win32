#include "protocol_framing.hpp"
#include "agent_serve_client.hpp"
#include "chrome_tool_server.hpp"
#include "extension_channel.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <condition_variable>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace {

struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string, nlohmann::json> responses;
};

SharedState g_state;
std::mutex g_stdout_mu;
std::atomic<bool> g_chrome_alive{true};

bool write_all(HANDLE handle, const void* data, DWORD size)
{
    const auto* bytes = static_cast<const char*>(data);
    DWORD sent = 0;
    while (sent < size) {
        DWORD wrote = 0;
        if (!::WriteFile(handle, bytes + sent, size - sent, &wrote, nullptr) || wrote == 0)
            return false;
        sent += wrote;
    }
    return true;
}

bool write_stdout(const nlohmann::json& message)
{
    const std::string payload = message.dump();
    if (payload.size() > 0x7fffffffu) return false;
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    std::lock_guard lock(g_stdout_mu);
    const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (!write_all(out, &len, sizeof(len))) return false;
    if (len == 0) return true;
    return write_all(out, payload.data(), len);
}

bool is_chat_host_message(const nlohmann::json& message)
{
    return message.is_object() && message.value("source", std::string{}) == "tanit-chat";
}

void reply_chat(const nlohmann::json& inbound, nlohmann::json reply)
{
    if (!reply.is_object())
        reply = nlohmann::json::object();
    if (!reply.contains("source"))
        reply["source"] = "tanit-chat";
    if (!reply.contains("chromeRequestId") && inbound.contains("chromeRequestId"))
        reply["chromeRequestId"] = inbound["chromeRequestId"];
    write_stdout(reply);
}

nlohmann::json error_response(const std::string& request_id,
                              const char* code,
                              const char* message)
{
    return nlohmann::json{
        {"ok", false},
        {"protocolVersion", media::win::chrome_inspect::k_protocol_version},
        {"requestId", request_id.empty() ? "unknown" : request_id},
        {"type", "error"},
        {"error", {{"code", code}, {"message", message}}},
    };
}

void reply_and_close_pipe(HANDLE pipe, const nlohmann::json& response)
{
    media::win::chrome_inspect::write_framed_json(pipe, response);
    ::FlushFileBuffers(pipe);
    ::DisconnectNamedPipe(pipe);
    ::CloseHandle(pipe);
}

std::string request_id_of(const nlohmann::json& message)
{
    if (!message.is_object()) return {};
    const auto it = message.find("requestId");
    if (it == message.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool is_bridge_response(const nlohmann::json& message)
{
    if (!message.is_object()) return false;
    return message.contains("ok") && message.contains("requestId");
}

void stdin_reader_loop()
{
    for (;;) {
        nlohmann::json message;
        std::string err;
        if (!media::win::chrome_inspect::read_framed_json(::GetStdHandle(STD_INPUT_HANDLE), message, err))
            break;
        if (is_chat_host_message(message)) {
            reply_chat(message, media::win::chrome_inspect::handle_chrome_chat(message));
            continue;
        }
        if (!is_bridge_response(message)) continue;
        const std::string request_id = request_id_of(message);
        if (request_id.empty()) continue;
        {
            std::lock_guard lock(g_state.mutex);
            g_state.responses.emplace(request_id, message);
        }
        g_state.cv.notify_all();
    }
    media::win::chrome_inspect::stop_owned_agent_server();
    g_chrome_alive.store(false, std::memory_order_release);
    {
        std::lock_guard lock(g_state.mutex);
        g_state.cv.notify_all();
    }
    // Chrome closed native messaging — exit so we do not leave a pipe-only zombie
    // that accepts tanit-cli connections but cannot reach the extension.
    ::ExitProcess(0);
}

bool wait_for_response(const std::string& request_id,
                       nlohmann::json& out,
                       DWORD timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock lock(g_state.mutex);
    for (;;) {
        const auto it = g_state.responses.find(request_id);
        if (it != g_state.responses.end()) {
            out = it->second;
            g_state.responses.erase(it);
            return true;
        }
        if (g_state.cv.wait_until(lock, deadline) == std::cv_status::timeout)
            return false;
    }
}

void pipe_server_loop()
{
    for (;;) {
        if (!g_chrome_alive.load(std::memory_order_acquire))
            break;

        HANDLE pipe = ::CreateNamedPipeA(
            media::win::chrome_inspect::k_pipe_name,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            1 << 20,
            1 << 20,
            5000,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            break;

        if (!::ConnectNamedPipe(pipe, nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED) {
            ::CloseHandle(pipe);
            continue;
        }

        nlohmann::json request;
        std::string err;
        if (!media::win::chrome_inspect::read_framed_json(pipe, request, err)) {
            reply_and_close_pipe(
                pipe,
                error_response({}, "INVALID_REQUEST", err.empty() ? "Failed to read pipe request" : err.c_str()));
            continue;
        }

        const std::string request_id = request_id_of(request);

        if (!g_chrome_alive.load(std::memory_order_acquire)) {
            reply_and_close_pipe(
                pipe,
                error_response(request_id, "HOST_SHUTDOWN", "Native host is shutting down"));
            break;
        }

        if (!write_stdout(request)) {
            reply_and_close_pipe(
                pipe,
                error_response(request_id, "EXTENSION_UNAVAILABLE",
                               "Chrome extension channel is unavailable"));
            continue;
        }

        nlohmann::json response = error_response(request_id, "TIMEOUT",
                                                 "Extension did not respond in time");
        if (!request_id.empty())
            wait_for_response(request_id, response, 15000);

        reply_and_close_pipe(pipe, response);
    }
}

} // namespace

namespace media::win::chrome_inspect {

void publish_chrome_chat_events(const nlohmann::json& events)
{
    if (!events.is_array() || events.empty())
        return;
    write_stdout({
        {"source", "tanit-chat"},
        {"kind", "hostChatEvents"},
        {"events", events},
    });
}

void publish_chrome_agent_status(const char* agent)
{
    if (!agent || !agent[0])
        return;
    write_stdout({
        {"source", "tanit-chat"},
        {"kind", "hostStatus"},
        {"nativeHost", "connected"},
        {"agent", agent},
    });
}

bool ask_extension(const nlohmann::json& request, nlohmann::json& response, unsigned timeout_ms)
{
    const std::string request_id = request.value("requestId", std::string{});
    if (request_id.empty()) return false;
    if (!write_stdout(request)) return false;
    return wait_for_response(request_id, response, timeout_ms);
}

} // namespace media::win::chrome_inspect

int main()
{
    ::SetHandleInformation(::GetStdHandle(STD_INPUT_HANDLE), HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(::GetStdHandle(STD_OUTPUT_HANDLE), HANDLE_FLAG_INHERIT, 0);

    std::thread reader(stdin_reader_loop);
    media::win::chrome_inspect::start_chrome_tool_server();
    pipe_server_loop();
    reader.join();
    return 0;
}
