#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace media::win::chrome_inspect {

bool send_bridge_request(const nlohmann::json& request,
                         nlohmann::json& response,
                         int timeout_ms,
                         std::string& err);

nlohmann::json make_bridge_request(std::string command,
                                   nlohmann::json payload = nlohmann::json::object());

} // namespace media::win::chrome_inspect
