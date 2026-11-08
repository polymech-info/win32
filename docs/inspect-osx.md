# macOS Inspect / App-Use TODOs

Status: investigation TODOs only. No implementation exists yet.

The Windows stack lives in `src/win/inspect.{hpp,cpp}` and `src/win/assistant/`
and is compiled only under `WIN32 && FEATURE_ASSISTANT`. The macOS path should be a
separate Objective-C++ backend, not a direct port of HWND/UIA code.

## Target Shape

- [ ] Add a platform-neutral assistant/app-use interface for the shared CLI and LLM
      tool surfaces to call.
- [ ] Add macOS implementation files under `src/osx/assistant/`, likely `.mm`
      files for AppKit, ApplicationServices, and CoreGraphics calls.
- [ ] Keep the existing JSON/CLI behavior as close as possible to Windows:
      `app_inspect_dump`, `app_inspect_find`, `app_screenshot`, `app_click`,
      `app_drag`, `app_open`, `app_type`, `app_hotkey`, `app_close`, and
      `app_batch`.
- [ ] Use capability flags in output where macOS cannot offer exact Windows
      parity, instead of pretending AX is UIA or CGWindow is HWND.

## Permissions

- [ ] Add a permission probe for Accessibility trust:
      `AXIsProcessTrustedWithOptions`.
- [ ] Add a permission probe for screen capture permission. Prefer
      ScreenCaptureKit where available; fall back to CoreGraphics checks if the
      minimum macOS target requires it.
- [ ] Document the required Settings panes:
      Accessibility, Screen Recording, and possibly Input Monitoring.
- [ ] Fail fast with clear errors when permissions are missing.
- [ ] Decide how the CLI should request permissions in headless/SSH builds, where
      macOS prompts may not be visible.

## Window and App Resolution

- [ ] Use `NSWorkspace` / `NSRunningApplication` for launch, activation, app name,
      bundle id, executable path, and PID.
- [ ] Use `CGWindowListCopyWindowInfo` for visible window inventory:
      window id, owner PID, owner name, title, bounds, layer, alpha, onscreen.
- [ ] Define a macOS window id type separate from HWND. Do not overload `hwnd` in
      new internal APIs; only preserve JSON field names if needed for compatibility.
- [ ] Implement ranking equivalent to `find_window_ex`: foreground, pid, process
      name, title substring, bundle id, and window area tiebreaks.
- [ ] Handle Spaces/minimized/offscreen windows explicitly. CGWindow inventory and
      Accessibility window lists do not behave like `EnumWindows`.

## Accessibility Inspect

- [ ] Use `AXUIElementCreateSystemWide`, `AXUIElementCreateApplication(pid)`, and
      `AXUIElementCopyElementAtPosition` for point inspection.
- [ ] Use `kAXWindowsAttribute`, `kAXFocusedWindowAttribute`, and
      `kAXFocusedUIElementAttribute` for app/window scoping.
- [ ] Map AX fields to the existing element JSON:
      role/subrole -> `controlType`, title/description/value -> `name`/`value`,
      identifier -> `automationId` when available, position/size -> `rect`.
- [ ] Capture supported actions via `AXUIElementCopyActionNames`, including
      `AXPress`, `AXConfirm`, `AXShowMenu`, and increment/decrement actions.
- [ ] Implement tree traversal with a limit, equivalent to `dump_windows(Query)`.
- [ ] Implement element matching/scoring using role, title/name, value, identifier,
      description, and nth match.
- [ ] Verify Chrome/Electron/Safari behavior. Web content may expose useful AX
      nodes, but identifiers are not the same as UIA AutomationId.

## Input Synthesis

- [ ] Use `CGEventCreateMouseEvent` + `CGEventPost` for mouse move, down/up, click,
      drag, and wheel.
- [ ] Use `CGEventCreateKeyboardEvent` for hotkeys and key press/down/up.
- [ ] Build a key-name to virtual-key mapping for macOS, including modifiers,
      arrows, function keys, escape, tab, return, delete, and space.
- [ ] Use Unicode text insertion via clipboard paste or AX value setting where
      plain key events are insufficient. macOS key events are layout-sensitive.
- [ ] Use `NSPasteboard` for clipboard set/get.
- [ ] Prefer AX actions/value setting for accessible controls when possible;
      fallback to coordinate input for canvas/custom UI.

## Picker and Highlight

- [ ] Implement an interactive picker using global event monitoring or a CGEvent
      tap to track mouse movement, left click commit, and escape cancel.
- [ ] Show a non-activating transparent `NSPanel` overlay for the highlight border.
- [ ] On hover, resolve both the CGWindow under the cursor and the AX element at
      that position.
- [ ] On commit, return app/window metadata, AX element metadata, point, monitor,
      and target rect.
- [ ] Decide whether the committing click should be swallowed. A CGEvent tap can
      do this, but it has stricter permission/runtime behavior.

## Screenshots

- [ ] Implement screen-rect capture to JPEG for `app_screenshot` and batch
      `screenshot` steps.
- [ ] Implement window capture by CGWindow id where available.
- [ ] Evaluate ScreenCaptureKit for modern macOS and CoreGraphics
      `CGWindowListCreateImage` as a simpler fallback.
- [ ] Normalize coordinate systems. AppKit uses bottom-left coordinates in some
      APIs; CoreGraphics/window bounds and screenshots need careful conversion.
- [ ] Account for Retina scale so JSON rects and output pixels are predictable.

## Batch Parity

- [ ] Port the state model from HWND to current macOS window id / PID.
- [ ] Preserve `xw`/`yw` window-relative coordinates.
- [ ] Preserve `apiWidth`/`apiHeight` scaling.
- [ ] Preserve scheduled playback fields: `at_ms`, `offset_ms`, `delayMs`, and
      `speed`.
- [ ] Preserve element-based actions: wait/click/set-value/select/expand where AX
      supports equivalent behavior.
- [ ] Mark unsupported actions per step with useful errors instead of silently
      falling back.

## Build Wiring

- [ ] Add CMake sources under `if(APPLE AND FEATURE_ASSISTANT)`.
- [ ] Link required frameworks:
      `AppKit`, `ApplicationServices`, `CoreGraphics`, `Foundation`, and possibly
      `ScreenCaptureKit`.
- [ ] Revisit `FEATURE_ASSISTANT` default. It currently defaults to 0 outside
      Windows in `src/constants.hpp`.
- [ ] Ensure `tanit` and the macOS CLI target both get the same app-use backend
      if computer-use tools are enabled.

## Known Limits

- [ ] Secure text fields and protected system dialogs may not be readable or
      writable.
- [ ] Sandboxed/App Store builds are likely too constrained for full desktop
      automation.
- [ ] Some apps expose poor AX metadata; coordinate fallback remains necessary.
- [ ] User permissions are mandatory and can be revoked while the app is running.
- [ ] Exact UIA concepts like HWND, NativeWindowHandle, FrameworkId, and
      AutomationId do not map 1:1 to macOS.
