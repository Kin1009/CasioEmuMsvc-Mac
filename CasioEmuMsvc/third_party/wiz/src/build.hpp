// build.hpp - the shared build pipeline: source text -> ROM image + assembly listing.
//
// This is the common back end used by the `wizc` / `wizrun` drivers for both
// source languages:
//
//     Wiz   (.wz)    ->  wiz.hpp   (the original Wiz front end + codegen)
//     CWiz  (.cwiz)  ->  cwiz.hpp  (the independent CWiz compiler)
//
// Both front ends hand back assembly text plus the variable map; this module wraps
// it with the boot template and the runtime, assembles it, and returns the image.
//
// Naming: one calculator is selected per build (see targets.hpp):
//     580  -> cw   (ML620906, nX-U16/100, 192x63, 1 bitplane)
//     880  -> cwii (ML620909, nX-U8/100,  192x64, 2 bitplanes)
//     cwapp       a 580 program patched into fx-580VN X firmware (patch_menu)
#pragma once

#include <map>
#include <string>
#include <vector>

#include "common.hpp"
#include "targets.hpp"

namespace wiz {
// cwapp #preserve: fixed runtime registration capacity and snapshot bytes.
// The hidden state block is allocated through the compiler RAM allocator:
//   0      count (uint8)
//   1      used snapshot bytes (uint8)
//   2..33  8 descriptors of 4 bytes: addr lo/hi, len lo/hi
//   34..   128 snapshot bytes
constexpr int kCwappPreserveRanges = 8;
constexpr int kCwappPreserveBytes = 128;
constexpr int kCwappPreserveStateBytes =
    2 + 4 * kCwappPreserveRanges + kCwappPreserveBytes;


struct BuildResult {
    std::vector<uint8_t> rom;
    std::string full_asm;
    std::map<std::string, int> variables;  // name -> RAM address
    std::map<std::string, int> syms;       // assembler symbol table
    bool cwapp = false;                    // built for the fx-580VN X menu
    int slot = 2;                          // #slot n (1..12); 2 when absent
    // Extra app form: `#cwapp <slot> <text> <unselected> <selected>`.
    // `cwapp_extra` is false for the legacy bare `#cwapp` form.
    bool cwapp_extra = false;
    int cwapp_slot = 0;
    std::string cwapp_text;
    std::string cwapp_icon;
    std::string cwapp_icon_selected;
};

// Where runtime.asm and the boot templates live. The drivers call set_data_dir()
// once at start-up (auto-detected from the executable's location); the default
// is "./compiler".
void set_data_dir(const std::string& dir);
const std::string& data_dir();

// Boot template for `target`, with the linker-only directives removed.
// `org` overrides the template's ORG (used to place each app of a .cwapplink
// project at its own code block); 0 keeps the template's own origin.
std::string make_boot(const Target& target, uint32_t org = 0);

// runtime.asm with the lines tagged for the other LCD type removed.
std::string runtime_for(const Target& target, bool extra_app = false,
                        bool preserve_runtime = false);

BuildResult build_source(const std::string& src, const std::string& target,
                         const std::string& path = "", uint32_t org = 0);

// `path` is the file `src` came from; it is only used to resolve #include and to
// label diagnostics. Pass "" when the source has no file.
BuildResult build_cwiz(const std::string& src, const std::string& target,
                       const std::string& path = "", uint32_t org = 0);

// Compile a file; `target` may be empty for the pipeline default, `forced_lang`
// may be "wiz" / "cwiz" / "" (by extension).
struct PathBuild {
    BuildResult r;
    std::string lang;
};
PathBuild build_path(const std::string& path, const std::string& target,
                     const std::string& forced_lang = "", uint32_t org = 0);

// "wiz" or "cwiz" for `path` (forced overrides the extension).
std::string language_of(const std::string& path, const std::string& forced = "");

}  // namespace wiz
