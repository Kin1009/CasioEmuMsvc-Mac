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

See `/Volumes/Code/casio-tools/temp/PORT_WINDOWS_FINAL.md` for the build
result, `file(1)`/PE export evidence, Wine runs and known gaps.

## 6. Known limitations

_Filled in after verification._
