// patch.hpp - put a Wiz/CWiz program into the fx-580VN X menu.
//
// The same engine is used by `wizc`, the patch_menu command-line tool and by the
// debugger's Wiz editor ("Patch ROM"): compile a cwapp program, write it at
// 3:0E40, install the menu hook and select the mode id of a menu slot.
//
// NOT verified on real firmware.  See docs/RUNNING.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wiz {

struct BuildResult;

struct RomPatchReport {
    bool ok = false;
    std::string error;       // human-readable, set when !ok
    int error_line = -1;     // source line for compile errors
    int error_col = -1;
    std::vector<std::string> warnings;
    std::vector<std::string> changes;  // one line per modified region
    std::vector<uint8_t> rom;          // the patched image
    int code_size = 0;
    int slot = 1;
    uint32_t app_entry = 0;
    bool hook_forced = false;
};

// `rom` must be a 262144-byte fx-580VN X image.  `source_name` selects the
// language by extension (.cwiz => CWiz, otherwise Wiz).  `slot` is 1..12; pass 0
// to use the program's `#slot n` directive (default 2).  `force` overwrites a
// non-empty flash area; `allow_hook_mismatch` accepts a firmware dump whose
// set_mode bytes differ from the known variant (the debugger and wizc use this
// for the bundled ROM and report hook_forced).
RomPatchReport patch_rom(std::vector<uint8_t> rom, const std::string& source,
                         const std::string& source_name, int slot, bool force,
                         bool allow_hook_mismatch);

// As above, but patches an image that has already been compiled.  `slot` may be
// 0 to use `image.slot`.  wizc uses this so a cwapp source is compiled only once.
RomPatchReport patch_rom_image(std::vector<uint8_t> rom, const BuildResult& image,
                               int slot, bool force, bool allow_hook_mismatch);

}  // namespace wiz
