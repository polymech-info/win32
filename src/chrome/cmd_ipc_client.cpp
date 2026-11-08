#include "cmd_ipc_client.hpp"
#include "pm_branding_config.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <cstdint>
#include <filesystem>
#include <vector>

namespace media::win::chrome_inspect {
namespace {

namespace fs = std::filesystem;

bool sha256(const uint8_t* data, size_t size, uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    DWORD obj_len = 0;
    DWORD got = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &got, 0) != 0
        || obj_len == 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<UCHAR> obj(obj_len);
    const bool ok = BCryptCreateHash(alg, &hash, obj.data(), obj_len, nullptr, 0, 0) == 0
        && BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0
        && BCryptFinishHash(hash, out, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> from_hex(const std::string& hex)
{
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string to_hex(const uint8_t* data, size_t n)
{
    static const char* k = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(k[(data[i] >> 4) & 0xF]);
        s.push_back(k[data[i] & 0xF]);
    }
    return s;
}

std::string proof_for(const std::string& token_hex, const std::string& nonce_hex)
{
    auto msg = from_hex(token_hex);
    const auto nonce = from_hex(nonce_hex);
    if (msg.empty() || nonce.empty()) return {};
    msg.insert(msg.end(), nonce.begin(), nonce.end());
    uint8_t digest[32]{};
    if (!sha256(msg.data(), msg.size(), digest)) return {};
    return to_hex(digest, 32);
}

bool process_alive(int64_t pid)
{
    if (pid <= 0) return false;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    DWORD code = 0;
    const BOOL ok = ::GetExitCodeProcess(process, &code);
    ::CloseHandle(process);
    return ok && code == STILL_ACTIVE;
}

struct UiEndpoint {
    std::string host = "127.0.0.1";
    unsigned port = 0;
    std::string token;
    int64_t started = 0;
};

void consider_descriptor(const fs::path& path, UiEndpoint& best);

void scan_instances(const fs::path& dir, UiEndpoint& best)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_regular_file(sec) && it->path().extension() == ".json")
            consider_descriptor(it->path(), best);
    }
}

void consider_descriptor(const fs::path& path, UiEndpoint& best)
{
    std::error_code ec;
    const auto text = [&]() {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return std::string{};
        std::string body;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), file)) > 0)
            body.append(buf, n);
        fclose(file);
        return body;
    }();
    if (text.empty()) return;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(text);
    } catch (...) {
        return;
    }
    if (doc.value("kind", std::string{}) != "ui") return;
    if (doc.value("transport", std::string{}) != "tcp") return;
    const int64_t pid = doc.value("pid", int64_t{0});
    if (!process_alive(pid)) return;
    const std::string endpoint = doc.value("endpoint", std::string{});
    const std::string token = doc.value("token", std::string{});
    if (endpoint.empty() || token.empty()) return;
    unsigned port = 0;
    try {
        port = static_cast<unsigned>(std::stoul(endpoint));
    } catch (...) {
        return;
    }
    if (port == 0 || port > 65535) return;
    const int64_t started = doc.value("startedUnixMs", int64_t{0});
    if (best.port != 0 && started < best.started) return;
    best.port = port;
    best.token = token;
    best.started = started;
    (void)ec;
}

UiEndpoint find_ui()
{
    UiEndpoint best;
    const DWORD needed = ::GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (needed == 0) return best;
    std::wstring roaming(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(L"APPDATA", roaming.data(), needed);
    if (written == 0) return best;
    roaming.resize(written);
    const fs::path root(roaming);
    scan_instances(root / pm::brand::k_config_subpath_w / "run-ipc" / "instances", best);
    if (best.port != 0) return best;
    const fs::path vendor = root / pm::brand::k_vendor_w;
    std::error_code ec;
    if (!fs::is_directory(vendor, ec)) return best;
    for (fs::directory_iterator it(vendor, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code sec;
        if (it->is_directory(sec))
            scan_instances(it->path() / "run-ipc" / "instances", best);
    }
    return best;
}

bool read_exact(SOCKET sock, void* buffer, int size, std::string& err)
{
    auto* out = static_cast<char*>(buffer);
    int remaining = size;
    while (remaining > 0) {
        const int n = ::recv(sock, out, remaining, 0);
        if (n <= 0) {
            err = "cmd_ipc recv failed";
            return false;
        }
        out += n;
        remaining -= n;
    }
    return true;
}

bool read_frame(SOCKET sock, std::string& out, std::string& err)
{
    uint32_t len = 0;
    if (!read_exact(sock, &len, 4, err)) return false;
    if (len > 16u * 1024u * 1024u) {
        err = "cmd_ipc frame too large";
        return false;
    }
    out.assign(len, '\0');
    if (len == 0) return true;
    return read_exact(sock, out.data(), static_cast<int>(len), err);
}

bool write_frame(SOCKET sock, const std::string& payload, std::string& err)
{
    const uint32_t len = static_cast<uint32_t>(payload.size());
    if (::send(sock, reinterpret_cast<const char*>(&len), 4, 0) != 4) {
        err = "cmd_ipc send failed";
        return false;
    }
    int sent = 0;
    while (sent < static_cast<int>(payload.size())) {
        const int n = ::send(sock, payload.data() + sent, static_cast<int>(payload.size()) - sent, 0);
        if (n <= 0) {
            err = "cmd_ipc send failed";
            return false;
        }
        sent += n;
    }
    return true;
}

} // namespace

bool exchange(const nlohmann::json& request, nlohmann::json& reply, std::string& err)
{
    static UiEndpoint cached;
    static DWORD cached_tick = 0;
    const DWORD now = ::GetTickCount();
    if (cached.port == 0 || now - cached_tick > 2000) {
        cached = find_ui();
        cached_tick = now;
    }
    const UiEndpoint ui = cached;
    if (ui.port == 0) {
        err = "tanit is not running";
        return false;
    }

    static bool wsa = false;
    if (!wsa) {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            err = "winsock startup failed";
            return false;
        }
        wsa = true;
    }

    SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        err = "cmd_ipc socket failed";
        return false;
    }
    const DWORD timeout = 15000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(ui.port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(sock);
        cached.port = 0;
        err = "cmd_ipc connect failed";
        return false;
    }

    std::string challenge;
    if (!read_frame(sock, challenge, err)) {
        ::closesocket(sock);
        return false;
    }
    std::string nonce;
    try {
        nonce = nlohmann::json::parse(challenge).value("nonce", std::string{});
    } catch (...) {
        ::closesocket(sock);
        err = "cmd_ipc malformed challenge";
        return false;
    }
    const std::string proof = proof_for(ui.token, nonce);
    if (proof.empty()) {
        ::closesocket(sock);
        err = "cmd_ipc proof failed";
        return false;
    }

    nlohmann::json req = request;
    req["proof"] = proof;
    if (!write_frame(sock, req.dump(), err)) {
        ::closesocket(sock);
        return false;
    }
    std::string raw;
    if (!read_frame(sock, raw, err)) {
        ::closesocket(sock);
        return false;
    }
    ::closesocket(sock);
    try {
        reply = nlohmann::json::parse(raw);
    } catch (...) {
        err = "cmd_ipc malformed reply";
        return false;
    }
    if (reply.value("type", std::string{}) == "error"
        && reply.value("error", std::string{}) == "auth failed") {
        err = "cmd_ipc authentication rejected";
        return false;
    }
    return true;
}

bool chat_via_cmd_ipc(const nlohmann::json& message, nlohmann::json& reply, std::string& err)
{
    return exchange({
        {"action", "chat"},
        {"message", message},
    }, reply, err);
}

bool command_run_via_cmd_ipc(const nlohmann::json& request, nlohmann::json& reply, std::string& err)
{
    return exchange({
        {"action", "command_run"},
        {"request", request},
    }, reply, err);
}

} // namespace media::win::chrome_inspect
