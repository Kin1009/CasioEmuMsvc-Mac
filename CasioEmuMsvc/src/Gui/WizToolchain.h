#pragma once
#include "Ui.hpp"

// ---------------------------------------------------------------------------
// Wiz / CWiz toolchain
//
// The emulator ships the Wiz toolchain (see third_party/wiz) built in-process as
// the static library `wizcore`. These two debugger windows use it:
//
//   Wiz Compiler - a small IDE for Wiz (.wz) and CWiz (.cwiz) sources.  Source
//                  and build output are side by side, like the Rop Compiler, and
//                  the diagnostics live in a panel below. "Build & Run"
//                  compiles the program and loads the image into the running
//                  model, so an edit-run loop needs no external tools.
//
//   Calculator - the complete calculator (face, keys and LCD) as a normal
//                debugger window. There is no separate OS calculator window.
// ---------------------------------------------------------------------------

UIWindow* CreateToolchainWindow();
UIWindow* CreateCalcScreenWindow();
