# Example programs — rebuilt library

Graphics examples draw into the shadow buffer (SB2) and call `render()` to flush
it to the visible LCD buffer (SB1, `0xF800`). Status examples use `renderStatus()`
for the smaller status-icon region. Nothing appears until the relevant render call.

## Categories

| Category | File | Description |
|---|---|---|
| **hello** | `hello/01_hello.cwiz`, `hello/01_hello.wz` | Smallest visible program. |
| **shapes** | `shapes/02_shapes.cwiz` | Drawing primitives (`rect`, `fillrect`, `circle`, `disc`, `line`). |
| **text** | `text/01_text_demo.cwiz` | Original Wiz 6x8 ASCII font (`print`, `text`); the CW Display font is 11x13 (`*_cw`). |
| **text** | `text/02_cw_font.cwiz`, `text/03_cw_font.cwiz` | ClassWiz CW Display font at 1x (`print_cw`, `text_cw`, `num_cw`). |
| **vietnamese** | `vietnamese/01_compile_time.cwiz` … `04_mixed_accented.cwiz` | Vietnamese on the CW Display: `__normalized__`, runtime `normalizeText()`, explicit F4 bytes, mixed ASCII. |
| **input** | `input/01_keys.cwiz`, `input/02_pong.wz` | Key echo; Pong game (`flip()` per frame). |
| **arrays** | `arrays/01_arrays.cwiz` | `rom8` / `rom int`; bar chart pattern. |
| **math** | `math/01_math.cwiz`, `math/02_demo.wz` | Arithmetic with clamped ranges; fibonacci/factorial demo. |
| **bitmap** | `bitmap/01_bitmap.cwiz` | ROM byte-table bitmap drawing (`sprite`). |
| **status** | `status/01_status_cycle_580.cwiz/.wz`, `status/02_status_cycle_880.cwiz/.wz` | Cycle every supported status icon with `setStatus()` / `renderStatus()`. |
| **showcase** | `showcase/01_example_showcase.cwiz`, `showcase/01_example_showcase.wz` | Interactive tour: arrays, helpers, math, keys, shapes, sprites, fonts, status, rendering. |
| **cwapp** | `cwapp/01_cwapp.cwiz`, `cwapp/02_app_hello.wz`, `cwapp/03_exit_menu.cwiz` | Full patched ROM (`#cwapp`, `#org`, `#for 580`); the exit example shows MENU, `exit()` and ON behaviour. |
| **580/880** | `580_880/01_target.cwiz` | Target-independent; 190x63 (580, 1 bp) or 192x64 (880, 2 bp). |
| **rop** | `rop/01_hello.cwiz` … `rop/07_rop_guide.cwiz` | CWiz- and Wiz-syntax ROP payloads (`#rop`, `#org 0xE9E0`, `#launcher common`). `04_keys`/`06_rop_keys` show `getkey()` + `keytable()`; `07_rop_guide` combines SHIFT waits, Vietnamese text, bitmap, line and key dispatch; `08_key_asm` writes the getkey chain with raw `asm { }` blocks. |

## Shadow buffer

- Wiz and CWiz graphics functions (`clear`, `pixel`, `char`, `text`, `print`,
  shapes, `char_cw`, `print_cw`, …) write **only** to SB2:
  `0xDDD3-0xE5D2` on the fx-580VN X, `0xCA54-0xE253` on the fx-880BTG.
- `render()` performs the only SB2 → SB1 (`0xF800`) copy. Without it the
  drawing stays invisible.
- `buffered()` is a compatibility no-op and `flip()` is an alias for `render()`.

## Font note

- `print_cw()` / `text_cw()` / `num_cw()` use ClassWiz CW Display: 11 px advance,
  13-row cell (12 px cap + 1 descender), rendered at 1x. Use 14 px between
  baselines (e.g. `y = 2, 16, 30, 44`).
- Wiz one-char helper: `char_cw`; CWiz: `glyph_cw`.
- The 2.4 KB `_cw_font` table is included only when a `*_cw` function is referenced.
- Vietnamese glyphs are stored as `0xF4, <code>` pairs.  `normalizeText()` (or the
  `__normalized__` declaration modifier) converts UTF-8 source text; the 3.5 KB
  `_cw_font_vn` table is emitted only when an F4 glyph or `normalizeText()` is used.
- `print_cw()` at `x = 0` fits at most 17 printable characters (including spaces)
  per line; wrap longer text with another `print_cw(0, y, ...)` call.

## Build quick-reference

```console
$ WIZ_ROOT=$PWD ./build/wizc examples/hello/01_hello.cwiz
$ WIZ_ROOT=$PWD ./build/wizc examples/hello/01_hello.wz
$ WIZ_ROOT=$PWD ./build/wizc examples/cwapp/01_cwapp.cwiz
```
