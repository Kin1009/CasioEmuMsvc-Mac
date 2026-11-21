# CasioEmuMsvc — Windows port (MinGW-w64 x86-64)

This branch (`port/windows`) builds the complete CasioEmuMsvc desktop
application as a native **PE32+ x86-64 Windows** executable, cross-compiled
from macOS or Linux with MinGW-w64 GCC 16.2.0. Sentry/Crashpad, the MCP
debugger plugin and the plugin self-test suite stay OFF (they are not needed
for the Windows port and require host-specific tooling).

## 1. Prerequisites

| Component | Version / location |
|---|---|
| CMake + Ninja | pre-existing (`/usr/local/bin`) |
| MinGW-w64 | `brew install mingw-w64` — GCC 16.2.0; root at `/usr/local/opt/mingw-w64/toolchain-x86_64` (override `CASIO_MINGW_ROOT`) |
| SDL2 / SDL2_image | repository submodules, copied into `SDL/` and `SDL_image/` for an isolated build |
| Go 1.26 + MinGW cgo | only needed to rebuild the CG100 core DLL (`cg100core.dll`); the shipped binary is current |
| Wine (optional) | `/usr/local/bin/wine` — used for headless verification on this macOS host |

## 2. Build the host

```sh
ROOT=/Volumes/Code/ports/port/windows
cmake -S "$ROOT" -B "$ROOT/build-windows" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-windows-mingw.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCASIOEMU_ENABLE_SENTRY=OFF \
  -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DCASIOEMU_WIZ_TOOLCHAIN=ON \
  -DBUILD_MCP_PLUGIN=OFF
cmake --build "$ROOT/build-windows" --target CasioEmuMsvc -- -j4
```

Artifact: `build-windows/CasioEmuMsvc/CasioEmuMsvc.exe` (PE32+ x86-64).

## 3. What was ported / fixed

* **Dynamic loading.** MinGW-w64 has no `<dlfcn.h>`. `src/WindowsCompat/dlfcn.h`
  maps `dlopen`/`dlsym`/`dlclose`/`dlerror` onto `LoadLibraryExA` /
  `GetProcAddress` / `FreeLibrary`. `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` makes
  `lib/cg100core.dll` inside an extracted `.plugin` resolvable. A small
  registry of `RTLD_GLOBAL` modules makes
  `dlsym(RTLD_DEFAULT, "casioemu_arch_core_*")` work like on POSIX.
* **Plugin package extraction.** `PluginPackage.cpp` uses Windows CryptoAPI
  SHA-256 instead of Apple's `CommonCrypto`.
* **Architecture plugins.** The early architecture pass in `PluginMan.cpp` and
  the C ABI bridge in `Emulator.cpp` / `Cg100CoreInfo.hpp` are now enabled on
  Windows. Plugin discovery also looks in `<exe dir>/plugins` (a GUI launch
  need not have the app folder as CWD).
* **SDL exports.** The host is linked with `-Wl,--export-all-symbols` so the
  statically linked SDL render entry points are reachable from a plugin DLL
  through `GetProcAddress(GetModuleHandle(nullptr), ...)`.
* **Platform tags.** `cmake/CasioPlatform.cmake` derives
  `CASIOEMU_PLATFORM_OS="windows"` and `CASIOEMU_PLATFORM_ARCH="x64"` from the
  cross toolchain; `PlatformTags.h` therefore selects `*-windows-x64.dll`
  binaries from `.plugin` ZIPs (falling back to compiler predefines).

## 4. CG100 SH4 Windows plugin

The current shared plugin source (rather than the stale legacy DLL) was ported
to Win32 in `tools/cg100-win/`:

* `dllmain.cpp` — `dlsym(RTLD_DEFAULT, "SDL_*")` → `GetProcAddress` on the host
  process; `extern "C"` ABI functions are `__declspec(dllexport)`.
* Exports: `fPluginLoad`, `fPluginLoadArch` and all nine `casioemu_arch_core_*`
  functions (framebuffer, handle, key_matrix, key_down/up, release_all,
  power_on, reboot, enter_diag).
* `cg100core.dll` is bound through a generated MinGW import library
  (`cg100core.def` → `libcg100core.a`); it is **not** statically embedded.
* ImGui is compiled into the DLL from the host's own sources (one private
  copy, bound to the host context with `ImGui::SetCurrentContext`).

Rebuild and package:

```sh
cd "$ROOT/tools/cg100-win"
./build-windows-plugin.sh
```

Output: `tools/cg100-win/artifacts/CG100SH4-Windows.plugin` — a ZIP containing

```
plugin.json
assets/info.json
CG100SH4-windows-x64.dll      # tagged native entry (PE32+ x86-64)
lib/cg100core.dll             # current Go core: 39 cg100_* / cg100_usb_* exports
lib/libwinpthread-1.dll       # MinGW runtime dependency
```

Install by copying the `.plugin` file into `<exe dir>/plugins/` (or setting
`CASIOEMU_PLUGINS_DIR`). The plugin id is `com.casioemu.arch.cg100sh4`.

## 5. Verification

* `file(1)`: `CasioEmuMsvc.exe`, `CG100SH4.dll` and `cg100core.dll` are all
  PE32+ x86-64 Windows binaries. `objdump -p` on the EXE confirms the four SDL
  entry points the plugin resolves are exported (13,730 exports total because
  `--export-all-symbols` is used).
* `tools/cg100-win/verify_win.cpp` (MinGW + Wine, real DLLs): all ABI
  handshakes and nine arch-core exports present; real 32 MiB CG100 flash ran
  250 billion cycles with 60,141/88,704 non-black framebuffer pixels and no
  fault. **ALL RUNTIME CHECKS PASSED**.
* `tools/cg100-win/verify_platform_tags.cpp` (MinGW + Wine): reports
  `current platform: windows-x64`, status `Matched`, selects
  `CG100SH4-windows-x64.dll`. **PLATFORM TAG SELECTION PASSED**.
* End-to-end host run under Wine Devel 11.9 with
  `--headless --model=<fx-CG100> --frames=2`: the host extracted the `.plugin`
  into `plugins/.cache/<sha256>/` (all five entries, including both bundled
  DLLs), loaded the tagged DLL and logged
  `[ArchCore] SH4 Architecture attached to Emulator; LCD rect = (89,251,812,468)`.
  No missing-DLL or plugin-load errors.

Logs: `/Volumes/Code/casio-tools/temp/windows-port-logs/`.

## 6. Known limitations

* No interactive Windows desktop is available on this macOS host; key presses,
  the diag buttons and the USB-drive UI were not clicked in a real GUI. Their
  exports and bridges were verified by PE exports and the Wine core harness.
* The MinGW CRT has no POSIX PTY: the built-in shell terminal reports
  `Shell terminal requires a POSIX PTY (not available on Windows)` rather than
  spawning a fake shell.
* `casioctl` (Unix-domain-socket control server), Sentry/Crashpad, the MCP
  debugger plugin and the plugin self-tests are not built on this target.
* `--export-all-symbols` publishes all host globals (13,730 exports) rather
  than a minimal four-symbol SDL export list; harmless but bloats the EXE.
* The host EXE is a console-subsystem PE; launching from Explorer shows a
  console window alongside the SDL GUI.
