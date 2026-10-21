#pragma once
#include "Ui.hpp"

// ---------------------------------------------------------------------------
// Wiz / CWiz toolchain
//
// The emulator ships the Wiz toolchain (see third_party/wiz) built in-process as
// the static library `wizcore`. These two debugger windows use it:
//
//   Toolchain  - a small IDE for Wiz (.wz) and CWiz (.cwiz) sources. It has three
//                tabs: Source (the editor), Build (the log) and Linter (the
//                diagnostics). "Build & Run" compiles the program and loads the
//                image into the running model, so an edit-run loop needs no
//                external tools.
//
//   Calculator - the LCD of the running model as a normal debugger window. With
//                the status-bar option "Dock screen" it is placed in its own dock
//                area of the debugger window (no tab, no separate OS window).
// ---------------------------------------------------------------------------

UIWindow* CreateToolchainWindow();
UIWindow* CreateCalcScreenWindow();

// Status-bar toggle ("Dock screen"): dock the calculator screen into the debugger
// window as a separate area.
extern bool g_dock_calc_screen;

// Create (docked) or remove the dock area reserved for the calculator screen.
// Called once per frame from the debugger render loop, before the windows render,
// because the dock layout has to be set up before the window is submitted.
void CalcScreenDockSync(bool docked, ImGuiID dockspace_id);
