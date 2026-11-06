// cwmode.hpp - add modes to a Casio fx-580VN X (0x40000) or fx-880BTG
// (0x60000 window) firmware image.
//
// This is a C++17 port of the two reference tools kept in
// tools/cwmode/reference/ (cw2mode for the fx-880BTG and cwxmode for the
// fx-580VN X).  The reference tools are interactive Windows/WPF programs; this
// library keeps their exact byte layout and adds validation, a reusable API and
// PNG/BMP icon decoding through stb_image.
//
// What a patch does (see docs/CWMODE.md for the table walk):
//   * bump the firmware mode count byte,
//   * append icon bitmaps, text bitmaps or names, an icon lookup table and a
//     string table to the free flash area after the firmware code,
//   * redirect the firmware's mode-entry, mode-select, icon and text hooks to
//     small generated routines at the end of the appended block.
//
// All addresses are physical ROM offsets.  The fx-880BTG maps code addresses
// 0x70000..0x71FFF onto the last 8 KB of its 0x60000 code window; the writer
// keeps the reference tool's +0x12000 relocation for stored pointers.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace wiz {
namespace cwmode {

enum class Model { Cw580, Cw880 };

// "580" / "880" (also accepts "fx-580", "fx-880BTG", ...).  Returns false for
// anything else.
bool parse_model(const std::string& text, Model& out);
const char* model_name(Model model);
// The ROM bytes the reference tool works on: 0x40000 for the 580, 0x60000 for
// the 880.  Larger 880 dumps are accepted by the library; only this window is
// patched.
uint32_t model_window(Model model);

// ------------------------------------------------------------------ images
// An 8-bit RGBA image, top-down, 4 bytes per pixel.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

// Decode PNG or BMP bytes through stb_image.  The reference tool used WPF
// BitmapImage, which accepted more formats; the port compiles only the PNG and
// BMP decoders to keep the vendored core small.
bool decode_image(const std::vector<uint8_t>& bytes, Image& out, std::string& error);
bool load_image(const std::string& path, Image& out, std::string& error);

// ------------------------------------------------------------------ analysis
struct Analysis {
    bool ok = false;
    std::string error;
    Model model = Model::Cw580;

    uint32_t window = 0;          // 0x40000 or 0x60000
    uint32_t rom_size = 0;        // bytes in the input file

    // signature sites
    uint32_t mode_num_ref = 0;
    uint32_t mode_num_addr = 0;
    uint32_t mode_count = 0;      // value at mode_num_addr (reference mode_num)
    uint32_t main_entry = 0;      // B/BL instruction that calls main
    uint32_t main_func = 0;
    uint32_t main_hook = 0;       // B/BL instruction replaced by the dispatcher
    uint32_t comp_func = 0;
    uint32_t menu_hook = 0;
    uint32_t mode_hook_1 = 0;
    uint32_t mode_hook_2 = 0;

    uint32_t sys_mode_table = 0;
    uint32_t sys_str_table = 0;

    // The reference tool's "patch from" address: first long 0xFF run, +2
    // (aligned up on the 880).
    uint32_t code_end = 0;

    // fx-580VN X only, addresses the generated routines call back into.
    uint32_t mul_func = 0;
    uint32_t unknown_func = 0;
    uint32_t line_print_func = 0;
};

// Find the signatures and tables.  Does not modify the image.
Analysis analyze(const std::vector<uint8_t>& rom, Model model);

// ------------------------------------------------------------------ patching
struct ModeSpec {
    uint8_t code = 0;

    // Required: the mode icon.  The 580 uses 48x23, the 880 64x16.  Larger
    // images are cropped at the top-left, like WPF CopyPixels; smaller images
    // are rejected.
    Image icon;

    // fx-580VN X: two text names (language 0 and 1).  Missing entries become
    // an empty string.  A name beginning with '*' is a hex byte string, e.g.
    // "*414243"; this matches the reference tool.
    std::vector<std::string> names;

    // fx-880BTG: two rendered text bitmaps, 64x13, one per language.
    std::vector<Image> text_images;

    // Entry point the mode routine calls.  The reference tool prompts for it.
    uint32_t entry = 0;
};

struct Options {
    Model model = Model::Cw580;
    // Physical write offset, or 0 to use the first long 0xFF run.  On the 880 a
    // logical 0x70000..0x71FFF value is accepted and relocated to
    // 0x5E000..0x5FFFF, mirroring the reference prompt.
    uint32_t at = 0;
    // Allow overwriting a non-blank target (the default refuses).
    bool force = false;
};

struct PatchResult {
    bool ok = false;
    std::string error;

    std::vector<uint8_t> rom;        // same size as the input
    Analysis analysis;

    uint32_t target = 0;             // physical write offset actually used
    uint32_t final_cursor = 0;       // first byte after the appended block
    uint32_t icon_table = 0;
    uint32_t str_table = 0;
    uint32_t mode_count_after = 0;

    // Addresses of the appended per-mode records, for tests and the UI.
    std::vector<uint32_t> icon_addr;       // icon bitmap (2 planes on the 880)
    std::vector<uint32_t> name_addr;       // two entries per mode, lang-major

    // Payload dumps in the reference tool's "[ + ] Build payload:" format.
    std::vector<std::string> log;

    // [begin, end) spans that differ from the input.
    std::vector<std::pair<uint32_t, uint32_t>> changed;
};

// Patch a copy of `rom`.  On failure `ok` is false, `error` explains why and
// `rom` is left empty, so a caller can never write a half-patched image.
PatchResult patch(const std::vector<uint8_t>& rom, const std::vector<ModeSpec>& modes,
                  const Options& options);

}  // namespace cwmode
}  // namespace wiz
