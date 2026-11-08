#pragma once
// Internal Shell / Win32 observation helpers for ContextStore.

#include "win/context/context_store.hpp"

#if defined(_WIN32)

struct IShellItem;
struct IDataObject;

#include <string>
#include <vector>

namespace media::win::context::shell {

struct ViewObservation {
    WindowContext window;
    ContextFolder folder;
    std::vector<ContextItem> selection;
    uint32_t totalCount = 0;
    bool truncated = false;
    bool desktop = false;
    ProviderStatus status = ProviderStatus::NotQueried;
    std::string error;
};

WindowContext describe_hwnd(HWND hwnd, uint64_t timestampMs);
bool is_our_ui_window(HWND hwnd);
bool is_our_helper_window(HWND hwnd);
bool is_explorer_window(HWND hwnd);
bool is_desktop_window(HWND hwnd);
bool is_shell_infrastructure(HWND hwnd);
bool is_window_cloaked(HWND hwnd);
bool IsTaskbarCandidate(HWND hwnd);
bool IsAltTabCandidate(HWND hwnd);

std::wstring process_name_of(uint32_t pid);
std::wstring class_name_of(HWND hwnd);
std::wstring title_of(HWND hwnd);

OpenAppWindow describe_open_app(HWND hwnd, uint64_t timestampMs, HWND foreground,
                                HWND previousExternal);
std::vector<OpenAppWindow> enumerate_open_apps(uint64_t timestampMs, HWND foreground,
                                               HWND previousExternal);

// IShellWindows → IFolderView2. GetSelection(FALSE). Failures are per-view.
// wantSelection=false reads folder identity only (foreground folder MRU).
std::vector<ViewObservation> enumerate_shell_views(uint64_t timestampMs);
ViewObservation query_desktop_view(uint64_t timestampMs, bool wantSelection = true);
ViewObservation query_view_for_hwnd(HWND hwnd, uint64_t timestampMs, bool wantSelection = true);

AppRecentContext query_app_document_lists(const std::wstring& appUserModelId, size_t maxItems = kMaxAppRecentItems);
// Consumer-only. FOLDERID_Recent .lnk targets, newest write-time first.
// Not stored in session MRU. Does not SHAddToRecentDocs.
std::vector<ContextItem> query_windows_recent(size_t maxItems = kMaxAppRecentItems);
void fill_app_identity(HWND hwnd, uint32_t pid, AppIdentity& id);

/// Same converter Recent/Selection uses (`SIGDN_*` + `SFGAO_FOLDER`).
ContextItem item_from_shell_item(::IShellItem* item, bool selected = true, bool focused = false);

/// Explorer file objects from an OLE data object (`CFSTR_SHELLIDLIST` /
/// `IShellItemArray`, then `CF_HDROP`). Empty when the payload is not files.
std::vector<ContextItem> items_from_data_object(::IDataObject* obj);

} // namespace media::win::context::shell

#endif
