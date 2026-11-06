// cwlink.hpp - build a .cwapplink project into one multi-app firmware ROM.
//
// A .cwapplink is a text manifest with one cwapp source per line.  Every source
// is compiled as an fx-580VN X firmware app, placed in its own code block in
// the free flash after 3:0E40, and registered as a mode in the firmware mode
// menu through the cwmode engine.  The result is one 262144-byte ROM.
//
// The legacy bare `#cwapp` + `#slot n` form keeps the original menu-slot patch
// for a single app.  With several legacy apps the builder installs a dispatcher
// at 3:0E40 that routes by the mode id in R8 (the value the firmware set_mode
// receives from the menu table), then jumps to each app's `_APP`.
//
// Extra apps use `#cwapp <slot> <text> <unselected image> <selected image>`
// and are appended to the mode list, so they are not limited to the twelve
// legacy menu slots.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wiz {

struct BuildResult;

struct CwappLinkApp {
    std::string path;         // resolved source path
    int line = 0;             // manifest line
    bool legacy = false;      // bare #cwapp + #slot form
    int slot = 0;             // app identity and shortcut slot (legacy 1..12, extra any positive)
    int legacy_slot = 0;      // 1..12 when legacy, else 0
    std::string key;          // on-screen shortcut label ("D", "+", ...), empty when none
    std::string key_legend;   // ASCII-safe hardware legend (for example "D sin")
    std::string text;         // mode name (extra form)
    std::string icon;         // resolved icon paths (extra form)
    std::string icon_selected;
    uint32_t base = 0;        // code block address
    uint32_t entry = 0;       // _APP entry
    uint32_t size = 0;        // code block size
    uint8_t mode_code = 0;    // firmware mode id (E0..FF)
};

struct CwappLinkResult {
    bool ok = false;
    std::string error;
    std::string error_file;
    int error_line = 0;
    std::vector<std::string> warnings;
    std::vector<CwappLinkApp> apps;
    std::vector<uint8_t> rom;
    std::vector<std::pair<uint32_t, uint32_t>> changed;  // diff spans
    std::vector<std::string> summary;                    // per-app human lines
};

// Parse and build `manifest_path` against `base_rom` (the stock fx-580VN X
// image).  Nothing is written to disk.  On failure `ok` is false and `rom` is
// empty.
CwappLinkResult build_cwapplink(const std::string& manifest_path,
                                const std::vector<uint8_t>& base_rom, bool force);

// Build one source that uses the extra `#cwapp <slot> <text> <icon> <selected>`
// form.  Used by wizc/wizrun and the debugger for a single app.
CwappLinkResult build_cwapp_single(const std::string& source_path,
                                   const std::vector<uint8_t>& base_rom, bool force);

// As above, but with an image that has already been compiled (the editor may
// have unsaved text).  `source_path` is still used to resolve the icon paths.
CwappLinkResult build_cwapp_single_image(const BuildResult& built,
                                         const std::string& source_path,
                                         const std::vector<uint8_t>& base_rom, bool force);

// Parse only (no compilation).  Exposed for tests and for the editor to list
// the project's entries.
struct CwappLinkEntry {
    std::string path;
    int line = 0;
};
bool parse_cwapplink_manifest(const std::string& manifest_path, std::vector<CwappLinkEntry>& out,
                              std::string& error, int& error_line);

// True when the path ends in .cwapplink (case-insensitive).
bool is_cwapplink_path(const std::string& path);

// The fx-580VN X mode-menu shortcut for an app slot.  Slots 1..24 map to
// "1".."9", "A".."F", "x", "y", "z", "M", "+", "-", "*", "/", "."
// (the on-screen key legends).  Slots 25 and above have no shortcut key and
// return an empty string.
std::string slot_key_label(int slot);

// A short ASCII-safe description of the hardware legend for the key, for the
// build log and the editor.  Empty for slots without a key.
std::string slot_key_legend(int slot);

}  // namespace wiz
