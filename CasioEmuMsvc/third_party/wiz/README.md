# Wiz / CWiz toolchain (vendored)

This directory is a copy of the `compiler/` tree of the **Wiz** project, built into
CasioEmuMsvc as the static library `wizcore` and used by the debugger's **Toolchain**
window (`src/Gui/WizToolchain.cpp`) to compile Wiz (`.wz`) and CWiz (`.cwiz`) programs
in-process and load the image into the running model.

```
src/    the toolchain sources (common, diag, targets, asm, sim, wiz, cwiz, cpre, build)
data/   runtime.asm and the boot templates, read at build time
```

Only the core library is built here. The command-line drivers (`wizc`, `wizrun`,
`customromrun`, `patch_menu`), the test suite and the documentation live in the Wiz
repository, which is the upstream of these files; keep the two in sync when either
side changes.

Build options the embedder may set:

- `WIZ_DATA_DIR` (environment) — where to look for `data/` at run time. When it is
  not set the GUI checks, in order, `wiz/` next to the app binary,
  `<bundle>/Contents/Resources/wiz/`, and the path compiled into the library
  (`WIZ_VENDORED_DATA_DIR`).

The language itself is documented in the Wiz repository: `README.md`,
`docs/GUIDE.md`, `docs/API.md`.

## ROP runtime contract

CWiz ROP sources use `#rop`, `#for 580` or `#for 880`, `#org`, and
`#launcher common`. The 580 common launcher writes the payload at `E9E0`,
copies it to runtime address `D730`, and enters it from `D180`. The compiler
uses the RAC/hdcompiler launcher pattern (`FD24`, `setlr_pc`, `pop xr0`,
`BL memcpy`, `sp = er14`) and inserts `setlr_pc; setsfr` when the source does
not provide startup setup. This prevents display chains from hanging after a
successful injection.

For a text chain, the payload must load XR0 with the font/line bytes and a
NUL-terminated string pointer, call `line_print`, and finish with
`render.ddd4`. When the 580 common launcher is used, code executes at `D730`
but the string pool remains at the backup address `E9E0`; pointers must target
the backup pool. See the upstream Wiz `docs/ROP.md` and the bundled ROP
examples for the complete byte layout.
