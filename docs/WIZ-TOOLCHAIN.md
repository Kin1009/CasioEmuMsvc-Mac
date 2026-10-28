# Wiz / CWiz toolchain inside the debugger

> The general debugger layout, themes, memory viewers and calculator behaviour are
> documented in [DEBUGGER-UI.md](DEBUGGER-UI.md).

The debugger ships the **Wiz** toolchain ([Wiz project](../../../compiler)) and uses it
in-process, so the emulator can compile a Casio program, show its diagnostics and run the
result without any external tool.

## What you get

Two debugger windows are added at start-up:

| Window | What it is |
| --- | --- |
| **Wiz Compiler** | A small IDE for Wiz (`.wz`) and CWiz (`.cwiz`) sources: editor, build output and diagnostics. It is deliberately not named "Toolchain" and is laid out like the Rop Compiler. |
| **Calculator** | The complete face (keys, LCD and status sprites) of the running model as a dock tab. |

The window follows the Rop Compiler: a menu bar, a chooser row, two columns and a diagnostics panel. It has no inner Source/Build/Linter tab bar.

- **Target** — which calculator to build for. It is selected from the model that is running
  (`fx-880BTG` → `880`, `fx-580VN X` → `580`); you can override it:
  `880 fx-880BTG`, `580 fx-580VN X`, or `580 app (firmware patch)`.
- **Language** — `Auto (by file name)`, `Wiz (.wz)` or `CWiz (.cwiz)`. `Auto` picks CWiz for a
  name ending in `.cwiz` and Wiz otherwise.
- **File** — the name used for the diagnostics, the on-disk copy and the `#include` path.
- **Build** — compile, write `wiz/<name>.bin`, `wiz/<name>.asm` and the source next to the
  model (a `wiz/` directory beside the ROM), and log the ROM bytes and the RAM used.
- **Build & Run** — build, then load the image into the running model (see below).
- **Preprocess** — show the source after the preprocessor and the target sections, like
  `wizc -E`; the numbered output replaces the build output panel.
- **Lint** — fill the diagnostics panel.
- **Save** / **Load** — write the editor to `wiz/<File>` and read it back. The **File → New /
  sample** menu restores the built-in example.

All choosers use the label-first style (`File  program.cwiz`), not ImGui's default value-first
layout. The editor is `TextEditor` (syntax colours, undo/redo, error markers). Diagnostics from
the linter are underlined in it, and hovering an underlined line shows the message. Clicking a
diagnostic location moves the editor cursor there.

### Build output

The log of the last build: the note lines the compiler produced (for example the stack
estimate for a recursive function), the ROM and RAM report, and the paths that were written.
_ROM n / limit_ compares the image with the target's ROM window; _RAM n / limit_ is the
highest variable address minus the target's variable base.

### Diagnostics

The list of diagnostics with `file:line:column`. Clicking the location moves the editor cursor
there.

The compiler reports one error at a time (it stops at the first one). To give you more than
that in a single pass, the linter blanks each reported line and compiles again, up to twelve
times, so a file with several independent mistakes usually shows all of them at once. It is
approximate — the **build output** always holds the authoritative first error. A successful build
shows *No diagnostics.*

## Text and fonts

The toolchain draws with the two native CW fonts. The screen is 192 × 64 pixels (192 × 63 on the
fx-580VN X, whose first VRAM row is not displayed).

| Call | Font | Cell | Advance | Use it for |
|------|------|------|---------|------------|
| `print(x, y, …)` | **CW Mono** | 8 × 8 | 8 px | fixed-width output: tables, counters, anything that must line up |
| `print_text(x, y, …)` | **CW Text** | 8 px tall, **proportional** | per glyph (1–8 px) | headings and prose; it is *not* squeezed into 8 × 8 cells |
| `text(x, y, s)` / `num(x, y, n)` | CW Mono | 6 × 8 | 6 px | the low-level string / number helpers |

`print` and `print_text` both take `(x, y, value, …)`, lay the arguments out left to right and
return the final `x`. The two sets never mix: `print` stays in CW Mono, `print_text` stays in CW
Text. CW Text only covers letters, a few marks and the space, so the characters it lacks (the
digits, punctuation) are filled from CW Mono at the same 8 px height — the fallback glyph is baked
into the CW Text table, so both sets keep identical height and cannot drift apart.

`text()` and `num()` are the older, lower-level calls. They use the 6 × 8 CW Mono font with a fixed
6 px advance; use them when you need the small font, not as a substitute for `print`.

Because the advance differs between the sets, a line laid out with `print` does not line up with a
line laid out with `print_text`. Keep the same call for the same column, and leave at least 8 px
between two text rows so the 8 px tall glyphs never touch.

## ROM sets

Two stock firmware images back the two models:

| Model | Target | ROM file | Size |
|-------|--------|----------|------|
| fx-580VN X | `cw` / `cwapp` | `models/fx-580VN X/rom.bin` | 262 144 bytes (256 KB) |
| fx-880BTG | `cwii` | `models/fx-880BTG/rom.bin` | 1 048 576 bytes (1 MB) |

The **Target** chooser picks the machine; the editor never writes to either ROM file. The
fx-580VN X dump is also embedded in the toolchain (`third_party/wiz/src/builtin_rom.cpp`, from
`compiler/tools/embed_rom.py`) so that `wizc` can patch a `#cwapp` program without a ROM next to
the sources. The menu patcher only works on a 256 KB fx-580VN X image: **Patch ROM** refuses a ROM
of any other size with a clear diagnostic instead of writing a broken image.

## Build & Run: getting the program into the emulator

`Build & Run` (and the **Stock ROM** button that appears afterwards) is deliberately
non-destructive:

1. The image is written to `<model>/wiz/wiz-build.bin`.
2. The model's `rom_path` is pointed at that file and the ROM is hot-reloaded
   (`Chipset::ReloadRom`), which also resets the CPU.
3. The stock ROM file is **not** touched. **Stock ROM** points `rom_path` back at it and
   reloads, so you can always return to the original firmware.

### Stock-ROM safety rules

- The stock ROM on disk is **read-only** for the toolchain. Every generated image is a *session
  image* in the model's `wiz/` directory (`wiz-build.bin`, `rom-patched-<slot>.bin`), and the model
  is pointed at it by path.
- The stock path is captured **before** anything replaces it, so a session image can never be
  mistaken for the stock ROM. **Stock ROM** always points back at the real firmware.
- The stock file keeps its hash across a session. The fx-580VN X dump is
  `fb775cb6b26b03010a7b14d3223e79a6245a26a49ba44f3b97d5423520de3b39`; if it ever changes, a
  build or a patch wrote to it.
- **Patch ROM** patches a *copy* of the stock dump in memory and writes the copy out; the
  original file is never opened for writing.

Notes and limits:

- The image is a **flat boot image** written at offset 0 of the model's ROM. The emulator keeps
  the model's ROM buffer, so bytes past the end of the image are the stock bytes; the program
  never reads them.
- An image larger than the model's ROM is refused.
- The `580 app (firmware patch)` target is patched into a copy of the bundled fx-580VN X ROM by
  the editor's **Patch ROM** action (slot 1..12). Upstream, `wizc` does the same automatically
  using its embedded 580 ROM; a CWiz `#slot n` directive chooses the menu slot (default 2).
- Programs here have been exercised in the simulator and in this emulator; they have **not**
  been run on a physical calculator.

## Calculator tab

The calculator is rendered inside the debugger: the **Calculator** tab shows the complete
calculator face (interface image, LCD and status sprites). The emulator no longer opens a second
SDL window. The first run splits the dock area so the calculator is on the right; you can move it
like any other tab.

- **Fit** grows the face until either the left/right or the top/bottom edge is reached. It uses
  the exact fractional scale, so the panel is filled instead of leaving an integer-scaled gap.
- **Original size (1:1)** sets one screen pixel per model pixel.
- The zoom slider appears when Fit is off; the native width × height is shown next to it.
- Clicking the on-screen keys presses the calculator key under the cursor. The physical keyboard
  mapping is unchanged.

Closing the calculator window (the native close button or the tab close button) reboots the model
to its main menu and brings the Calculator tab back; it does not terminate the debugger. Use
**Cmd+Q** or the application menu to quit.

## Where the code lives

| Path | What it is |
| --- | --- |
| `CasioEmuMsvc/src/Gui/WizToolchain.cpp` / `.h` | the **Wiz Compiler** window |
| `CasioEmuMsvc/src/Gui/CalcScreenWindow.cpp` | the docked calculator face and LCD view |
| `CasioEmuMsvc/src/Gui/Ui.cpp` | registers the windows and the default dock layout |
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
