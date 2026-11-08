# win32

Windows UI Automation, clipboard, and shell helpers. Include root is `src/`. Callers use `inspect/app_inspect.hpp`, `inspect/uia_spy.hpp`, `clipboard/context_clipboard.hpp`, and `shell/context_shell.hpp`. Namespaces are unchanged (`media::assistant`, `media::assistant::app_inspect`, `media::win::context`).

`pm-win32-lib` is a static library, added with `EXCLUDE_FROM_ALL`, and linked by `tanit.exe` and `tanit-cli.exe`. It holds the translation units that do not include host headers:

- `inspect/app_inspect` — window dump, element value, toggle, list selection, subtree, window and screen JPEG
- `inspect/app_targets` — process-name classification for the spy
- `inspect/uia_spy` — foreground UIAutomation poll

`tanit-cli assistant` includes `objbase.h` before `inspect/uia_spy.hpp` so `MIDL_INTERFACE` is still defined after CLI11.

UTF-8 conversion goes through `media::process::narrow_utf8` in `packages/commons.cpp/src/process/process_helpers.hpp`. Wide case-fold stays in this package: `media::strutil::lower_ascii` takes `std::string`.

`clipboard/` and `shell/` live here and are still compiled by the host. Both headers include `win/context/context_store.hpp` (`ContextItem`, `ClipboardHistoryItem`). The same two `.cpp` files are on the UI source list and the CLI source list. With a shared `pm-media` they compile into that library; otherwise they compile into the executable that asked for them.

`inspect/app_use`, `inspect/app_batch`, `inspect/inspect`, `inspect/highlight_overlay`, `inspect/pick_hud`, and `inspect/pick_loupe` are compiled by `tanit.exe` and `tanit-cli.exe`. The xblox-only image compiles the picker, highlight, HUD, and loupe. `app_use.cpp` calls `ContextStore::PreferredExternalHwnd`. `chrome/` is the native-messaging host and the picker DOM bridge (`tanit-chrome-host`, the assistant feature, and the xblox picker). `picker_enrich.cpp` still includes `win/web/webview_dom_inspect.hpp`.

`capture/display_affinity.hpp` and `capture/window_bounds.hpp` forward to `pm/video/…`. The `.cpp` files stay in `packages/video.cpp` and are linked once through `pm-video-lib`.

These stay in `src/win`: `context_store`, the assistant bar, WebView2, and `detect_overlay`. `tanit-security` does not link this library.

`cli/` and `tests/` are the commons command harness (`string`, `path`, `uri`, `fs`, `process`, `task`, `ipc`). They are separate from `pm-win32-lib`.
