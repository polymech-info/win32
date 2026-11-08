#pragma once

#include <nlohmann/json.hpp>

namespace media::win::chrome_inspect {

// One side-panel host message. Starts `tanit-cli llm agent --serve` on demand
// and answers presets and turns over HTTP. The desktop window is not required.
nlohmann::json handle_chrome_chat(const nlohmann::json& message);

// Stop a serve process this host spawned. A server that was already listening
// is left running.
void stop_owned_agent_server();

// %APPDATA%\<vendor>\<config-slug>\chrome-extension (e.g. PolyMech\tanit,
// PolyMech\tanit-chat for Store Chat, PolyMech\tanit-viewer for the viewer SKU).
// Created on first use. Follows PM_CONFIG_SLUG (cmake/Branding.cmake).
std::wstring chrome_extension_work_dir();

// Write a batch of pmChat events to the native-messaging port. No request id,
// so the service worker forwards them instead of treating them as a reply.
void publish_chrome_chat_events(const nlohmann::json& events);

// Side panel link. agent is "connected" or "disconnected". The native host
// itself stays up; this only reports the spawned tanit-cli.
void publish_chrome_agent_status(const char* agent);

} // namespace media::win::chrome_inspect
