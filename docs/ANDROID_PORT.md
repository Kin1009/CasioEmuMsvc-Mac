# Android port (branch `port/android`)

This document describes the Android build of CasioEmuMsvc: a native AArch64
shared library (`libmain.so`), an optional Gradle APK/AAB with the SDL2 Java
bridge, plugin loading from app-private storage, and the CG100 SH4 plugin.

Everything is built from the single CMake source tree used by the macOS,
Linux and Windows ports. The Android UI is SDL2's native Android activity with
a thin Java bridge (`com.tele.u8emulator.Game`).

---

## 1. Prerequisites

Verified on macOS 15 (Intel host) with:

| Component | Version / location |
|---|---|
| Android NDK | r27c, `/Volumes/Code/casio-tools/toolchains/android-ndk-r27c` (`ANDROID_NDK`) |
| CMake | 4.4.2 (`/usr/local/bin/cmake`) |
| Ninja | 1.13.2 |
| ccache | optional (`CCACHE_BIN`) |
| JDK | 17–21 required to run Gradle 8.9 (a Temurin 21 installation was used for verification; JDK 25 is not supported by Gradle 8.9) |
| Android SDK | Platform 35 + build-tools 35 + command-line tools, via `ANDROID_HOME` / `ANDROID_SDK_ROOT` (or `local.properties` with `sdk.dir`) |

Third-party sources are gitignored and must be present in the worktree:

* `SDL/` — SDL2 2.32.x (includes `android-project/app/src/main/java/org/libsdl/app`)
* `SDL_image/`
* `CasioEmuMsvc/third_party/asmjit/`

They can be symlinks to an existing checkout (the Android worktree uses
symlinks to `/Users/kin/Desktop/CasioEmuMsvc-Mac/...`). `sentry-native` and
`discord-rpc` are not needed: Sentry and the MCP plugin are disabled.

---

## 2. Native library build (no Android SDK required)

```sh
cd <repo-root>
./build-android.sh native
```

or explicitly:

```sh
cmake -S . -B build-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchain-android.cmake" \
  -DANDROID_NDK=/Volumes/Code/casio-tools/toolchains/android-ndk-r27c \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCASIOEMU_ENABLE_SENTRY=OFF -DCASIOEMU_PLUGIN_TESTS=OFF \
  -DCASIOEMU_WIZ_TOOLCHAIN=ON -DBUILD_MCP_PLUGIN=OFF
cmake --build build-android --target main -- -j4
```

Artifact: `build-android/CasioEmuMsvc/libmain.so` — ELF AArch64 shared object.

`cmake/toolchain-android.cmake` wraps the official NDK toolchain and sets the
`CASIOEMU_TARGET_OS` / `CASIOEMU_TARGET_ARCH` hints. `cmake/CasioPlatform.cmake`
derives the target tags; configure must print:

```
-- CasioEmuMsvc target platform: android/arm64-android
```

The same module applies `CASIOEMU_PLATFORM_OS="android"` and
`CASIOEMU_PLATFORM_ARCH="arm64-android"` as **valid** C string-literal compile
definitions (single-escaped quotes; over-escaped values would produce an
invalid `\"android\"` predefine). `PlatformTags.h` (owned by the plugin-format
work in the main tree) additionally derives the same tags from compiler
predefines, so this header is not required to build the Android port.

Set `-DANDROID_ABI=armeabi-v7a` for 32-bit ARM; the arch tag becomes `any`.
Use a separate build directory per ABI.

---

## 3. APK / AAB build (Android SDK + Gradle)

```sh
export ANDROID_HOME=/path/to/android-sdk     # platforms;android-35, build-tools;35.0.0
export JAVA_HOME=/path/to/jdk-21              # Gradle 8.9 cannot run on JDK 25
./build-android.sh apk       # debug APK
./build-android.sh aab       # debug AAB
```

Equivalent Gradle commands:

```sh
./gradlew :app:assembleDebug
./gradlew :app:bundleRelease
```

`app/build.gradle` uses AGP's `externalNativeBuild` with the repository CMake
file, `minSdkVersion 24`, `targetSdkVersion 35` and `abiFilters 'arm64-v8a'`.
`defaultConfig.ndkVersion` is pinned to `27.2.12479018` (r27c) so AGP accepts
the external NDK. Pass
`-Pandroid.ndkPath=/Volumes/Code/casio-tools/toolchains/android-ndk-r27c` or
set `ndk.dir` in `local.properties` to select that installation. The CMake
version declared in the Gradle file is 4.1.2; install it with
`sdkmanager --install "cmake;4.1.2"` or adjust the `version` field to a locally
installed CMake.

Outputs (both were produced in this port; see `PORT_ANDROID_FINAL.md`):

* `app/build/outputs/apk/debug/CasioEmuMsvc-debug-arm64-v8a.apk`
* `app/build/outputs/bundle/debug/app-debug.aab`

The verified APK is ~20 MB and contains `lib/arm64-v8a/libmain.so`,
`libSDL2.so`, `libSDL2_image.so`, `classes.dex`, locale/ROM assets and
`assets/plugins/CG100SH4-Android.plugin`.

Release builds are unsigned by default. Java sources are Java 17 compatible;
the SDL2 Java bridge is vendored in `app/src/main/java/org/libsdl/app`.

---

## 4. Plugin packaging and loading on Android

### 4.1 Naming

A `.plugin` is a ZIP container. The embedded native library must carry the
host platform tag derived from `PlatformTags.h` (see `cmake/CasioPlatform.cmake`):

| Android ABI | `CASIOEMU_PLATFORM_ARCH` | preferred file name |
|---|---|---|
| arm64-v8a | `arm64-android` | `my-plugin-android-arm64-android.so` |
| armeabi-v7a | `any` | `my-plugin-android-any.so` |
| x86 | `x32` | `my-plugin-android-x32.so` |
| x86_64 | `x64` | `my-plugin-android-x64.so` |

Minimum container layout:

```
plugin.json
assets/info.json
my-plugin-android-arm64-android.so
```

`plugin.json`: `schema: 1`, reverse-DNS `id`, semver `version`, `type: "tool"`
(or `"tab"`), `platforms: ["android"]`, `entry_point: "assets/info.json"`,
`native_entry: "my-plugin-android-arm64-android.so"`, optional `permissions`.
`PluginManifest.cpp` on this branch also accepts `linux`, `windows`,
`android-arm64-android` and `any` platform strings.

Selection order in the native loader: exact `android-<arch>` tag →
`-android-any` / `-any` → untagged `.so` as a legacy fallback. The manifest
`native_entry` wins when it points at an existing `.so`.

### 4.2 Paths and lifecycle

* APK assets: `assets/plugins/*.plugin` → copied by
  `Game.checkAndExtractPluginAssets()` into the app-private
  `filesDir/plugins/`.
* Plugin APK libraries: extracted from `lib/<abi>/*.so` into
  `cacheDir/plugins_so/`, with `load_order.txt` (legacy bridge).
* Prebuilt Gradle libraries: `app/src/main/jniLibs/arm64-v8a/*plugin*.so`
  are installed in `nativeLibraryDir`.
* The Java activity stores those paths in static fields; native code reads
  them with `JNIEnv::CallStaticObjectMethod` and publishes them with the
  POSIX `setenv()` (`CASIOEMU_PLUGIN_PACKAGES_DIR`, `CASIOEMU_PLUGINS_DIR`,
  `CASIOEMU_NATIVE_LIBRARY_DIR`, `TMPDIR`). This avoids the hidden
  `android.system.Os.setenv()` API.
* Early pass (`LoadArchitecturePlugins`, before `Emulator` construction)
  resolves each package, extracts ZIPs to `cacheDir/.cache`, `dlopen`s the
  tagged `.so` and calls `fPluginLoadArch`.
* Normal pass (`LoadPlugins`, after the SDL window exists) calls
  `fPluginLoad` on the same handle. Handles are cached so neither entry point
  runs twice.

`dlopen` uses `RTLD_NOW | RTLD_GLOBAL`; extracted files are chmod'ed with the
executable bits. On Android versions that disallow mapping libraries from
writable app directories, ship the `.so` in `jniLibs` instead.

---

## 5. CG100 SH4 plugin

The source is vendored in `plugins/cg100-sh4/` (from
`/Volumes/Code/casio-tools/cg100-sh4-cpp`). It is self-contained C++17 — no
macOS frameworks, no Go dylib, no IOKit/CoreFoundation calls. The only
Darwin-specific linker flag (`-undefined dynamic_lookup`) is confined to the
`APPLE` branch; Android uses `dlsym(RTLD_DEFAULT, ...)` for the optional SDL
texture bridge and exports default-visibility C ABI symbols.

```sh
./build-android.sh plugin
# or
plugins/cg100-sh4/build-android.sh
```

This cross-builds
`plugins/cg100-sh4/artifacts/android/arm64-v8a/libcg100sh4-android-arm64-android.so`
and packages it as
`app/src/main/assets/plugins/CG100SH4-Android.plugin` with an Android manifest.
`SH4_ANDROID_PLUGIN_OUT` overrides the output path.

CG100 features that use the host (touch mapping, key routing, diag buttons,
reboot, USB simulation, package import and app-private save storage) are
compiled from the shared source tree; see the emulator's CG100 integration
docs for their desktop behaviour. Runtime validation on Android requires a
device or emulator — see §7.

---

## 6. JNI / architecture overview

```
Gradle/SDLActivity (Java, com.tele.u8emulator.Game)
        │ loads libmain.so, owns surface + lifecycle + input
        ▼
libmain.so  (CMake target "main", SDL_main in src/casioemu.cpp)
        │ SDL_AndroidGetJNIEnv() / SDL_AndroidGetActivity()
        ├── Java helpers: file pickers, shortcuts, network requests,
        │   device identity, vibration, clipboard, image save, updates
        ├── app-private storage: filesDir (save data, plugins),
        │   cacheDir (extraction), externalFilesDir (ROMs, locales)
        └── plugin loader: .plugin ZIP → jniLibs/filesDir → dlopen
                ├── fPluginLoadArch  (early, architecture providers)
                └── fPluginLoad      (normal, plugin UI/windows)
```

* `src/StartupUi/StartupUi.cpp` defines `JNI_OnLoad`.
* `src/Gui/AndroidSystemFont.*`, `ThemeManager`, `Ui` and `RendererBackend`
  already contain Android-specific paths (system fonts, touch/mouse separation,
  frame pacing).
* Touch is mapped by SDL; `casioemu.cpp` disables SDL's synthesized
  touch→mouse events on Android (`SDL_HINT_TOUCH_MOUSE_EVENTS=0`).
* No mouse or hardware writes are performed by the port/build tooling.

---

## 7. Verification performed in this port

* CMake configure prints `android/arm64-android` and the generated command
  lines contain valid `-DCASIOEMU_PLATFORM_OS=\"android\"` /
  `-DCASIOEMU_PLATFORM_ARCH=\"arm64-android\"` definitions.
* `cmake --build build-android --target main` links `libmain.so`
  (ELF AArch64); see `PORT_ANDROID_FINAL.md` for the exact artifact status.
* The CG100 plugin cross-build produces
  `libcg100sh4-android-arm64-android.so` with `fPluginLoad`,
  `fPluginLoadArch`, `fPluginApiVersion`, `fPluginAbiSignature` and
  `casioemu_arch_core_*` exports; its packaged `plugin.json` parses with
  `PluginManifest.cpp`.
* The vendored CG100 core's host-side interpreter tests pass
  (`sh4_interpreter`, `cg100_interp`).
* `Crypto.hpp`'s SHA-256 output was checked against `shasum -a 256`.

## 8. Known limitations

* An Android SDK and JDK 21 were installed for this port so the APK/AAB were
  built successfully. No emulator/device was available, so runtime UI/plugin
  behaviour is **not** device-verified. The native `.so` is built, linked and
  checked with `file`/ELF headers; see `PORT_ANDROID_FINAL.md` for details.
* The CG100 plugin resolves SDL entry points through `dlsym(RTLD_DEFAULT, ...)`.
  Android does not necessarily export `libmain.so` symbols into the global
  lookup scope; when unavailable, the plugin's optional SDL texture path is
  skipped (core emulation still works).
* The bundled plugin-format work in the main tree (`PlatformTags.h`, native
  package selection) is owned by another agent. This branch implements a
  compatible tag scheme and three small portability fixes that the main tree
  should cherry-pick: `PluginManifest.cpp` accepts `android`/`linux`/`windows`/
  `any` platform strings, `PluginPackage.cpp` uses the header-only `Crypto.hpp`
  SHA-256 instead of Apple `CommonCrypto` off macOS, and the CMake platform-tag
  macros are single-escaped valid C string literals.
* Sentry/Crashpad, discord-rpc, the MCP plugin and plugin self-tests are disabled
  on Android.
* Some pre-existing absolute host paths remain in unrelated source files
  (Wiz docs path, plugin-test fixture root); they are not used by Android.
* 32-bit ARM / x86 Android ABIs are configured but not built or tested here.
