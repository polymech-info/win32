#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace media::assistant::app_inspect {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct ElementInfo {
    int index = 0;
    DWORD pid = 0;
    HWND hwnd = nullptr;
    std::wstring process;
    std::wstring window_title;
    std::wstring control_type;
    std::wstring name;
    std::wstring value;
    std::wstring automation_id;
    std::wstring class_name;
    std::wstring framework_id;
    Rect rect;
    bool enabled = false;
    bool offscreen = false;
    bool focused = false;
    bool keyboard_focusable = false;
    bool has_invoke = false;
    bool has_value = false;
    bool has_text = false;
    bool has_selection_item = false;
    bool has_expand_collapse = false;
    bool has_scroll = false;
    int native_hwnd = 0;
};

struct WindowDump {
    DWORD pid = 0;
    HWND hwnd = nullptr;
    std::wstring process;
    std::wstring title;
    Rect rect;
    std::vector<ElementInfo> elements;
};

struct Query {
    bool foreground = false;
    DWORD pid = 0;
    HWND hwnd = nullptr;
    std::wstring process_contains;
    std::wstring title_contains;
    int limit = 500;
    bool probe_cells = false;
};

struct MarkdownOptions {
    std::wstring control_types_csv;
    int text_max_chars = 80;
};

bool dump_windows(const Query& query, std::vector<WindowDump>& out, std::string& err);
bool find_element_rect(const Query& query, int element_index, Rect& out, std::string& err);
std::string dump_windows_markdown(const std::vector<WindowDump>& windows, const MarkdownOptions& opts);
bool save_window_jpeg(HWND hwnd, Rect& out_rect, const std::wstring& out_path, int quality, std::string& err);
bool save_screen_rect_jpeg(const Rect& rect, const std::wstring& out_path, int quality, std::string& err);

/// Set the UIA Value pattern on an element identified by AutomationId.
/// Works for Edit (text/number), ComboBox, and any element that exposes IValueProvider.
bool set_element_value(const Query& query,
                       const std::wstring& automation_id,
                       const std::wstring& value,
                       std::string& err);

/// Toggle a CheckBox (or any element with IToggleProvider) to a desired on/off state.
bool set_element_checked(const Query& query,
                         const std::wstring& automation_id,
                         bool desired_checked,
                         std::string& err);

/// Select a ListItem by its Name property using ISelectionItemProvider::Select().
bool select_list_item(const Query& query,
                      const std::wstring& item_name,
                      std::string& err);

/// ExpandCollapse pattern: Expand() or Collapse() a UIA element by name within
/// a window resolved from @p query (pid / hwnd / title / process).
/// @p element_name   — case-insensitive substring against UIA element Name property.
/// @p element_control_type — optional control-type filter (e.g. L"SplitButton").
/// @p element_automation_id — optional AutomationId filter.
/// @p expand = true → Expand(), false → Collapse().
/// @p expanded_state — receives the resulting ExpandCollapseState name on success.
bool expand_collapse_element(const Query& query,
                             const std::wstring& element_name,
                             const std::wstring& element_control_type,
                             const std::wstring& element_automation_id,
                             bool expand,
                             std::string& expanded_state, std::string& err);

/// Dump the UIA sub-tree rooted at an element identified by name / control-type / automation-id.
/// Useful after expanding a dropdown: returns only the children of the matched element.
/// Falls back to full dump_windows if the element is not found.
bool dump_element_subtree(const Query& query,
                          const std::wstring& element_name,
                          const std::wstring& element_control_type,
                          const std::wstring& element_automation_id,
                          std::vector<WindowDump>& out, std::string& err);

} // namespace media::assistant::app_inspect
