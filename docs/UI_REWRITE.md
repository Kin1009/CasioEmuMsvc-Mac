# UI rewrite

Status: design locked, implementation phased
Related: `PREVIEW.md`, `PLUGIN_SPEC.md`

This document describes the preview shell in enough detail to implement it
without re-litigating the layout. It assumes macOS only and a frameless main
window with native traffic lights.

---

## 1. Goals

- Replace the "debugger with a calculator dock tab" model with a proper
  application shell.
- Make session switching the primary navigation action.
- Keep every existing debugger tool reachable, but stop making every tool a
  peer of the calculator.
- Move appearance, layout, and workspace state into a documented JSON settings
  file that plugins can extend.
- Make the shell replaceable by a trusted `ui_revamp` plugin without forking
  the app.

Non-goals for the first release:

- Tearing a session out into a second OS window.
- Porting the new shell to Windows, Android, or the web.
- Rewriting the calculator face, disassembler, or memory viewers.
- A plugin store, ratings, or automatic remote updates.

---

## 2. Window and chrome

### 2.1 Native traffic lights over a frameless window

Implementation:

- Keep `SDL_WINDOW_RESIZABLE`.
- Use the Cocoa bridge in `CasioEmuMsvc/src/Ext/` to set
  `NSWindowStyleMaskFullSizeContentView`, `titlebarAppearsTransparent = YES`,
  `titleVisibility = NSWindowTitleHidden`, and keep the standard window
  buttons visible.
- Do not draw custom close/minimize/maximize glyphs.
- The top toolbar starts after the native button area (about 78 px) and must
  not paint under the buttons.
- Double-clicking the toolbar performs the system zoom action.
- Fullscreen uses the native green button; the toolbar hides in fullscreen and
  returns on hover at the top edge.
- Window position, size, and fullscreen state are saved in `settings.json`.

### 2.2 Native menu

The application menu is the standard macOS menu bar. Required items:

| Menu | Items |
| --- | --- |
| App | About, Settings..., Services, Hide, Quit |
| File | New Session, Open Model..., Open Folder..., Open Recent, Close Session, Save, Save As, Install Plugin... |
| Edit | Undo, Redo, Cut, Copy, Paste, Select All, Find |
| View | Show/Hide Sidebar, Show/Hide Panel, Show/Hide Tools, Reset Layout, Reset Shell, Enter Full Screen |
| Session | Pause/Resume, Reset, Snapshot, Build, Build & Run, Stop |
| Window | Minimize, Zoom, Bring All to Front |
| Help | Documentation, Plugin Spec, Report an Issue |

Menu items are enabled/disabled from the active session and focus state. The
native menu is the only place for destructive or rarely used commands. The
in-window toolbar contains build/run, pause/resume, zoom, plugin manager, and
layout.

---

## 3. Shell regions

```
+--------------------------------------------------------------------------+
| [traffic lights] | session tabs ........................ | toolbar        |
+------------------+---------------------------------------+----------------+
| sidebar          | session content                       | tool dock      |
| (resizable)      | (calculator / editor / welcome)       | (resizable)    |
+------------------+---------------------------------------+----------------+
| bottom panel: Output | Problems | Terminal     | status bar             |
+--------------------------------------------------------------------------+
```

Each region is optional except the session content and status bar.

### 3.1 Session tabs

- Owned by the shell, not by ImGui's dock builder.
- Drawn in a custom horizontal strip at the top of the content area.
- Reorderable by drag. The order is persisted.
- Close button on hover; middle-click closes.
- `+` opens the welcome page in a new session slot.
- Tab label: model name. Duplicate models get ` (2)`, ` (3)`, and so on.
- State badge: paused, running, building, error.
- Keyboard: Cmd+1..9 selects a tab; Cmd+Shift+] / [ cycles; Cmd+W closes.
- Closing the active tab selects the tab to its right, then left, then welcome.
- Closing the last tab shows the welcome page; it does not quit.

### 3.2 Session content

The content area is a stack with one visible page:

- **Welcome** - the empty state (section 4).
- **Session** - calculator view, editor documents, or a split of both.
- **Tool page** - when a global tool is maximized into the content area.

The calculator view is always available for a live session. Source files open
as documents in a per-session document set. The document tabs are separate from
session tabs (like editor tabs inside a VS Code window).

### 3.3 Sidebar

A VS Code-style activity rail is not used in v1; the sidebar has a single
primary view:

- **Explorer** - workspace tree, open editors, recently opened files.
- Header actions: Open Folder, Refresh, Collapse All, Reveal in Finder.
- Double-click a `.wz`/`.cwiz` file: open in the active session's editor if the
  target matches, otherwise ask or open read-only.
- Double-click a `.plugin` file: install through the plugin manager.
- Drag a folder onto the window: open as workspace.
- The sidebar can be hidden with Cmd+B and resized by dragging its separator.

Future views (Search, Plugins, Debug) can be added as icons later, but v1 keeps
one view to avoid a half-built activity bar.

### 3.4 Tool dock

- A normal ImGui dock space on the right.
- Contains all existing debugger windows, grouped by default:
  - **State**: Variables, Hardware, Labels, Watch, Funcs, Code.
  - **Memory**: RAM, ROM, All, PRAM, Flash, Bitmap.
  - **Analysis**: Call Analysis, Snapshot, Breakpoints.
  - **Build**: Wiz Compiler, Rop Compiler, Injection.
  - **App**: Theme, Plugin Manager, QR, Log.
- The dock has its own tab bar. It is visually distinct from session tabs:
  session tabs are larger and use the accent underline; tool tabs are smaller
  and use the muted foreground.
- Hiding the dock changes the content width, not the session.
- A tool can be maximized into the content area; Escape or the layout menu
  returns it.

### 3.5 Bottom panel

A custom panel with three tabs. Height is draggable and persisted.

| Tab | Source | Behavior |
| --- | --- | --- |
| Output | build/run/plugin logs | Auto-scroll, clear, copy, filter by source |
| Problems | compiler diagnostics | `file:line:column` jumps; severity filters; counts in the tab label |
| Terminal | PTY shell | Full xterm, job control, resize propagation |

Rules:

- Builds write to Output and Problems. A failed build focuses Problems unless
  the user has pinned a different tab.
- The status bar sits below the panel and is always visible.
- Cmd+J toggles the panel; Cmd+` focuses Terminal.
- Closing the panel keeps its state; reopening restores the active tab.

---

## 4. Welcome page

The welcome page is a normal page in the session content area, not a second
window.

Contents:

- **Start** - New Calculator, Open Model, Open Folder, Install Plugin.
- **Recent sessions** - model, ROM, last opened time; click to reopen. Missing
  files show a Repoint action.
- **Recent folders** - workspace paths.
- **Learn** - Open documentation, Plugin spec, Example plugins, Keyboard
  shortcuts.
- **Status** - app version, host API version, plugin count, update status.

Empty states are explicit. If restoring sessions succeeds, the welcome page is
not shown; it appears when the last tab closes or when restore has nothing to
open.

---

## 5. Settings

File: `~/Library/Application Support/CasioEmuMsvc/settings.json`.

Portable override: if `portable.flag` exists next to the executable, use
`<app>/settings/settings.json`.

Top-level schema:

```json
{
  "schema": 1,
  "appearance": {
    "theme": "vscode-dark",
    "pluginTheme": null,
    "uiScale": 1.0
  },
  "shell": {
    "sidebar": { "visible": true, "width": 240 },
    "bottom": { "visible": true, "height": 160, "active": "output" },
    "tools": { "visible": true, "width": 360 },
    "window": { "x": 0, "y": 0, "width": 1600, "height": 1000, "fullscreen": false }
  },
  "workspace": {
    "folder": null,
    "recentFolders": [],
    "recentFiles": []
  },
  "sessions": [
    {
      "id": "session-1",
      "model": "fx-580VN X",
      "rom": "models/fx-580VN X/rom.bin",
      "title": "fx-580VN X",
      "active": true,
      "documents": []
    }
  ],
  "editor": {
    "showLineNumbers": true,
    "wordWrap": false,
    "fontSize": 13,
    "tabSize": 4
  },
  "emulator": {
    "backgroundThrottle": 0.25,
    "lowPerformanceMode": false
  },
  "integrations": {
    "discordRPC": true,
    "updateChecker": true
  },
  "plugins": {
    "enabled": [],
    "disabled": [],
    "trustPath": "plugin-trust.json"
  },
  "language": "en_US"
}
```

Migration:

- Read `theme.bin` once and map `isDarkMode`, `scale`, `language`,
  `injectionFilePath`, window geometry, `lowPerformanceMode`,
  `enableDiscordRPC`, and `enableUpdateChecker`.
- Write `settings.json` atomically (temp file + rename).
- Never fail startup because migration failed; log and use defaults.
- Keep `theme.bin` for one release.

---

## 6. Visual system

### 6.1 Source of truth

The built-in themes are JSON files under `resources/themes/`:

- `vscode-dark.json`
- `vscode-light.json`

They use the `theme` plugin schema from `PLUGIN_SPEC.md`. Built-ins and plugin
themes share one code path, so theme bugs are caught by the same tests.

VS Code is MIT-licensed. The palette is inspired by its open-source default
themes; no source code, icons, or trademarked assets are copied.

### 6.2 Style rules

- 8 px spacing grid; 4 px inside dense rows.
- 6 px window rounding, 4 px frame rounding, 1 px border.
- Flat surfaces; no bevels or gradients except a subtle 1 px top highlight.
- Accent is used for selection, focus, and progress only.
- Error/warning/success colors are reserved for diagnostics and state badges.
- Table rows use a hover background and a selection background; no zebra
  striping.
- Scrollbars are thin (10 px), rounded, and fade when idle if the backend
  supports it.
- Icons are monochrome and inherit text color.
- Focus rings are 2 px accent outlines with a 1 px inner gap.

### 6.3 Typography

- UI: SF Pro Text / system font, 13 px default.
- Code and terminal: SF Mono / Menlo fallback, 13 px default.
- Font scale setting multiplies both; minimum 0.8, maximum 2.0.
- The UI must remain usable at 0.8 scale on a 13-inch display.

---

## 7. Terminal

The Terminal tab is a real PTY, not a command runner.

Implementation plan:

- Allocate a PTY with `forkpty`/`openpty`.
- Spawn the user's login shell (`$SHELL`, fallback `/bin/zsh`) with the
  workspace root as cwd and the app environment.
- Propagate window size with `TIOCSWINSZ` on resize.
- Parse xterm sequences: cursor movement, erase, insert/delete, SGR colors
  (16/256/truecolor), scroll regions, alternate screen, bracketed paste, mouse
  reporting, title changes.
- Maintain a scrollback buffer (default 10,000 lines).
- Support job control signals: Ctrl+C, Ctrl+Z, Ctrl+D, Ctrl+L.
- Copy/paste with Cmd+C/Cmd+V; selection is terminal-aware.
- Links: Cmd+click opens URLs.
- Search: Cmd+F opens a scrollback search.
- Close the tab: send SIGHUP, wait briefly, then SIGKILL.

The terminal is isolated in `CasioEmuMsvc/src/Terminal/` so the parser can be
tested without starting the app.

---

## 8. Keyboard and focus

- Global shortcuts are handled by the native menu or a shell-level handler
  before ImGui consumes keys.
- Calculator input is captured only when the calculator view has focus and the
  active session is live.
- Text inputs and the terminal swallow printable keys.
- SDL text input is suppressed for the calculator target, as today.
- Focus restoration: switching sessions restores the last focused widget in
  that session; switching tools does not steal calculator input.
- Required shortcuts:

| Shortcut | Action |
| --- | --- |
| Cmd+N | New session |
| Cmd+O | Open model |
| Cmd+Shift+O | Open folder |
| Cmd+S | Save document |
| Cmd+W | Close active session or document |
| Cmd+1..9 | Select session tab |
| Cmd+Shift+[ / ] | Previous/next session |
| Cmd+B | Toggle sidebar |
| Cmd+J | Toggle bottom panel |
| Cmd+` | Focus terminal |
| Cmd+, | Settings |
| Cmd+Shift+P | Command palette (later phase) |
| Cmd+Q | Quit |

---

## 9. Accessibility and localization

- Every new string goes through the existing `_lc` localization mechanism.
- New locale keys are added to `locales/en_US.lc` and the other shipped locales
  as translations arrive; missing keys fall back to English.
- Controls have readable labels and tooltips; icon-only buttons have tooltips.
- Color is never the only indicator of state; pair it with text or an icon.
- UI scale, keyboard navigation, and screen-reader labels are part of the
  Phase 5 checklist.

---

## 10. Non-macOS code removal

The preview targets macOS only. Removal is staged so feature work does not
collide with deletions:

1. Feature commits add macOS implementations behind `#ifdef __APPLE__` where a
   shared file is unavoidable.
2. Phase 5 removes Windows, Android, and web UI/plugin branches, along with
   their build options and docs.
3. The emulator core remains portable; only the shell, plugin host, and
   platform glue are macOS-specific.
4. `McpPlugin` stays because it is the debugger automation surface, even
   though its Windows build instructions are no longer supported.

---

## 11. Implementation order

1. Shell skeleton: regions, session tab strip, welcome page, status bar.
2. Settings JSON and theme token loader.
3. Sidebar + bottom Output/Problems.
4. Frameless window bridge and native menu.
5. Session runtime and live switching.
6. Terminal subsystem.
7. Plugin shell takeover and injection integration.
8. Cleanup, localization, tests.

Each step must keep the app buildable and the calculator usable. No step may
leave the default shell dependent on a plugin.
