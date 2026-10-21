<div align="center">

<img src="CasioEmuMsvc/icon.ico" width="96" alt="CasioEmuMsvc Logo"/>

# CasioEmuMsvc — macOS

**A high-performance emulator and developer toolkit for the nX-U8/100 & nX-U16/100 MCU series — with the Wiz/CWiz compiler built in.**

[![Fork](https://img.shields.io/badge/fork-of-telecomadm1145/CasioEmuMsvc-blue?style=flat-square)](https://github.com/telecomadm1145/CasioEmuMsvc)
[![Upstream](https://img.shields.io/badge/upstream-stable-8C8C8C?style=flat-square)](https://github.com/telecomadm1145/CasioEmuMsvc/commits/stable)
[![License](https://img.shields.io/github/license/telecomadm1145/CasioEmuMsvc?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-macOS%20(universal)-blue?style=flat-square)](#-install)
[![Releases](https://img.shields.io/github/v/release/Kin1009/CasioEmuMsvc-Mac?style=flat-square)](https://github.com/Kin1009/CasioEmuMsvc-Mac/releases)

</div>

---

A macOS-native build of [CasioEmuMsvc](https://github.com/telecomadm1145/CasioEmuMsvc) by
[telecomadm1145](https://github.com/telecomadm1145), extended with a **built-in Casio programming
toolchain** (the [Wiz / CWiz compiler](https://github.com/Kin1009/CWiz)) so you can write, compile,
patch and run real programs on the emulated fx-580VN X and fx-880BTG — without leaving the app.

This repository is a **fork of `telecomadm1145/CasioEmuMsvc`** (branch `stable`) and tracks it:
upstream changes are merged in, this fork only adds the macOS port and the toolchain work.

---

## ✨ What this fork adds

| | |
| --- | --- |
| 🖥️ **Universal macOS app** | One `.app` that runs natively on **Apple Silicon and Intel** (`x86_64` + `arm64`), built with CMake/Ninja, no installer, no runtime dependencies. |
| 🧰 **Wiz tab (built-in compiler)** | Compile CWiz / C sources for the ClassWiz models, write the result into an emulator ROM slot, and boot it in the running model — all from inside the app. |
| 🧬 **ROP compiler** | Translate high-level CWiz syntax into an **nX-U16 ROP gadget chain** (call/pop/flow words), inject it into calculator RAM and launch it with the community *launcher chung*. |
| 📚 **Arduino-style Examples menu** | `File ▸ Examples` browses the example library by folder; loading one fills the editor and sets the language, target and program type. |
| 🔔 **Update checker with a toggle** | The checker can be switched off entirely from `Theme ▸ Enable update checker`. |
| 🎨 **Theme, zoom and polish fixes** | Theme manager, localization, Discord Rich Presence, and an exact-value zoom control (2.00x is selectable). |

Everything else — the emulator core, debugger, disassembler, hex editors, snapshot system, plugin
API — is upstream's work, unchanged apart from the macOS port.

---

## 📦 Install

Grab the latest **universal** build from the [releases page](https://github.com/Kin1009/CasioEmuMsvc-Mac/releases):

```
CasioEmuMsvc-macOS-universal.zip   →  CasioEmuMsvc.app
```

> [!IMPORTANT]
> The app is ad-hoc signed, so macOS Gatekeeper quarantines it and the first launch may fail with
> *"Launch failed"* or *"App is damaged"*. Clear the quarantine flag once:
>
> ```bash
> xattr -cr /path/to/CasioEmuMsvc.app
> ```
>
> (type `xattr -cr ` and drag the app into the Terminal window to paste the path)

The `.app` is **standalone**: it ships the model files (`models/fx-580VN X`, `models/fx-880BTG`),
the compiler data and the example library inside `Contents/Resources`. Nothing outside the bundle
is needed.

---

## 🧰 The Wiz tab

Select the model you want to work on (fx-580VN X or fx-880BTG) and open the **Wiz** tab:

1. **Write** a program in the editor (CWiz syntax) or pick one from `File ▸ Examples`.
2. **Build** — compiles the source and reports `building for <target> (<model>)`, then
   `wrote <bin> & <asm>`.
3. **Load** — writes the image into the running model (`loaded into the running model`), or
   **patch a ROM slot** in place (`patched slot X (N bytes) & rebooted`).
4. **Build & Run** — compiles, injects and boots the program in the emulator.

The compiler is the same code as the standalone `wizc` driver in the
[CWiz repository](https://github.com/Kin1009/CWiz), so a program built in the app and a program
built on the command line are byte-identical.

### Example

```c
// hello.cwiz — a normal CWiz program
#include "cwiz.h"

int score = 0;

int main() {
    screen_del();
    print(4, 4, "Score ", score);
    render();
    return 0;
}
```

---

## 🧬 ROP payloads

A `.cwiz` file that starts with `#rop` is not turned into a ROM image — it is translated into a
**ROP gadget chain** and injected into calculator RAM.

```c
#rop
#for 580
#org E9E0          // payload address (E9E0 on the 580, E330 on the 880)
#launcher common   // the community "launcher chung"

int main() {
    setlr();
    setsfr();
    screen_del();
    print(0x30, 0x01, "HELLO ROP");
    render();
}
```

How a ROP program runs:

1. **Payload** — the gadget chain is written to `#org` (`E9E0` on the 580, `E330` on the 880).
2. **Launcher** — a small chain is written to the calculator's program input area
   (`D180` on the 580, `9268` on the 880). The launcher copies the payload to the execution area
   and jumps into it.
3. **Run** — in the calculator's **Program mode**, press **LEFT** then **EXE**.

The app verifies both blocks after writing them and says so:

```
payload verified @ E9E0 (53 bytes)
launcher verified @ D180 (26 bytes)
press LEFT then EXE
```

The exact gadget ABI (register layout, `printline` / `smallprint` / `line_draw` /
`render_bitmap` conventions, chain encoding, launcher bytes) is documented in the
[CWiz repository](https://github.com/Kin1009/CWiz/blob/main/docs/ROP.md).

---

## 🔨 Build from source

Requirements: macOS 12+ , Xcode command line tools, CMake ≥ 3.20 and Ninja.

```bash
git clone --recurse-submodules https://github.com/Kin1009/CasioEmuMsvc-Mac.git
cd CasioEmuMsvc-Mac

cmake -G Ninja -DCASIOEMU_ENABLE_SENTRY=OFF -DCMAKE_BUILD_TYPE=Release ..
ninja CasioEmuMsvc
```

The bundle is produced at `build/CasioEmuMsvc/CasioEmuMsvc.app`.

| Option | Meaning |
| --- | --- |
| `CASIOEMU_ENABLE_SENTRY=OFF` | Build without crashpad/Sentry (recommended — no crashpad handler is shipped). |
| `CASIOEMU_WIZ_TOOLCHAIN=ON` | Build the bundled Wiz/CWiz compiler into the GUI (default). |
| `CASIOEMU_WEB=ON` | Build the web front-end instead of the desktop app. |

To produce a **universal** binary (arm64 + x86_64) build with an Xcode generator that supports
both architectures and check the result with `lipo -archs`.

---

## 📝 Notes & known limitations

- The ROP backend targets the **nX-U16** models (fx-580VN X, fx-880BTG); the 880 uses its own
  gadget table and launcher.
- ROP payloads must fit the launcher's copy length (510 / 766 / 1022 bytes, selected by the RAC
  length code); the compiler warns when they do not.
- The macOS port drops the Windows-only build files (MSVC project, Android Gradle app) and keeps
  the upstream `stable` branch merged in.
- The `discord-rpc` submodule is required by the desktop build (`git submodule update --init --recursive`).

---

## 💬 Community & contact

- 🐛 **Issues**: [github.com/Kin1009/CasioEmuMsvc-Mac/issues](https://github.com/Kin1009/CasioEmuMsvc-Mac/issues)
- 📧 **Email**: [khoi.nguyenminh100912@gmail.com](mailto:khoi.nguyenminh100912@gmail.com)
- 💬 **Upstream Discord**: [discord.gg/NM39VPdJTf](https://discord.gg/NM39VPdJTf)

## 📜 Credits

- **CasioEmuMsvc** — [telecomadm1145/CasioEmuMsvc](https://github.com/telecomadm1145/CasioEmuMsvc) —
  the emulator, debugger, disassembler, snapshot system and plugin API. Licensed under the
  [LICENSE](LICENSE) of the upstream project.
- **Wiz / CWiz** — the Casio programming language and compiler used by the Wiz tab.
- The nX-U16 ROP conventions follow the community ROP documentation that ships with the
  [CWiz repository](https://github.com/Kin1009/CWiz/tree/main/docs/rop-notes).
