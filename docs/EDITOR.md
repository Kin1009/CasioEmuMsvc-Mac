# Wiz Editor Reference — Directives, Targets & Autocomplete

## Directive → Target / Language mapping

Directives are parsed from source lines starting with `#`.  The mapping below is
used by the editor status bar (1.5) and tooltip parser (1.4).

| Directive | Target (hardware) | Language | Description |
|---|---|---|---|
| `#for` | 580 / 880 (cw / cwii) | Wiz / CWiz | Target-section directive (pick ROM section or build target) |
| `#cwapp` | 580 (cw) | CWiz | Firmware app / patch section only |
| `#slot N` | 580 (cw) | CWiz | Menu slot (1–C) for `#cwapp` |
| `#rop` | 580 (cw) / 880 (cwii) | CWiz | ROP payload / gadget-chain output |
| `org` / `orginit` | 580 / 880 | Wiz / CWiz | Origin / initialization label |
| `db` / `dw` / `dd` / `dsb` / `dsl` | 580 / 880 | Wiz / CWiz | Data directives (byte, word, double, block) |
| `def` / `endfor` / `loop` / `until` | 580 / 880 | Wiz | Control-flow keywords |

Target names used in editor dropdown / status bar:
- **580** — `cw` / `cwapp` / `cwrop`
- **880** — `cwii` / `cwiirop`
- **fwnx** — firmware / ROP payload (same chip, different emission mode)

Language names:
- **Wiz** — `.wz` source (original)
- **CWiz** — `.cwiz` source (C-style syntax)

## Autocomplete rules (1.1–1.2)

Trigger conditions:
- **Ctrl+Space** always opens dropdown.
- **Word-boundary trigger**: when typing an alphabetic key (`[A-Za-z]`) or after a period (`.`), the dropdown is refreshed with suggestions that match the current token.
- **Directive trigger**: when typing `#`, the list prioritises directives (`#cwapp`, `#slot`, `#for`, `#rop`).

Dropdown contents (keyword list from API.md + g_labels + file names):
- Built-in functions: `clear`, `clearRender`, `pixel`, `key`, `key_down`, `key_hit`, `delay`, `peek`, `poke`, `render`, `flip`, `exit`, `print`, `text`, `num`, `str`, `num_cw`, `char`, `glyph`, `sprite`, `hline`, `vline`, `rect`, `fillrect`, `line`, `circle`, `disc`, `abs`, `min`, `max`, `clamp`, `sign`, `wait_key`, `srand`, `rand`, `char_cw`, `glyph_cw`, `text_cw`, `num_cw`, `print_cw`
- Directives: `#cwapp`, `#slot`, `#rop`, `#for`, `org`, `db`, `dw`, `dd`, `def`, `endfor`, `loop`, `until`
- File names from project / examples (`program.cwiz`, `01_hello.wz`, `01_target.cwiz`)

Next to each item a docs snippet is shown beside the cursor (1.2):
- Example: `#cwapp` → snippet: `cwapp | cwiz | 580 — firmware app directive`
- Example: `print()` → snippet: `Built-in: layout text/numbers at (x,y)`

The editor status bar (1.5) displays the active directive/target/lang badge:
`cwapp | cwiz | 580` or `Wiz · 580` depending on parsed source line.
