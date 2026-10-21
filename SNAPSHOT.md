# CasioEmuMsvc — macOS stable snapshot

This folder is a **stable, macOS-only snapshot** of the CasioEmuMsvc emulator with the **Wiz / CWiz
toolchain built into its debugger**. It is a working copy that builds as-is; this document says
exactly what is in it.

| Item | Value |
| --- | --- |
| Upstream | `telecomadm1145/CasioEmuMsvc`, branch `stable`, commit `2d26202d56e437654296af85c94a9036bb34ad58` |
| Platform | macOS only (Windows/Android/Linux sources and build files were stripped) |
| Architectures | universal — `x86_64` + `arm64` |
| Prebuilt app | `CasioEmuMsvc.app` (in this folder; `Contents/Resources/wiz` holds the toolchain data files) |
| Build recipe | [`BUILD-MACOS.md`](BUILD-MACOS.md) |
| Toolchain integration | [`docs/WIZ-TOOLCHAIN.md`](docs/WIZ-TOOLCHAIN.md) |

## Running it

The emulator needs a model directory (the calculator's hardware description plus its ROM image):

```console
$ open -n CasioEmuMsvc.app --args "$PWD/models/fx-580VN X"
```

Launch the bundle with `open` (not by executing the binary directly). Every model directory that
was used during development is in `models/` **on disk**, but see the note below: the ROM images
are not part of the repository.

In the debugger, the **Toolchain** window compiles Wiz (`.wz`) and CWiz (`.cwiz`) programs with the
vendored toolchain, and **Build & Run** loads the image into the running model without touching its
stock ROM. The status-bar option **Dock screen** moves the calculator screen into its own dock area
of the main window.

## What is tracked, and what is not

Tracked: the emulator source (`CasioEmuMsvc/`), the plugin (`McpPlugin/`), the helper projects
(`cw2tools_en/`, `RomPacker/`, `UnitTest1/`, `lua_config_convert/`, `tools/`), the resources, the
prebuilt `CasioEmuMsvc.app`, the build scripts and the documentation — including the vendored Wiz
toolchain under `CasioEmuMsvc/third_party/wiz/`.

Deliberately **not** tracked (they are present in this working copy, but they are third-party
source, generated files or copyrighted firmware — see the local section at the end of
`.gitignore`):

| Path | Why |
| --- | --- |
| `SDL/`, `SDL_image/`, `sentry-native/`, `discord-rpc/` | dependency checkouts named in `.gitmodules` |
| `CasioEmuMsvc/third_party/asmjit/` | dependency checkout named in `.gitmodules` |
| `models/` | ROM dumps — copyrighted Casio firmware, supplied separately, not part of the project |
| `AppIcon.iconset/` | generated from `AppIcon.icns` during the build |
| `build/`, `build-macos.log` | build output |

So a fresh clone needs the dependency checkouts before it can build (`git submodule update --init`
where `.gitmodules` names them, and the ROM images dropped into `models/` yourself).

## Caveats

- Programs written with the bundled toolchain are exercised in this emulator and in the toolchain's
  own simulator. They have **not** been run on a physical calculator, and the fx-580VN X firmware-app
  (patch) target is written from disassembly and has never run on real firmware.
- `emupacks`/model images are user-supplied; nothing here redistributes them.
- The prebuilt `CasioEmuMsvc.app` was built on macOS 15.7.7 with Apple clang 17 and CMake 4.4.2.
