#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace media::win::chrome_inspect {

// Loopback catalog the spawned agent registers as host-tool provider "chrome".
void start_chrome_tool_server();
std::string chrome_tool_provider_url();
std::string chrome_context_import_url();
std::string chrome_tool_session_token();

// Latest tab list from a side-panel send. The context route turns this into
// the system note the model sees before it calls a browser tool.
void note_browser_snapshot(const nlohmann::json& browser);

} // namespace media::win::chrome_inspect
