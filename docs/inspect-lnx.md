# Linux Inspect / App-Use TODOs

Status: investigation TODOs only. No implementation exists yet.

Linux needs two tracks: an X11 backend that can reach near-Windows parity, and a
Wayland backend that must be designed around compositor security limits. GNOME
Wayland should be treated as partial support unless we use portals or a shell
extension.

## Target Shape

- [ ] Add a platform-neutral assistant/app-use interface for the shared CLI and LLM
      tool surfaces to call.
- [ ] Add Linux implementation files under `src/linux/assistant/`.
- [ ] Split runtime backend selection by session type:
      `XDG_SESSION_TYPE=x11`, `XDG_SESSION_TYPE=wayland`, and fallback probes.
- [ ] Keep the existing JSON/CLI behavior as close as possible to Windows:
      `app_inspect_dump`, `app_inspect_find`, `app_screenshot`, `app_click`,
      `app_drag`, `app_open`, `app_type`, `app_hotkey`, `app_close`, and
      `app_batch`.
- [ ] Report backend/capability metadata in tool output so agents know whether
      they are using X11, Wayland portals, AT-SPI only, or unsupported fallback.

## Backend Strategy

- [ ] Implement X11 first if full desktop automation is the priority.
- [ ] Implement AT-SPI inspection independently so both X11 and Wayland can share
      the accessibility tree path.
- [ ] Implement Wayland support as portal-first:
      `xdg-desktop-portal` Screenshot, ScreenCast, and RemoteDesktop portals.
- [ ] Decide whether GNOME-specific deeper support is worth a GNOME Shell
      extension. Do not assume compositor-private APIs are stable.
- [ ] Provide clear unsupported errors on native Wayland when global input or
      arbitrary window capture is blocked.

## Permissions and Session Limits

- [ ] Detect and log session type, desktop environment, portal availability, and
      whether the app is running under Flatpak/sandboxing.
- [ ] Probe AT-SPI availability over D-Bus.
- [ ] Probe X11 extensions: XTest, XInput2, XFixes, XComposite, XRandR, and XShm
      as needed.
- [ ] Probe portal services:
      `org.freedesktop.portal.Desktop` Screenshot, ScreenCast, RemoteDesktop.
- [ ] Document consent prompts and session lifetime for portal-based capture/input.
- [ ] Fail fast with clear errors when a compositor blocks an operation.

## Window and App Resolution

- [ ] X11: enumerate top-level windows via EWMH/NETWM, not raw children only.
- [ ] X11: read window title, PID, class/name, geometry, visibility, workspace,
      minimized state, and active window.
- [ ] X11: implement ranking equivalent to `find_window_ex`: foreground, pid,
      process name, title substring, class, and area tiebreaks.
- [ ] Wayland: use portals for user-selected windows/screens where possible.
- [ ] Wayland: use AT-SPI application/window objects for accessible apps, but do
      not expect compositor window ids or absolute window management APIs.
- [ ] Define a Linux window token separate from HWND. Preserve JSON field names
      only at the compatibility boundary if needed.

## Accessibility Inspect

- [ ] Use AT-SPI2 via D-Bus/libatspi for application and accessible tree dumps.
- [ ] Map AT-SPI fields to the existing element JSON:
      role -> `controlType`, name/description -> `name`, value/text -> `value`,
      object path or toolkit id -> `automationId`/`ref` where stable enough,
      extents -> `rect`.
- [ ] Collect AT-SPI states: enabled, showing, visible, focused, focusable,
      editable, checked, selected, expanded.
- [ ] Collect supported actions via `Action` interface and invoke them where safe.
- [ ] Collect text/value via `Text`, `EditableText`, and `Value` interfaces.
- [ ] Implement tree traversal with a limit, equivalent to `dump_windows(Query)`.
- [ ] Implement element matching/scoring using role, name, description, value,
      toolkit id/object path, and nth match.
- [ ] Verify GTK, Qt, Electron/Chromium, Firefox, LibreOffice, and terminal apps.

## X11 Input Synthesis

- [ ] Use XTest for mouse movement, button down/up, clicks, wheel, drag, and key
      events.
- [ ] Use XKB/X11 keysyms for key-name mapping, including modifiers, arrows,
      function keys, escape, tab, return, delete, and space.
- [ ] Implement Unicode text insertion. Prefer clipboard paste or AT-SPI
      EditableText where keysyms are insufficient.
- [ ] Use XSetInputFocus / EWMH active-window requests carefully; window managers
      may deny focus stealing.
- [ ] Implement clipboard set/get for X selections and common clipboard managers.

## Wayland Input Synthesis

- [ ] Treat arbitrary global input as unavailable by default.
- [ ] Explore RemoteDesktop portal for pointer/keyboard control after user consent.
- [ ] Keep portal session handles in batch state where possible.
- [ ] Prefer AT-SPI actions/value setting over synthetic input for accessible
      controls.
- [ ] Do not rely on `ydotool`, `/dev/uinput`, or compositor-private tricks as the
      default product path. If added, mark as privileged/experimental.

## Picker and Highlight

- [ ] X11: implement an interactive picker using XInput2/XGrabPointer or an input
      overlay window, with escape cancel and click commit.
- [ ] X11: show a transparent override-redirect or shaped overlay window for the
      highlight border.
- [ ] X11: resolve both the X window under the cursor and the AT-SPI element at the
      point when available.
- [ ] Wayland: prefer portal user selection for screen/window capture.
- [ ] Wayland: investigate whether an app-owned transparent GTK overlay can help
      only for our own windows; do not assume it can cover arbitrary apps.
- [ ] Decide whether the committing click should be swallowed. X11 can do this;
      Wayland generally cannot without compositor/portal cooperation.

## Screenshots

- [ ] X11: implement screen-rect capture via XComposite/XShm or existing image
      stack helpers.
- [ ] X11: implement window capture from XID, including compositor/redirected
      window caveats.
- [ ] Wayland: implement screenshots through `xdg-desktop-portal` Screenshot or
      ScreenCast.
- [ ] Decide how to handle per-request portal prompts for batch screenshots.
- [ ] Normalize coordinates across X11, mixed DPI, Wayland logical coordinates,
      and portal stream sizes.
- [ ] Save JPEG output with the same quality/path behavior as Windows.

## Batch Parity

- [ ] Port the state model from HWND to Linux backend target token / PID.
- [ ] Preserve `xw`/`yw` window-relative coordinates on X11.
- [ ] Preserve `apiWidth`/`apiHeight` scaling.
- [ ] Preserve scheduled playback fields: `at_ms`, `offset_ms`, `delayMs`, and
      `speed`.
- [ ] Preserve element-based actions: wait/click/set-value/select/expand where
      AT-SPI supports equivalent behavior.
- [ ] Mark unsupported Wayland/global-input actions per step with useful errors
      instead of silently falling back.

## Build Wiring

- [ ] Add CMake sources under `if(UNIX AND NOT APPLE AND FEATURE_ASSISTANT)`.
- [ ] Add optional dependencies for X11 backend:
      X11, Xext, Xtst, Xfixes, Xi, Xrandr, Xcomposite, and possibly Xdamage/Xshm.
- [ ] Add optional dependencies for AT-SPI:
      `atspi-2`, `dbus-1`, and GLib/GObject if using libatspi directly.
- [ ] Add optional dependencies for portals:
      D-Bus client library and portal request/session handling.
- [ ] Revisit `FEATURE_ASSISTANT` default. It currently defaults to 0 outside
      Windows in `src/constants.hpp`.
- [ ] Ensure `tanit` and the Linux CLI/GUI targets get the same app-use backend
      if computer-use tools are enabled.

## Known Limits

- [ ] Native Wayland blocks arbitrary global input and window capture by design.
- [ ] Portal flows require user consent and may not be suitable for unattended
      agent loops.
- [ ] AT-SPI metadata quality varies by toolkit and app.
- [ ] Apps can disable or omit accessibility support.
- [ ] Secure fields, lock screens, compositor UI, and privileged prompts should be
      treated as unavailable.
- [ ] X11 has much better automation reach but weaker security and different
      behavior under XWayland/native Wayland mixes.
