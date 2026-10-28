# CasioEmuMsvc Preview - product and engineering plan

Status: active
Branch: `preview`
Target: macOS only
Last updated: 2026-10-28

This document is the source of truth for the preview rewrite. It replaces the
older `plan.md` checklist and reconciles the conflicting notes in
`DEBUGGER-UI.md`, `SESSION_TABS.md`, and `PLUGIN_SPEC.md`.

The rewrite has two products that ship together:

1. **A new app shell** with session tabs, a workspace sidebar, a tabbed bottom
   panel, a welcome page, JSON settings, and a VS Code-inspired visual system.
2. **A plugin platform** for `.plugin` packages with declarative themes and
   layouts, optional native dylibs, explicit permissions, and a trust prompt.

Neither is a reskin of the current UI. The shell and the plugin host are being
designed as one system so that a `ui_revamp` plugin can replace the shell
without forking the app.

---

## 1. Locked decisions

These decisions came out of the requirements interview and are not open for
casual reversal. Changing one requires updating this document and the affected
spec.

### Product and UI

| Area | Decision |
| --- | --- |
| Scope | Full information architecture and visual refresh |
| Navigation | Hybrid: top-level session tabs plus a global tool dock |
| Session | A separate live model instance |
| Background sessions | All sessions run; inactive sessions are throttled |
| Start screen | VS Code-style welcome, merged into the main shell |
| Startup | Restore last layout and sessions; fall back to welcome |
| Sidebar | Project file explorer rooted at a user-chosen workspace folder |
| Bottom panel | Tabbed Output, Problems, and Terminal |
| Terminal | Real PTY shell with full xterm behavior and job control |
| Window chrome | Frameless window with native macOS traffic lights |
| Menus | Native macOS application menu |
| Session tabs | Top horizontal strip |
| Existing tools | Keep all, regroup logically |
| Persistence | Layout, sessions, and recent files |
| Dock layout | Global tool layout; per-session documents and calculator state |
| Settings | JSON in Application Support, with a portable override |
| Visual direction | VS Code-inspired open-source styling, adapted for macOS |
| Localization | All new UI strings go through the existing localization system |
| Non-macOS code | Remove non-macOS UI and plugin paths |
| Branching | `preview`, small commits per subsystem |

### Plugins

| Area | Decision |
| --- | --- |
| Package | Zip archive named `<name>.plugin` |
| Manifest | `plugin.json` at the zip root |
| Schema | Frozen at version 1 (see `PLUGIN_SPEC.md`) |
| Types | `theme`, `tab`, `inject`, `ui_revamp`, `tool` |
| Execution | Declarative files by default; optional native dylib |
| Native ABI | Stable, versioned C++ ABI; additive interfaces only |
| Permissions | Explicit manifest list; warn on all requested permissions |
| Trust | In-app modal pinned to plugin id, version, and SHA-256 |
| Discovery | User Application Support folder plus app-local `plugins/` |
| Hot reload | Declarative plugins live; native plugins on restart/unload |
| Full shell takeover | Both declarative shell tree and native shell interface |
| Injection | Requires `emulator.inject`; no repeat confirmation after approval |
| Manager | Installed list, enable/disable/remove/reveal, permissions, log |
| Platform | macOS only |

---

## 2. What is being replaced

The current app is a single SDL window with one ImGui dock space. The
calculator is a dock tab. There is one global `Emulator`, one global `windows`
vector, one global calculator focus flag, and a separate `StartupUi` SDL window
before the emulator starts. Plugin loading is compiled out on macOS.

The preview replaces four things:

1. **The window model** - one frameless app window with a shell owned by the
   app, not by the dock builder.
2. **The session model** - session tabs own live emulator instances and
   per-session documents; tools are global.
3. **The settings model** - a versioned JSON document with migration from
   `theme.bin`.
4. **The plugin host** - a real `.plugin` loader, manifest validation, trust
   store, declarative theme/layout application, and a native SDK.

The emulator core, chipset models, debugger logic, assembler, Wiz compiler, and
snapshot format are reused. They are not rewritten.

---

## 3. Shell layout

```
+--------------------------------------------------------------------------+
| native traffic lights | session tab 1 | session tab 2 | + |  toolbar    |
+----------------------+-----------------------------------+---------------+
|                      |                                   |               |
|  File Explorer       |        active session content     |  tool dock    |
|  (workspace tree)    |   (calculator / editor / docs)    |  (optional)   |
|                      |                                   |               |
+----------------------+-----------------------------------+---------------+
| Output | Problems | Terminal                         status bar        |
+--------------------------------------------------------------------------+
```

Rules:

- The shell is always present, including on the welcome page.
- Session tabs are the primary switcher. The tool dock has its own tab bar for
  debugger tools; it is visually subordinate to session tabs.
- The sidebar and bottom panel are collapsible and resizable. Their geometry
  is global, not per session.
- The active session owns the calculator, open source documents, and scroll
  positions. Switching tabs restores that state.
- Closing a session tab closes that emulator instance. Closing the last tab
  returns to the welcome page, not the quit prompt.
- Cmd+Q is the only normal way to quit.
- The tool dock can be hidden for a distraction-free calculator view. The
  setting is global.

### 3.1 Frameless window

The window keeps the native macOS traffic lights and native resize, drag,
shadow, fullscreen, and window snapping. The title bar is transparent and the
content extends under it. No custom close/minimize/maximize glyphs are drawn;
the real controls remain in the top-left.

The app menu is the native macOS menu bar. The in-window toolbar contains only
frequently used actions: build/run, pause/resume, zoom, plugin manager, and a
layout menu.

### 3.2 Session tabs

- Horizontal, reorderable, closable, with a trailing `+`.
- Label is the model name, with a session suffix when the same model is open
  more than once.
- A dirty or paused indicator is drawn in the tab.
- Middle-click closes; Cmd+W closes the active session; Cmd+Shift+T reopens the
  most recently closed session if its model and ROM still exist.
- Drag a tab to reorder. There is no tear-out into a second OS window in the
  first release.

### 3.3 Sidebar

- Rooted at a workspace folder chosen with **File > Open Folder**.
- Tree view with folders, source files, ROMs, and `.plugin` files.
- Double-click opens a source file in the editor session associated with the
  active calculator session when the language/target matches; otherwise it
  opens a standalone editor document.
- Recent folders and recent files appear above the tree when no workspace is
  open.
- Drag-and-drop a folder onto the window opens it as a workspace.
- Drag-and-drop a `.plugin` installs it through the plugin manager.

### 3.4 Bottom panel

| Tab | Contents |
| --- | --- |
| Output | Build output, run messages, plugin load log, calculator messages |
| Problems | Compiler diagnostics with `file:line:column` jump targets |
| Terminal | Real PTY shell, full xterm behavior, job control |

The panel remembers its active tab and height. Problems auto-focuses on a
failed build unless the user pinned another tab. The Terminal starts in the
workspace root and inherits the app environment.

---

## 4. Session runtime

A session is a live emulator instance plus its documents. It is not a saved
snapshot.

- Each session owns an `Emulator`, its renderer texture, its model resources,
  and its calculator input state.
- The renderer is shared. Each session renders into its own offscreen texture;
  the active session's texture is composited into the session content area.
- All sessions tick. Inactive sessions are throttled to a configurable lower
  cycle rate (default: 25% of active, with a floor that keeps timers sane).
- The active session receives keyboard input. Calculator keys are routed only
  when the calculator view has focus.
- Global tools operate on the active session. The status bar always names it.
- Snapshot, MCP, and plugin APIs take a session handle or default to the active
  session.

Restoring at launch:

- Reopen each saved session's model and last ROM image.
- Reboot to the model main menu; do not restore RAM from a snapshot.
- Restore tab order, active tab, workspace folder, open documents, panel
  geometry, and recent files.
- If a model or ROM is missing, keep the tab as a placeholder with a Repoint
  action instead of silently dropping it.

---

## 5. Settings

Settings move to a versioned JSON document:

```
~/Library/Application Support/CasioEmuMsvc/settings.json
```

Portable mode: if `portable.flag` exists next to the app, use
`<app>/settings/settings.json` instead.

Migration:

1. Read `theme.bin` if it exists.
2. Convert theme, scale, language, injection path, window geometry, and the
   integration toggles into schema version 1.
3. Write `settings.json` atomically.
4. Leave `theme.bin` in place for one release, then stop reading it.

The JSON document also stores shell layout, session list, recent files,
workspace, plugin trust, and per-tool visibility. Secrets are never stored in
this file.

---

## 6. Visual system

The visual direction is VS Code-inspired, using the MIT-licensed VS Code
default themes as a color reference. No VS Code code or trademarked assets are
copied.

Baseline dark palette (approximate, final values live in the theme JSON):

| Token | Value |
| --- | --- |
| `background` | `#1f1f1f` |
| `surface` | `#181818` |
| `surfaceAlt` | `#202020` |
| `foreground` | `#cccccc` |
| `muted` | `#9d9d9d` |
| `border` | `#2b2b2b` |
| `accent` | `#0078d4` |
| `selection` | `#264f78` |
| `hover` | `#2a2d2e` |
| `error` | `#f14c4c` |
| `warning` | `#cca700` |
| `success` | `#89d185` |

Rules:

- 8 px base spacing; 4 px increments inside dense controls.
- 6 px window rounding, 4 px frame rounding, 1 px borders.
- No default ImGui bevels, gradients, or bright blue selection.
- Tables are quiet: row backgrounds only on hover and selection.
- Icons are monochrome and inherit the foreground color.
- The calculator face is never restyled; only its frame and background are.

Themes are data, not code. Presets ship as JSON token files in
`resources/themes/`. A `theme` plugin supplies the same token schema.

---

## 7. Plugin platform

The full contract is in `PLUGIN_SPEC.md`. Engineering summary:

- `.plugin` is a zip. `plugin.json` is required at the root.
- Declarative types: `theme` (tokens), `inject` (payload list), `ui_revamp`
  (layout tree or shell tree).
- Native types: `tab` and `tool` add `UIWindow`s; `ui_revamp` may also provide a
  native shell implementation.
- Every plugin declares permissions. The manager shows them before loading and
  the trust modal repeats them on first install and on native hash changes.
- Native plugins are loaded with `dlopen` from an extracted, hash-checked
  package directory. They must export `fPluginLoad(PluginApi*)`.
- The v1 C++ interfaces are frozen. New capabilities are added as new
  interfaces discovered through `PluginApi::QueryInterface`, never by changing
  an existing vtable.
- Declarative plugins can be hot-reloaded. Native plugins require an explicit
  unload or app restart.
- Plugin code runs in-process. There is no sandbox for native code; the trust
  prompt is the security boundary. This is stated in the UI, not hidden.

---

## 8. Phases

Each phase ends with a build, the relevant tests, and a commit on `preview`.

### Phase 0 - Contract and docs

- Replace the stale plan with `PREVIEW.md`, `PLUGIN_SPEC.md`, `UI_REWRITE.md`.
- Add the JSON theme token schema and example themes.
- Acceptance: docs agree with each other; no code changes required.

### Phase 1 - Plugin foundation

- `plugin.json` v1 parser and validator.
- Zip extraction with path traversal protection and SHA-256 hashing.
- Discovery in Application Support and app-local `plugins/`.
- Trust store and in-app trust modal.
- Declarative theme application.
- Native `dlopen` loading and `fPluginLoad`.
- Plugin manager window with enable/disable/remove/reveal and log.
- Example plugins for all five types.
- Acceptance: a valid theme loads, a malformed package is rejected with a
  readable error, a native test plugin loads after approval, and the manager
  reflects every state.

### Phase 2 - Shell

- Frameless window with native traffic lights.
- Native menu wiring.
- Session tab strip (single live session first).
- Sidebar file explorer.
- Bottom Output/Problems panel.
- Welcome page in the main shell.
- JSON settings and `theme.bin` migration.
- VS Code-inspired theme tokens and presets.
- Acceptance: the app opens on welcome, opens a model into a session tab,
  builds from the editor, shows diagnostics in Problems, and restores layout.

### Phase 3 - Multi-session runtime

- Per-session `Emulator` ownership and offscreen textures.
- Session switching, throttling, close/open, and persistence.
- Active-session routing for input, tools, MCP, and plugins.
- Acceptance: two models run concurrently, inactive tabs throttle, input and
  tools follow the active tab, and both sessions restore after relaunch.

### Phase 4 - Terminal and full shell plugins

- PTY allocation, resize propagation, xterm parser, scrollback, job control.
- Declarative shell tree and native shell interface for `ui_revamp`.
- Injection UI integration for `inject` plugins.
- Acceptance: zsh runs in the Terminal tab with colors and job control; a
  declarative shell can replace the default layout; a native shell plugin can
  replace the shell without crashing the host.

### Phase 5 - Cleanup and release

- Remove non-macOS UI/plugin paths.
- Localize all new strings.
- Plugin fixtures, malformed-package tests, permission-denial tests.
- Manual UI checklist and release notes.
- Acceptance: full test suite, MCP smoke test, plugin fixtures, and manual
  checklist pass.

---

## 9. Risks

| Risk | Mitigation |
| --- | --- |
| Multiple live emulators expose hidden globals | Phase 3 refactor with a session context object; no new code may depend on the active-session globals |
| Stable C++ ABI is hard across toolchains | Freeze v1 vtables, add interfaces additively, publish an SDK with a fixed libc++/compiler baseline, and version-gate |
| Full xterm is a large subsystem | Isolate the terminal in a module; start with PTY + parser + scrollback, add job control before enabling the tab |
| Full shell takeover can destabilize the app | Run shell plugins behind a capability and a fallback: if the shell plugin throws or fails validation, restore the default shell |
| Plugin permissions cannot sandbox native code | State this plainly in the trust modal; native code is an explicit trust decision |
| Removing non-macOS paths breaks upstream merges | Keep platform code isolated in `Platform/` and remove only after Phase 5; do not mix deletions with feature commits |
| VS Code styling can drift toward a clone | Use color/token references only; no copied assets, icons, or branding |

---

## 10. Definition of done

- `./build-mac.sh` succeeds.
- `wiztests` passes.
- MCP smoke test passes.
- Plugin fixtures cover valid, malformed, permission-denied, and hash-changed
  packages.
- Manual checklist covers: welcome, open model, session switch, calculator
  input, build, diagnostics jump, terminal, theme apply, plugin install,
  plugin disable, plugin remove, restart restore.
- Every new user-visible string is localized.
