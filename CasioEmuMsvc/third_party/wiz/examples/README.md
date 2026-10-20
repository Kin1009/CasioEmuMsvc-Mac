# Example programs — organized library

Every category has parallel `.wz` / `.cwiz` pairs where appropriate.
All examples use 8-px line spacing and stay inside 192 x 64 px.

## Categories

| Category | File | Description |
|----------|------|-------------|
| **hello** | `hello/01_hello.wz` / `.cwiz` | Smallest visible program (`clear`, `text`, `wait_key`). |
| **text** | `text/02_text_demo.wz` / `.cwiz` | Showcase: `print` (CW Mono, native 8x8) vs `printMini` (CW Text, native 8 px, proportional). |
| **shapes** | `shapes/03_shapes.wz` / `.cwiz` | Drawing primitives (`rect`, `line`, `circle`, `disc`, `fillrect`, `hline`, `vline`). Labels sit on 8-px lines (`y` = 8, 16, 24, 32, 40, 48, 56). |
| **input** | `input/04_keys.wz` / `.cwiz` | Arrow-key box + `key()` echo using `print`; double-buffered (`buffered`/`flip`). |
| **arrays** | `arrays/05_arrays.wz` / `.cwiz` | RAM array + `rom8` / `rom int`; bar chart with 8-px text grid. |
| **math** | `math/06_math.wz` / `.cwiz` | Arithmetic, clamped ranges (`clamp`), `printMini` for proportional numbers. |
| **cwapp** | `cwapp/07_cwapp.cwiz` | `#cwapp`, `#slot 2`, `#for 580`, `exit()`; builds to a full patched 580 ROM `.bin` (does not overwrite embedded stock ROM). |
| **580/880** | `580_880/08_target.wz` / `.cwiz` | Target-independent; works unchanged for 580 (`192x63`, 1 bp) or 880 (`192x64`, 2 bp). |

## Two-print-set note

- `print(x, y, ...)` draws the **CW Mono** font at its native 8 px glyph size and native 7 px advance (`FONT_LARGE`).
- `printMini(x, y, ...)` draws the **CW Text** font at its native 8 px glyph size with its native proportional advance (`FONT_MINI` / `FONT_TEXT8 + ADV`).
- `text()` / `num()` still use the embedded 6x8 CW Mono (`FONT6X8`) with 6 px advance.
- There are no legacy print aliases: use `print` for CW Mono and `printMini` for CW Text.

## Build quick-reference

```console
# Wiz (.wz)
$ WIZ_ROOT=$PWD ./build/wizc examples/hello/01_hello.wz
# CWiz (.cwiz)
$ WIZ_ROOT=$PWD ./build/wizc examples/hello/01_hello.cwiz
# cwapp (writes full 256 KB patched ROM to source-dir .bin)
$ WIZ_ROOT=$PWD ./build/wizc examples/cwapp/07_cwapp.cwiz
```

All examples stay inside the 192 x 64 pixel screen. No external dependencies.
