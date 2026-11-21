# CasioEmuMsvc — four-target build guide

This repository can be configured for **macOS, Linux, Windows and Android**
from a single source tree. All paths in the scripts are derived from the
script's own location, so renaming the checkout (for example
`CasioEmuMsvc-Mac` → `CasioEmuMsvc-Mod`) needs no edits.

Quick start:

```sh
./build-all.sh                 # configure + build all four targets
./build-all.sh macos linux     # subset
./build-all.sh --verify-only   # re-check existing artifacts without building
```

Logs: `$CASIO_BUILD_LOG_DIR` (default `/Volumes/Code/casio-tools/temp/build-4target-logs`).

---

## 1. Platform / architecture tag mapping

`cmake/CasioPlatform.cmake` detects the **target** (not the host) and derives
the two string-literal compile definitions consumed by
`CasioEmuMsvc/src/Plugin/PlatformTags.h`:

| Target | `CASIOEMU_PLATFORM_OS` | `CASIOEMU_PLATFORM_ARCH` |
|---|---|---|
| macOS x86_64 (`build-ui3`) | `"macos"` | `"x64"` |
| macOS arm64 | `"macos"` | `"arm64"` |
| macOS universal | `"macos"` | `"any"` |
| Linux x86_64 | `"linux"` | `"x64"` |
| Linux aarch64 | `"linux"` | `"arm64"` |
| Windows x86_64 (MinGW) | `"windows"` | `"x64"` |
| Windows i686 (MinGW) | `"windows"` | `"x32"` |
| Android arm64-v8a | `"android"` | `"arm64-android"` |
| Android armeabi-v7a | `"android"` | `"any"` |
| Android x86 / x86_64 | `"android"` | `"x32"` / `"x64"` |

The definitions are applied with `casioemu_apply_platform_tags()` to the main
target (`CasioEmuMsvc` or Android's `main`) and to the test/fixture targets
compiled from the same plugin sources. `PlatformTags.h` also has compiler-
predefine fallbacks, so the definitions are an explicit override rather than a
hard requirement.

CMake functions are called from the root `CMakeLists.txt` (detection) and
`CasioEmuMsvc/CMakeLists.txt` (per-target application); toolchain files may
hint with `CASIOEMU_TARGET_OS` / `CASIOEMU_TARGET_ARCH`.

---

## 2. Prerequisites

Installed and verified on this host (Intel macOS 15, Apple clang 17):

| Component | Version | Source / location |
|---|---|---|
| CMake | 4.4.2 | `/usr/local/bin/cmake` (pre-existing) |
| Ninja, ccache | pre-existing | `/usr/local/bin/{ninja,ccache}` |
| macOS SDK | Xcode 26.2 SDK | Xcode.app (pre-existing) |
| MinGW-w64 | GCC 16.2.0 | `brew install mingw-w64` (pre-existing) |
| Linux cross GCC | GCC 15.2.0, glibc sysroot | messense `homebrew-macos-cross-toolchains` release `v15.2.0`, asset `x86_64-unknown-linux-gnu-x86_64-darwin.tar.gz` |
| Android NDK | r27c | direct download `https://dl.google.com/android/repository/android-ndk-r27c-darwin.zip` |

Toolchains are installed under `/Volumes/Code/casio-tools/toolchains/` and are
overridable:

* `CASIO_LINUX_TOOLCHAIN_ROOT` — Linux cross toolchain directory
* `CASIO_MINGW_ROOT` — MinGW-w64 `toolchain-x86_64` directory
* `ANDROID_NDK` / `ANDROID_NDK_HOME` — NDK directory
* `CASIO_TOOLCHAIN_DIR` — parent directory used by `build-all.sh`
* `CCACHE_BIN` — set to an empty string to disable ccache
* `CASIO_BUILD_JOBS` — parallel jobs (default 4)

No Homebrew upgrades were performed: the working macOS build (cmake, LLVM,
ccache) was left untouched.

---

## 3. Exact commands

All commands below run from the repository root. `$ROOT` is that root.

### macOS (reference)

```sh
cmake -S "$ROOT" -B "$ROOT/build-ui3" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build-ui3" --target CasioEmuMsvc -- -j4
```

Artifact: `build-ui3/CasioEmuMsvc/CasioEmuMsvc.app/Contents/MacOS/CasioEmuMsvc`
(Mach-O x86_64). This build keeps Sentry/Crashpad, Wiz, discord-rpc and the
plugin test suite enabled.

### Linux (cross from macOS)

```sh
cmake -S "$ROOT" -B "$ROOT/build-linux" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-linux.cmake" \
  -DCASIO_LINUX_TOOLCHAIN_ROOT=/Volumes/Code/casio-tools/toolchains/x86_64-unknown-linux-gnu \
  -DCMAKE_BUILD_TYPE=Release \
  -DCASIOEMU_ENABLE_SENTRY=OFF -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DBUILD_MCP_PLUGIN=OFF
cmake --build "$ROOT/build-linux" --target CasioEmuMsvc -- -j4
```

Artifact: `build-linux/CasioEmuMsvc/CasioEmuMsvc` (ELF x86-64).

### Windows (MinGW-w64 cross from macOS)

```sh
cmake -S "$ROOT" -B "$ROOT/build-windows" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-windows-mingw.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCASIOEMU_ENABLE_SENTRY=OFF -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DBUILD_MCP_PLUGIN=OFF
cmake --build "$ROOT/build-windows" --target CasioEmuMsvc -- -j4
```

Artifact: `build-windows/CasioEmuMsvc/CasioEmuMsvc.exe` (PE32+ x86-64).

### Android (NDK r27c, arm64-v8a)

```sh
cmake -S "$ROOT" -B "$ROOT/build-android" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-android.cmake" \
  -DANDROID_NDK=/Volumes/Code/casio-tools/toolchains/android-ndk-r27c \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCASIOEMU_ENABLE_SENTRY=OFF -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DCASIOEMU_WIZ_TOOLCHAIN=OFF -DBUILD_MCP_PLUGIN=OFF
cmake --build "$ROOT/build-android" --target main -- -j4
```

Artifact: `build-android/CasioEmuMsvc/libmain.so` (ELF AArch64 shared object).
Android builds produce a shared library by design (`BUILD_EXECUTABLE=OFF` for
mobile platforms in the root `CMakeLists.txt`).

---

## 4. Verification results

_Filled in after the four builds; see the summary table returned by the build
agent. Full configure/build logs live in `build-4target-logs/`._

---

## 5. Known gaps

* Cross-built artifacts were **compiled and linked but not executed** on their
  target OS (no Linux/Windows/Android test host in this environment). `file(1)`
  and ELF/PE headers are used to verify the target format.
* Sentry/Crashpad and the Wiz toolchain are disabled on the cross builds where
  they require host-specific tooling; the macOS reference build keeps them on.
* The MCP debugger plugin is Windows-only and is built only in a native MSVC
  Windows configuration; it is explicitly off in the MinGW cross build.
* Plugin loader self-tests are enabled only for the macOS reference build.
* The Android job copies locale/ROM assets into `app/src/main/assets` in the
  **source tree** (existing behaviour of `CasioEmuMsvc/CMakeLists.txt`); no APK
  is produced here — only the native shared library.
* Some pre-existing absolute host paths remain inside the project sources
  (`WIZ_DOCS_SOURCE = /Users/kin/Desktop/Wiz/docs`, plugin-test fixture root
  `/Users/kin/Desktop/Wiz/temp/plugin-tests`). They are unrelated to the
  build-system switch and were left untouched to avoid clobbering the other
  agent's work.
* `CASIOEMU_PLATFORM_ARCH` is `"any"` for a macOS universal binary and for
  32-bit ARM Android because `PlatformTags.h` has no dedicated tags for them.
