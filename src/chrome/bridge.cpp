#include "bridge.hpp"

#include "protocol_framing.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <rpc.h>
#pragma comment(lib, "rpcrt4.lib")

#include <chrono>
#include <sstream>
#include <thread>

namespace media::win::chrome_inspect {
namespace {

std::string make_request_id()
{
    UUID uuid{};
    if (::UuidCreate(&uuid) == RPC_S_OK) {
        RPC_CSTR str = nullptr;
        if (::UuidToStringA(&uuid, &str) == RPC_S_OK && str) {
            std::string out(reinterpret_cast<char*>(str));
            ::RpcStringFreeA(&str);
            return out;
        }
    }
    std::ostringstream fallback;
    fallback << "req-" << ::GetTickCount64();
    return fallback.str();
}

} // namespace

nlohmann::json make_bridge_request(std::string command, nlohmann::json payload)
{
    return nlohmann::json{
        {"protocolVersion", k_protocol_version},
        {"requestId", make_request_id()},
        {"command", std::move(command)},
        {"payload", std::move(payload)},
    };
}

bool send_bridge_request(const nlohmann::json& request,
                         nlohmann::json& response,
                         int timeout_ms,
                         std::string& err)
{
    if (timeout_ms <= 0) timeout_ms = 5000;

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(150 * attempt));

        if (!::WaitNamedPipeA(k_pipe_name, static_cast<DWORD>(timeout_ms))) {
            err = "Chrome inspect bridge unavailable (native host not connected)";
            continue;
        }

        HANDLE pipe = ::CreateFileA(
            k_pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            err = "CreateFile on Chrome inspect pipe failed";
            continue;
        }

        if (!write_framed_json(pipe, request)) {
            ::CloseHandle(pipe);
            err = "failed to write bridge request";
            continue;
        }

        if (!read_framed_json(pipe, response, err)) {
            ::CloseHandle(pipe);
            if (err.empty()) err = "failed to read bridge response";
            continue;
        }

        ::CloseHandle(pipe);
        return true;
    }

    return false;
}

} // namespace media::win::chrome_inspect
