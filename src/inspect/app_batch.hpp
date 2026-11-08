#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace media::assistant::app_batch {

struct RunOptions {
    int default_delay_ms = 50;
    int default_wait_timeout_ms = 5000;
    int default_wait_interval_ms = 100;
    bool stop_on_error = true;
    std::uintptr_t initial_hwnd = 0;
    /// Replay speed multiplier (video-playback convention):
    ///   1.0 = natural pacing (default_delay_ms between steps, scheduling unchanged)
    ///   0.5 = half speed (delays × 2, scheduled onsets × 2)
    ///   2.0 = double speed (delays ÷ 2, scheduled onsets ÷ 2)
    ///   0.0 = instant (no sleeps, scheduling skipped)
    double speed = 1.0;
};

bool run_json_batch(const nlohmann::json& doc,
                    const RunOptions& opts,
                    nlohmann::json& report,
                    std::string& err);

} // namespace media::assistant::app_batch
