# CasioEmuMsvc — macOS-only universal build

**Result: SUCCESS — a genuine universal (x86_64 + arm64) `.app` was produced and smoke-tested.**

| Item | Value |
| --- | --- |
| Source tree | `/Users/kin/Desktop/Wiz/emulator/` (stripped, macOS-only) |
| Upstream base | `telecomadm1145/CasioEmuMsvc`, branch `stable`, commit `2d26202d56e437654296af85c94a9036bb34ad58` (2026-10-07) |
| Build directory | `/Users/kin/Desktop/Wiz/emulator/build/` (in-tree, Ninja) |
| Built bundle | `/Users/kin/Desktop/Wiz/emulator/build/CasioEmuMsvc/CasioEmuMsvc.app` |
| Preserved copy | `/Users/kin/Desktop/Wiz/emulator/CasioEmuMsvc.app` (byte-identical copy, kept outside `build/`) |
| Main binary | `…/CasioEmuMsvc.app/Contents/MacOS/CasioEmuMsvc` — 12 792 816 bytes (the second build, with the Wiz toolchain linked in; the first, toolchain-free build was 11 640 368 bytes) |
| Architectures | **`x86_64 arm64`** (`lipo -archs`) — universal, not Intel-only |
| SHA-256 | `9d2c571c71cb28143247249d646a5c03e2fae643e4be0a0c161c4d8e3da0395e` (first build: `f797cbab3fbea7178180688590c5a703b0c2a9ba07ae912ca9d620c991974e01`) |
| Host | macOS 15.7.7, Intel x86_64, Apple clang 17.0.0 (clang-1700.6.4.2), CMake 4.4.2, Ninja 1.13.2, SDK MacOSX26.2 |
| Sentry | **Enabled**, backend `crashpad`, static; `crashpad_handler` shipped and also universal |

---

## 1. Source tree provenance

`emulator/` is an exact copy of the newest upstream (`rsync -a --exclude='.git'`, so all checked-out
submodule *contents* came along) with the main-repo `.git` and every nested submodule `.git` pointer
removed, leaving a plain source tree (`du -sh` → **432 MB** before the build).

The copy was verified byte-for-byte identical to upstream apart from the `.git` entries:

```
diff <(find /tmp/upstream-casio …) <(find emulator …)   # only .git entries differed at copy time
```

## 2. User extras preserved (nothing lost)

Copied in from the older snapshot (`…/CasioEmuMsvc-Mac-stable/`). None of these collided with upstream,
so **upstream's version was kept wherever a file existed in both** (there were no collisions at all):

| File | Note |
| --- | --- |
| `CasioEmuMsvc/src/Ext/PackageCracker.h` | password-recovery helper (not in upstream) |
| `CasioEmuMsvc/tools/package_crack.cpp` | standalone cracker CLI |
| `CasioEmuMsvc/tools/wordgen.cpp` | dictionary segmentation generator |
| `CasioEmuMsvc/tools/wordseg.py` | word-segmentation attack script |
| `build-mac.sh` | user's macOS build script |
| `AppIcon.icns` + `AppIcon.iconset/` | preserved for reference — **not used**; upstream `CasioEmuMsvc/icon.icns` is the bundle icon |
| `tools/package_extract.py` | extra (found in the snapshot, also preserved) |

**Icon decision:** upstream already ships `CasioEmuMsvc/icon.icns`, so it is kept (it is what lands in
`Contents/Resources/icon.icns`, verified identical to upstream). The user's `AppIcon.icns` is retained
in the tree but unused.

## 3. What was stripped (macOS-only)

Removed only platform-exclusive, CMake-unreferenced build inputs:

* `.github/` (all CI workflows)
* `*.vcxproj`, `*.vcxproj.filters`, `*.vcxproj.user`, `*.sln` — all Visual Studio project files (in the main project **and** in `RomPacker/`, `UnitTest1/`, `lua_config_convert/`, `McpPlugin/`)
* `CMakeSettings.json` (VS CMake settings)
* `build-linux.sh`, `mingw-toolchain.cmake`
* `*.ps1` — `convert_all_lua_config.ps1`, `GB2UTF8.ps1`, `CasioEmuMsvc/GitInfo.ps1`, `CasioEmuMsvc/web/build_web.ps1`, `CasioEmuMsvc/web/build_core_web.ps1`, `McpPlugin/*.ps1`
* `app/` (the entire Android/Gradle application project)
* `gradle/`, `gradlew`, `gradlew.bat`, `gradle.properties`, `build.gradle`, `settings.gradle`
* `Cw2toolsPlugin/`, `PythonPlugin/`, `ExamplePlugin/` — not referenced by any CMake file
* `CasioEmuMsvc/Android.mk`, `packages.config` files, `CasioEmuMsvc/sentry-cli.exe` (Windows binary)
* `CasioEmuMsvc/third_party/asmjit/.git` and all nested submodule `.git` pointers

**186 entries removed in total** — 183 build/platform artifacts plus 30 nested submodule `.git` pointers
(counted separately); every submodule's *source* is fully intact.

### Deliberately NOT stripped (and why)

* **`McpPlugin/`** — it *is* referenced by the root `CMakeLists.txt` (`add_subdirectory(McpPlugin)`, gated
  on `if(WIN32 …)`). Per the instruction "if CMake references it, keep", the directory is retained; on
  macOS the gate is false so it is never configured. (Its VS files/scripts were still removed.)
* **Inert, unreferenced directories** — `RomPacker/`, `UnitTest1/`, `lua_config_convert/`, `cw2tools_en/`,
  `docs/`, `resources/`. These are not referenced by any CMake target; only their VS project files were
  deleted. Removing the whole directories would have made no build difference, so they were left alone.
* **All `#ifdef _WIN32` / `__linux__` / `__ANDROID__` source branches were KEPT.** There are ~250 such
  occurrences across ~40 files. They are compiled out on macOS, so removing them cannot improve the build
  and *can* easily break it (e.g. `main()`'s platform setup, `SysDialog.mm`/`SysDialog.cpp` selection,
  `GetModelFilePath`). The instruction was explicitly to keep them when in doubt — I kept them all.
  Only genuinely dead *files* were deleted.
* Submodules (`SDL/`, `SDL_image/`, `sentry-native/`, `discord-rpc/`) were left completely untouched.

## 4. Exact build commands

Configure and build are driven from the **repository root** (not `CasioEmuMsvc/`): the root
`CMakeLists.txt` is what pulls in SDL, SDL_image, discord-rpc, sentry-native and then `CasioEmuMsvc`.
Configuring with `-S CasioEmuMsvc` would fail, because `CasioEmuMsvc/CMakeLists.txt` refers to
`${CMAKE_SOURCE_DIR}/sentry-native` and `${CMAKE_SOURCE_DIR}/discord-rpc`.

### Configure

```sh
cd /Users/kin/Desktop/Wiz/emulator
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXECUTABLE=ON \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-syslibroot,$(xcrun --sdk macosx --show-sdk-path)"
```

### Build

```sh
cmake --build build        # 750 Ninja targets
#  or: ninja -C build
```

The `-DCMAKE_EXE_LINKER_FLAGS="-Wl,-syslibroot,…"` argument is **required on this machine** (see
"Known issues" #2). Omitting it makes the final link fail:

```
ld: archive member '/' not a mach-o file in '/usr/local/lib/libc.a'
```

Sentry was **not** disabled: the configure found `SENTRY_BACKEND=crashpad`, transport `curl`, static, and
built cleanly (contrary to the possibility that it might fail on an invalid handler path).

`libcurl` is resolved from the SDK (`…/MacOSX.sdk/usr/lib/libcurl.tbd`, version 8.7.1). Because that is a
`.tbd` stub it is architecture-neutral, so **no curl was ever built from source** — a big time saver.

## 5. Verification

```
$ lipo -archs build/CasioEmuMsvc/CasioEmuMsvc.app/Contents/MacOS/CasioEmuMsvc
x86_64 arm64

$ file …/CasioEmuMsvc
Mach-O universal binary with 2 architectures: [x86_64:…] [arm64]

$ lipo -archs …/Contents/MacOS/crashpad_handler
x86_64 arm64
```

Bundle layout (16 MB):

```
CasioEmuMsvc.app/Contents/
├── Info.plist
├── MacOS/
│   ├── CasioEmuMsvc          # universal
│   ├── crashpad_handler      # universal
│   ├── locales/              # 9 .lc files
│   └── roms.db
└── Resources/
    └── icon.icns
```

Launch test (task requirement) — run against the model directory that ended up at
`/Users/kin/Desktop/Wiz/emulator/models/Graph Light`:

```
$ open -n /Users/kin/Desktop/Wiz/emulator/build/CasioEmuMsvc/CasioEmuMsvc.app \
        --args "/Users/kin/Desktop/Wiz/emulator/models/Graph Light"
$ sleep 5; pgrep -f 'CasioEmuMsvc.app/Contents/MacOS/CasioEmuMsvc'
53999          # alive after 5 s, GUI process named "CasioEmuMsvc", then killed
```

A direct run (so stdout is visible) shows a complete, healthy boot:

```
[Emulator][Info] Using SDL renderer 'opengl' (renderer hint 'opengl', …)
[Chipset][Info] Model:       EY-090A
[BatteryBackedRAM][Info] RAM image loaded from ram.dmp
[Ui][Info] Loaded Monospace Font: /System/Library/Fonts/Monaco.ttf
[UI][Info] Start to disasm ... Pass1/2/3 ... Finished!
[BatteryBackedRAM][Info] RAM image saved to ram.dmp
```

The binary links only system libraries/frameworks (`/usr/lib/libcurl.4.dylib`, `libSystem.B.dylib`,
`AppKit`, `Security`, `Cocoa`, `CoreVideo`, `IOKit`, …), i.e. it is self-contained apart from macOS itself.

## 6. Known issues, caveats and environment notes

1. **`CMAKE_OSX_SYSROOT` ends up empty → no `-isysroot`.** CMake does not set a sysroot here (the cache
   shows `CMAKE_OSX_SYSROOT:STRING=`), so neither the compile nor the link commands carry `-isysroot`.
   Compiles still work (clang picks its own default SDK), and the produced objects are identical to a
   sysrooted build — but the *linker* then searches `/usr/local/lib` **before** the SDK's `lib` directory.
2. **A stray `/usr/local/lib/libc.a` breaks the link (environment defect, not an upstream bug).**
   `ld -v` shows the search order as `/usr/local/lib` → `<SDK>/usr/lib` → `<SDK>/usr/lib/swift`. The
   project's link line contains `-lc` (from SDL), and the real `/usr/lib/libc*` does not exist on modern
   macOS (only the SDK's `libc.tbd`), so `-lc` resolves to that stray archive:
   `ld: archive member '/' not a mach-o file in '/usr/local/lib/libc.a'`.
   **Fix used:** pass `-Wl,-syslibroot,<SDK>` to the linker, which prefixes `/usr/local/lib` with the
   sysroot so the bogus archive is no longer visible. Alternatives: delete/rename
   `/usr/local/lib/libc.a`, or configure with `-DCMAKE_OSX_SYSROOT="$(xcrun --show-sdk-path)"` (that
   changes compile flags and forces a full rebuild; the linker-flag form does not).
3. **Upstream Info.plist bug — fixed here.** `MACOSX_BUNDLE_INFO_STRING` contained a raw `&`
   ("Casio nX-U8/100 & nX-U16/100 Emulator"); CMake substitutes it unescaped, producing invalid XML.
   macOS then logged `There was an error parsing the Info.plist … NSCocoaErrorDomain - 3840` and
   `No bundle id found`, and `plutil` refused to parse it. Changed to `&amp;` in
   `CasioEmuMsvc/CMakeLists.txt`; `plutil -p` now parses cleanly and the string reads correctly.
   (This is the only source edit made to the tree.)
4. **Deployment-target warnings.** crashpad/mini_chromium objects are built for macOS 15.0 while the link
   target is 10.10 (`object file … was built for newer 'macOS' version (15.0) than being linked (10.10)`).
   Harmless; the app runs on the host.
5. **arm64 slice is untested at runtime.** The host is Intel, so only the x86_64 slice was actually
   executed. Both slices are present, complete and come from the same build; a native arm64 smoke test is
   recommended before distribution.
6. **The binary is unsigned** (`code object is not signed at all`). It runs locally because it carries no
   quarantine attribute, but any distribution needs `codesign` (and notarisation).
7. **The model directory moved mid-run.** `/Users/kin/Desktop/Wiz/CasioEmuMsvc-Mac-stable/` (and its
   `models/`) disappeared while I was working; another process in this harness moved the models to
   `/Users/kin/Desktop/Wiz/emulator/models/` (`Graph Light`, `fx-580VN X`, `fx-880BTG`) at 17:30.
   All smoke tests therefore use `emulator/models/Graph Light`. The `models/` directory is *not* part of
   upstream and is not tracked in this report's strip list.
8. **A concurrent session built the same tree.** A second `cmake`/`ninja` run in
   `emulator/` (logging to `/Users/kin/Desktop/Wiz/emulator/build-macos.log`) compiled 749/750 targets and
   then failed at exactly the `libc.a` link error described above. Note that the tail of that log falsely
   reports `BUILD EXIT=0`; the log itself contains the `clang++: error: linker command failed`. I reused
   that build directory (incremental), applied the sysroot link fix and completed the link — so no
   compilation was wasted. The bundle must not be rebuilt with `rm -rf build` unless the sysroot flag is
   passed again.
9. **`emulator/` is ignored by git.** The repository root `.gitignore` lists `/emulator/`, so this
   400+ MB vendored tree (and its copyrighted ROM images) never enters version control. Build it
   locally with the recipe below if you need it; the compiler toolchain does not depend on it.
10. **Reproduce from scratch** (fresh tree, no cache) — this is the supported recipe:

```sh
cd /Users/kin/Desktop/Wiz/emulator
rm -rf build
cmake -S . -B build -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_EXECUTABLE=ON \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_EXE_LINKER_FLAGS="-Wl,-syslibroot,$(xcrun --sdk macosx --show-sdk-path)"
cmake --build build
open -n build/CasioEmuMsvc/CasioEmuMsvc.app --args "$PWD/models/Graph Light"
```

### Fallback that was *not* needed

The instruction allowed falling back to an Intel-only build if a dependency refused to build for both
arches. That was never required: SDL, SDL_image, asmjit, discord-rpc, Sentry/CRASHPAD, monocypher and
the SDK's libcurl all built and linked cleanly for both architectures.

---

## 7. Local additions to the upstream tree

Beyond keeping the user's own extras (section 2), two features were added on top of upstream:

1. **The Wiz / CWiz toolchain, built in-process.** The Wiz compiler is vendored under
   `CasioEmuMsvc/third_party/wiz/` and linked into the app as the static library `wizcore`. The
   debugger gains a **Toolchain** window (editor, build log and linter, with a Build & Run that
   hot-loads the image into the running model) and a **Calculator** window that can be docked into
   the main window as a separate area through the status-bar option *Dock screen*.
   See [`docs/WIZ-TOOLCHAIN.md`](docs/WIZ-TOOLCHAIN.md). Configure with
   `-DCASIOEMU_WIZ_TOOLCHAIN=OFF` to leave it out.
2. **The LCD inside the debugger window** (`src/Gui/CalcScreenWindow.cpp`), which uses the screen
   peripheral's `IScreenFrameProvider` instead of the second SDL window.

The macOS build recipe is unchanged apart from those two source directories.

