# CasioEmuMsvc — Linux port

This document describes how to build and run CasioEmuMsvc for **Linux x86-64**
from a macOS (or any Unix) host using the prebuilt GCC cross toolchain, how
native plugins are named and loaded on Linux, and what is known to work.

## 1. Prerequisites

| Component | Requirement | Notes |
|---|---|---|
| CMake | 3.20+ (tested 4.4.2) | `cmake` on PATH |
| Ninja | any recent | `ninja` on PATH |
| Linux cross GCC | GCC 15.2.0 + glibc sysroot | messense `homebrew-macos-cross-toolchains`, `x86_64-unknown-linux-gnu` release asset; installed under `/Volumes/Code/casio-tools/toolchains/x86_64-unknown-linux-gnu` and overridable via `CASIO_LINUX_TOOLCHAIN_ROOT` |
| Go | 1.21+ (tested 1.26.11) | **only** needed to (re)build the CG100 SH4 core `.so`; `/usr/local/go/bin/go` |
| ccache | optional | pass `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` to speed rebuilds |

SDL2 and SDL2_image are **vendored as git submodules** and built statically as
part of the project — no system SDL dev packages are required. On the target
Linux machine, the SDL video backend needs a display stack (X11 or Wayland);
for headless runs set `SDL_VIDEODRIVER=dummy`.

The build does **not** require Sentry, the Wiz toolchain to be disabled, or
the MCP plugin (Windows-only). Options used below disable Sentry, the plugin
self-tests and the MCP plugin; Wiz stays enabled.

## 2. Build

From the repository root (`ROOT`):

```sh
cmake -S "$ROOT" -B "$ROOT/build-linux" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-linux.cmake" \
  -DCASIO_LINUX_TOOLCHAIN_ROOT=/Volumes/Code/casio-tools/toolchains/x86_64-unknown-linux-gnu \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_EXECUTABLE=ON \
  -DCASIOEMU_ENABLE_SENTRY=OFF \
  -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DCASIOEMU_WIZ_TOOLCHAIN=ON \
  -DBUILD_MCP_PLUGIN=OFF \
  -DCURL_USE_LIBIDN2=OFF -DCURL_ENABLE_SSL=OFF
cmake --build "$ROOT/build-linux" --target CasioEmuMsvc -- -j4
```

Artifact: `build-linux/CasioEmuMsvc/CasioEmuMsvc` — an ELF 64-bit x86-64
executable. `file(1)` and ELF headers are used to confirm the format.

Notes on the flags:

* **`BUILD_EXECUTABLE=ON` is required.** The root `CMakeLists.txt` defines it
  with `option(... OFF)`, so the desktop auto-detection block is dead code;
  without this flag a fresh configure produces `libmain.so` instead of the
  executable. (This is pre-existing upstream behaviour, not Linux-specific.)
* `CURL_ENABLE_SSL=OFF`: the bundled CURL fallback (used for online features)
  cannot find OpenSSL in the cross sysroot. HTTP works; HTTPS-based online
  features do not. Set `OPENSSL_ROOT_DIR` to a *Linux* OpenSSL installation and
  drop this flag if you need HTTPS. `CURL_USE_LIBIDN2=OFF` prevents host
  Homebrew `libidn2` headers from leaking into the cross compile.
* `PKG_CONFIG_LIBDIR` / `PKG_CONFIG_PATH` should point at the sysroot's
  `usr/lib/pkgconfig` (see `build-all.sh`) to avoid host `.pc` leaks.
* The toolchain file adds `-static-libgcc -static-libstdc++`, so the produced
  ELF does not depend on the build machine's `libstdc++` version.

## 3. Platform tags and plugin naming

`cmake/CasioPlatform.cmake` detects the **target** and the build applies:

```
CASIOEMU_PLATFORM_OS="linux"
CASIOEMU_PLATFORM_ARCH="x64"
```

`CasioEmuMsvc/src/Plugin/PlatformTags.h` is the source of truth for selecting
native binaries from a `.plugin` ZIP. On a Linux x86-64 build it also derives
`linux`/`x64` from the compiler predefines (`__linux__`, `__x86_64__`), so the
selection is correct even if the compile definitions are omitted. A file is
considered a native library when its name matches `<name>-<os>-<arch>.<ext>`
with `ext` in `.dylib`, `.so`, `.dll`, `.plugin`. Recognised tags:
`macos|linux|windows|android|any` and `x64|x32|arm64|arm64-android|any`.

Therefore a Linux x86-64 plugin package must contain a binary named e.g.:

```
CG100SH4-linux-x64.so
```

An untagged `CG100SH4.plugin` is treated as the legacy fallback only. If a
package contains tagged binaries but none match the host, installation fails
with `No native binary for linux-x64`.

Linux uses `dlopen()` / `dlsym()` for plugin loading (`PluginMan.cpp`,
`PluginRegistry.cpp`); the `.so` extension is recognised alongside `.dylib`.
Two Linux-specific fixes were made in this port:

1. `LoadPlugins()` now routes Linux to `PluginRegistry::Instance().Initialize()`
   (previously Linux fell through to an empty stub, so the in-app Plugin
   Manager never initialised).
2. The executable is linked with `-Wl,--export-dynamic` so that SDL symbols
   from the statically linked SDL2 are visible to plugins that resolve them
   with `dlsym(RTLD_DEFAULT, ...)` (macOS gets this from the flat namespace).

## 4. CG100 SH4 plugin on Linux

The CG100 architecture plugin is a small C++ shim (`dllmain.cpp`) plus a
private copy of ImGui, linked against the real Go SH4 core
(`libcig100core.so`). Porting it to Linux required no source rewrites: the
shim already uses `dlopen`/`dlsym` (`#if defined(__APPLE__) || defined(__linux__)`),
and the Apple-specific `@rpath`/`install_name_tool` handling in the old build
was replaced with the ELF equivalent `-Wl,-rpath,'$ORIGIN:$ORIGIN/lib'`.

Build it with the helper script:

```sh
CASIO_LINUX_TOOLCHAIN_ROOT=/Volumes/Code/casio-tools/toolchains/x86_64-unknown-linux-gnu \
  bash "$ROOT/CasioEmuMsvc/tools/build_linux_cg100_plugin.sh"
```

The script:

1. cross-builds `libcig100core.so` with Go (`CGO_ENABLED=1`, `GOOS=linux`,
   `GOARCH=amd64`, `CC=<cross gcc>`);
2. compiles `dllmain.cpp` and ImGui with the cross `g++` (ImGui compiled with
   `-fvisibility=hidden` and linked statically);
3. links `CG100SH4.so` with `-static-libgcc -static-libstdc++` and RPATH
   `$ORIGIN:$ORIGIN/lib`, so the embedded core is found after extraction;
4. packages `CG100SH4.plugin` (ZIP) via `tools/build_plugin.py` with entry
   `CG100SH4-linux-x64.so` and `lib/libcig100core.so`;
5. verifies the ELF type, `DT_NEEDED`/`RPATH`, and exported ABI symbols.

The flash image comes from the model's `flash_path` (passed as
`ArchInitInfo::flash_path`) or the `CG100_FLASH` environment variable. The
hardcoded temp path in the plugin is only a standalone-test fallback.

A headless ABI harness is provided at
`CasioEmuMsvc/plugins/CG100SH4/verify_linux.cpp`; see its header comment for
build/run instructions.

## 5. Verification performed

* Configure and build of the full `CasioEmuMsvc` target on the cross toolchain
  (see `PORT_LINUX_FINAL.md` for the final status and log locations).
* `file(1)` / `readelf` / `nm` on the executable, plugin `.so` and Go core `.so`.
* Static checks: ELF x86-64, expected ABI symbols (`fPluginLoad`,
  `fPluginLoadArch`, `casioemu_arch_core_*`), RPATH `$ORIGIN:$ORIGIN/lib`,
  no dependency on a host-specific `libstdc++`.
* The plugin-format header's platform selection was compile-checked to yield
  `linux-x64`.

## 6. Known limitations

* **No Linux host runtime in this build environment.** The cross-compiled ELF
  artifacts were built and inspected statically but were **not executed** on a
  Linux kernel here (no qemu/Docker daemon available). Runtime verification
  must be repeated on a real Linux x86-64 host with the provided harness.
* HTTPS/online features are disabled in the default Linux configure because
  the cross sysroot has no OpenSSL (see §2).
* The Wiz data/examples path and plugin-test fixtures contain pre-existing
  absolute macOS paths (`/Users/kin/...`) in `CMakeLists.txt`; they are
  irrelevant to the executable build and were left unchanged.
* Wayland/X11, audio backend behaviour and the ImGui UI have not been exercised
  at runtime on Linux.
* `GLIBC` requirement of the produced binaries is modest (the plugin needs
  only up to `GLIBC_2.27`), but the Go core's exact minimum should be checked
  on the target distro.
