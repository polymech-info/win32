# Computer Use

Win32 computer-use support lives behind the assistant CLI:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect ...
dist\win-x64\tanit-cli.exe assistant app-use ...
```

The implementation is split into reusable library code under `src/win/assistant/`:

- `app_inspect.*`: UI Automation window/element scanning and cropped JPEG screenshots.
- `app_use.*`: app launch, activation, mouse, keyboard, hotkeys, clipboard, coordinate scaling.
- `app_batch.*`: JSON action sequences that run in one process with per-step delays and wait/poll steps.

Use `app-inspect` before using coordinates. Native apps often move across monitors, show first-run screens, or expose useful UIA elements that are more reliable than guessed positions.

## LLM Tool Flow

Path-mode agents get these computer-use tools when `FEATURE_AGENT_COMPUTER_USE` is enabled:

- `app_inspect_dump`: observe an app and cache a small session tree.
- `app_inspect_find`: query the cached tree for targetable elements.
- `app_click`: click coordinates, or click the center of a found element.
- `app_screenshot`: capture a window, element, or rectangle.

For LLMs, prefer the observe/query/action loop instead of asking for a full JSON dump:

```json
{
  "target": { "process": "wordpad.exe", "title": "Document" },
  "session_id": "doc",
  "view": "actions"
}
```

Then query that cached tree:

```json
{
  "session_id": "doc",
  "query": { "view": "editable", "name": "Rich Text" },
  "nth": 0
}
```

And click by selector without re-reading the whole app:

```json
{
  "session_id": "doc",
  "query": { "view": "editable", "name": "Rich Text" },
  "nth": 0
}
```

The default LLM view is intentionally bounded: `actions` maps to buttons, menus, and editable controls with a default inspect limit of 160 and Markdown text values capped at 1200 characters. Use `view: "cells"` for spreadsheet-like apps. Use `view: "all"` or `format: "json"` only when the task needs exhaustive details, because those outputs can be large.

For multi-step UI work, agents should batch the actual input sequence. A good agent pattern is:

1. `app_inspect_dump` to observe the target window and get screen coordinates.
2. `app_screenshot` when the visual state matters or UIA text is ambiguous.
3. `write_file` a batch JSON file using `steps[]` and `action`.
4. `run` `dist\win-x64\tanit-cli.exe assistant app-use batch --file <file> --json`.
5. `app_inspect_dump` and/or `app_screenshot` again to verify the result.

Do not invent alternate batch schemas. The batch runner expects either an array of steps or an object with `steps`; each step uses `action`. It does not understand `{ "version": 1, "actions": [...] }` or per-step `type`.

When calling the CLI from the `run` tool on Windows, prefer a relative executable path from the current package folder:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use batch --file calc-agent.batch.json --json --default-delay-ms 50
```

Avoid quoted absolute executable strings followed by arguments in PowerShell, e.g. `"C:\...\tanit-cli.exe" assistant ...`; PowerShell requires `&` for that form and agents often forget it.

## Inspect Apps

Dump the foreground window:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --foreground --json
```

Dump all windows with a title containing `LibreOffice`:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --title LibreOffice --json
```

Limit output while exploring:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --title LibreOffice --limit 40 --json
```

Inspect one process:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --pid 24436 --json
```

Inspect by process name:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --process notepad.exe --json
```

Inspect as a compact Markdown tree for humans and LLMs:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --process notepad.exe --title "2 - Notepad" --md
```

Filter the Markdown tree to specific control kinds:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --process soffice.bin --md --controls buttons,menus,editable
```

By default, `--md` shows compact actionable/editable elements: invoke-capable controls, menus, text fields, and editable inputs. `--controls` is case-insensitive and supports exact UIA control types plus shorthand groups like `buttons`, `menus`, `inputs`, `textfields`, `textboxes`, `editable`, `text`, and `all`.

Native child controls such as WordPad's `RICHEDIT50W` editor are inspected through their child HWND UIA provider, so rich text content can appear as a `Document` row with a compact `value`.

Markdown text/value fields are capped by `--text-max-chars` (default `4096`) to keep dumps bounded. Lower it for compact prompts, or raise it when inspecting large editors.

For virtualized grids such as LibreOffice Calc, `app-inspect` can probe visible document/table regions with UIA `ElementFromPoint` and merge discovered cells into the dump. This is slower, so it is auto-enabled only for spreadsheet-looking `--md` targets or when `--controls cells` is used; you can force it with `--probe-cells`. Use `--controls cells,editable` to focus on visible spreadsheet cells and edit fields; empty cells are hidden in Markdown unless you use `--controls all`. Cell content is reported from `ValuePattern` when exposed.

The JSON includes:

- Window identity: `pid`, `hwnd`, `process`, `title`, `rect`.
- Element identity: `index`, `name`, `controlType`, `automationId`, `className`, `frameworkId`.
- Coordinates: `rect` and `center` in screen coordinates.
- State: `enabled`, `offscreen`, `focused`, `keyboardFocusable`, `nativeHwnd`.
- UIA pattern hints: `patterns.invoke`, `patterns.value`, `patterns.text`, `patterns.selectionItem`, `patterns.expandCollapse`, `patterns.scroll`.
- Text/value fields when UIA exposes `ValuePattern` or `TextPattern`.

Example element:

```json
{
  "index": 80,
  "name": "Formula",
  "controlType": "Button",
  "enabled": true,
  "focused": false,
  "keyboardFocusable": true,
  "patterns": {
    "invoke": true,
    "value": false,
    "text": false,
    "selectionItem": false,
    "expandCollapse": false,
    "scroll": false
  },
  "rect": { "x": 2199, "y": 368, "w": 25, "h": 27 },
  "center": { "x": 2211, "y": 381 }
}
```

## Screenshots

Save a screen crop as JPEG:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect screenshot --rect 0,0,640,360 --output .\scratch\screen.jpg --json
```

Save a crop for an element from a matching dump:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect screenshot --title LibreOffice --element 80 --output .\scratch\formula-button.jpg --json
```

Save a matching window screenshot:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect screenshot --process notepad.exe --title "2 - Notepad" --output .\scratch\notepad-window.jpg --json
```

Window screenshots use a non-activating HWND capture path and report `"capture": "window"` in JSON. Explicit `--rect` captures desktop pixels and reports `"capture": "screen"`, so overlapping windows are included.

Save a crop for the foreground window's element:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect screenshot --foreground --element 12 --output .\scratch\element.jpg --json
```

Screenshots are currently JPEG. Use `--quality 1..100` to tune size/quality.

## One-Shot App Actions

Open an app:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use open-app --exe notepad.exe --args C:\Temp\note.txt --x 40 --y 40 --width 720 --height 420 --wait-ms 3000 --json
```

Move the mouse:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use mouse-move --x 300 --y 240 --json
```

Click a point:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use click --x 300 --y 240 --button left --count 1 --json
```

Double-click:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use click --x 300 --y 240 --count 2 --json
```

Right-click:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use click --x 300 --y 240 --button right --json
```

Type text into the focused control:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use type --text "hello from pm-image" --json
```

Send a hotkey:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use hotkey --keys ctrl+s --json
```

Report cursor and screen dimensions:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use cursor-position --json
```

Use scaled LLM/API coordinates:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use cursor-position --api-width 1024 --api-height 768 --json
dist\win-x64\tanit-cli.exe assistant app-use click --x 512 --y 384 --api-width 1024 --api-height 768 --json
```

## Batch Actions

Batching avoids process startup cost between steps and lets one sequence keep a current target window. It also supports optional steps and wait/poll behavior.

Run a batch:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use batch --file .\scratch\steps.json --json
```

Continue after failed optional/non-critical steps:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use batch --file .\scratch\steps.json --continue-on-error --json
```

Set the default post-step delay:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use batch --file .\scratch\steps.json --default-delay-ms 25 --json
```

Batch JSON may be an array or an object with `steps`:

```json
{
  "steps": [
    {
      "action": "open-app",
      "exe": "notepad.exe",
      "args": "C:\\Temp\\note.txt",
      "waitMs": 3000,
      "x": 40,
      "y": 40,
      "width": 720,
      "height": 420,
      "delayMs": 150
    },
    { "action": "type", "text": "hello\n" },
    { "action": "hotkey", "keys": "ctrl+s", "delayMs": 500 },
    { "action": "hotkey", "keys": "alt+f4" }
  ]
}
```

Important schema rules:

- Use `steps`, not `actions`.
- Use `action`, not `type`.
- `optional: true` is supported on any step; with `--continue-on-error`, optional failed steps are reported but do not stop the batch.
- `delayMs` waits after that step. `--default-delay-ms` applies when a step has no explicit `delayMs`.
- `wait-element` and `click-element` accept `timeoutMs` and `intervalMs`.

Supported batch actions:

- `open-app`
- `activate`
- `mouse-move`
- `click`
- `drag`
- `scroll`
- `mouse-down`
- `mouse-up`
- `type`
- `hotkey`
- `clipboard-set`
- `clipboard-get`
- `wait-element`
- `click-element`
- `screenshot`
- `wait`

## Batch Examples

Open Notepad, type, save, close:

```json
{
  "steps": [
    {
      "action": "open-app",
      "exe": "notepad.exe",
      "args": "C:\\Temp\\computer-use-note.txt",
      "waitMs": 3000,
      "x": 40,
      "y": 40,
      "width": 720,
      "height": 420,
      "delayMs": 150
    },
    { "action": "type", "text": "computer-use batch works" },
    { "action": "hotkey", "keys": "ctrl+s", "delayMs": 500 },
    { "action": "hotkey", "keys": "alt+f4" }
  ]
}
```

Dismiss a possible modal, but do not fail if it is absent:

```json
{
  "steps": [
    {
      "action": "click-element",
      "name": "OK",
      "controlType": "Button",
      "optional": true,
      "timeoutMs": 1000,
      "delayMs": 200
    },
    {
      "action": "click-element",
      "name": "Skip",
      "controlType": "Button",
      "optional": true,
      "timeoutMs": 1000,
      "delayMs": 200
    }
  ]
}
```

Wait for a button in the current target window:

```json
{
  "steps": [
    {
      "action": "wait-element",
      "name": "Save",
      "controlType": "Button",
      "timeoutMs": 5000,
      "intervalMs": 100
    }
  ]
}
```

Clipboard-driven paste and copy:

```json
{
  "steps": [
    { "action": "clipboard-set", "text": "1\n2\n3\n4\n5\n=SUM(A1:A5)" },
    { "action": "hotkey", "keys": "ctrl+v", "delayMs": 500 },
    { "action": "hotkey", "keys": "ctrl+c", "delayMs": 250 },
    { "action": "clipboard-get" }
  ]
}
```

Calculator resize, type, and verify workflow:

```json
{
  "steps": [
    {
      "action": "open-app",
      "exe": "calc.exe",
      "waitMs": 3000,
      "x": 120,
      "y": 96,
      "width": 927,
      "height": 941,
      "delayMs": 300
    },
    {
      "action": "drag",
      "x": 1045,
      "y": 1035,
      "dx": 200,
      "dy": 200,
      "steps": 24,
      "durationMs": 450,
      "delayMs": 300
    },
    { "action": "type", "text": "10*10", "delayMs": 100 },
    { "action": "hotkey", "keys": "enter", "delayMs": 500 }
  ]
}
```

For agent-driven calculator tasks, ask the agent to capture `calculator_current.jpg` before the batch and `calculator_result.jpg` after the batch via `app_screenshot`. Screenshots give a visual audit trail when UIA results are incomplete or the model falls back to button clicks.

## Targeting And Safety

The batch runner keeps a current target HWND:

- `open-app` sets the current target to the launched app's first visible window.
- `activate` sets the current target to the matched window.
- `wait-element` / `click-element` set the current target to the matched element's window.

By default, `wait-element`, `click-element`, and screenshots by `element` search within the current target HWND when no `foreground`, `pid`, `hwnd`, or `title` filter is supplied.

Pointer actions (`mouse-move`, `click`, `scroll`, `type`, `hotkey`) activate the current target before acting. `mouse-move` and `click` also fail if the target point is outside the current app window. This prevents a flow from closing or clicking another app if the target app disappears.

### Verified foreground activation

`activate_window` (the primitive every batch step and every LLM tool that takes a target eventually calls) is now defensively hardened. It is NOT a thin wrapper around `SetForegroundWindow` -- that call silently no-ops in Windows when another process owns the focus (anti focus-stealing protection), which is exactly how the agent ended up "drawing on the wrong app" in earlier sessions. The current implementation:

1. Resolves the target's root HWND, restores it from minimised, raises Z-order.
2. Attaches our input queue to the current foreground thread's queue via `AttachThreadInput`, then calls `SetForegroundWindow` / `SetActiveWindow` / `SetFocus`. This bypasses the focus-stealing block.
3. Detaches the input queues.
4. Polls `GetForegroundWindow` after a 40 ms settle, retries up to 3 times with growing waits (40 ms, 100 ms, 160 ms) if the OS hasn't committed yet.
5. Returns a hard error (`"could not bring target window to foreground after 3 attempts"`) if all three retries failed, so the calling tool returns `ok:false` instead of silently sending input to the wrong window.

Tools that take a target selector (`title` / `pid` / `hwnd` / `process` / `foreground`) and now run through this hardened activator BEFORE synthesising input or capturing pixels:

- `app_click`, `app_drag`, `app_type`, `app_hotkey` (only when a selector is supplied; otherwise the current foreground is accepted, matching the prior contract).
- `app_screenshot` when the target is resolved by window query. Default is `activate: true`; pass `"activate": false` to opt out of foregrounding (e.g. non-intrusive background polling). Full-screen / explicit-`rect` captures don't activate anything because there's no target window.
- `app_batch` `activate` steps, every action that targets `current_hwnd`, and `open-app` (which also sets `current_hwnd`).

CLI parity: `assistant app-inspect screenshot --title <app>` activates the matched window before capture by default. Pass `--no-activate` to skip (useful for capturing a background window non-intrusively).

The orchestrator `play-agent-loop.mjs` benefits automatically: every tick screenshot foregrounds Paint, so the next agent move lands on the surface the agent just saw.

Use window-relative coordinates only after you have inspected the target window:

```json
{ "action": "click", "x": 50, "y": 155, "windowRelative": true }
```

Prefer this order:

1. `app-inspect dump --title <app> --json`
2. Identify `hwnd`, `rect`, useful elements, and pattern flags.
3. Use `click-element` / `wait-element` by `name` and `controlType` where possible.
4. Use coordinates only as a fallback, and prefer `windowRelative`.
5. Use `screenshot` crops to debug ambiguous regions.

## LibreOffice Notes

LibreOffice exposes a `SALFRAME` main window and many UIA menu/toolbar elements. Menus and toolbars are visible to `app-inspect`, but spreadsheet cells are not always reliable UIA data items. For Calc, prefer:

- launch with `--calc --norestore --nolockcheck --nofirststartwizard`;
- paste tab/newline-delimited data through the clipboard;
- use `app-inspect` to verify the main window and formula toolbar coordinates;
- avoid desktop-wide optional blocker clicks, especially generic `Close`, because titlebar buttons also match.

Example inspection:

```powershell
dist\win-x64\tanit-cli.exe assistant app-inspect dump --title LibreOffice --limit 80 --json
```

Example launch:

```powershell
dist\win-x64\tanit-cli.exe assistant app-use open-app --exe "C:\Program Files\LibreOffice\program\soffice.exe" --args "--calc --norestore --nolockcheck --nofirststartwizard" --wait-ms 9000 --json
```

The dedicated LibreOffice test script is available as:

```powershell
npm run test:app-use:libreoffice
```

It is intentionally not part of the default `test:app-use` suite while Calc targeting is being refined.

## Tests

Stable suite:

```powershell
npm run test:app-use
```

Focused suites:

```powershell
npm run test:app-use:inspect
npm run test:app-use:notepad
npm run test:app-use:chrome
npm run test:app-use:calculator
npm run test:app-use:terminal
npm run test:app-use:agent:notepad
npm run test:app-use:agent:chrome
npm run test:app-use:agent:calculator
npm run test:app-use:libreoffice
```

Build:

```powershell
npm run build:cpp
```

## TODOs / Open Improvements

Captured after the first round of live LLM agent runs (Notepad, Chrome, Calculator). Ordered by expected impact, not by effort. Re-evaluate before adding more LLM-facing tools.

### 1. Fix the `run` tool, not the agent

The Calculator agent run failed its batch step purely because `run` defaults to PowerShell and the model produced `"C:\...\tanit-cli.exe" assistant ...`, which PowerShell requires `&` for. The model is unlikely to ever get this right reliably.

- [ ] Add an argv mode to the `run` tool so agents can pass `command: ["tanit-cli.exe", "assistant", "app-use", "batch", "--file", "x.json"]` and skip shell quoting entirely.
- [ ] Alternative: change the default shell on Windows to `cmd.exe`, where `"path\exe" args` works without a leading operator.
- [ ] Document the chosen contract in `kbot` / agent docs so prompts can stop teaching shell rules.

This single fix also removes a class of Notepad / generic-CLI failures, not just computer-use ones.

### 2. Cut token bloat before adding any new tools

The Calculator agent run was roughly 5 rounds × ~30k input tokens, dominated by the system prompt plus 25 tool schemas resent every round. Each new tool schema costs another ~1-2k tokens per round.

- [ ] Add a per-task tool allowlist / preset, e.g. `--tools computer-use` that ships only `app_inspect_dump`, `app_inspect_find`, `app_click`, `app_screenshot`, `run`, `write_file`, `read_file`, `image_understand`.
- [ ] Trim or lazy-load injected skill prompts; today multiple `SKILL.md` files are concatenated into the system prompt regardless of task.
- [ ] Investigate stateful replay for OpenRouter (`/responses` previous_response_id flow) so tool schemas are not resent every round. If unsupported, fall back to schema diffing.
- [ ] Re-measure cost-per-task after the above and only then consider adding more LLM tools.

### 3. ~~Reconsider `app_batch` as an LLM tool~~ DONE (Paint tic-tac-toe agent)

`app_batch` is now a first-class LLM tool. Plus a sibling `app_drag` for single strokes. The Paint tic-tac-toe agent run dropped from **7 rounds / 84.8k tokens / $0.088** (write_file + 2× run + shell-out batch JSON) to **5 rounds / 68.6k tokens / $0.073** (`app_open` → `app_inspect_dump` → `app_screenshot` → ONE `app_batch` with 4 drags → `app_screenshot`). −29% rounds, −19% tokens, −17% cost, and `run`/`write_file` are no longer needed for desktop automation.

- [x] Add `app_drag` (single stroke with target activation).
- [x] Add `app_batch` (run an array of UI actions in ONE round-trip).
- [x] `app_batch` returns a per-step `steps[]` report and a `first_failure` summary, so the model is not blind when one step fails.
- [ ] Add an `image_understand` chain on the final `app_screenshot` for tasks where the model also needs to "see" the result (optional, only when the prompt asks for it).

### 4. ~~Be lenient in the batch schema~~ DONE (`app_batch`)

- [x] `app_batch` accepts `actions:` as an alias for `steps:`.
- [x] `app_batch` accepts per-step `type:` as an alias for `action:`.
- [x] `app_batch` returns a structured `first_failure` step instead of bailing out without context.
- [ ] Backport these aliases to the CLI `app-use batch --file` path too (currently only `app_batch` is lenient).
- [ ] On full schema validation failure, return a short JSON example of the accepted shape so the agent can self-correct in one round.

### 5. Make `app_screenshot` more useful per call

The Calculator agent paid for an extra `image_understand` round because `app_screenshot` is image-only.

- [ ] Add an optional `read: "ocr" | "caption"` mode to `app_screenshot` that returns text alongside the image path, avoiding a second LLM round.
- [ ] Document `app_screenshot` before/after as a required step in agent prompts for any task that involves coordinates, drag, or numeric UI state.

### 6. Justify or drop `app_inspect_find`

In the Calculator trace, the agent parsed the Markdown dump and never called `app_inspect_find`. Either the dump is good enough, or `find` is not attractive enough.

- [ ] Measure: in the next two agent runs, does `find` ever beat re-parsing the dump?
- [ ] If not, either remove it or rewrite its description to emphasize "cheaper than re-dumping" with a concrete selector example.

### 7. Keep both test tracks

Direct JS tests (`test:app-use:notepad`, `:chrome`, `:calculator`, `:terminal`) are deterministic, fast, and free. Agent tests (`test:app-use:agent:*`) prove the LLM contract.

- [ ] Do not migrate JS tests to agent tests. Keep both.
- [ ] Add a smoke `test:app-use:agent` aggregator that runs only the cheapest agent suite by default in CI.
- [ ] Gate expensive agent suites behind an env flag, since each run costs real credits.

### 8. Safer agent cleanup

We already removed `alt+f4` from the agent suites because it could close the dev window. The fix was reactive.

- [ ] Cleanup should only target windows by `pid` recorded from `open-app`, never by foreground.
- [ ] Add a `assistant app-use close-pid --pid <pid>` helper so tests and agents have a safe close primitive.

### 9. Make session caching visible to the agent

`app_inspect_dump` already caches a session tree, but the agent in the Calculator trace did not re-use it for `app_click` decisions and re-dumped instead.

- [ ] When `app_inspect_dump` returns, include the `session_id`, element count, and a short hint like `"reuse with app_click { session_id, query }"`.
- [ ] Add an `app_session_list` / `app_session_clear` debug tool if sessions start drifting across long agent runs.

### 10. Stable element selectors

Coordinates drift with DPI, monitor, and resize. `name` + `controlType` are not always unique.

- [ ] Promote `automationId` + `className` + `nth` as the canonical selector in `query`, with `name` as a fallback.
- [ ] Return a stable `ref` from `app_inspect_dump` per element, and accept that `ref` in `app_click` / `app_screenshot` to avoid re-querying.

### 12. ~~Play against the agent interactively~~ DONE

You can now actually play tic-tac-toe against the agent in MS Paint without writing any code:

```
npm run play:paint:tictactoe                # 4x4 (default), you = X, agent = O
npm run play:paint:tictactoe -- --3x3       # 3x3 variant
npm run play:paint:tictactoe -- --moves "10,11,7,6"   # script your moves (no stdin)
```

The script opens Paint, draws the grid, and then loops: it prompts you for a cell number, draws your X via `app-use batch`, calls the LLM agent with `--session-id <stable>` (so the agent's `memory_state` and event log persist across turns to `%APPDATA%\PolyMech\pm-image\sessions\<id>.json`), draws the O the agent picked, and waits for your next move. Type `b` to print the current board, `shot` to save a screenshot, `q` to quit. Final board JPEG lands at `tests/play-final-<session>.jpg`.

The matching automated test is `npm run test:app-use:agent:paint:tictactoe:human-vs-agent` — same orchestrator, but a tactical 4-in-a-row heuristic (`pickXMove4x4`) plays your turns. A run takes ~75 s and ~$0.20 of API credits.

### 13. ~~Scheduler from CLI~~ DONE

The agent scheduler (`schedule_every` / `schedule_in` / `schedule_at`) used to be a no-op when called from `tanit-cli`: the scheduler thread is only started by `ChatWebPanel`, so CLI invocations queued tasks that never fired. Three new flags fix that:

```
tanit-cli llm agent --prompt "..." \
    --scheduler                       # start the in-process scheduler after the first turn
    --scheduler-timeout 60            # auto-exit after N seconds (0 = run until Ctrl+C / idle)
    --scheduler-exit-when-idle        # exit cleanly when every task is disabled
```

The first turn can register tasks via `schedule_in` / `schedule_every`; once it replies, the process keeps running, the scheduler ticks each due task as its own `Turn` (memory + event log on `TaskStore` are shared in-process across ticks), and the process exits on Ctrl+C, timeout, or idle. SIGINT is wired through `abort_active_speak` / `abort_active_run` so an in-flight `speak` / `run` tool call doesn't hang shutdown.

Verified end-to-end:
- `schedule_in` one-shot: agent schedules a 3-second task, scheduler fires it (`tick done ok=yes`), task auto-disables, idle exit. Total elapsed 7.5 s.
- `schedule_every` with `max_runs=2`: scheduler fires twice 5 s apart, task auto-disables on the second tick, idle exit. Total elapsed 16.7 s.

- [x] `--scheduler`, `--scheduler-timeout`, `--scheduler-exit-when-idle` on `llm agent`
- [x] SIGINT handler that stops the scheduler and aborts any in-progress `speak`/`run` (mirrors `ChatWebPanel`'s shutdown path)
- [x] Provider settings captured per-CLI-invocation -- the factory returns a snapshot so settings.json changes mid-loop are NOT hot-reloaded (CLI sessions are short-lived; restart to pick up changes)
- [ ] Persist scheduler tasks to disk so a task created in process A can be ticked by process B (today tasks die with the process; this is the same TODO as item 11's last bullet point about scheduler v2)

### 11. ~~Session memory persistence~~ DONE

`memory_read` / `memory_write` / `memory_append_event` used to be in-memory only — each CLI invocation started with an empty store, which silently broke any cross-process A2A pattern. Now sessions are persisted to disk and survive `tanit-cli` restarts.

- [x] Per-session JSON files at `%APPDATA%\PolyMech\pm-image\sessions\<id>.json` (override with `set_session_store_dir()` or `PM_IMAGE_SESSION_DIR` env var).
- [x] Atomic `.tmp + rename` writes on every `memory_write` / `memory_append_event` / `session_append_turn`.
- [x] CLI: pass `--session-id <id>` on `llm agent` (single-turn or multi-turn) to opt into a stable, on-disk session. The chat-panel session also persists automatically (auto-generated id with millisecond timestamp to avoid concurrent-launch collisions).
- [x] CWD `memory.json` / `scheduler.json` debug dumps are off by default; re-enable with `PM_IMAGE_DEBUG_DUMPS=1` if you need them.
- [x] Sanity test: `npm run test:app-use:agent:memory:persist` (two CLI invocations, second one reads what the first wrote — 7 s, ~$0.001).
- [ ] Add a `llm memory list` / `show <id>` / `clear <id>` CLI for inspection.
- [ ] TTL / size cap for the sessions directory (today it grows unboundedly).
- [ ] Scheduler tasks (`task_store_*` non-session path) remain in-process only — promote to disk if/when we ship the scheduler v2.

