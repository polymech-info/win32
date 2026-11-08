#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace media::win::chrome_inspect {

// One request/reply against the running Tanit UI cmd_ipc server.
// `message` is the same JSON the in-app chat posts to the host.
bool chat_via_cmd_ipc(const nlohmann::json& message, nlohmann::json& reply, std::string& err);

// `request` is the chat `commandRun` payload (`id`, and any app/ribbon fields).
// The UI posts the same WM_COMMAND the in-app chat uses.
bool command_run_via_cmd_ipc(const nlohmann::json& request, nlohmann::json& reply, std::string& err);

} // namespace media::win::chrome_inspect
