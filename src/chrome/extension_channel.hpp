#pragma once

#include <nlohmann/json.hpp>

namespace media::win::chrome_inspect {

// Write a bridge request on the native-messaging stdout and wait for the
// matching stdin reply (ok + requestId). Used by the host-tool HTTP server.
bool ask_extension(const nlohmann::json& request, nlohmann::json& response, unsigned timeout_ms);

} // namespace media::win::chrome_inspect
