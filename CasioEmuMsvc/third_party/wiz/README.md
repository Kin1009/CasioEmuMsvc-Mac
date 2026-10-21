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
