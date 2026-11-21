# CasioEmuMsvc — iOS Port Plan (PLAN ONLY)

> **Status:** planning document. No iOS build, no device/simulator run and no
> source changes were performed for this task. The host is an **Intel x86_64
> Mac**; it can cross-compile an `arm64` iOS binary with Xcode but cannot
> execute it, so all verification claims below remain design-level.
>
> Worktree: `/Volumes/Code/ports/port/ios` · Branch: `port/ios` · Date: 2026-11-21
> Cross-references: `BUILD_TARGETS.md` and
> `CasioEmuMsvc/src/Plugin/PlatformTags.h` in the main checkout
> (`/Volumes/Code/_imports/Desktop/CasioEmuMsvc-Mac/`).

---

## 0. Executive summary

An iOS port is **feasible in stages**, but full desktop parity is not:

| Desktop capability | iOS outcome |
|---|---|
| Core nX-U8 / nX-U16 emulation, LCD rendering, settings, locales, Wiz in-process compiler | Reusable, mostly unchanged C++20 |
| nX-U8 asmjit JIT | **Blocked** (W^X, no iOS JIT entitlement); interpreter is already the live code path in this tree |
| Runtime-installed native `.plugin` dylibs | **Blocked** (code signing / W^X / App Store rules); data-only plugins yes, first-party static/embedded code only |
| CG100 SH4 Go core via DLL/dylib | **Blocked** as a dynamic plugin; static embed or long-term C++ interpreter only |
| Menu bar / dock / multi-window debugger UI | Must be redesigned as UIKit navigation (tab bar, split view) |
| Background emulation, external processes (RAC, Crashpad handler) | **Blocked**; pause on background, in-process alternatives only |

Recommended sequence: **Phase 1 data-only → Phase 2 static core & UIKit shell →
Phase 3 first-party static/embedded plug-ins only**. "Full plugin support" in
the desktop sense is not a realistic target for the App Store; it can only mean
plugins compiled into, or signed into, the app at build time.

An important repository-state caveat: this `port/ios` worktree is an **older
snapshot** (based on `preview`-era code). It does **not** contain
`cmake/CasioPlatform.cmake`, `BUILD_TARGETS.md`,
`CasioEmuMsvc/src/Plugin/PlatformTags.h`, `ArchPluginApi.h` or
`docs/DLL_PLUGIN_FORMAT.md`, and its git submodules (`SDL`, `SDL_image`,
`asmjit`, `sentry-native`, `discord-rpc`, `wiz`) are **not checked out**. The
main checkout (`stable` + uncommitted plug-in/CG100 work) has them. Any real
implementation must first forward-port/synchronise from the main checkout and
initialise submodules (`git submodule update --init --recursive`).

---

## 1. Architecture analysis — subsystem-by-subsystem mapping

### 1.1 Current architecture (evidence from this worktree)

`CMakeLists.txt` builds SDL 2.32 (`SDL/`, `SDL_image/`) from source, then
`CasioEmuMsvc/CMakeLists.txt` compiles a single C++20 executable from:
`Chipset/` (CPU, MMU, chipset, EPS6800 core), `Peripheral/`, `JitU8/`,
`Gui/` + vendored ImGui, `StartupUi/`, `Shell/`, `Settings/`, `Plugin/`,
`Rop/`, `Ctl/`, `Terminal/`, `CrashHandler/`, `Ext/`, plus vendored
`miniz`, `tinyxml2`, `asmjit`, `monocypher` (FetchContent) and the in-process
Wiz/CWiz toolchain. Desktop-only pieces are already conditionally compiled:
`discord-rpc` is skipped for `ANDROID OR IOS` (root `CMakeLists.txt:80-83`),
Sentry/Crashpad is skipped for `ANDROID OR IOS` (`CasioEmuMsvc/CMakeLists.txt:207,911`).

### 1.2 Mapping table

| Subsystem | Current implementation (evidence) | iOS equivalent | Notes / risk |
|---|---|---|---|
| CPU / MMU / chipset / peripherals | Portable C++ & C in `src/Chipset`, `src/Peripheral`; no OS-specific syscalls in the decode/execute path | **Reuse as-is** | Main emulation work stays platform-neutral. |
| Emulation clock / tick thread | `std::thread` in `Emulator.cpp:557,577,725,745`; SDL timers for 25 ms peripheral ticks; LCD history worker in `Chipset.cpp:779` | Keep `std::thread` for the core tick; use **GCD** (`dispatch_queue_t`, `dispatch_async`) for UI-side background work (settings I/O, asset copy, scans) | iOS suspends background threads; pause the core on `applicationDidEnterBackground`. Avoid blocking the main thread > ~1 s (watchdog termination). Reduced core count / thermal throttling on iOS. |
| Threads / locking | `std::mutex`, `std::atomic<std::thread::id>` (`Containers/ConcurrentObject.h`), recursive lock in `Emulator.cpp:1618` | POSIX threads are available on iOS, so `std::thread` works; map auxiliary workers to GCD queues with explicit QoS | Audit thread teardown: the existing screen-thread lifetime test (`CMakeLists.txt:457-478`) exists for a shutdown abort — keep an equivalent smoke test on iOS. |
| SDL2 video/input | `SDL_CreateWindow`, `SDL_CreateRenderer`, `SDL_RENDERER_TARGETTEXTURE`, renderer driver hint `"metal"` on Apple (`RendererBackend.h`); SDL 2.32 includes `src/video/uikit`, `src/main/uikit`, `src/hidapi/ios` | **Option A (recommended):** keep SDL2 with its UIKit backend; `SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal")` (fall back to `opengles2`/software). **Option B:** native `UIViewController` + `MTKView`/`CAMetalLayer`, SDL retained only for texture upload | Option A preserves event/key code and `TouchMouseTranslator`. SDL2 iOS creates its own `SDL_uikitviewcontroller`; host it inside a `UIViewController` container for navigation/safe area. |
| ImGui | Vendored ImGui with `imgui_impl_sdl2.cpp` and `imgui_impl_sdlrenderer2.cpp` only; `ImGui_ImplSDL2_InitForSDLRenderer` used in `Ui.cpp:546` and `StartupUi.cpp:2888`; `InitForMetal` declaration exists | Keep SDL2 platform backend for input; add upstream **`imgui_impl_metal.mm/.h`** as renderer backend with `UIKit` view (`MTKView`) — or keep `imgui_impl_sdlrenderer2` on top of SDL's Metal renderer | No Metal backend file ships in this tree: must vendor it. `imgui.cpp:19981` already disables the `fork()/execvp()` "open in shell" default for `TARGET_OS_IPHONE` — no patch needed there. Set `SDL_HINT_IME_SHOW_UI=1` before `SDL_CreateWindow()` (already documented in `imgui_impl_sdl2.h:11`). |
| Windows / docking | Multiple `SDL_CreateWindow` calls (`Emulator.cpp:515`, `Ui.cpp:484`, `StartupUi.cpp:2853`); ImGui tab bars/docks/panels | Single UIKit window/scene; treat debugger windows as ImGui tabs/panels inside one root tab; iPad may use secondary scene later | iOSApps are single-window by default; remove `SDL_WINDOW_RESIZABLE`/`SDL_WINDOW_HIDDEN` expectations, no OS window menus. |
| File I/O / paths | `std::filesystem`, `std::getenv("HOME")`, `$HOME/Library/Application Support/CasioEmuMsvc` (`Settings.cpp:163`, `PluginRegistry.cpp:156`), `_NSGetExecutablePath` (`Settings.cpp:130`), CWD-relative hints (`.renderer_hint.cfg`, `.crash.switch_renderer` in `casioemu.cpp:56-57`), env vars `CASIOEMU_MODELS_DIR`, `CASIOEMU_PLUGINS_DIR` | Use Foundation paths: `Bundle.main.resourceURL` (models, locales, `roms.db`, themes, Wiz data — read-only), `~/Library/Application Support/CasioEmuMsvc` (settings, trust store, cache), `~/Documents` (user imports, model packages, exported snapshots); `UIFileSharingEnabled` + `UIDocumentPickerViewController` for Files app import | **Replace all env-var/CWD logic** — iOS has no useful CWD and no inherited env. Wrap in a `PlatformPaths` shim returning `std::filesystem::path`; copy read-only assets to Application Support on first launch; mark cache dir `isExcludedFromBackup`. |
| Preferences | JSON `SettingsStore` (`Settings/Settings.cpp`, writes `settings.json`, portable-mode flag file) | Keep the JSON file in Application Support as the source of truth; mirror small UI flags to `NSUserDefaults` if desired | In-app settings screen is primary; a `Settings.bundle` can only display a sparse set of read-only keys and cannot write app files. |
| Crash reporting | Sentry-native + Crashpad (`sentry_init` in `casioemu.cpp:197-205`, crash handler in `src/CrashHandler`), already gated off for `IOS` in CMake | Define `DISABLE_SENTRY`; optionally add `sentry-cocoa` via SPM for UI-level crash reports | Crashpad spawns a helper process: impossible on iOS. `Config.hpp` gates `ENABLE_SENTRY` on `!DISABLE_SENTRY`. |
| Signals / traps | No POSIX signal handler in app code; `PANIC()` → `__builtin_trap()` (`Config.hpp`); ImGui `fork()` shell helper | Remove/neutralise traps in shipping builds (log + `UIAlertController` / fatal error handler); no `SIG*` handling needed | A trap looks like a crash to users and App Store; add an iOS-friendly fatal handler. |
| fork / exec / subprocesses | ImGui shell helper (already disabled on iOS); Wiz ROP path invokes external RAC compiler (GPL-3.0, documented in `README.md:130-134`); Sentry Crashpad handler | **No `fork`/`exec`/`posix_spawn` on iOS.** Compile RAC in-process or disable ROP on iOS; Crashpad disabled | Also a licensing consideration: GPL RAC is not linked into the desktop app today; in-process linking would change that — prefer disabling ROP in Phase 1/2. |
| Networking / online features | libcurl + `OnlineModelClient`, `UpdateChecker`, `OnlineLoopbackServer` (`StartupUi/`) | HTTPS via libcurl built against **Apple TLS (Secure Transport)** or a thin `NSURLSession` bridge; ATS defaults, no cleartext; foreground-only loopback | App review may question a listening socket; document or disable the loopback server on iOS. Update checker should use the App Store rather than self-update. |
| asmjit JIT | `JitU8/JitU8.cpp` uses `asmjit/x86.h` only; compiles into the target (`CMakeLists.txt:259,326`) but **no code in this tree instantiates `JitU8`** — the interpreter in `CPU.cpp` is the actual runtime path | Exclude `JitU8/*.cpp` from the iOS target and define `ASMJIT_NO_JIT` (same mechanism the web build uses at `CMakeLists.txt:731`) | W^X forbids `mprotect(PROT_EXEC)` on written pages. There is **no iOS JIT entitlement** (the macOS `com.apple.security.cs.allow-jit` entitlement does not apply to iOS); even if there were, asmjit's x86 emitter cannot target ARM64. No behavioural loss in practice because the JIT is dormant. |
| Native plug-in loading | `dlopen`/`dlsym` in `Plugin/PluginMan.cpp:282,325`, `Plugin/PluginRegistry.cpp:577`, `Plugin/PluginNative.cpp:14`; arch hooks via `dlsym(RTLD_DEFAULT, ...)` in `Emulator.cpp:790-817` | **Not available for user-installed code.** Data-only plug-ins (theme/inject/layout/keymap JSON) work; native code must be built into the app or embedded and signed at build time (§3) | All `dlopen` call sites must be compiled out or guarded on iOS. `RTLD_GLOBAL` symbol interposition has no equivalent. |
| Discord RPC | Built only for non-iOS (`CMakeLists.txt:80-83,911-917`) | Leave disabled | No Discord client IPC on iOS. |
| Wiz / CWiz toolchain | In-process `wizcore` (`CMakeLists.txt:620-712`), data/examples copied into bundle; ROP uses external RAC | Reuse `wizcore` as a static library; bundle `wiz/` data + examples as resources | RAC (external process) disabled; editor/compile/load-to-ROM flow remains viable. |
| Touch input | `Gui/TouchMouseTranslator.{h,cpp}` already maps SDL finger events → mouse click/drag/wheel with 500 ms long-press and 10 px drag threshold | Feed it from SDL iOS touch events; use ImGui touch-friendly flags and dedicated calculator key grid | Existing translator is a major head start; add UIKit-level gestures if moving to a native MTKView. |

---

## 2. App structure (Xcode project, bundle, entitlements, lifecycle)

### 2.1 Xcode project / build system

* Keep **CMake as the source of truth** and generate the Xcode project:
  `cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS ...` (exact flags in §6).
  Commit no `.xcodeproj`; if a hand-maintained wrapper project is needed for
  signing/SPM, generate it and keep CMake as the build driver.
* One application target (suggested name `CasioEmuIOS`, target from
  `CasioEmuMsvc/CMakeLists.txt`, `BUILD_EXECUTABLE=ON`), with:
  * `MACOSX_BUNDLE` + `MACOSX_BUNDLE_INFO_PLIST` pointing at an iOS-specific
    `Info.plist`;
  * `MACOSX_BUNDLE_GUI_IDENTIFIER` e.g. `com.telecomadm1145.casioemumsvc.ios`
    (must be unique in App Store Connect);
  * `XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS`, `DEVELOPMENT_TEAM`,
    `CODE_SIGN_STYLE` set per configuration;
  * resource copies for `models/`, `locales/`, `roms.db`, `resources/themes/`,
    `wiz` data/examples, fonts;
  * a scheme `CasioEmuIOS` (Debug/Release/Archive) with TestFlight archive
    sharing enabled.
* iOS is an **app**, not a shared library: Android's `BUILD_EXECUTABLE=OFF`
  / `libmain.so` pattern does **not** apply. Set `BUILD_EXECUTABLE=ON`.

### 2.2 Info.plist (key entries)

| Key | Value / purpose |
|---|---|
| `CFBundleIdentifier` | `com.telecomadm1145.casioemumsvc.ios` |
| `CFBundleDisplayName` | `CasioEmuMsvc` |
| `UILaunchScreen` | simple launch screen (no storyboard dependency) |
| `UISupportedInterfaceOrientations` | Landscape Left/Right + Portrait (iPhone); all four on iPad |
| `UIFileSharingEnabled` | `YES` — expose `Documents/` in Finder/Files |
| `LSSupportsOpeningDocumentsInPlace` | `YES` — open `.plugin`, `.zip`, model/ROM files in place |
| `CFBundleDocumentTypes` | importer for `.plugin`, `.zip`, `.g1m`/`.g2m`/`.g3m` if used, `.bin`, `.json` |
| `UIBackgroundModes` | **empty** — no background execution claim |
| `NSAppTransportSecurity` | default ATS (HTTPS only); document any local-only HTTP exception |
| `ITSAppUsesNonExemptEncryption` | `NO` (verify with export compliance; no custom crypto beyond public Ed25519/SHA-256) |
| `UIRequiredDeviceCapabilities` | `arm64` |

### 2.3 Entitlements — what is and is not needed

* **App sandbox:** iOS enforces a sandbox for every app automatically; there
  is **no `com.apple.security.app-sandbox` entitlement** (that is macOS-only).
  A minimal `.entitlements` file (possibly empty on a plain app target) is
  sufficient.
* **JIT:** there is **no JIT entitlement available to third-party iOS apps**.
  `com.apple.security.cs.allow-jit` is a *macOS Hardened Runtime* entitlement
  and cannot be used on iOS. W^X plus code signing forbids making written
  pages executable. Consequently: **asmjit JIT must be disabled** (`ASMJIT_NO_JIT`,
  drop `JitU8/*.cpp`) and the interpreter used. This costs nothing today
  because no code instantiates `JitU8`.
* No iCloud, Keychain-sharing, push or App Group capability is required for
  Phase 1/2. `get-task-allow` is inserted by Xcode for Debug automatically.
* Any embedded first-party dylib/framework must be **signed with the same
  team identifier and embedded at build time**; no entitlement grants the
  ability to load a newly downloaded, unsigned or differently-signed dylib.

### 2.4 Deployment target, devices, orientation, safe area

* Deployment target: **iOS 15.0** (wide device coverage; Xcode 16+ SDK),
  arm64 devices; simulator slice `x86_64` on this Intel host, `arm64` simulator
  on an Apple-Silicon CI runner. iPad and iPhone share one binary.
* Root controller: `UITabBarController` → per-tab `UINavigationController`.
  On iPad use `UISplitViewController` (`UISplitViewControllerColumnSecondary`
  etc.) for the model/file sidebar, collapsing to a pushed detail view on
  iPhone; respond to `traitCollection` size-class changes.
* Safe area: read `UIView.safeAreaInsets` (and `UIKeyboardWillShowNotification`
  insets), subtract from the SDL view/ImGui `DisplaySafeAreaInsets` before
  computing key-grid and panel layout; handle notches, Dynamic Island and home
  indicator; recompute on `viewWillTransitionToSize` / rotation.
* Orientation: prefer landscape for the calculator screen (396×224 framebuffer
  scaled to fit width), allow portrait for model browsing/editor; lock the
  calculator tab to landscape if its portrait layout proves unusable.
* Lifecycle: `applicationDidEnterBackground` → pause tick thread, flush RAM
  image/settings/snapshot (short `beginBackgroundTask` only for the flush),
  unregister SDL timers; `applicationWillEnterForeground` → re-acquire
  renderer if needed. No "continue running in background" claim.
* VoiceOver/Dynamic Type are out of scope for Phase 1; document as known
  accessibility gaps.

### 2.5 Data layout in the sandbox

```
<App.app>/                         read-only bundle resources
  models/ locales/ roms.db themes/ wiz/ docs/
<Documents>/                       user-visible via Files app
  Models/  Snapshots/  Exports/  ImportedPlugins/   (declarative only)
<Library/Application Support/CasioEmuMsvc>/
  settings.json  plugin-trust.json  Plugins/  plugin-cache/  wiz-work/
```
Replace `ExecutableDir()`, `$HOME/Library/Application Support` and CWD-relative
paths with a single `PlatformPaths` abstraction; iOS has no portable CWD and
`getenv()`-based configuration must be removed.

---

## 3. Plugin system on iOS

### 3.1 What exists today

* `.plugin` is a ZIP container with `plugin.json` (schema 1; types
  `Theme`, `Tab`, `Inject`, `UiRevamp`, `Tool`; `platforms` defaults to
  `{"macos"}`, `nativeEntry` marks native plug-ins — `PluginManifest.h`).
* `PluginRegistry` scans `~/Library/Application Support/CasioEmuMsvc/Plugins`,
  the app bundle's `plugins/` and `../Resources/plugins`, extracts to a
  SHA-256 cache, records trust, and `dlopen`s matching binaries
  (`PluginRegistry.cpp:156-320,577`).
* Native entry points: `fPluginLoad`, `fPluginLoadArch`, `fPluginApiVersion`,
  `fPluginAbiSignature`, optional `fPluginUnload`; architecture providers are
  registered through `IArchitectureRegistry` and their plain-C framebuffer /
  key hooks are resolved with `dlsym(RTLD_DEFAULT, ...)`.
* `PlatformTags.h` in the **main checkout** selects binaries named
  `<name>-<os>-<arch>.<ext>` (e.g. `my-plugin-ios-arm64-iphone.dylib` once the
  new tags exist). The iOS worktree predates this file.

### 3.2 iOS constraints

1. All executable code must be signed and part of the app's code-signed bundle.
   Writing a dylib into `Documents/` (or any sandbox directory) and calling
   `dlopen()` fails: the page cannot be made executable (W^X) and the signature
   is invalid.
2. Loading a dylib that *was* embedded and correctly signed at build time is
   possible, but the set of dylibs is fixed by the submitted binary; updates
   require an App Store/TestFlight release.
3. App Store Review Guidelines 2.5.2 and 3.3.2 prohibit apps downloading or
   executing code that changes their submitted functionality. A "plugin
   store" that installs native code would be rejected.
4. `RTLD_GLOBAL` + `dlsym(RTLD_DEFAULT, ...)` symbol interposition has no
   supported iOS equivalent; static registration must replace it.

### 3.3 Three implementation options

| Option | Description | Pros | Cons | Verdict |
|---|---|---|---|---|
| **(a) Static registration at build time** | Known plug-ins (e.g. CG100 provider, first-party themes) compiled into the app; registry gains `RegisterBuiltinPlugin()` / `RegisterArchitectureProvider()` called from app startup before `Emulator` construction; ImGui windows register through the normal host API | No dynamic loading; full access to host symbols; deterministic ABI; passes review | Plug-in set fixed at build time; each plug-in update = app update | **Recommended for native plug-ins (Phase 2+).** |
| **(b) Embedded, correctly signed dylibs** | Ship `<name>-ios-arm64-iphone.dylib` inside `Contents/Frameworks`, sign with the app's identity at build time, `dlopen` it from the bundle | Keeps ABI isolation and separate plug-in binaries | Still fixed at build time; extra signing/embedding complexity; no user-installed plug-ins; App Store must approve; `dlopen` of embedded frameworks is allowed, but this buys little over (a) | **Conditional** — only for a large first-party core where link-time coupling is unacceptable. |
| **(c) Pure-data "declarative" plug-ins only** | Accept ZIP packages containing `plugin.json` + theme/layout/injection/keymap JSON, assets; reject `nativeEntry`, show a clear "native plug-ins are not supported on iOS" message | Matches sandbox and review rules; reuses `PluginPayloads`, `PluginLayout`, `ThemeTokens`, trust/hash machinery; user-installable via Files app | No native extensibility | **Recommended baseline (Phase 1)** and kept for all phases. |

**Recommendation:** Phase 1 = (c) only; Phase 2 = (c) + (a) for first-party
native features; (b) only if a concrete first-party need justifies it. The iOS
plugin UI must not advertise native `.plugin` installation, and any
`dlopen`/`dlsym` code must be `#if`-guarded out. Startup ordering (plug-ins
registered before `Emulator` construction — see `DLL_PLUGIN_FORMAT.md §2.1` in
the main checkout) must be preserved for architecture providers.

### 3.4 Host format change: new platform tags (future implementation)

`PlatformTags.h` currently has no `ios` OS tag and no `arm64-iphone` arch tag
(`kKnownOsTags = {macos, linux, windows, android, any}`,
`kKnownArchTags = {x64, x32, arm64, arm64-android, any}`). Proposed additive
change (plan only, not applied here):

1. Add `ios` to `kKnownOsTags`, constants `kOsIos`, `OsTagDescription("ios")`.
2. Add `arm64-iphone` to `kKnownArchTags`, constants `kArchArm64Iphone`,
   `ArchTagDescription("arm64-iphone")`; optionally add simulator tags
   (`x64-iphone-simulator`, `arm64-iphone-simulator`) if simulator plug-in
   testing is ever needed.
3. Add a compiler-predefine branch **before** the generic `__APPLE__` case:
   `#if defined(__APPLE__) && defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE`
   (`#include <TargetConditionals.h>`) → `CASIOEMU_PLATFORM_OS "ios"`;
   on `__aarch64__`/`__arm64__` → `CASIOEMU_PLATFORM_ARCH "arm64-iphone"`.
4. Mirror in `cmake/CasioPlatform.cmake`: detect `IOS` /
   `CMAKE_SYSTEM_NAME STREQUAL "iOS"` before the `APPLE` branch, set
   `CASIOEMU_TARGET_ARCH=arm64-iphone` for arm64 device builds, add both
   values to the validation lists; apply with `casioemu_apply_platform_tags()`
   to the app and test targets.
5. Update `BUILD_TARGETS.md` tag table with `iOS arm64 → ios / arm64-iphone`.
6. Update `PluginManifest` platform validation/defaults so `platforms` may
   contain `"ios"` (currently defaults to `{"macos"}`).
7. Filename parsing already iterates known OS/arch suffixes, so hyphenated
   arch tags such as `arm64-iphone` work automatically; add tests for
   `ParseTaggedNativeFileName("my-plugin-ios-arm64-iphone.dylib")`,
   `SelectPlatformBinariesForRoot`, and `NoCompatibleBinaryError()` producing
   `"No native binary for ios-arm64-iphone"`.

Format examples after the change:

```
my-plugin-ios-arm64-iphone.dylib     (device, build-time embedded only)
my-plugin-any-any.json               (declarative, works everywhere)
```

---

## 4. UI adaptation — desktop shell → iOS navigation

### 4.1 What must be adapted

Desktop shell (`Gui/Shell/Shell.cpp`, `StartupUi/StartupUi.cpp`) currently
provides a menu bar (File / View / Emulator / Tools / Settings / Help),
a file-system sidebar with context menus, tabs, bottom Output/Problems/
Terminal panel, tool dock, calculator panel, welcome page, command palette,
model editor tabs (`General`, `Buttons`, `Sprites`, `Status`) and a separate
debugger window (`Gui/Ui.cpp`). None of these OS-level affordances exist on iOS.

### 4.2 Proposed iOS information architecture

```
UITabBarController
 ├─ Emulator  (UINav → SDL/Metal view: calculator + ImGui overlays;
 │            toolbar: power/reset/diag, pause, zoom, keyboard toggle)
 ├─ Models    (UINav → model list/detail, import via UIDocumentPicker,
 │            launch, edit model; iPad: UISplitViewController sidebar)
 ├─ Editor    (UINav → Wiz/CWiz editor, Build, Load, Build & Run;
 │            ImGui TextEditor hosted in the SDL view or a native UITextView)
 ├─ Tools     (ImGui debugger panels as tabs; plugin manager (declarative),
 │            settings shortcut, logs)
 └─ Settings (in-app form backed by SettingsStore JSON; theme, zoom,
              localization, renderer, plugin trust; "Open in Settings" deep link)
```

* Desktop menu actions map to: toolbar buttons, `UIContextMenuInteraction`
  (long-press), `UIAlertController` action sheets, and the existing command
  palette. Keep ImGui's internal menus inside the emulation view only where
  touch-sized.
* iPad: `UISplitViewController` with sidebar; iPhone: collapsed list → push.
  No floating OS windows; debugger becomes an ImGui tab. Defer multi-scene
  iPad support.
* Share/export uses `UIActivityViewController` (snapshots, screenshots,
  exported model packages); import uses `UIDocumentPickerViewController`
  (`.plugin`, `.zip`, model/ROM files) writing to `Documents/`.

### 4.3 Touch input and on-screen keyboard

* Reuse `Gui/TouchMouseTranslator`: it already converts SDL finger
  down/up/motion into mouse motion, click, long-press (500 ms → right click),
  drag threshold (10 px), two-finger pan and wheel emulation. iOS SDL emits
  `SDL_FINGERDOWN/UP/MOTION`, so the existing routing in `casioemu.cpp` can be
  kept.
* Set `SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1")` before window creation so ImGui
  text fields trigger the system keyboard; pair with an `UITextField`
  `inputAccessoryView` for IME completion and a "Done" key. On
  `UIKeyboardWillShowNotification`, shrink the SDL view / scroll the ImGui
  frame so the focused field stays visible.
* Calculator keys: render an ImGui virtual keypad (or UIKit `UIButton` grid)
  with ≥ 44×44 pt targets, mapped to the existing `(row, col)` key matrix
  used by `Emulator::Key`/`ArchSetHeldKey`; support multitouch by tracking
  `SDL_FingerID` per key, long-press for alternate legends, and a visible
  "keyboard" toggle. Pinch-zoom and two-finger pan handled by the translator;
  provide zoom buttons for accessibility.
* Add `UIImpactFeedbackGenerator` haptics on key press (optional, iOS-only).

### 4.4 Settings UI

Use an **in-app** Settings screen (`UITableViewController`-style or ImGui
form) bound to `SettingsStore`; a `Settings.bundle` may expose 2–3 read-only
keys (version, build) but cannot write the JSON store. Localization can reuse
the existing `locales/*.lc` files; iOS system language can seed
`SettingsStore::language` on first launch.

---

## 5. CG100 (SH4) plug-in feasibility

### 5.1 Current implementation

* Windows: `CG100Sh4DllPlugin/` wraps a Go `c-shared` core (`cg100core.dll`,
  16 exports including `cg100_power_on`) behind `IArchitectureProvider`; host
  side adds `ArchPluginApi.h`, `HW_CG100`, `Emulator::SetupArchitectureCore`,
  `ArchFrame()` (`docs/DLL_PLUGIN_FORMAT.md §7-9` in the main checkout; note the
  Windows fork renumbers hardware id 10 while the authoritative host uses 14).
* macOS: a `.plugin` Mach-O wrapper around `libcig100core.dylib` with
  `fPluginLoadArch`; the iOS worktree already contains integration hooks
  (`Emulator.cpp:790-817`, `dlsym(RTLD_DEFAULT, "casioemu_arch_core_*")`).
* Contract: 396×224 **big-endian RGB565** framebuffer (host byte-swaps),
  `(row, col)` key matrix, 32 MiB flash model, ABI handshake
  `fPluginApiVersion=3`, `fPluginAbiSignature=8194` (0x2002).

### 5.2 Go core on iOS

`GOOS=ios GOARCH=arm64 CGO_ENABLED=1 go build -buildmode=c-shared` is **not a
supported Go toolchain combination**: cgo cross-compilation for iOS requires
X SDK headers and Apple's toolchain plumbing that `go build` does not
provide; `gomobile` produces frameworks/bindings rather than a `c-shared`
dylib with this C ABI. Even if a static archive were obtained through
`gomobile bind`, bridging it into the CMake target and the C++ architecture
ABI is substantial work, and it still could not be installed at runtime.
**Verdict:** not viable for Phase 1/2; at best a Phase 3 first-party static
embed, medium-high effort, with no guarantee the exported-C ABI is preserved.

### 5.3 Pure-C/C++ interpreter path

`/Volumes/Code/casio-tools/cg100-sh4-cpp/` is a self-contained C++20 SH4
interpreter with vendored ABI headers, KEYSC/LCD/RTC MMIO, save-state and a
`fPluginLoadArch` wrapper — i.e. exactly the shape that could be **statically
linked** into an iOS app (no Go, no JIT, no dylib). But its own `DESIGN.md`
and `STATUS.md` say explicitly: **DO NOT USE YET** — the OS boot stalls in the
RTC 64 Hz poll loop at `0xA0000B5C` after ~180 M instructions; instruction
coverage, INTC/timers, NOR flash command state machine, LCD DMA, and boot
strap are incomplete. Self-tests pass but functional boot gates are unmet.
**Verdict:** architecturally the right long-term path, functionally months away
from replacing the authoritative Go core.

### 5.4 Recommendation

* Exclude CG100 from Phase 1 and Phase 2; hide the model if its core is not
  registered (avoid a hard `PANIC` on launch).
* In Phase 3, choose one: (i) static C++ interpreter once it boots OS 2.00, or
  (ii) first-party static Go framework if a spike proves `gomobile` can export
  the required ABI. Keep hardware id **14** (authoritative host), reuse the
  BE-RGB565 byte-swap and key bridge, and register the provider through the
  static registry of §3.3(a).
* The nX-U8 asmjit JIT and the SH4 core are unrelated: SH4 already interprets,
  so iOS JIT restrictions do not change CG100 performance.

---

## 6. Build and integration steps (future execution outline)

1. **Synchronise sources.** From the main checkout, forward-port
   `cmake/CasioPlatform.cmake`, `cmake/toolchain-*.cmake`,
   `BUILD_TARGETS.md`, `PlatformTags.h`, `ArchPluginApi.h`, registry/
   platform-tag changes and the CG100 host integration into the `port/ios`
   branch; coordinate with the uncommitted work in the main checkout.
   `git submodule update --init --recursive` to populate SDL 2.32, SDL_image,
   asmjit, sentry-native, discord-rpc, wiz.
2. **Toolchain.** Xcode 16.x with iOS 17+ SDK, CMake ≥ 3.28 (Xcode generator),
   Apple Developer account. Intel host: cross-compile `arm64` device binaries
   is possible, but execution requires an arm64 device or an Apple-Silicon
   CI runner; `x86_64` simulator smoke tests run locally.
3. **Configure (illustrative, not executed):**
   ```sh
   cmake -S . -B build-ios -G Xcode \
     -DCMAKE_SYSTEM_NAME=iOS \
     -DCMAKE_OSX_SYSROOT=iphoneos \
     -DCMAKE_OSX_ARCHITECTURES=arm64 \
     -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
     -DBUILD_EXECUTABLE=ON \
     -DCASIOEMU_ENABLE_SENTRY=OFF \
     -DCASIOEMU_PLUGIN_TESTS=OFF \
     -DCASIOEMU_WIZ_TOOLCHAIN=ON \
     -DCASIOEMU_PLATFORM_OS=ios \
     -DCASIOEMU_PLATFORM_ARCH=arm64-iphone
   ```
   Force `DISABLE_SENTRY`, `ASMJIT_NO_JIT` and any `CASIOEMU_IOS` guard;
   exclude `JitU8/*.cpp`, `CrashHandler/*.cpp`, `sdl_win32_extra*`,
   `Ext/MacWindow*`, Discord/Sentry sources from the iOS target. iOS simulator
   builds switch `CMAKE_OSX_SYSROOT=iphonesimulator` and arch `x86_64`.
4. **Dependencies.** SDL2/SDL_image from the vendored submodules (iOS UIKit
   backend is present in SDL 2.32); vendor upstream `imgui_impl_metal.mm/.h`;
   keep `miniz`, `tinyxml2`, `monocypher`, `wizcore` static; build libcurl
   against Apple TLS (or replace with `NSURLSession`); optional `sentry-cocoa`
   via SPM. CocoaPods is unnecessary; SPM is preferred for any Apple framework.
5. **Assets.** Copy `models/`, `locales/`, `roms.db`, themes, Wiz data/examples
   and docs into the app bundle at build time; add a first-launch migration
   that populates Application Support and Documents. Audit asset size (CG100
   32 MiB flash model) and strip unnecessary docs from the shipping bundle.
6. **Signing / provisioning.** Register the bundle id, create Development and
   App Distribution certificates + provisioning profile (or Xcode automatic
   signing), add devices for ad-hoc builds, set entitlements, code-sign every
   embedded framework/dylib with the same team ID. Add `PrivacyInfo.xcprivacy`
   based on `PRIVACY.md` (required for App Store submissions).
7. **Verification.** Local: `xcodebuild` simulator build + smoke test with
   software/OpenGL ES renderer, data-plugin install via Files, settings
   persistence, orientation/safe-area checks. Remote: arm64 device build on an
   Apple-Silicon runner, TestFlight internal → external beta, then App Store
   review. Verify exports with `file`/`codesign -dv`/`plutil -lint` (no devices
   in this environment).
8. **Review / distribution constraints.** Expect scrutiny of any "plugin"
   language: describe it as data themes/layouts, no executable download
   (guidelines 2.5.2, 3.3.2). No self-update, no JIT, no background
   execution. Sideloading alternative: ad-hoc/development IPA (AltStore,
   Sideloadly, Xcode Devices) with 7-day free provisioning — useful for
   internal testing, not public distribution.

---

## 7. Effort estimate / phased plan

Estimates assume one experienced C++ engineer plus part-time iOS assistance;
each phase ends with a testable IPA. No time limit is imposed on this planning
task, but the estimates are engineering calendar weeks.

| Phase | Goal | Deliverables | Exit criteria | Est. |
|---|---|---|---|---|
| **0 — Sync & tooling** | Make the branch buildable-in-principle | Forward-port platform tags/CMake, init submodules, iOS CMake target, Info.plist, signing skeleton, `PlatformPaths` shim | CMake Xcode generation succeeds; simulator archive produced (execution optional) | 1–2 w |
| **1 — Data-only app** | Playable calculator, no native plugins | SDL UIKit + ImGui (SDL renderer or Metal), touch translator, virtual keys, safe area, bundled models/locales, JSON settings, declarative plugin loader, Sentry/fork/JIT disabled | Boots an fx-580VN X/fx-880BTG ROM from bundle, keys respond to touch, theme/layout plugin installs from Files and renders | 3–5 w |
| **2 — Static core & full shell** | Feature-complete first-party app | UIKit tab/split shell, navigator for model/editor/tools/settings, Wiz in-process build & load, debugger as ImGui tabs, snapshot share/import, static registration API for first-party native features, NSUserDefaults mirror, crash diagnostics | iPad/iPhone layout passes, settings persist across relaunch, static first-party native plug-in registers before `Emulator` construction | 4–8 w |
| **3 — First-party native plug-ins** | As far as Apple allows | Signed embedded static/framework plug-ins or static CG100 provider; `ios`/`arm64-iphone` tags in host format; optional sentry-cocoa; App Store submission | No unsigned/dynamic plug-in installation path remains; TestFlight approval; CG100 only if its core matures | 8–16+ w (CG100 core work additional, outcome uncertain) |

Desktop "arbitrary plug-in" parity is **explicitly out of scope** in all phases.

---

## 8. Risk table

| Risk | Impact | Likelihood | Mitigation | Residual |
|---|---|---|---|---|
| JIT blocked (W^X, no iOS JIT entitlement, asmjit x86-only) | No JIT acceleration | Certain | `ASMJIT_NO_JIT`, drop `JitU8` sources, rely on interpreter (already the runtime path) | None; performance only |
| Dynamic plug-ins / `dlopen` blocked | Cannot ship desktop plug-in ecosystem | Certain | Data-only loader + static/embedded first-party registration; guard all `dlopen`/`dlsym` code | No arbitrary third-party native plug-ins |
| App Store rules 2.5.2 / 3.3.2 (downloaded code) | Rejection | High if native plug-in UI remains | Remove native install UI; document declarative-only plug-ins; no self-update | Review still discretionary |
| No background execution | Emulation pauses when app backgrounds | Certain | Save state/pause in lifecycle callbacks; no `UIBackgroundModes`; no long background tasks | Expected behaviour |
| `fork`/`exec` unavailable (RAC, Crashpad, ImGui shell) | Crash reporter / ROP features unusable | High | Disable Sentry-native + ROP on iOS; ImGui already guards iOS shell helper; compile RAC in-process only after licence review | ROP unavailable in Phase 1/2 |
| Path/env assumptions (`HOME`, CWD, `_NSGetExecutablePath`, `CASIOEMU_*_DIR`) | Files not found / writes fail | High | `PlatformPaths` shim over Foundation directories; bundle resources read-only | Needs thorough code audit |
| ImGui Metal backend missing from tree | No rendering path if SDL renderer is dropped | Medium | Vendor upstream `imgui_impl_metal` or keep SDLRenderer2 + SDL Metal renderer | Extra dependency to maintain |
| SDL iOS event/view integration | Touch, IME, rotation bugs | Medium | Feed `TouchMouseTranslator`, `SDL_HINT_IME_SHOW_UI`, safe-area-aware layout, simulator tests | Device-specific quirks possible |
| Main-thread stalls / watchdog kills | App terminated by iOS | Medium | GCD for scanning/I/O, pause emulation before heavy operations, keep smoke tests | Needs profiling on device |
| Code signing of embedded dylibs/frameworks | Launch failure | Medium (only Phase 3) | Same team ID, embed + sign at build, no runtime-installed code | Updates require app release |
| CG100 core viability (Go cgo iOS unsupported; C++ rewrite stalls at RTC loop) | CG100 cannot ship | High | Exclude from Phases 1–2; static C++ interpreter long-term; spike `gomobile` only in Phase 3 | CG100 likely absent initially |
| Intel host cannot run iOS arm64 binary | No local device verification | Certain | Simulator smoke tests locally, arm64 CI runner for device binaries, TestFlight for real validation | Plan-only by design |
| 32 MiB CG100 flash + model assets | Large bundle / memory pressure | Low–Medium | On-demand model import to Documents; exclude unused models from bundle | Manageable |
| Sentry/crash diagnostics absent | Harder field debugging | Medium | sentry-cocoa via SPM in Phase 3, structured log export via share sheet | No native crashpad |
| Accessibility (VoiceOver, Dynamic Type) | App Store quality / usability | Medium | Larger touch targets now; full accessibility deferred | Known gap |

---

## 9. Cross-reference — what is reusable from macOS / Linux / iOS shared code

### 9.1 `BUILD_TARGETS.md` (main checkout)

* Documents a **four-target** build (macOS, Linux, Windows, Android) driven by
  `cmake/CasioPlatform.cmake` and `casioemu_apply_platform_tags()`; the tag
  mapping table (§1) currently ends at Android `arm64-android`. iOS should be
  added as `ios / arm64-iphone`.
* Reusable patterns:
  * target-based (not host-based) platform detection via `CMAKE_SYSTEM_NAME`;
  * per-cross-build feature flags (`CASIOEMU_ENABLE_SENTRY=OFF`,
    `CASIOEMU_PLUGIN_TESTS=OFF`, `BUILD_MCP_PLUGIN=OFF`,
    `CASIOEMU_WIZ_TOOLCHAIN=OFF`) — iOS follows the same "strip desktop-only
    services" approach;
  * asset staging into a target-specific location (Android copies locales/
    `roms.db` into `app/src/main/assets`; iOS copies into the app bundle);
  * verification via artefact inspection (`file`, headers, plist) when the
    target OS is unavailable — exactly the constraint of this task.
* Known gaps noted there (untested target execution, absolute
  `/Users/kin/Desktop/Wiz` paths, plugin fixture root) must be fixed or
  parameterised for iOS CI.

### 9.2 `PlatformTags.h` (main checkout)

* Provides OS/arch constants, compiler-predefine fallbacks, tag matching,
  `<name>-<os>-<arch>.<ext>` parsing and `NoCompatibleBinaryError()`.
* `ParseTaggedNativeFileName()` matches **known suffixes**, so adding
  `arm64-iphone` to `kKnownArchTags` immediately makes names such as
  `my-plugin-ios-arm64-iphone.dylib` parseable; no parser rewrite needed.
* Its `#undef` of injected `CASIOEMU_PLATFORM_*` definitions means CMake
  definitions must be well-formed C string literals — relevant when wiring
  `casioemu_apply_platform_tags()` for iOS (§3.4).
* This iOS worktree does **not** contain the header or the CMake module: they
  must be forward-ported before the tag work.

### 9.3 Shared code inventory

| Reuse directly | Rework / guard | Drop on iOS |
|---|---|---|
| `src/Chipset`, `src/Peripheral`, `src/Containers`, `src/Emulator.*` core logic, `src/Gui` ImGui windows, `src/Settings`, `src/locales`, `src/Rop` (except external RAC), `src/Terminal`, `src/Ctl`, miniz, tinyxml2, monocypher, in-process `wizcore`, `Gui/TouchMouseTranslator.*`, plugin data payloads/layout/theme/trust code, model/SVG loaders | `Settings::ExecutableDir`/`DefaultFilePath`, `PluginRegistry::SettingsDir/CacheDir`, `casioemu.cpp` renderer hint files, `RendererBackend.h` driver hint, `PluginMan`/`PluginRegistry` `dlopen` calls (guard), `ImGui` backends (add Metal), `Emulator.cpp` multi-window creation | `JitU8` (x86 JIT), `CrashHandler`/Sentry-native, Discord RPC, `sdl_win32_extra*`, `Ext/MacWindow*`, `PluginNative.cpp` dlopen path, external RAC execution, Windows DLL plugin shims |

**Bottom line:** the emulator core is portable; the port is mostly a shell,
sandbox-path and plug-in-policy exercise, with CG100 and JIT as the two hard
functional limitations.
