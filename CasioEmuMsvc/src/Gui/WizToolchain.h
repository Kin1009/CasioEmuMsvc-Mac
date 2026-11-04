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

namespace casioemu::wizui {
enum class BuildMode { None, Rom, Cwapp, Rop };
struct BuildSummary {
	BuildMode mode = BuildMode::None;
	bool ready = false;
	std::string target;
	std::string source;
	std::string binPath;
	std::string asmPath;
	std::string launcherHex;
	std::string listing;
	std::string error;
	std::string info;
	size_t size = 0;
};
const BuildSummary& GetBuildSummary();
void ResetBuildSummary();
} // namespace casioemu::wizui

UIWindow* CreateToolchainWindow();
UIWindow* CreateCalcScreenWindow();
