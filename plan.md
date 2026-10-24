# CasioEmuMsvc Preview — Detailed Plan (plan.md)
Regenerated: 2026-10-24
Branch target: preview (CasioEmuMsvc-Mac only, from stable)
Repo exclusions: plan.md in .gitignore; not committed
Encoding: UTF-8 without BOM (AGENTS.md)
Length: extended; every item has problem, prerequisites, exact steps

================================================================================
SECTION 1: HIDDEN PROBLEMS FOUND (TEST TRAPS) — WHAT, WHY, HOW
================================================================================

--- ITEM 10.1: Plugin loader broken on macOS ---
WHAT IS WRONG: PluginMan.cpp line 96 defines `void LoadPlugins() {};` empty for non-Windows/non-Android. On Mac the plugin system is entirely disabled; PluginLogWindow shows nothing and no .plugin can load.
WHAT IS NEEDED FOR THIS: A macOS loader mechanism, either using `dlopen` / `dlsym` for `.dylib` extracted from `.plugin`, or a pure manifest-based loader that scans a plugin folder and applies hooks without binary loading if the plugin is theme/script based.
EXACT INSTRUCTIONS:
1. Open `emulator/CasioEmuMsvc/src/Plugin/PluginMan.cpp`
2. Replace the `#else` block (line 96) with a macOS implementation using `<dlfcn.h>`
3. Implement scanning of `~/Library/Application Support/CasioEmuMsvc/Plugins/` (or working-dir `plugins/`) for `.plugin` zips
4. Unzip `.plugin` to a temp folder; read `plugin.json`; if type is `theme` apply settings; if `tab` load UI hook; if `inject` load payload; if `tool` add window
5. Call `load(g_pluginapi)` for binary plugins; for manifest-only plugins apply API hooks directly
6. Update `PluginMan.h` to expose new loader status flags
7. Test by placing a dummy `.plugin` with manifest only and verifying log

--- ITEM 10.2: .plugin feature missing on macOS ---
WHAT IS WRONG: No `.plugin` package format exists. `PluginApi` exists (`PluginApi_Impl.cpp`) but there is no zip/manifest loader, no spec, no docs. User wants `.plugin` to be a zip with manifest + payload, able to do themes, tab plugins, UI revamps, injection.
WHAT IS NEEDED FOR THIS: A defined `.plugin` zip format (`plugin.json` inside), loader code, documentation file, examples per plugin type, and unrestrictive scopes (can include any files).
EXACT INSTRUCTIONS:
1. Design `plugin.json` schema: `name`, `version`, `author`, `target`, `permissions[]`, `entry_point`, `type` (theme/tab/inject/ui_revamp/tool)
2. Create `docs/PLUGIN_SPEC.md` with sections: create (`mkdir plugin; touch plugin.json`), package (`zip -r my.plugin .`), build (version bump/check), use (drag/drop or loader), APIs (`CalcSession`, `UITheme`, `Injection`, `FileExplorer`, `ThemeSettings`), security (manifest-only permissions, no arbitrary binary execution), examples for all 5 types
3. Implement loader in `PluginMan.cpp` for macOS: scan folder; unzip `.plugin`; parse manifest; validate; apply
4. Add plugin folder config in `ThemeManager.h` / settings
5. Generate example plugins (theme, tab injector) in `examples/plugins/`
6. Ensure `.plugin` is unrestrictive: any extra folders/files allowed inside zip

--- ITEM 10.3: Keyboard RETURN alias trap ---
WHAT IS WRONG: Keyboard.cpp 621-624 maps `SDLK_RETURN` to `SDLK_SPACE` when model has no RETURN binding. Hidden confusion: user presses Enter and gets EXE instead of expected behavior.
WHAT IS NEEDED FOR THIS: Explicit alias tracking, visual label in UI, documentation, and optional disable.
EXACT INSTRUCTIONS:
1. In `Keyboard.cpp` add a `bool return_alias_active` flag set during init
2. Only apply alias if `keyboard_map.find(SDLK_RETURN) == end()` and a config option is enabled (default true)
3. In `Ui.cpp` / calculator window, if alias is active show a small indicator ("Ret=Exe") near keyboard status
4. Document in `docs/EDITOR.md` or keyboard docs
5. Test with fx-580VN X model: press Return, verify EXE behavior; disable alias via settings if needed

--- ITEM 10.4: Multi-touch ReleaseAll hidden ---
WHAT IS WRONG: Keyboard.cpp ~800 comments out `ReleaseAll();` for multi-touch. Hidden test: multi-key release doesn't fully reset button holds.
WHAT IS NEEDED FOR THIS: Restore conditional release based on finger count / key count, not blanket comment-out.
EXACT INSTRUCTIONS:
1. Read `Keyboard.cpp` around line 800: find the `else if (!AnyFingerPressing())` block
2. Uncomment `ReleaseAll();` inside the condition but guard it: only if `held_keyboard_keys.empty()` or number of pressed fingers < 1
3. Ensure `keyboard_button_holds` is cleared appropriately when all keys released
4. Add test: press two keys, release both, verify buttons reset to unpressed state
5. Verify with `HandleKeyboardKey` release tracking (already has `held_keyboard_keys` erase logic)

--- ITEM 10.5: Focus timing risk ---
WHAT IS WRONG: `Ui.cpp` line 268 sets `calculator_panel_focused = false` at start of `gui_loop()`, but line 69 sets it only when Calculator window is focused. If focus changes mid-loop, state may be wrong.
WHAT IS NEEDED FOR THIS: A more reliable focus tracking mechanism, possibly deferred reset at end of loop or using `ImGui::IsWindowFocused()` directly at event time.
EXACT INSTRUCTIONS:
1. Change `gui_loop()` to not reset at top; instead set `false` only when closing or when no Calculator window exists
2. In `Render()` (Ui.hpp line 69), keep `calculator_panel_focused = ImGui::IsWindowFocused()` when name is "Calculator"
3. In `casioemu.cpp` event routing (line 505: `calc_window || calculator_panel_focused`), verify routing uses latest value
4. Test: open calculator, click editor, verify keys go to editor (not calc); click calc, verify keys go to calc; multi-key releases work

--- ITEM 10.6: Editor language trap ---
WHAT IS WRONG: `WizToolchain.cpp` line 237 uses `TextEditor::LanguageDefinition::CPlusPlus()` for Wiz source. Autocomplete and syntax highlighting are wrong for Wiz directives.
WHAT IS NEEDED FOR THIS: A custom `LanguageDefinition` for Wiz/CWiz with keywords (`print`, `str`, `num`, `for`, `if`, `while`, `hex`, etc.), directives (`#for`, `#cwapp`, `ORG`), and comment rules.
EXACT INSTRUCTIONS:
1. Create `compiler/src/wiz_lang_def.hpp` or add to `WizToolchain.cpp` a new `LanguageDefinition` struct
2. Define keywords, identifiers, comments (`//` and `/* */` if used), strings (`"..."`), numbers (hex/dec)
3. In `WizToolchain.cpp`, replace `CPlusPlus()` with new Wiz definition (around line 237 and 988 palette setup)
4. Add directive recognition: lines starting with `#` should highlight differently
5. Update `docs/EDITOR.md` with language spec

--- ITEM 10.7: Theme binary format locked ---
WHAT IS WRONG: `ThemeManager.h` uses binary magic header `0x47464354`; new themes can't easily extend settings without breaking old saves.
WHAT IS NEEDED FOR THIS: Backward-compatible extension using `seedColor` + auto-tint + style variables rather than changing binary struct.
EXACT INSTRUCTIONS:
1. Keep `ThemeSettings` binary format unchanged
2. Add new theme presets as functions (`ApplyMidnightBlue()`, `ApplySolarized()`, `ApplyMonoGreen()`) that set `igs_dark`/`igs_light` and `seedColor`
3. In `ThemeManager::ApplyDefaultTheme()` or new methods, load preset by name
4. Add theme selector in Settings UI (dropdown or buttons)
5. Save preset name in `language` or new string if needed, or just rely on style differences visible to user

================================================================================
SECTION 2: WIZ COMPILER / EDITOR — FEATURES
================================================================================

--- ITEM 1.1: Editor autocompletion ---
WHAT IS WRONG: No word-boundary trigger for keywords; user must type full identifier.
WHAT IS NEEDED: A trigger list (keywords, built-ins, labels from `g_labels`, file names from folder), dropdown rendering.
EXACT INSTRUCTIONS:
1. In `WizToolchain.cpp`, create `AutocompleteEngine` class or function
2. Collect keywords from `docs/API.md`, compiler token list, `g_labels`
3. On `SDL_KEYDOWN` for `.` or alphabetic, build suggestion list based on current word prefix
4. Render dropdown with `ImGui::BeginChild` / `ImGui::Selectable` near cursor
5. On selection, insert text via `editor_.InsertText()`

--- ITEM 1.2: Autosuggestions with docs ---
WHAT IS WRONG: Suggestions have no context or documentation snippet.
WHAT IS NEEDED: A doc snippet per keyword, shown beside or below dropdown.
EXACT INSTRUCTIONS:
1. Create `docs/EDITOR.md` with entries: keyword -> one-line description
2. In dropdown code, show snippet using `ImGui::TextDisabled()` next to selectable
3. Keep snippets short (< 60 chars)

--- ITEM 1.3: Docs for directive/target/lang ---
WHAT IS WRONG: No reference linking directives to targets/languages.
WHAT IS NEEDED: A reference table in docs.
EXACT INSTRUCTIONS:
1. Write `docs/EDITOR.md` sections: Directives (`#for 580`, `#cwapp`, section headers), Targets (580VN X / 880BTG / fwnx / nightmare), Languages (Wiz / CWiz)
2. Add mapping rules: file extension `.wz` -> Wiz; `.cwiz` -> CWiz; `#for 580` -> target 580
3. Show in editor status bar using `ThemeManager` or window title

--- ITEM 1.4: Editor UI revamp ---
WHAT IS WRONG: Plain text editor; no gutter, no split docs pane, no minimap.
WHAT IS NEEDED: Improved layout with side panel for docs/reference.
EXACT INSTRUCTIONS:
1. Modify `WizToolchain` render to split table: left = editor (70%), right = docs/reference (30%)
2. Add line numbers using `TextEditor` settings or custom render
3. Add palette options (dark, light, high-contrast) by setting `editor_.SetPalette()`

--- ITEM 1.5: Directive/target/lang identification ---
WHAT IS WRONG: Editor doesn't parse directives.
WHAT IS NEEDED: Parser that reads first 10 lines for directives; updates status.
EXACT INSTRUCTIONS:
1. Read `editor_.GetText()` first lines
2. Regex for `#for\s+(\w+)`, `#cwapp`, `ORG\s+([0-9A-Fa-f]+)`
3. Update window name / status bar with target and language

--- ITEM 1.6: Editor settings sync ---
WHAT IS WRONG: Font/theme settings not linked to editor.
WHAT IS NEEDED: Sync font scale and palette with ThemeSettings.
EXACT INSTRUCTIONS:
1. In editor init, call `ThemeManager::Instance().fontScale` to set UI scale
2. Set palette based on `IsDarkMode()`
3. Save preferences in theme settings if needed

================================================================================
SECTION 3: GENERAL / UI REVAMP
================================================================================

--- ITEM 2.1: Entire UI design revamp ---
WHAT IS WRONG: UI uses default ImGui flat style; no rounded corners, inconsistent spacing.
WHAT IS NEEDED: Modern style with rounded windows, consistent padding, cleaner typography.
EXACT INSTRUCTIONS:
1. Modify `ThemeManager` style application: set `ImGuiStyle::WindowRounding = 8.0f`, `FrameRounding = 4.0f`
2. Ensure padding uses `ThemeManager::padding` everywhere
3. Add subtle borders and shadow colors via `Colors` array

--- ITEM 2.2: Settings revamp ---
WHAT IS WRONG: Settings is a simple window; categories not clear.
WHAT IS NEEDED: Category separation (Theme, Editor, Emulator, Plugins, System).
EXACT INSTRUCTIONS:
1. Modify `MakeSettingsWindow()` / `Theme.cpp` / settings UI
2. Use collapsible headers per category (`ImGui::CollapsingHeader`)
3. Group settings by logical area; add descriptions

--- ITEM 2.3: New themes ---
WHAT IS WRONG: Only dark/light.
WHAT IS NEEDED: Midnight Blue, Solarized, Mono Green.
EXACT INSTRUCTIONS:
1. Create preset functions in ThemeManager or settings UI
2. Define `ImGuiStyle` colors for each preset
3. Apply preset by changing `igs_dark`/`igs_light` and calling `SetDarkMode()` / `SetLightMode()` accordingly
4. Add theme chooser buttons/selector in Settings

--- ITEM 2.4: Calculator bottom resize ---
WHAT IS WRONG: Bottom option area may overflow on small screens.
WHAT IS NEEDED: Dynamic height based on window size.
EXACT INSTRUCTIONS:
1. In `CalcScreenWindow.cpp` or calculator face rendering, compute available height = window height - top bar - display area
2. Scale bottom row heights proportionally; ensure minimum visible buttons
3. Test at 800x600 and full screen

================================================================================
SECTION 4: INJECTION / ROP TAB
================================================================================

--- ITEM 3.1: Rename ROP -> Injection ---
WHAT IS WRONG: Tab named "Rop Compiler"; should be "Injection".
WHAT IS NEEDED: Window title change; preserve APIs.
EXACT INSTRUCTIONS:
1. In `Ui.cpp` or `RopCompilerUI.cpp`, change `UIWindow("Rop Compiler")` to `UIWindow("Injection")`
2. Update any menu references; keep internal names if needed for compatibility

--- ITEM 3.2: Single Injection panel ---
WHAT IS WRONG: Split source/output table; user wants one intuitive hex panel.
WHAT IS NEEDED: Combine hex input + description + inject into single view with live preview.
EXACT INSTRUCTIONS:
1. Redesign `LibCompilerImGuiWindow::RenderCore()` to use a single-column or simpler split (input on top, preview bottom)
2. Add input fields: hex bytes (editable), description (string), inject button
3. Show live hex preview of assembled payload
4. Keep compile logic (`compileCallback_`) intact

--- ITEM 3.3: Remove/merge Payload Input and Custom Payload ---
WHAT IS WRONG: Separate tabs exist (or planned); need consolidation.
WHAT IS NEEDED: Migrate saved payload data to Injection state; delete separate window subclasses.
EXACT INSTRUCTIONS:
1. Check `Ui.cpp` windows list for extra payload windows; remove them
2. Store payload path/injection text in `ThemeSettings::injectionFilePath` or new field
3. Load saved injection text when Injection window opens

--- ITEM 3.4: Validation and safe mode ---
WHAT IS WRONG: No validation on hex input.
WHAT IS NEEDED: Hex format check, length limits, safe toggle.
EXACT INSTRUCTIONS:
1. Add function to parse hex string: check characters are 0-9A-Fa-f, pairs, max length
2. If invalid, show `ImGui::TextColored()` error message
3. Add `bool safeMode` checkbox; when safe, restrict injection to non-critical addresses

================================================================================
SECTION 5: ROM / RAM
================================================================================

--- ITEM 4.1: ROM tab -> All ---
WHAT IS WRONG: Separate ROM tab and All tab duplicate.
WHAT IS NEEDED: Single unified view.
EXACT INSTRUCTIONS:
1. Find window names in Ui.cpp; rename ROM window to "All" or consolidate
2. Ensure content displays ROM data, disassembly, and status in one scrollable region
3. Remove duplicate "All" entry from windows list if present

--- ITEM 4.3: RAM resize dynamic ---
WHAT IS WRONG: Fixed-width table overflows for 64 bytes.
WHAT IS NEEDED: 16 bytes = perfect; 64 bytes = +1.5 char wider span; lower = tighter.
EXACT INSTRUCTIONS:
1. Modify `AddressWindow` table setup: compute column width based on bytes-per-row setting
2. Use `ImGui::TableSetupColumn` with `WidthStretch` calculated from byte count
3. Test at 16, 32, 64 bytes/row; verify all hex groups visible with spaces

================================================================================
SECTION 6: VS CODE SESSION MODEL
================================================================================

--- ITEM 5.1: Start screen VS Code style ---
WHAT IS WRONG: StartupUi shows model selection; needs welcome with recent, new calc, open folder.
WHAT IS NEEDED: Welcome page design.
EXACT INSTRUCTIONS:
1. Modify `StartupUi.cpp`: create welcome view with buttons/links
2. Show recent calc sessions from a saved list (store in settings or file)
3. Add "New Calculator" button that opens model chooser; add "Open Folder" for file explorer

--- ITEM 5.2: Session tabs ---
WHAT IS WRONG: Each model opens separate window; no tab session inside main window.
WHAT IS NEEDED: Tabs named `<model>`; calc window renders inside tab; multiple allowed.
EXACT INSTRUCTIONS:
1. Design session tab strip (leftmost vertical or top horizontal)
2. When model opens, instead of `SDL_CreateWindow`, add to session list; render inside main window tab
3. Store session list with model name, window state, calculator panel focus
4. Close tab -> return to start or previous session; open new tab -> start screen

--- ITEM 5.3: Left sidebar file explorer ---
WHAT IS WRONG: No file manager inside calc screen.
WHAT IS NEEDED: Mini file tree showing folder contents.
EXACT INSTRUCTIONS:
1. Create or reuse `5800FileSystem` view in sidebar (left area of calc session)
2. Show folders/files selectable; double-click opens in WizToolchain editor tab
3. Keep simple tree using `std::filesystem` iteration

--- ITEM 5.4: Bottom output / top chrome ---
WHAT IS WRONG: No dedicated output area; window chrome basic.
WHAT IS NEEDED: Bottom output panel; top close/min/max buttons clean.
EXACT INSTRUCTIONS:
1. Add bottom panel (height ~120px) for calc output/status messages
2. Style top chrome with small icons/buttons at top-right; match theme colors
3. Ensure chrome doesn't overlap calculator display area

================================================================================
SECTION 7: PLUGIN MANAGER / .PLUGIN FORMAT
================================================================================

--- ITEM 6.1: .plugin package format ---
WHAT IS WRONG: No format defined.
WHAT IS NEEDED: Zip named `.plugin`; manifest `plugin.json`; folders allowed.
EXACT INSTRUCTIONS:
1. Define manifest fields precisely (see 10.2)
2. Specify zip creation command and extraction rules
3. Document folder layout: `plugin.json` at root; `assets/`, `src/`, `docs/` optional

--- ITEM 6.2: Plugin loader for macOS ---
WHAT IS WRONG: Empty loader.
WHAT IS NEEDED: macOS loader that reads `.plugin` zip, validates manifest, applies hooks.
EXACT INSTRUCTIONS:
1. Implement scanning of `plugins/` folder for `.plugin` files
2. Unzip each to temp directory; read `plugin.json`
3. For `theme`: apply colors/styles; for `tab`: register new UIWindow; for `inject`: add to injection list; for `ui_revamp`: modify UI elements; for `tool`: add tool window
4. Log results to `PluginLogWindow`

--- ITEM 6.3: Plugin docs (extensive) ---
WHAT IS WRONG: No docs for .plugin.
WHAT IS NEEDED: Full docs covering create/package/build/use/APIs.
EXACT INSTRUCTIONS:
1. Write `docs/PLUGIN_SPEC.md` with all sections listed in 10.2
2. Include 5 examples (one per plugin type) with manifest and folder contents
3. Include API reference for `CalcSession`, `UITheme`, `Injection`, `FileExplorer`, `ThemeSettings`
4. Include security notes (manifest permissions only)

--- ITEM 6.4: Remove/rework old Plugin Manager ---
WHAT IS WRONG: Plugin Manager unclear; possibly windows-only.
WHAT IS NEEDED: Replace with new loader-based system; keep log if useful.
EXACT INSTRUCTIONS:
1. Remove old Plugin Manager tab if it exists and is windows-only
2. Add Plugin loader view in Settings or standalone Plugin tab
3. Keep `PluginLogWindow` as debug/optional view

================================================================================
SECTION 8: BRANCH / REPO / CLEANUP
================================================================================

--- ITEM 7.1: Create preview branch ---
WHAT IS WRONG: Feature work not isolated.
WHAT IS NEEDED: Branch `preview` on CasioEmuMsvc-Mac.
EXACT INSTRUCTIONS:
1. In clone `/private/var/folders/.../CasioEmuMsvc-Mac`, run `git checkout -b preview`
2. Push to origin: `git push origin preview`
3. Do not merge to stable

--- ITEM 7.2: Plan excluded from git ---
WHAT IS WRONG: Could accidentally commit plan.
WHAT IS NEEDED: `.gitignore` includes `plan.md`.
EXACT INSTRUCTIONS:
1. Confirm `.gitignore` line exists (`plan.md` or `plan.txt`)
2. Verify `git status` does not show plan.md under tracked or untracked

--- ITEM 7.3: Binary verification ---
WHAT IS WRONG: Changes might break build or speed.
WHAT IS NEEDED: Rebuild and test.
EXACT INSTRUCTIONS:
1. `cmake --build emulator/build --target CasioEmuMsvc --parallel 2`
2. Copy `emulator/build/CasioEmuMsvc/CasioEmuMsvc.app` to `emulator/CasioEmuMsvc.app`
3. Run binary briefly; confirm calculator keyboard works; check no crash
4. Verify `SDL_WaitEventTimeout` loop still present (speed fix preserved)

--- ITEM 7.4: Tests pass ---
WHAT IS WRONG: Compiler changes could break.
WHAT IS NEEDED: `wiztests` passes.
EXACT INSTRUCTIONS:
1. `./build/wiztests`
2. Confirm 351 passes / 0 fails

--- ITEM 7.5: Preserve keyboard / speed / focus ---
WHAT IS WRONG: New UI could break existing fixes.
WHAT IS NEEDED: Keep `HandleKeyboardKey`, `held_keyboard_keys`, `calculator_panel_focused`, `SDL_TEXTINPUT` suppression, `SDL_WaitEventTimeout`.
EXACT INSTRUCTIONS:
1. Before any edit, save current `Keyboard.cpp`, `casioemu.cpp`, `Ui.cpp`
2. After redesigns, diff against saved versions; restore any accidentally removed fixes

================================================================================
SECTION 9: DOCUMENTATION
================================================================================

--- ITEM 8.1: docs/PLUGIN_SPEC.md ---
WHAT IS NEEDED: Full spec.
EXACT INSTRUCTIONS:
1. Create file with manifest schema, zip format, build steps, APIs, examples, security
2. Make it long (at least 300 lines) with code examples

--- ITEM 8.2: docs/INJECTION.md ---
WHAT IS NEEDED: Redesign docs.
EXACT INSTRUCTIONS:
1. Describe new Injection panel; hex input format; validation; safe mode

--- ITEM 8.3: docs/EDITOR.md ---
WHAT IS NEEDED: Autocomplete, directives, language.
EXACT INSTRUCTIONS:
1. Document directive mapping; language detection rules; autocomplete trigger keys

--- ITEM 8.4: docs/SESSION_TABS.md ---
WHAT IS NEEDED: VS Code session behavior.
EXACT INSTRUCTIONS:
1. Document tab lifecycle; sidebar navigation; bottom output; chrome layout

--- ITEM 8.5: docs/THEMES.md ---
WHAT IS NEEDED: Theme definitions.
EXACT INSTRUCTIONS:
1. Document new preset colors; how to apply; seedColor usage

--- ITEM 8.6: README.md update ---
WHAT IS NEEDED: Preview overview.
EXACT INSTRUCTIONS:
1. Add preview feature list; `.plugin` quick start; branch info

================================================================================
END OF PLAN
================================================================================
This document replaces the previous short checklist. Every feature, bug, and hidden test trap now has: (1) what is wrong/needed, (2) prerequisites, (3) exact instructions. Implementation deferred until user switches to implement agent.
