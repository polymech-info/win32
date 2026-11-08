#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace media::win::chrome_inspect {

inline constexpr const char* k_pipe_name = R"(\\.\pipe\tanit-chrome-inspect)";
inline constexpr int k_protocol_version = 1;

inline bool write_framed_json(HANDLE handle, const nlohmann::json& message)
{
    const std::string payload = message.dump();
    if (payload.size() > 0x7fffffffu) return false;
    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    DWORD written = 0;
    if (!::WriteFile(handle, &len, sizeof(len), &written, nullptr) || written != sizeof(len))
        return false;
    if (len == 0) return true;
    return ::WriteFile(handle, payload.data(), len, &written, nullptr) && written == len;
}

inline bool read_exact(HANDLE handle, void* buffer, DWORD size, std::string& err)
{
    auto* out = static_cast<std::uint8_t*>(buffer);
    DWORD remaining = size;
    while (remaining > 0) {
        DWORD read = 0;
        if (!::ReadFile(handle, out, remaining, &read, nullptr)) {
            err = "ReadFile failed";
            return false;
        }
        if (read == 0) {
            err = "unexpected EOF";
            return false;
        }
        out += read;
        remaining -= read;
    }
    return true;
}

inline bool read_framed_json(HANDLE handle, nlohmann::json& out, std::string& err)
{
    std::uint32_t len = 0;
    if (!read_exact(handle, &len, sizeof(len), err)) return false;
    if (len == 0) {
        out = nlohmann::json::object();
        return true;
    }
    if (len > 16 * 1024 * 1024) {
        err = "frame too large";
        return false;
    }
    std::string payload(static_cast<size_t>(len), '\0');
    if (!read_exact(handle, payload.data(), len, err)) return false;
    try {
        out = nlohmann::json::parse(payload);
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
    return true;
}

} // namespace media::win::chrome_inspect
