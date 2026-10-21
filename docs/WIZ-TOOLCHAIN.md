# Wiz / CWiz toolchain inside the debugger

The debugger ships the **Wiz** toolchain ([Wiz project](../../../compiler)) and uses it
in-process, so the emulator can compile a Casio program, show its diagnostics and run the
result without any external tool.

## What you get

Two debugger windows are added at start-up:

| Window | What it is |
| --- | --- |
| **Toolchain** | A small IDE for Wiz (`.wz`) and CWiz (`.cwiz`) sources: editor, log and diagnostics. |
| **Calculator** | The LCD of the running model as a debugger window. It can be parked in its own dock area. |

### Toolchain — Source tab

- **Target** — which calculator to build for. It is selected from the model that is running
  (`fx-880BTG` → `880`, `fx-580VN X` → `580`); you can override it:
  `880 fx-880BTG`, `580 fx-580VN X`, or `580 app (firmware patch)`.
- **Language** — `Auto (by file name)`, `Wiz (.wz)` or `CWiz (.cwiz)`. `Auto` picks CWiz for a
  name ending in `.cwiz` and Wiz otherwise.
- **File** — the name used for the diagnostics, the on-disk copy and the `#include` path.
- Buttons:
  - **Build** — compile, write `wiz/<name>.bin`, `wiz/<name>.asm` and the source next to the
    model (a `wiz/` directory beside the ROM), and log the ROM bytes and the RAM used.
  - **Build & Run** — build, then load the image into the running model (see below).
  - **Preprocess** — show the source after the preprocessor and the target sections, like
    `wizc -E` (the Build tab fills with the numbered output).
  - **Lint** — fill the Linter tab.
  - **Save** / **Load** / **Sample** — write the editor to `wiz/<File>`, read it back, and
    restore the built-in example.

The editor is `TextEditor` (syntax colours, undo/redo, error markers). Diagnostics from the
linter are underlined in it, and hovering an underlined line shows the message.

### Toolchain — Build tab

The log of the last build: the note lines the compiler produced (for example the stack
estimate for a recursive function), the ROM and RAM report, and the paths that were written.
_ROM n / limit_ compares the image with the target's ROM window; _RAM n / limit_ is the
highest variable address minus the target's variable base.

### Toolchain — Linter tab

The list of diagnostics with `file:line:column`. Clicking the location moves the editor cursor
there.

The compiler reports one error at a time (it stops at the first one). To give you more than
that in a single pass, the linter blanks each reported line and compiles again, up to twelve
times, so a file with several independent mistakes usually shows all of them at once. It is
approximate — the **Build** tab always holds the authoritative first error. A successful build
shows *No diagnostics.*

## Build & Run: getting the program into the emulator

`Build & Run` (and the **Stock ROM** button that appears afterwards) is deliberately
non-destructive:

1. The image is written to `<model>/wiz/wiz-build.bin`.
2. The model's `rom_path` is pointed at that file and the ROM is hot-reloaded
   (`Chipset::ReloadRom`), which also resets the CPU.
3. The stock ROM file is **not** touched. **Stock ROM** points `rom_path` back at it and
   reloads, so you can always return to the original firmware.

Notes and limits:

- The image is a **flat boot image** written at offset 0 of the model's ROM. The emulator keeps
  the model's ROM buffer, so bytes past the end of the image are the stock bytes; the program
  never reads them.
- An image larger than the model's ROM is refused.
- The `580 app (firmware patch)` target is not loadable directly: it is a *fragment* for an
  fx-580VN X firmware. Build it, patch it into a dump with `patch_menu` (Wiz repository), then
  open the patched dump as the model.
- Programs here have been exercised in the simulator and in this emulator; they have **not**
  been run on a physical calculator.

## Calculator window and the docked screen

The desktop build normally shows the calculator in a second SDL window. The **Calculator**
debugger window shows the same LCD inside the debugger (pixels come from the screen
peripheral's `IScreenFrameProvider`, so every model works).

The status bar has a **Dock screen** option (and the Calculator window has the same checkbox).
When it is on, a dock node is reserved on the right of the debugger window and the screen is
docked into it with `ImGuiDockNodeFlags_NoTabBar`: a separate area of the main window — not a
tab in the tab bar, and not a floating window. Turning it off returns the space and leaves the
screen docked as a normal tab.

The window also has **Fit** (scale the LCD to the area) and a zoom slider.

## Where the code lives

| Path | What it is |
| --- | --- |
| `CasioEmuMsvc/src/Gui/WizToolchain.cpp` / `.h` | the IDE window |
| `CasioEmuMsvc/src/Gui/CalcScreenWindow.cpp` | the LCD window and the dock area handling |
| `CasioEmuMsvc/src/Gui/Ui.cpp` | registers both windows and renders the status-bar option |
| `CasioEmuMsvc/third_party/wiz/` | the vendored toolchain (`wizcore`) and its data files |

`third_party/wiz` is a copy of the `compiler/` tree of the Wiz project: the sources in `src/`
and `runtime.asm` + the boot templates in `data/`. Only the core library is built here (the CLI
drivers and the test suite stay upstream) and it is linked into the app as the static library
`wizcore`. **Keep the copy and the upstream tree in sync when either changes.**

Configure with `-DCASIOEMU_WIZ_TOOLCHAIN=OFF` to build the emulator without the toolchain; the
two windows are then left out (`Ui.cpp` guards every reference).

### Data files at run time

`runtime.asm` and `boot templates` are read from disk when a program is built. The window looks
for them in this order:

1. `$WIZ_DATA_DIR`
2. `wiz/` next to the app binary
3. `<bundle>/Contents/Resources/wiz/` — the macOS build copies `third_party/wiz/data` there
4. the path compiled into the library (`WIZ_VENDORED_DATA_DIR`)

The **Preprocess** action and every build log the directory in use, so a "cannot read
runtime.asm" error tells you where it looked.

## The languages

Wiz (`.wz`, Python-like) and CWiz (`.cwiz`, C-like) are documented in the Wiz repository:
`README.md`, `docs/GUIDE.md` (language) and `docs/API.md` (built-ins, memory map, targets). The
toolchain window's **Sample** button loads a small CWiz program that runs on both models.
