#include "app_batch.hpp"
#include "fs/path_sanitizer.hpp"
#include "process/process_helpers.hpp"

#include "app_inspect.hpp"
#include "app_use.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <thread>
#include "string/string_utils.hpp"

namespace media {
namespace assistant {
namespace app_batch {
namespace {

std::wstring u2w(const std::string& s) {
    return media::process::widen_utf8(s);
}

std::string w2u(const std::wstring& ws) {
    return media::process::narrow_utf8(ws);
}

int jint(const nlohmann::json& j, const char* key, int fallback = 0) {
    return j.contains(key) && j[key].is_number_integer() ? j[key].get<int>() : fallback;
}

bool jbool(const nlohmann::json& j, const char* key, bool fallback = false) {
    return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : fallback;
}

std::string jstr(const nlohmann::json& j, const char* key, const std::string& fallback = {}) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}

void sleep_ms(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/// Sleep, scaled by opts.speed (video-playback convention):
///   1.0 = natural pacing (ms unchanged)
///   0.5 = half speed (ms * 2 — slower)
///   2.0 = double speed (ms / 2 — faster)
///   0.0 = instant (no sleep)
void scaled_sleep_ms(int ms, double speed) {
    if (ms <= 0) return;
    if (speed <= 0.0) return;
    const int scaled = static_cast<int>(ms / speed);
    if (scaled > 0) std::this_thread::sleep_for(std::chrono::milliseconds(scaled));
}

struct BatchState {
    HWND current_hwnd = nullptr;
};

bool window_rect(HWND hwnd, RECT& out) {
    return hwnd && ::IsWindow(hwnd) && ::GetWindowRect(hwnd, &out) &&
           out.right > out.left && out.bottom > out.top;
}

nlohmann::json rect_json(const RECT& r) {
    const int w = static_cast<int>(r.right - r.left);
    const int h = static_cast<int>(r.bottom - r.top);
    return {
        {"x", static_cast<int>(r.left)},
        {"y", static_cast<int>(r.top)},
        {"w", w},
        {"h", h},
        {"left", static_cast<int>(r.left)},
        {"top", static_cast<int>(r.top)},
        {"right", static_cast<int>(r.right)},
        {"bottom", static_cast<int>(r.bottom)},
        {"center", {
            {"x", static_cast<int>(r.left + w / 2)},
            {"y", static_cast<int>(r.top + h / 2)},
        }},
    };
}

nlohmann::json current_window_json(HWND hwnd) {
    nlohmann::json out{{"hwnd", reinterpret_cast<std::uintptr_t>(hwnd)}};
    DWORD pid = 0;
    if (hwnd) ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid) out["pid"] = pid;
    wchar_t title_buf[512] = {};
    if (hwnd && ::GetWindowTextW(hwnd, title_buf, static_cast<int>(sizeof(title_buf) / sizeof(title_buf[0]))) > 0)
        out["title"] = w2u(title_buf);
    RECT r{};
    if (window_rect(hwnd, r)) out["rect"] = rect_json(r);
    return out;
}

bool is_paint_window(HWND hwnd) {
    wchar_t title_buf[512] = {};
    if (!hwnd || ::GetWindowTextW(hwnd, title_buf, static_cast<int>(std::size(title_buf))) <= 0)
        return false;
    std::wstring title(title_buf);
    std::transform(title.begin(), title.end(), title.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return title.find(L"paint") != std::wstring::npos;
}

bool text_mentions_color_swatch(const std::string& text) {
    std::string t = text;
    t = media::strutil::lower_ascii(std::move(t));
    return t.find("red") != std::string::npos ||
           t.find("color") != std::string::npos ||
           t.find("swatch") != std::string::npos ||
           t.find("palette") != std::string::npos;
}

bool ensure_current_window(BatchState& state, std::string& err) {
    if (!state.current_hwnd)
        return true;
    if (!::IsWindow(state.current_hwnd)) {
        err = "current target window is no longer available";
        return false;
    }
    return app_use::activate_window(state.current_hwnd, err);
}

// Resolve a 2D coordinate from a step object.
//
//   x  / y               → absolute screen coords (back-compat)
//   xw / yw              → window-relative coords (origin = current_hwnd top-left). Discoverable
//                          shorthand for the older `windowRelative: true` flag — strongly preferred
//                          because the agent can't accidentally forget the flag.
//   apiWidth/apiHeight   → optional virtual canvas size; we map back to physical screen pixels.
//
// `xw`/`yw` win over `x`/`y` when both are present, with a warning suppressed (the LLM might emit
// both during a refactor). Falls back gracefully if the target window has died.
app_use::Point resolve_xy(const nlohmann::json& step, const BatchState& state,
                          const char* x_abs = "x",  const char* y_abs = "y",
                          const char* x_win = "xw", const char* y_win = "yw",
                          std::string* err = nullptr) {
    const bool has_abs_pair = step.contains(x_abs) || step.contains(y_abs);
    const bool has_win_pair = step.contains(x_win) || step.contains(y_win);
    if (has_abs_pair && !(step.contains(x_abs) && step.contains(y_abs))) {
        if (err) *err = std::string("coordinate requires both ") + x_abs + " and " + y_abs;
        return {};
    }
    if (has_win_pair && !(step.contains(x_win) && step.contains(y_win))) {
        if (err) *err = std::string("window-relative coordinate requires both ") + x_win + " and " + y_win;
        return {};
    }
    const bool has_win = step.contains(x_win) || step.contains(y_win) ||
                         jbool(step, "windowRelative", jbool(step, "relative"));
    app_use::Point pt{jint(step, x_abs), jint(step, y_abs)};
    if (step.contains(x_win)) pt.x = jint(step, x_win);
    if (step.contains(y_win)) pt.y = jint(step, y_win);
    if (has_win) {
        RECT r{};
        if (!window_rect(state.current_hwnd, r)) {
            if (err) *err = "window-relative coordinates require a current target window; add an activate/open-app step or pass hwnd/title";
            return pt;
        }
        pt.x += r.left;
        pt.y += r.top;
    }
    const int api_w = jint(step, "apiWidth", jint(step, "api_width"));
    const int api_h = jint(step, "apiHeight", jint(step, "api_height"));
    if (api_w > 0 && api_h > 0)
        pt = app_use::scale_point_from_api(pt, api_w, api_h);
    return pt;
}

app_use::Point scaled_point(const nlohmann::json& step, const BatchState& state) {
    return resolve_xy(step, state);
}

// Read a list of {x,y} (or {xw,yw}) points from `step[field]` and resolve each through the same
// xw/yw / windowRelative / apiWidth pipeline used for primary coords. Empty result on missing /
// malformed input — caller decides whether that's an error.
std::vector<app_use::Point> resolve_path(const nlohmann::json& step, const BatchState& state,
                                         const char* field = "path",
                                         std::string* err = nullptr) {
    std::vector<app_use::Point> out;
    if (!step.contains(field) || !step[field].is_array()) return out;
    // The path inherits the parent step's apiWidth/apiHeight + windowRelative flag — but individual
    // points can override with their own xw/yw. Build a per-point merged object so resolve_xy gets
    // the parent context without us copy-pasting it.
    nlohmann::json parent_ctx;
    for (auto k : {"apiWidth", "api_width", "apiHeight", "api_height", "windowRelative", "relative"})
        if (step.contains(k)) parent_ctx[k] = step[k];
    for (const auto& p : step[field]) {
        if (!p.is_object()) continue;
        nlohmann::json merged = parent_ctx;
        for (auto it = p.begin(); it != p.end(); ++it) merged[it.key()] = it.value();
        out.push_back(resolve_xy(merged, state, "x", "y", "xw", "yw", err));
        if (err && !err->empty()) return {};
    }
    return out;
}

// Sample an arc into a polyline. `arc` is an object with:
//   cx, cy            : centre (absolute or windowRelative depending on parent step)
//   radius            : pixels
//   start_deg, end_deg: 0° = +x axis, 90° = +y axis (screen y grows down so this is CW visually)
//   segments          : optional; defaults to ~one sample per 8° of sweep, min 8, max 120
//
// Common helper for arcs in drag (signature loops, O's, circular menus).
std::vector<app_use::Point> arc_to_points(const nlohmann::json& step, const BatchState& state) {
    std::vector<app_use::Point> out;
    if (!step.contains("arc") || !step["arc"].is_object()) return out;
    const auto& a = step["arc"];

    nlohmann::json centre_ctx;
    centre_ctx["x"]  = jint(a, "cx", jint(a, "center_x"));
    centre_ctx["y"]  = jint(a, "cy", jint(a, "center_y"));
    centre_ctx["xw"] = jint(a, "cxw");
    centre_ctx["yw"] = jint(a, "cyw");
    if (!a.contains("cxw") && step.contains("xw")) centre_ctx.erase("xw");
    if (!a.contains("cyw") && step.contains("yw")) centre_ctx.erase("yw");
    for (auto k : {"apiWidth", "api_width", "apiHeight", "api_height", "windowRelative", "relative"})
        if (step.contains(k)) centre_ctx[k] = step[k];
    if (a.contains("cxw") || a.contains("cyw"))
        centre_ctx["windowRelative"] = true;
    std::string coord_err;
    const app_use::Point centre = resolve_xy(centre_ctx, state, "x", "y", "xw", "yw", &coord_err);
    if (!coord_err.empty()) return out;

    const double radius    = static_cast<double>(jint(a, "radius"));
    if (radius < 1.0) return out;
    const double start_deg = static_cast<double>(jint(a, "start_deg", jint(a, "startDeg")));
    const double end_deg   = static_cast<double>(jint(a, "end_deg",   jint(a, "endDeg", 360)));
    const double sweep_deg = end_deg - start_deg;
    if (std::abs(sweep_deg) < 0.5) return out;

    int segments = jint(a, "segments");
    if (segments <= 0) {
        // ~one sample per 8° gives a visibly round circle in Paint without spamming SendInput.
        segments = (std::max)(8, (std::min)(120, static_cast<int>(std::abs(sweep_deg) / 8.0)));
    }
    if (segments > 360) segments = 360;
    constexpr double kPi = 3.14159265358979323846;
    out.reserve(static_cast<std::size_t>(segments + 1));
    for (int i = 0; i <= segments; ++i) {
        const double t   = start_deg + (sweep_deg * i) / segments;
        const double rad = (t * kPi) / 180.0;
        const int    x   = centre.x + static_cast<int>(std::cos(rad) * radius);
        const int    y   = centre.y + static_cast<int>(std::sin(rad) * radius);
        out.push_back({x, y});
    }
    return out;
}

app_inspect::Query query_from_json(const nlohmann::json& j, const BatchState* state = nullptr) {
    app_inspect::Query q;
    q.foreground = jbool(j, "foreground");
    q.pid = static_cast<DWORD>(jint(j, "pid"));
    q.hwnd = jint(j, "hwnd") ? reinterpret_cast<HWND>(static_cast<std::uintptr_t>(jint(j, "hwnd"))) : nullptr;
    q.process_contains = u2w(jstr(j, "process"));
    q.title_contains = u2w(jstr(j, "title"));
    q.limit = jint(j, "limit", 500);
    q.probe_cells = jbool(j, "probe", jbool(j, "probeCells", jbool(j, "probe_cells")));
    if (!q.foreground && !q.pid && !q.hwnd && q.process_contains.empty() && q.title_contains.empty() && state && state->current_hwnd)
        q.hwnd = state->current_hwnd;
    return q;
}

bool point_inside_current_window(const BatchState& state, app_use::Point pt, std::string& err) {
    if (!state.current_hwnd)
        return true;
    RECT r{};
    if (!window_rect(state.current_hwnd, r)) {
        err = "current target window is no longer available";
        return false;
    }
    if (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom) {
        err = "target point is outside the current app window";
        return false;
    }
    return true;
}

bool element_matches(const app_inspect::ElementInfo& e, const nlohmann::json& step, int* out_score = nullptr) {
    // Build selector from step JSON
    app_use::ElementSelector sel;
    sel.ref = u2w(jstr(step, "ref"));
    sel.name = u2w(jstr(step, "name"));
    sel.value = u2w(jstr(step, "value"));
    sel.control_type = u2w(jstr(step, "controlType"));
    sel.automation_id = u2w(jstr(step, "automation_id", jstr(step, "automationId")));
    sel.class_name = u2w(jstr(step, "class_name", jstr(step, "className")));
    sel.nth = jint(step, "nth", 0);
    sel.min_score = jint(step, "minScore", 1);

    // If ref is provided, compute and compare
    if (!sel.ref.empty()) {
        const std::wstring computed = app_use::compute_element_ref(
            e.control_type, e.name, e.automation_id, sel.nth);
        if (sel.ref == computed) {
            if (out_score) *out_score = 1000;
            return true;
        }
    }

    // Use scoring system for fuzzy matching
    const int score = app_use::score_element_match(
        e.control_type, e.name, e.automation_id, e.value, sel);
    if (score < sel.min_score) return false;
    if (out_score) *out_score = score;
    return true;
}

bool has_element_selector(const nlohmann::json& step) {
    return step.contains("name") || step.contains("text") || step.contains("value") ||
           step.contains("controlType") || step.contains("automation_id") ||
           step.contains("class_name") || step.contains("ref");
}

bool wait_element(const nlohmann::json& step, const RunOptions& opts, BatchState& state, nlohmann::json& out, std::string& err) {
    const int timeout_ms = jint(step, "timeoutMs", opts.default_wait_timeout_ms);
    const int interval_ms = jint(step, "intervalMs", opts.default_wait_interval_ms);
    const int nth = jint(step, "nth", 0);  // Support selecting the n-th best match
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int match_count = 0;
    do {
        std::vector<app_inspect::WindowDump> windows;
        if (!app_inspect::dump_windows(query_from_json(step, &state), windows, err))
            return false;

        // Collect all matches with scores for ranking
        struct Match {
            const app_inspect::WindowDump* w;
            const app_inspect::ElementInfo* e;
            int score;
        };
        std::vector<Match> matches;

        for (const auto& w : windows) {
            for (const auto& e : w.elements) {
                int score = 0;
                if (!element_matches(e, step, &score)) continue;
                matches.push_back({&w, &e, score});
            }
        }

        // Sort by score descending (best matches first)
        std::sort(matches.begin(), matches.end(), [](const auto& a, const auto& b) {
            return a.score > b.score;
        });

        // Select nth match (default 0 = first/best)
        if (nth >= 0 && nth < static_cast<int>(matches.size())) {
            const auto& m = matches[nth];
            const auto& w = *m.w;
            const auto& e = *m.e;

            out["windowTitle"] = w2u(w.title);
            out["pid"] = w.pid;
            out["hwnd"] = reinterpret_cast<std::uintptr_t>(w.hwnd);
            state.current_hwnd = w.hwnd;

            // Compute stable ref for this element
            const std::wstring ref = app_use::compute_element_ref(
                e.control_type, e.name, e.automation_id, nth);

            out["element"] = {
                {"index", e.index},
                {"ref", w2u(ref)},
                {"name", w2u(e.name)},
                {"value", w2u(e.value)},
                {"automationId", w2u(e.automation_id)},
                {"className", w2u(e.class_name)},
                {"controlType", w2u(e.control_type)},
                {"focused", e.focused},
                {"keyboardFocusable", e.keyboard_focusable},
                {"patterns", {
                    {"invoke", e.has_invoke},
                    {"value", e.has_value},
                    {"text", e.has_text},
                    {"selectionItem", e.has_selection_item},
                    {"expandCollapse", e.has_expand_collapse},
                    {"scroll", e.has_scroll},
                }},
                {"rect", {{"x", e.rect.x}, {"y", e.rect.y}, {"w", e.rect.w}, {"h", e.rect.h}}},
                {"center", {{"x", e.rect.x + e.rect.w / 2}, {"y", e.rect.y + e.rect.h / 2}}},
            };
            out["matchScore"] = m.score;
            out["matchIndex"] = nth;
            out["totalMatches"] = static_cast<int>(matches.size());
            return true;
        }
        sleep_ms(interval_ms);
    } while (std::chrono::steady_clock::now() < deadline);

    err = "wait-element timed out";
    return false;
}

bool click_element(const nlohmann::json& step, const RunOptions& opts, BatchState& state, nlohmann::json& out, std::string& err) {
    nlohmann::json found;
    if (!wait_element(step, opts, state, found, err))
        return false;
    out = found;
    const auto& center = found["element"]["center"];
    const int x = center.value("x", 0);
    const int y = center.value("y", 0);
    if (!app_use::click_point(x, y, u2w(jstr(step, "button", "left")),
                              jint(step, "count", 1), jbool(step, "virtual"), err))
        return false;
    out["clicked"] = {{"x", x}, {"y", y}};
    return true;
}

bool run_step(const nlohmann::json& step, const RunOptions& opts, BatchState& state, nlohmann::json& out, std::string& err) {
    const std::string action = jstr(step, "action");
    if (action.empty()) { err = "step action is required"; return false; }

    if (action == "wait") {
        scaled_sleep_ms(jint(step, "ms", opts.default_delay_ms), opts.speed);
    } else if (action == "open-app") {
        app_use::LaunchOptions lo;
        lo.exe = u2w(jstr(step, "exe", jstr(step, "process")));
        lo.args = u2w(jstr(step, "args"));
        lo.cwd = u2w(jstr(step, "cwd"));
        lo.x = jint(step, "x"); lo.y = jint(step, "y");
        lo.w = jint(step, "width", jint(step, "w"));
        lo.h = jint(step, "height", jint(step, "h"));
        lo.wait_ms = jint(step, "waitMs", 1000);
        app_use::LaunchResult lr;
        if (!app_use::open_app(lo, lr, err)) return false;
        out["pid"] = lr.pid;
        out["hwnd"] = reinterpret_cast<std::uintptr_t>(lr.hwnd);
        if (lr.hwnd) {
            state.current_hwnd = lr.hwnd;
            out["window"] = current_window_json(lr.hwnd);
        }
    } else if (action == "activate") {
        HWND hwnd = nullptr;
        const bool process_only = state.current_hwnd && step.contains("process") &&
                                  !step.contains("pid") && !step.contains("hwnd") &&
                                  !step.contains("title") && !jbool(step, "foreground");
        if (process_only) {
            hwnd = state.current_hwnd;
        } else if (!app_use::find_window(static_cast<DWORD>(jint(step, "pid")),
                                         jint(step, "hwnd") ? reinterpret_cast<HWND>(static_cast<std::uintptr_t>(jint(step, "hwnd"))) : nullptr,
                                         u2w(jstr(step, "title")), hwnd, err)) return false;
        if (!app_use::activate_window(hwnd, err)) return false;
        state.current_hwnd = hwnd;
        out["hwnd"] = reinterpret_cast<std::uintptr_t>(hwnd);
        out["window"] = current_window_json(hwnd);
    } else if (action == "mouse-move") {
        // Animated by default — apps with hover-driven UI (tooltips, drag previews, custom
        // highlights) need to see WM_MOUSEMOVE at intermediate positions. Opt out with
        // `move_duration_ms: 0` to teleport.
        if (!ensure_current_window(state, err)) return false;
        const auto pt = resolve_xy(step, state, "x", "y", "xw", "yw", &err);
        if (!err.empty()) return false;
      
        if (!point_inside_current_window(state, pt, err)) return false;
        const int dur = jint(step, "move_duration_ms", jint(step, "moveDurationMs", -1));
        if (!app_use::smooth_move(pt.x, pt.y, dur, err)) return false;
        out["x"] = pt.x; out["y"] = pt.y;
        if (state.current_hwnd) out["target"] = current_window_json(state.current_hwnd);
    } else if (action == "click") {
        if (has_element_selector(step) && !step.contains("x") && !step.contains("y") &&
                !step.contains("xw") && !step.contains("yw")) {
            return click_element(step, opts, state, out, err);
        }
        if (!ensure_current_window(state, err)) return false;
        const auto pt = resolve_xy(step, state, "x", "y", "xw", "yw", &err);
        if (!err.empty()) return false;
        if (!point_inside_current_window(state, pt, err)) return false;
        if (!app_use::click_point(pt.x, pt.y, u2w(jstr(step, "button", "left")),
                                  jint(step, "count", 1), jbool(step, "virtual"), err)) return false;
        out["x"] = pt.x; out["y"] = pt.y;
        if (state.current_hwnd) out["target"] = current_window_json(state.current_hwnd);
    } else if (action == "drag") {
        // Three drag flavours, in priority order:
        //   1) `path: [{x|xw, y|yw}, ...]`  — explicit polyline; first = down, last = up.
        //   2) `arc:  { cx, cy, radius, start_deg, end_deg, segments? }` — sampled circle / arc.
        //   3) `to`/`toX,toY` or `dx,dy`    — classic point-to-point (also internally a 2-point path).
        // All accept `xw,yw` (window-relative) anywhere a coordinate is consumed.
        if (!ensure_current_window(state, err)) return false;
        const std::wstring button   = u2w(jstr(step, "button", "left"));
        const int          duration = jint(step, "durationMs", jint(step, "duration_ms", 250));

        std::vector<app_use::Point> path = resolve_path(step, state, "path", &err);
        if (!err.empty()) return false;
        const char*                 mode = "path";
        if (path.empty()) {
            path = arc_to_points(step, state);
            mode = "arc";
        }
        if (path.empty()) {
            mode = "linear";
            const auto from = resolve_xy(step, state, "x", "y", "xw", "yw", &err);
            if (!err.empty()) return false;
            app_use::Point to{};
            if (step.contains("toX") || step.contains("toY") ||
                step.contains("to_x") || step.contains("to_y") ||
                step.contains("toXw") || step.contains("toYw")) {
                nlohmann::json to_step = step;
                if (step.contains("toX"))  to_step["x"]  = step["toX"];
                if (step.contains("to_x")) to_step["x"]  = step["to_x"];
                if (step.contains("toY"))  to_step["y"]  = step["toY"];
                if (step.contains("to_y")) to_step["y"]  = step["to_y"];
                if (step.contains("toXw")) to_step["xw"] = step["toXw"];
                if (step.contains("toYw")) to_step["yw"] = step["toYw"];
                to = resolve_xy(to_step, state, "x", "y", "xw", "yw", &err);
                if (!err.empty()) return false;
            } else {
                to = {from.x + jint(step, "dx"), from.y + jint(step, "dy")};
            }
            path = {from, to};
        }
        if (path.size() < 2) { err = "drag: need a path with at least 2 points (or to/dx,dy / arc)"; return false; }
        if (std::string(mode) == "path" && path.size() > 2 && !jbool(step, "continuous")) {
            err = "drag: path with more than 2 points is one continuous stroke and is ambiguous; "
                  "use separate drag steps for separate strokes such as a cross/X/plus, or set "
                  "continuous:true when a connected polyline is intentional";
            return false;
        }
        if (!point_inside_current_window(state, path.front(), err)) return false;
        if (!point_inside_current_window(state, path.back(),  err)) return false;
        if (!app_use::drag_path(path, button, duration, err)) return false;
        out["mode"]   = mode;
        out["from"]   = {{"x", path.front().x}, {"y", path.front().y}};
        out["to"]     = {{"x", path.back().x},  {"y", path.back().y}};
        out["points"] = static_cast<int>(path.size());
        if (state.current_hwnd) out["target"] = current_window_json(state.current_hwnd);
    } else if (action == "scroll") {
        // Default scrolls at the cursor's current position (Win32 behaviour). Pass x/y or xw/yw to
        // explicitly target a control; pass `axis: "h"` for horizontal wheels (wide canvases /
        // timelines / code editor horizontal scroll).
        if (!ensure_current_window(state, err)) return false;
        if (step.contains("x") || step.contains("y") || step.contains("xw") || step.contains("yw")) {
            const auto pt = resolve_xy(step, state, "x", "y", "xw", "yw", &err);
            if (!err.empty()) return false;
            if (!point_inside_current_window(state, pt, err)) return false;
            const int dur = jint(step, "move_duration_ms", jint(step, "moveDurationMs", -1));
            if (!app_use::smooth_move(pt.x, pt.y, dur, err)) return false;
            out["x"] = pt.x; out["y"] = pt.y;
        }
        const std::string axis = jstr(step, "axis", "v");
        const bool horizontal  = (axis == "h" || axis == "horizontal" || axis == "x");
        const int  clicks      = jint(step, "clicks");
        if (!app_use::scroll_wheel(clicks, horizontal, err)) return false;
        out["clicks"] = clicks;
        out["axis"]   = horizontal ? "h" : "v";
    } else if (action == "mouse-down") {
        if (!app_use::mouse_button_down(u2w(jstr(step, "button", "left")), err)) return false;
    } else if (action == "mouse-up") {
        if (!app_use::mouse_button_up(u2w(jstr(step, "button", "left")), err)) return false;
    } else if (action == "type") {
        if (!ensure_current_window(state, err)) return false;
        if (!app_use::type_text(u2w(jstr(step, "text")), err)) return false;
    } else if (action == "hotkey") {
        if (!ensure_current_window(state, err)) return false;
        std::vector<std::wstring> keys = app_use::split_keys(u2w(jstr(step, "keys")));
        if (keys.empty() && step.contains("key") && step["key"].is_array()) {
            for (const auto& k : step["key"]) {
                if (k.is_string()) keys.push_back(u2w(k.get<std::string>()));
            }
        }
        if (keys.empty() && step.contains("key") && step["key"].is_string()) {
            if (step.contains("modifiers") && step["modifiers"].is_array()) {
                for (const auto& m : step["modifiers"]) {
                    if (m.is_string()) keys.push_back(u2w(m.get<std::string>()));
                }
            }
            keys.push_back(u2w(step["key"].get<std::string>()));
        }
        if (!app_use::send_hotkey(keys, err)) return false;
        nlohmann::json kj = nlohmann::json::array();
        for (const auto& k : keys) kj.push_back(w2u(k));
        out["keys"] = std::move(kj);
    } else if (action == "key-down") {
        // Press a key WITHOUT releasing it. Pair with a later `key-up` step
        // to model sustained / overlapping notes or held modifiers across
        // several batch steps. The activated target wins focus before the
        // event so the press goes to the right app.
        if (!ensure_current_window(state, err)) return false;
        if (!app_use::key_down(u2w(jstr(step, "key")), err)) return false;
        out["key"] = jstr(step, "key");
    } else if (action == "key-up") {
        if (!app_use::key_up(u2w(jstr(step, "key")), err)) return false;
        out["key"] = jstr(step, "key");
    } else if (action == "key-press") {
        // Down -> hold for `hold_ms` -> up. Designed for rhythmic / melodic
        // input where note length matters (e.g. FreePiano, virtual MIDI
        // controllers): the host app sees KEYDOWN as NoteOn and KEYUP as
        // NoteOff, so `hold_ms` IS the sustain length. Inter-step rest is
        // controlled by the existing `delayMs` (which sleeps AFTER the up).
        // `modifiers` is an optional list of ctrl/shift/alt/win held during
        // the press (rare for piano, useful for shortcut sequences).
        if (!ensure_current_window(state, err)) return false;
        std::vector<std::wstring> mods;
        if (step.contains("modifiers") && step["modifiers"].is_array()) {
            for (const auto& m : step["modifiers"]) {
                if (m.is_string()) mods.push_back(u2w(m.get<std::string>()));
            }
        }
        const std::string key_name = jstr(step, "key");
        const int hold_ms = jint(step, "hold_ms", jint(step, "holdMs", 50));
        if (!app_use::press_key(u2w(key_name), mods, hold_ms, err)) return false;
        out["key"] = key_name;
        out["hold_ms"] = hold_ms;
        if (!mods.empty()) {
            nlohmann::json mj = nlohmann::json::array();
            for (const auto& m : mods) mj.push_back(w2u(m));
            out["modifiers"] = std::move(mj);
        }
    } else if (action == "clipboard-set") {
        if (!app_use::set_clipboard_text(u2w(jstr(step, "text")), err)) return false;
    } else if (action == "clipboard-get") {
        std::wstring text;
        if (!app_use::get_clipboard_text(text, err)) return false;
        out["text"] = w2u(text);
    } else if (action == "wait-element") {
        if (!wait_element(step, opts, state, out, err)) return false;
    } else if (action == "click-element") {
        if (!click_element(step, opts, state, out, err)) return false;
    } else if (action == "screenshot") {
        app_inspect::Rect r{jint(step, "x"), jint(step, "y"), jint(step, "w", jint(step, "width")),
                            jint(step, "h", jint(step, "height"))};
        HWND capture_hwnd = nullptr;
        app_inspect::Query q = query_from_json(step, &state);
        if (q.hwnd || q.pid || q.foreground || !q.title_contains.empty() || !q.process_contains.empty()) {
            std::vector<app_inspect::WindowDump> windows;
            if (!app_inspect::dump_windows(q, windows, err))
                return false;
            if (windows.empty()) {
                err = "screenshot: no matching window";
                return false;
            }
            capture_hwnd = windows.front().hwnd;
            state.current_hwnd = capture_hwnd;
        }
        if (step.contains("element") && step["element"].is_number_integer()) {
            if (!app_inspect::find_element_rect(q, step["element"].get<int>(), r, err))
                return false;
        }
        const std::string out_arg = jstr(step, "output",
                                    jstr(step, "output_path",
                                    jstr(step, "path",
                                    jstr(step, "filename"))));
        if (out_arg.empty()) {
            err = "screenshot: output/path/output_path/filename is required";
            return false;
        }
        const std::filesystem::path output = std::filesystem::absolute(media::path::path_from_utf8(out_arg));
        std::error_code ec;
        std::filesystem::create_directories(output.parent_path(), ec);
        const int quality = jint(step, "quality", 85);
        if (capture_hwnd) {
            app_inspect::Rect out_rect;
            if (!app_inspect::save_window_jpeg(capture_hwnd, out_rect, output.wstring(), quality, err))
                return false;
            out["capture"] = "window";
            out["window"] = current_window_json(capture_hwnd);
            out["rect"] = {{"x", out_rect.x}, {"y", out_rect.y}, {"w", out_rect.w}, {"h", out_rect.h}};
        } else {
            if (!app_inspect::save_screen_rect_jpeg(r, output.wstring(), quality, err))
                return false;
            out["capture"] = "screen";
            out["rect"] = {{"x", r.x}, {"y", r.y}, {"w", r.w}, {"h", r.h}};
        }
        out["output"] = media::path::path_to_utf8(output);
    } else if (action == "set-value") {
        // Set the UIA Value pattern on an element found by automationId.
        // Works for Edit (text/number/multiline) and ComboBox fields.
        //
        // Step fields:
        //   automationId  — required (e.g. "pdfjs_internal_id_126R")
        //   value         — string to set (default: "")
        //   pid / hwnd / title / process — window selector
        {
            app_inspect::Query q = query_from_json(step, &state);
            const std::wstring aid   = u2w(jstr(step, "automationId",
                                              jstr(step, "automation_id")));
            const std::wstring value = u2w(jstr(step, "value"));
            if (aid.empty()) { err = "set-value: automationId is required"; return false; }
            if (!app_inspect::set_element_value(q, aid, value, err)) return false;
            out["automationId"] = w2u(aid);
            out["value"]        = w2u(value);
        }
    } else if (action == "set-checked") {
        // Toggle a CheckBox / RadioButton to desired checked state via IToggleProvider.
        //
        // Step fields:
        //   automationId  — required
        //   checked       — boolean, desired state (default: true)
        //   pid / hwnd / title / process — window selector
        {
            app_inspect::Query q = query_from_json(step, &state);
            const std::wstring aid     = u2w(jstr(step, "automationId",
                                               jstr(step, "automation_id")));
            const bool         checked = jbool(step, "checked", true);
            if (aid.empty()) { err = "set-checked: automationId is required"; return false; }
            if (!app_inspect::set_element_checked(q, aid, checked, err)) return false;
            out["automationId"] = w2u(aid);
            out["checked"]      = checked;
        }
    } else if (action == "select-item") {
        // Select a ListItem by Name via ISelectionItemProvider::Select().
        // Works for <select size> list boxes rendered by pdfjs.
        //
        // Step fields:
        //   item   — required: the item Name to select (e.g. "October")
        //   pid / hwnd / title / process — window selector
        {
            app_inspect::Query q    = query_from_json(step, &state);
            const std::wstring item = u2w(jstr(step, "item",
                                           jstr(step, "name",
                                           jstr(step, "value"))));
            if (item.empty()) { err = "select-item: item name is required"; return false; }
            if (!app_inspect::select_list_item(q, item, err)) return false;
            out["item"] = w2u(item);
        }
    } else if (action == "expand-element" || action == "collapse-element") {
        // Expand or collapse a UIA element by name / control-type / automation-id.
        // The element must support the ExpandCollapse UIA pattern (dropdowns, combo-boxes,
        // tree nodes, split-button arrow parts).
        //
        // Step fields:
        //   name          — substring of UIA element Name (e.g. "Viewer")
        //   controlType   — optional control-type filter (e.g. "SplitButton")
        //   automationId  — optional AutomationId filter
        //   pid / hwnd / title / process — window selector (falls back to current_hwnd)
        //   delayMs       — post-step wait (default from batch opts)
        {
            app_inspect::Query q = query_from_json(step, &state);
            const std::wstring el_name = u2w(jstr(step, "name"));
            const std::wstring el_ct   = u2w(jstr(step, "controlType",
                                              jstr(step, "control_type")));
            const std::wstring el_aid  = u2w(jstr(step, "automationId",
                                              jstr(step, "automation_id")));
            const bool do_expand = (action == "expand-element");
            std::string expanded_state;
            if (!app_inspect::expand_collapse_element(q, el_name, el_ct, el_aid,
                                                      do_expand, expanded_state, err))
                return false;
            out["action"]        = action;
            out["expandedState"] = expanded_state;
        }
    } else if (action == "find-app") {
        // Find an already-running app window and latch it as the batch's current target.
        // Unlike open-app, this does NOT spawn a new process.
        //
        // Step fields (all optional — if none given, matches any visible window):
        //   process   — process name substring (e.g. "pm-image")
        //   title     — window title substring
        //   pid       — exact process id
        //   hwnd      — exact window handle
        //
        // On success, sets state.current_hwnd and emits pid/hwnd/title/process.
        // Returns an error if no matching window is found.
        {
            app_use::FindWindowOptions fwo;
            fwo.pid           = static_cast<DWORD>(jint(step, "pid"));
            fwo.hwnd          = jint(step, "hwnd") ? reinterpret_cast<HWND>(
                                    static_cast<uintptr_t>(jint(step, "hwnd"))) : nullptr;
            fwo.title_contains    = u2w(jstr(step, "title"));
            fwo.process_name      = u2w(jstr(step, "process"));
            fwo.foreground        = jbool(step, "foreground");

            // Convenience: "pm-image" should also match "tanit.exe"
            if (!fwo.process_name.empty() &&
                fwo.process_name.find(L'.') == std::wstring::npos)
                fwo.process_name += L".exe";

            app_use::FindWindowResult fwr;
            if (!app_use::find_window_ex(fwo, fwr, err)) return false;
            if (!fwr.hwnd) { err = "find-app: no matching window"; return false; }

            state.current_hwnd = fwr.hwnd;
            out["hwnd"]    = reinterpret_cast<uintptr_t>(fwr.hwnd);
            out["pid"]     = fwr.pid;
            out["title"]   = w2u(fwr.title);
            out["process"] = w2u(fwr.process);
            out["score"]   = fwr.score;
            out["window"]  = current_window_json(fwr.hwnd);
        }
    } else if (action == "dump-tree") {
        // Dump the UIA accessibility tree of the current / specified window.
        // Optionally scoped to a sub-tree rooted at a named element.
        //
        // Step fields:
        //   pid / hwnd / title / process  — window selector
        //   name          — if set, scope the dump to the sub-tree of this element
        //   controlType   — element control-type filter for scoped dump
        //   automationId  — element automation-id filter for scoped dump
        //   limit         — max elements (default 500)
        app_inspect::Query q = query_from_json(step, &state);
        if (q.limit <= 0) q.limit = jint(step, "limit", 500);
        const std::wstring el_name    = u2w(jstr(step, "name"));
        const std::wstring el_ct      = u2w(jstr(step, "controlType", jstr(step, "control_type")));
        const std::wstring el_aid     = u2w(jstr(step, "automationId",  jstr(step, "automation_id")));

        std::vector<app_inspect::WindowDump> windows;
        if (!el_name.empty() || !el_ct.empty() || !el_aid.empty()) {
            if (!app_inspect::dump_element_subtree(q, el_name, el_ct, el_aid, windows, err))
                return false;
        } else {
            if (!app_inspect::dump_windows(q, windows, err))
                return false;
        }

        // Serialise into the step output.
        nlohmann::json wins = nlohmann::json::array();
        for (const auto& w : windows) {
            nlohmann::json jw{
                {"hwnd",    reinterpret_cast<std::uintptr_t>(w.hwnd)},
                {"pid",     w.pid},
                {"title",   w2u(w.title)},
                {"process", w2u(w.process)},
                {"rect",    {{"x", w.rect.x}, {"y", w.rect.y}, {"w", w.rect.w}, {"h", w.rect.h}}},
                {"elements", nlohmann::json::array()},
            };
            for (const auto& e : w.elements) {
                jw["elements"].push_back({
                    {"index",       e.index},
                    {"name",        w2u(e.name)},
                    {"value",       w2u(e.value)},
                    {"controlType", w2u(e.control_type)},
                    {"automationId",w2u(e.automation_id)},
                    {"className",   w2u(e.class_name)},
                    {"rect",        {{"x", e.rect.x}, {"y", e.rect.y}, {"w", e.rect.w}, {"h", e.rect.h}}},
                    {"center",      {{"x", e.rect.x + e.rect.w / 2}, {"y", e.rect.y + e.rect.h / 2}}},
                    {"enabled",     e.enabled},
                    {"focused",     e.focused},
                    {"patterns",    {
                        {"invoke",         e.has_invoke},
                        {"value",          e.has_value},
                        {"expandCollapse", e.has_expand_collapse},
                        {"selectionItem",  e.has_selection_item},
                    }},
                });
            }
            wins.push_back(std::move(jw));
        }
        out["windows"]       = std::move(wins);
        out["element_count"] = [&]{ int c = 0; for (const auto& w : windows) c += static_cast<int>(w.elements.size()); return c; }();
    } else {
        err = "unknown batch action: " + action;
        return false;
    }
    scaled_sleep_ms(jint(step, "delayMs", opts.default_delay_ms), opts.speed);
    return true;
}

} // namespace

bool run_json_batch(const nlohmann::json& doc,
                    const RunOptions& opts,
                    nlohmann::json& report,
                    std::string& err) {
    const nlohmann::json steps = doc.is_array() ? doc : doc.value("steps", nlohmann::json::array());
    if (!steps.is_array()) {
        err = "batch JSON must be an array or an object with steps[]";
        return false;
    }

    // Allow speed / defaultDelayMs to be set inside the JSON document as well as via RunOptions.
    // CLI-supplied values (opts.speed != 1.0, opts.default_delay_ms != 50) take precedence.
    RunOptions merged_opts = opts;
    if (!doc.is_array()) {
        if (merged_opts.speed == 1.0 && doc.contains("speed") && doc["speed"].is_number())
            merged_opts.speed = doc["speed"].get<double>();
        if (merged_opts.default_delay_ms == 50 && doc.contains("defaultDelayMs") && doc["defaultDelayMs"].is_number_integer())
            merged_opts.default_delay_ms = doc["defaultDelayMs"].get<int>();
    }
    const RunOptions& effective = merged_opts;

    report = {{"ok", true}, {"steps", nlohmann::json::array()},
              {"speed", effective.speed}, {"defaultDelayMs", effective.default_delay_ms}};
    BatchState state;
    if (opts.initial_hwnd) {
        HWND initial = reinterpret_cast<HWND>(opts.initial_hwnd);
        if (::IsWindow(initial)) {
            state.current_hwnd = initial;
            report["initial_target"] = current_window_json(initial);
        }
    }

    // ── Per-step scheduling ──────────────────────────────────────────────────
    //
    // Every step accepts two optional timing fields (works uniformly for click, mouse-move, drag,
    // scroll, key-down/up/press, type, hotkey, wait, anything):
    //
    //   at_ms      — absolute target onset, in milliseconds since the batch started. Source of
    //                truth for rhythmic / musical / sequenced playback. Use this when timing
    //                accuracy matters more than the cost of an individual step taking longer.
    //
    //   offset_ms  — onset relative to the PREVIOUS step's scheduled onset. Convenient for
    //                tempo-by-ear transcription: a 120 BPM 8th-note pattern is just
    //                `offset_ms: 250` on every step.
    //
    // Precedence: at_ms > offset_ms > free-running. When neither is set, the step fires immediately
    // after the previous step's body + post-step delay (legacy behaviour).
    //
    // When the NEXT step uses scheduling, the CURRENT step's `default_delay_ms` is suppressed
    // (explicit per-step `delayMs` is still honoured) — so the agent doesn't double-pay for
    // post-step delay AND a scheduled onset.
    //
    // Per-step report fields: `scheduled_at_ms`, `started_at_ms`, `late_ms` (positive = drifted
    // late). `late_ms` is 0 for free-running steps so it doesn't poison rhythm metrics.

    auto get_ll = [](const nlohmann::json& j, const char* key, long long fallback) -> long long {
        return j.contains(key) && j[key].is_number() ? j[key].get<long long>() : fallback;
    };
    auto has_schedule = [](const nlohmann::json& s) {
        return s.contains("at_ms") || s.contains("offset_ms") ||
               s.contains("atMs")  || s.contains("offsetMs");
    };

    const auto       batch_t0    = std::chrono::steady_clock::now();
    long long        prev_sched  = 0;
    bool             prev_was_sc = false;

    for (size_t i = 0; i < steps.size(); ++i) {
        nlohmann::json item{{"index", i}, {"action", jstr(steps[i], "action")}};

        // ── Resolve scheduled onset for this step ───────────────────────────
        const bool      sched_now  = has_schedule(steps[i]);
        long long       sched_ms   = -1;
        if (steps[i].contains("at_ms") || steps[i].contains("atMs")) {
            sched_ms = get_ll(steps[i], "at_ms", get_ll(steps[i], "atMs", 0));
        } else if (steps[i].contains("offset_ms") || steps[i].contains("offsetMs")) {
            const long long ofs = get_ll(steps[i], "offset_ms", get_ll(steps[i], "offsetMs", 0));
            // First scheduled step (or following an un-scheduled step) treats offset_ms as at_ms.
            sched_ms = (prev_was_sc ? prev_sched : 0) + ofs;
        }

        // ── Sleep until the scheduled time (no-op if we're already past it) ─
        if (sched_ms >= 0) {
            // Scale scheduled onset by 1/speed — speed=0.5 doubles all timings.
            const long long scaled_sched = (effective.speed > 0.0)
                ? static_cast<long long>(sched_ms / effective.speed)
                : 0LL;
            const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - batch_t0).count();
            const long long wait = scaled_sched - now_ms;
            if (wait > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(wait));
            prev_sched  = sched_ms;
            prev_was_sc = true;
        } else {
            prev_was_sc = false;
        }

        // ── Suppress default post-step delay if the NEXT step is scheduled ──
        RunOptions step_opts = effective;
        if (i + 1 < steps.size() && has_schedule(steps[i + 1])) {
            // Explicit per-step delayMs still wins inside run_step; we only neuter the default.
            step_opts.default_delay_ms = 0;
        }

        // ── Run ─────────────────────────────────────────────────────────────
        std::string    step_err;
        const auto     t0          = std::chrono::steady_clock::now();
        const long long started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t0 - batch_t0).count();
        nlohmann::json data        = nlohmann::json::object();
        const bool     ok          = run_step(steps[i], step_opts, state, data, step_err);
        const auto     elapsed     = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        item["ok"]              = ok;
        item["elapsedMs"]       = elapsed;
        item["scheduled_at_ms"] = (sched_ms >= 0 ? sched_ms : started_ms);
        item["started_at_ms"]   = started_ms;
        item["late_ms"]         = sched_now ? (started_ms - sched_ms) : 0;
        item["data"]            = std::move(data);
        if (!ok) item["error"]  = step_err;
        report["steps"].push_back(std::move(item));

        if (!ok) {
            const bool optional = jbool(steps[i], "optional", false);
            if (optional)
                continue;
            report["ok"] = false;
            if (opts.stop_on_error) {
                err = step_err;
                return false;
            }
        }
    }
    return report.value("ok", false);
}

} // namespace app_batch
} // namespace assistant
} // namespace media
