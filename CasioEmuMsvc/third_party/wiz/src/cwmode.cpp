// cwmode.cpp - firmware mode patcher, C++17 port of cw2mode / cwxmode.
//
// The reference tools (tools/cwmode/reference) are interactive WPF programs.
// The arithmetic and the emitted byte strings below follow them exactly; the
// helpers that produced hex strings there produce bytes directly here.  The
// `changed` spans and the payload log make the result easy to diff.
//
// Provenance: the signatures, the hook offsets and the generated routines are
// copied from the reference Program.cs files.  See docs/CWMODE.md for the
// annotated flow.

#include "cwmode.hpp"

#include <algorithm>
#include <cstdio>
#include <array>
#include <cctype>
#include <cstring>
#include <initializer_list>
#include <string>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#include "third_party/stb_image.h"

namespace wiz {
namespace cwmode {
namespace {

// ------------------------------------------------------------------ hex / bytes
int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

struct PatternByte {
    uint8_t value = 0;
    uint8_t mask = 0;  // nibble mask: 0xF0, 0x0F, 0xFF or 0
};

// Parse "ce f8 01 ?? ?? ?? f? 90" into bytes.  The reference cw2mode matcher
// compares nibbles, so "f?" and "?4" work as well as "??".
std::vector<PatternByte> parse_pattern(const char* sig) {
    std::vector<PatternByte> out;
    int hi = -2;
    for (const char* p = sig; *p; ++p) {
        const char c = *p;
        if (c == ' ') {
            hi = -2;
            continue;
        }
        if (c == '?') {
            if (hi == -2) {
                hi = -1;
            } else {
                PatternByte b;
                b.value = (hi > 0) ? (uint8_t)(hi << 4) : 0;
                b.mask = (hi > 0) ? 0xF0 : 0x00;
                out.push_back(b);
                hi = -2;
            }
            continue;
        }
        const int d = hex_digit(c);
        if (d < 0) {
            hi = -2;
            continue;
        }
        if (hi == -2) {
            hi = d;
        } else {
            PatternByte b;
            b.value = (uint8_t)(((hi > 0 ? hi : 0) << 4) | d);
            b.mask = (uint8_t)((hi > 0 ? 0xF0 : 0x00) | 0x0F);
            out.push_back(b);
            hi = -2;
        }
    }
    return out;
}

// First match in [start, start+size), or -1.  Clamped to the buffer.
int64_t find_signature(const std::vector<uint8_t>& rom, uint32_t start, uint32_t size,
                       const char* sig) {
    const std::vector<PatternByte> pat = parse_pattern(sig);
    if (pat.empty()) return -1;
    const uint64_t begin = start;
    uint64_t end = (uint64_t)start + size;
    if (end > rom.size()) end = rom.size();
    if (begin + pat.size() > end) return -1;
    for (uint64_t i = begin; i + pat.size() <= end; ++i) {
        bool hit = true;
        for (size_t k = 0; k < pat.size(); ++k) {
            if ((rom[(size_t)(i + k)] & pat[k].mask) != pat[k].value) {
                hit = false;
                break;
            }
        }
        if (hit) return (int64_t)i;
    }
    return -1;
}

uint16_t read_u16(const std::vector<uint8_t>& rom, uint32_t at) {
    return (uint16_t)(rom[at] | (rom[at + 1] << 8));
}

// B / BL exactly as the reference helpers print them:
//   B : 00 F{(func>>16)&0xf} lo hi
//   BL: 01 F{(func>>16)&0xf} lo hi
std::vector<uint8_t> branch(uint8_t opcode, uint32_t func) {
    return {opcode, (uint8_t)(0xF0 | ((func >> 16) & 0x0F)), (uint8_t)(func & 0xFF),
            (uint8_t)((func >> 8) & 0xFF)};
}
std::vector<uint8_t> B(uint32_t func) { return branch(0x00, func); }
std::vector<uint8_t> BL(uint32_t func) { return branch(0x01, func); }

std::string hex_dump(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i && i % 16 == 0) out += '\n';
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0xF];
        out += ' ';
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

// ------------------------------------------------------------------ writer
struct Writer {
    Model model = Model::Cw580;
    uint32_t window = 0;
    std::vector<uint8_t> rom;
    uint64_t cursor = 0;
    bool ok = true;
    std::string error;
    std::vector<std::string> log;

    // The 880's segment 7 aliases physical 0x5E000..0x5FFFF as 0x70000..0x71FFF.
    uint32_t logical(uint64_t addr) const {
        if (model == Model::Cw880 && addr >= 0x5E000 && addr < 0x60000)
            return (uint32_t)(addr + 0x12000);
        return (uint32_t)addr;
    }

    bool fail(const std::string& msg) {
        if (ok) error = msg;
        ok = false;
        return false;
    }

    bool write_at(uint64_t addr, const uint8_t* data, size_t n, bool dump) {
        if (!ok) return false;
        if (addr + n > window)
            return fail("patch would write past the " + std::to_string(window) +
                        "-byte ROM window (at " + std::to_string(addr) + ", " + std::to_string(n) +
                        " bytes)");
        if (addr + n > rom.size()) return fail("patch write outside the ROM image");
        std::memcpy(rom.data() + addr, data, n);
        if (dump && n) log.push_back("[ + ] Build payload:\n" + hex_dump(std::vector<uint8_t>(data, data + n)));
        return true;
    }

    bool write_at(uint64_t addr, const std::vector<uint8_t>& data, bool dump = true) {
        return write_at(addr, data.data(), data.size(), dump);
    }

    bool emit(const std::vector<uint8_t>& data, bool dump = true) {
        if (!ok) return false;
        if (!write_at(cursor, data.data(), data.size(), dump)) return false;
        cursor += data.size();
        return true;
    }

    bool emit(std::initializer_list<uint8_t> data, bool dump = true) {
        return emit(std::vector<uint8_t>(data), dump);
    }

    // Skip padding without writing it, matching the reference (the byte keeps
    // its original 0xFF value).
    bool skip(uint32_t n) {
        if (!ok) return false;
        if (cursor + n > window) return fail("patch would run past the ROM window");
        cursor += n;
        return true;
    }
};

// ------------------------------------------------------------------ bitmaps
// Set3/Set4 from the reference Bitmap.cs.  The 880 icon has two bit planes,
// addressed through the same base: the lower plane at base, the upper at
// base+0x80.  `image` is cropped at the top-left like WPF CopyPixels.
void pack_set3(uint8_t* dst1, uint8_t* dst2, const Image& image, int width, int height) {
    const int w2 = width / 8;
    const int stride = image.width * 4;
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < w2; ++i) {
            for (int k = 0; k < 8; ++k) {
                // WPF CopyPixels to Bgr32: byte 0 is blue.  The reference shifts
                // the blue channel, so the other channels are irrelevant.
                const uint8_t blue = image.rgba[(size_t)j * stride + (size_t)(i * 8 + k) * 4 + 0];
                const uint8_t b = (uint8_t)(blue >> 6);
                const uint8_t bit = (uint8_t)(0x80 >> k);
                if ((b & 0x2) == 0)
                    dst2[j * w2 + i] |= bit;
                else
                    dst2[j * w2 + i] &= (uint8_t)~bit;
                if ((b & 0x1) == 0)
                    dst1[j * w2 + i] |= bit;
                else
                    dst1[j * w2 + i] &= (uint8_t)~bit;
            }
        }
    }
}

void pack_set4(uint8_t* dst, const Image& image, int width, int height) {
    const int w2 = width / 8;
    const int stride = image.width * 4;
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < w2; ++i) {
            for (int k = 0; k < 8; ++k) {
                const uint8_t blue = image.rgba[(size_t)j * stride + (size_t)(i * 8 + k) * 4 + 0];
                const uint8_t bit = (uint8_t)(0x80 >> k);
                if (((blue >> 7) & 1) == 0)
                    dst[j * w2 + i] |= bit;
                else
                    dst[j * w2 + i] &= (uint8_t)~bit;
            }
        }
    }
}

bool check_image(const Image& image, int width, int height, const char* what, Writer& w) {
    if (image.empty()) return w.fail(std::string(what) + " is missing");
    if (image.width < width || image.height < height) {
        return w.fail(std::string(what) + " is " + std::to_string(image.width) + "x" +
                      std::to_string(image.height) + ", expected at least " +
                      std::to_string(width) + "x" + std::to_string(height));
    }
    return true;
}

// ------------------------------------------------------------------ analysis
constexpr const char* kSigModeCount580 =
    "ce f8 01 ?? ?? ?? 10 90 ?? ?? 04 70 02 c0 01 10 01 ce";
constexpr const char* kSigModeCount880 =
    "ce f8 01 ?? ?? ?? 10 90 ?? ?? 03 70 02 c0 01 10 01 ce";
constexpr const char* kSigMainEntry = "00 00 11 90 00 f0 00 ?? ?? ??";

bool analyze_or_fail(std::vector<uint8_t>& rom, Model model, Analysis& a) {
    a.model = model;
    a.window = model_window(model);
    a.rom_size = (uint32_t)rom.size();
    if (model == Model::Cw580) {
        if (rom.size() != 0x40000)
            return a.error = "an fx-580VN X image must be exactly 262144 (0x40000) bytes, got " +
                             std::to_string(rom.size()),
                   false;
    } else {
        if (rom.size() < 0x60000)
            return a.error = "an fx-880BTG image must be at least 393216 (0x60000) bytes, got " +
                             std::to_string(rom.size()),
                   false;
    }

    const auto need = [&](int64_t at, uint32_t n, const char* what) {
        if (at < 0) {
            a.error = std::string("signature not found: ") + what;
            return false;
        }
        if ((uint64_t)at + n > a.window) {
            a.error = std::string(what) + " points outside the ROM window";
            return false;
        }
        return true;
    };

    if (model == Model::Cw580) {
        int64_t ref = find_signature(rom, 0, a.window, kSigModeCount580);
        if (!need(ref, 10, "mode count reference")) return false;
        a.mode_num_ref = (uint32_t)ref;
        a.mode_num_addr = read_u16(rom, a.mode_num_ref + 8);

        int64_t main = find_signature(rom, 0, 0x10000, kSigMainEntry);
        if (!need(main, 10, "main entry")) return false;
        a.main_entry = (uint32_t)main + 6;
        if (!need(a.main_entry, 4, "main entry call")) return false;
        a.main_func = ((rom[a.main_entry + 1] & 0x0F) << 16) | rom[a.main_entry + 2] |
                      (rom[a.main_entry + 3] << 8);
        if (a.main_func >= a.window) {
            a.error = "main function address is outside the ROM window";
            return false;
        }

        int64_t hook = find_signature(rom, a.main_func, 0x100, "70 ?? c9 01 ?? ?? ?? ?? ce");
        if (!need(hook, 3, "main hook")) return false;
        a.main_hook = (uint32_t)hook + 3;
        if (!need(a.main_hook, 4, "main hook call")) return false;
        a.comp_func = ((rom[a.main_hook + 1] & 0x0F) << 16) | rom[a.main_hook + 2] |
                      (rom[a.main_hook + 3] << 8);

        int64_t menu = find_signature(
            rom, 0, a.window,
            "90 80 00 01 c7 f0 03 c8 04 00 11 90 38 d1 c5 f0 06 e2 01 ?? ?? ??");
        if (!need(menu, 18, "menu hook")) return false;
        a.menu_hook = (uint32_t)menu + 18;
        if (!need(a.menu_hook, 0x104, "menu hook block")) return false;
        a.mul_func = ((rom[a.menu_hook + 1] & 0x0F) << 16) | rom[a.menu_hook + 2] |
                     (rom[a.menu_hook + 3] << 8);
        a.unknown_func = ((rom[a.menu_hook + 0x37] & 0x0F) << 16) | rom[a.menu_hook + 0x38] |
                         (rom[a.menu_hook + 0x39] << 8);

        int64_t line = find_signature(rom, 0, a.window,
                                      "ce f8 7e f8 05 f8 25 fa 0b 0d 10 90 37 d1 0e 70 01 c9");
        if (!need(line, 18, "line print function")) return false;
        a.line_print_func = (uint32_t)line;

        int64_t h1 = find_signature(
            rom, 0, a.window,
            "00 70 01 c8 04 06 00 e4 0d ce 45 f0 06 e2 01 ?? ?? ?? 08 90 ?? ??");
        if (!need(h1, 18, "mode hook 1")) return false;
        a.mode_hook_1 = (uint32_t)h1 + 18;
        if (!need(a.mode_hook_1, 4, "mode hook 1 call")) return false;

        int64_t h2 = find_signature(rom, 0, a.window,
                                    "60 80 00 01 06 e2 01 ?? ?? ?? 08 90 ?? ?? 00 85");
        if (!need(h2, 10, "mode hook 2")) return false;
        a.mode_hook_2 = (uint32_t)h2 + 10;
        if (!need(a.mode_hook_2, 0x22, "mode hook 2 block")) return false;

        a.sys_mode_table = read_u16(rom, a.mode_hook_1 + 2);
        a.sys_str_table = read_u16(rom, a.menu_hook + 0xF6);
        if (!need(a.sys_str_table, 4, "string table")) return false;

        int64_t end = find_signature(rom, 0x20000, 0x20000, "ff ff ff ff ff ff ff ff");
        if (!need(end, 2, "code end (long 0xFF run)")) return false;
        a.code_end = (uint32_t)end + 2;
    } else {
        int64_t ref = find_signature(rom, 0, a.window, kSigModeCount880);
        if (!need(ref, 10, "mode count reference")) return false;
        a.mode_num_ref = (uint32_t)ref;
        a.mode_num_addr = read_u16(rom, a.mode_num_ref + 8);

        int64_t main = find_signature(rom, 0, 0x10000, kSigMainEntry);
        if (!need(main, 10, "main entry")) return false;
        a.main_entry = (uint32_t)main + 6;
        if (!need(a.main_entry, 4, "main entry call")) return false;
        a.main_func = ((rom[a.main_entry + 1] & 0x0F) << 16) | rom[a.main_entry + 2] |
                      (rom[a.main_entry + 3] << 8);
        if (a.main_func >= a.window) {
            a.error = "main function address is outside the ROM window";
            return false;
        }

        int64_t hook =
            find_signature(rom, a.main_func, 0x200, "70 ?? c9 01 f? ?? ?? 01 f? ?? ?? ?? ce");
        if (!need(hook, 3, "main hook")) return false;
        a.main_hook = (uint32_t)hook + 3;
        if (!need(a.main_hook, 4, "main hook call")) return false;
        a.comp_func = ((rom[a.main_hook + 1] & 0x0F) << 16) | rom[a.main_hook + 2] |
                      (rom[a.main_hook + 3] << 8);

        int64_t menu =
            find_signature(rom, 0, a.window, "02 e1 a0 80 00 01 3b 91 3a 90 08 a2 ?? ?? 08 94");
        if (!need(menu, 10, "menu hook")) return false;
        a.menu_hook = (uint32_t)menu + 10;
        if (!need(a.menu_hook, 0x112, "menu hook block")) return false;

        int64_t h1 = find_signature(
            rom, 0, a.window,
            "00 84 00 70 01 c8 03 04 00 e2 0c ce 25 f0 3b 91 3a 90 08 90");
        if (!need(h1, 18, "mode hook 1")) return false;
        a.mode_hook_1 = (uint32_t)h1 + 18;
        if (!need(a.mode_hook_1, 4, "mode hook 1 call")) return false;

        int64_t h2 = find_signature(rom, 0, a.window,
                                    "60 80 00 01 3b 91 3a 90 08 90 ?? ?? 00 84 00 f?");
        if (!need(h2, 8, "mode hook 2")) return false;
        a.mode_hook_2 = (uint32_t)h2 + 8;
        if (!need(a.mode_hook_2, 0x16, "mode hook 2 block")) return false;

        a.sys_mode_table = (uint16_t)(read_u16(rom, a.mode_hook_1 + 2) - 7);
        a.sys_str_table = read_u16(rom, a.menu_hook + 0x8E);
        if (!need(a.sys_str_table, 4, "string table")) return false;

        int64_t end =
            find_signature(rom, 0x10000, 0x50000,
                           "ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff");
        if (!need(end, 2, "code end (long 0xFF run)")) return false;
        a.code_end = (uint32_t)end + 2;
        if (a.code_end & 1) ++a.code_end;
    }

    if (!need(a.mode_num_addr, 1, "mode count variable")) return false;
    a.mode_count = rom[a.mode_num_addr];
    if (a.mode_count == 0 || a.mode_count > 64) {
        a.error = "implausible mode count " + std::to_string(a.mode_count) + " at " +
                  std::to_string(a.mode_num_addr);
        return false;
    }
    if (a.code_end < 0x10000 || a.code_end >= a.window) {
        a.error = "code end " + std::to_string(a.code_end) + " is outside the code window";
        return false;
    }
    a.ok = true;
    return true;
}

}  // namespace

// ------------------------------------------------------------------ public bits
bool parse_model(const std::string& text, Model& out) {
    std::string lower;
    for (char c : text) lower += (char)std::tolower((unsigned char)c);
    if (lower.find("880") != std::string::npos) {
        out = Model::Cw880;
        return true;
    }
    if (lower.find("580") != std::string::npos) {
        out = Model::Cw580;
        return true;
    }
    return false;
}

const char* model_name(Model model) { return model == Model::Cw880 ? "880" : "580"; }

uint32_t model_window(Model model) { return model == Model::Cw880 ? 0x60000 : 0x40000; }

bool decode_image(const std::vector<uint8_t>& bytes, Image& out, std::string& error) {
    if (bytes.empty()) {
        error = "image file is empty";
        return false;
    }
    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &comp, 4);
    if (!pixels) {
        error = std::string("cannot decode image: ") + stbi_failure_reason();
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    return true;
}

bool load_image(const std::string& path, Image& out, std::string& error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open image: " + path;
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
    if (!decode_image(bytes, out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

Analysis analyze(const std::vector<uint8_t>& rom, Model model) {
    Analysis a;
    std::vector<uint8_t> copy = rom;  // analyze_or_fail only reads, but keeps the API const.
    analyze_or_fail(copy, model, a);
    return a;
}

// ------------------------------------------------------------------ patcher
namespace {

bool resolve_target(const Analysis& a, const Options& opt, uint32_t& target, std::string& error) {
    const uint32_t window = a.window;
    uint32_t at = opt.at;
    if (at == 0) {
        target = a.code_end;
        return true;
    }
    if (a.model == Model::Cw880) {
        // Mirrors the reference prompt: logical >= 0x70000 is stored at
        // physical addr - 0x12000.
        if (at >= 0x70000) {
            const uint32_t physical = at - 0x12000;
            if (physical < 0x5E000 || physical >= 0x60000) {
                error = "0x" + std::to_string(at) +
                        " is not a mapped code address (use 0x70000..0x71FFF)";
                return false;
            }
            target = physical;
            return true;
        }
        if (at >= 0x60000) {
            error = "0x" + std::to_string(at) + " is outside the 0x60000 code window";
            return false;
        }
    }
    target = at;
    if (target >= window) {
        error = "patch address is outside the ROM window";
        return false;
    }
    return true;
}

}  // namespace

PatchResult patch(const std::vector<uint8_t>& input, const std::vector<ModeSpec>& modes,
                  const Options& options) {
    PatchResult result;
    Analysis a = analyze(input, options.model);
    result.analysis = a;
    if (!a.ok) {
        result.error = a.error;
        return result;
    }
    if (modes.empty()) {
        result.error = "no modes requested";
        return result;
    }
    if (modes.size() > 64) {
        result.error = "too many modes in one patch (maximum 64)";
        return result;
    }
    if ((uint32_t)a.mode_count + modes.size() > 255) {
        result.error = "mode count would overflow one byte";
        return result;
    }

    uint32_t target = 0;
    if (!resolve_target(a, options, target, result.error)) return result;
    if (target < a.code_end && !options.force) {
        result.error = "patch address 0x" + std::to_string(target) +
                       " is inside the firmware code (first free byte is 0x" +
                       std::to_string(a.code_end) + "); pass --force to override";
        return result;
    }
    if (target & 1) {
        result.error = "patch address must be even";
        return result;
    }

    Writer w;
    w.model = a.model;
    w.window = a.window;
    w.rom = input;
    w.cursor = target;

    const uint32_t old_modes = a.mode_count;
    const uint32_t new_modes = (uint32_t)modes.size();
    const bool extended = old_modes + new_modes > 14;  // languages is fixed at 2

    // Validate the input images before touching the ROM.
    if (a.model == Model::Cw580) {
        for (size_t i = 0; i < modes.size(); ++i) {
            const ModeSpec& m = modes[i];
            if (!check_image(m.icon, 48, 23, "mode icon", w)) {
                result.error = w.error;
                return result;
            }
            if (m.names.size() > 2) {
                result.error = "the 580 takes two names per mode (language 0 and 1)";
                return result;
            }
            if (m.entry > 0xFFFFF) {
                result.error = "mode entry does not fit the 20-bit branch field";
                return result;
            }
        }
    } else {
        for (size_t i = 0; i < modes.size(); ++i) {
            const ModeSpec& m = modes[i];
            if (!check_image(m.icon, 64, 16, "mode icon", w)) {
                result.error = w.error;
                return result;
            }
            if (m.text_images.size() != 2) {
                result.error = "the 880 takes two text images per mode (64x13, language 0 and 1)";
                return result;
            }
            for (const Image& t : m.text_images)
                if (!check_image(t, 64, 13, "text image", w)) {
                    result.error = w.error;
                    return result;
                }
            if (m.entry > 0xFFFFF) {
                result.error = "mode entry does not fit the 20-bit branch field";
                return result;
            }
        }
    }

    // ---- common per-mode data: icons, text, entries
    std::vector<uint32_t> pbmp(new_modes, 0);
    std::vector<std::array<uint32_t, 2>> pstr(new_modes);
    std::vector<uint32_t> pfunc(new_modes, 0);
    std::vector<uint8_t> codes(new_modes, 0);

    for (uint32_t i = 0; i < new_modes; ++i) {
        const ModeSpec& m = modes[i];
        codes[i] = m.code;
        pfunc[i] = m.entry;
        if (a.model == Model::Cw580) {
            // 48x23 1bpp icon, 0x8A bytes.
            if (!w.write_at(w.cursor, std::vector<uint8_t>(0x8A, 0), false)) {
                result.error = w.error;
                return result;
            }
            pack_set4(w.rom.data() + w.cursor, m.icon, 48, 23);
            pbmp[i] = (uint32_t)w.cursor;
            if (!w.skip(0x8A)) {
                result.error = w.error;
                return result;
            }
            for (int j = 0; j < 2; ++j) {
                const std::string name = (size_t)j < m.names.size() ? m.names[(size_t)j] : "";
                pstr[i][(size_t)j] = (uint32_t)w.cursor;
                std::vector<uint8_t> bytes;
                if (!name.empty() && name[0] == '*') {
                    // Reference: uppercase the string, then read byte pairs after
                    // the '*'.  '?' means 0x00.
                    for (size_t k = 1; k + 1 < name.size(); k += 2) {
                        const char hi = (char)std::toupper((unsigned char)name[k]);
                        const char lo = (char)std::toupper((unsigned char)name[k + 1]);
                        const int h = hi == '?' ? 0 : hex_digit(hi);
                        const int l = lo == '?' ? 0 : hex_digit(lo);
                        if (h < 0 || l < 0) {
                            result.error = "invalid hex name '" + name + "' (expected '*' + hex pairs)";
                            return result;
                        }
                        bytes.push_back((uint8_t)((h << 4) | l));
                    }
                } else {
                    // Encoding.ASCII.GetBytes: anything above U+007F becomes '?'.
                    for (char c : name)
                        bytes.push_back((unsigned char)c < 0x80 ? (uint8_t)(unsigned char)c : '?');
                }
                bytes.push_back(0);
                if (!w.emit(bytes, false)) {
                    result.error = w.error;
                    return result;
                }
                if (w.cursor & 1)
                    if (!w.skip(1)) {
                        result.error = w.error;
                        return result;
                    }
            }
        } else {
            // 64x16 two-plane icon, 0x100 bytes (planes at +0 and +0x80).
            const uint64_t plane1 = w.cursor;
            if (!w.write_at(plane1, std::vector<uint8_t>(0x100, 0), false)) {
                result.error = w.error;
                return result;
            }
            pack_set3(w.rom.data() + plane1, w.rom.data() + plane1 + 0x80, m.icon, 64, 16);
            pbmp[i] = (uint32_t)plane1;
            if (!w.skip(0x100)) {
                result.error = w.error;
                return result;
            }
            for (int j = 0; j < 2; ++j) {
                const uint64_t at = w.cursor;
                if (!w.write_at(at, std::vector<uint8_t>(0x68, 0), false)) {
                    result.error = w.error;
                    return result;
                }
                pack_set4(w.rom.data() + at, m.text_images[(size_t)j], 64, 13);
                pstr[i][(size_t)j] = (uint32_t)at;
                if (!w.skip(0x68)) {
                    result.error = w.error;
                    return result;
                }
            }
        }
        result.icon_addr.push_back(pbmp[i]);
    }

    // Address of the two names per mode, language-major, for tests and the UI.
    for (int j = 0; j < 2; ++j)
        for (uint32_t i = 0; i < new_modes; ++i) result.name_addr.push_back(pstr[i][(size_t)j]);

    // ---- 880: icon table, string tables and routines
    if (a.model == Model::Cw880) {
        // NOTE: the reference computes this before appending the table; the
        // generated icon routine indexes backwards from it.
        const int64_t icon_table_signed = (int64_t)w.cursor - 8 * (int64_t)old_modes - 8;
        if (icon_table_signed < 0) {
            result.error = "icon table underflow";
            return result;
        }
        uint32_t icon_table = (uint32_t)icon_table_signed;

        for (uint32_t i = 0; i < new_modes; ++i) {
            const uint32_t p1 = w.logical(pbmp[i]);
            const uint32_t p2 = w.logical(pbmp[i]) + 0x80;
            std::vector<uint8_t> rec = {(uint8_t)(p1 & 0xFF), (uint8_t)((p1 >> 8) & 0xFF),
                                        (uint8_t)((p1 >> 16) & 0xFF), 0,
                                        (uint8_t)(p2 & 0xFF), (uint8_t)((p2 >> 8) & 0xFF),
                                        (uint8_t)((p2 >> 16) & 0xFF), codes[i]};
            if (!w.emit(rec, false)) {
                result.error = w.error;
                return result;
            }
        }

        uint32_t ppstr[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            const int64_t p = (int64_t)w.cursor - 4 * (int64_t)(14 + 1);
            if (p < 0) {
                result.error = "string table underflow";
                return result;
            }
            ppstr[i] = (uint32_t)p;
            for (uint32_t j = 0; j < new_modes; ++j) {
                if ((int)(j + old_modes) < 14) {
                    const uint32_t ptr = a.sys_str_table + (old_modes + j + 1) * 4 + (uint32_t)i * 60;
                    const uint32_t addr = w.logical(pstr[j][(size_t)i]);
                    std::vector<uint8_t> rec = {(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF),
                                                (uint8_t)((addr >> 16) & 0xFF)};
                    if (!w.write_at(ptr, rec, false)) {
                        result.error = w.error;
                        return result;
                    }
                } else {
                    const uint32_t addr = w.logical(pstr[j][(size_t)i]);
                    if (!w.emit({(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF),
                                 (uint8_t)((addr >> 16) & 0xFF), 0},
                                false)) {
                        result.error = w.error;
                        return result;
                    }
                }
            }
        }
        const uint32_t str_table = (uint32_t)w.cursor;
        const uint32_t icon_base = w.logical(icon_table);
        if (extended) {
            for (int i = 0; i < 2; ++i) {
                const uint32_t addr = w.logical(ppstr[i]);
                if (!w.emit({(uint8_t)(addr & 0xFF), (uint8_t)((addr >> 8) & 0xFF),
                             (uint8_t)((addr >> 16) & 0xFF), 0},
                            false)) {
                    result.error = w.error;
                    return result;
                }
            }
        }

        // main hook: dispatcher
        {
            const uint32_t addr = w.logical(w.cursor);
            if (!w.write_at(a.main_hook, B(addr))) {
                result.error = w.error;
                return result;
            }
        }
        for (uint32_t i = 0; i < new_modes; ++i) {
            if (!w.emit({codes[i], 0x70,
                         (uint8_t)(4 + i + 2 * (new_modes - 1)), 0xC9},
                        false)) {
                result.error = w.error;
                return result;
            }
        }
        if (!w.emit(BL(a.comp_func), false)) { result.error = w.error; return result; }
        if (!w.emit(B(a.main_hook + 4), false)) { result.error = w.error; return result; }
        for (uint32_t i = 0; i < new_modes; ++i) {
            if (!w.emit(BL(pfunc[i]), false)) { result.error = w.error; return result; }
            if (!w.emit({(uint8_t)(uint32_t)(-3 * (int)(i + 1) - 2), 0xCE}, false)) {
                result.error = w.error;
                return result;
            }
        }

        {
            const uint32_t addr = w.logical(w.cursor);
            if (!w.write_at(a.menu_hook, B(addr))) {
                result.error = w.error;
                return result;
            }
        }
        {
            std::vector<uint8_t> f;
            const uint8_t hi = (uint8_t)(old_modes << 3);
            f.insert(f.end(), {hi, 0x70, 0x04, 0xC2, 0x08, 0xA2,
                               (uint8_t)((a.sys_mode_table + 4) & 0xFF),
                               (uint8_t)(((a.sys_mode_table + 4) >> 8) & 0xFF)});
            const std::vector<uint8_t> b1 = B(a.menu_hook + 4);
            f.insert(f.end(), b1.begin(), b1.end());
            const uint32_t it4 = icon_base + 4;
            f.insert(f.end(), {(uint8_t)((it4 >> 16) & 0xFF), 0xE3, 0x08, 0xA2,
                               (uint8_t)(it4 & 0xFF), (uint8_t)((it4 >> 8) & 0xFF)});
            const uint32_t it6 = icon_base + 6;
            f.insert(f.end(), {(uint8_t)((it6 >> 16) & 0xFF), 0xE3, 0x08, 0x94,
                               (uint8_t)(it6 & 0xFF), (uint8_t)((it6 >> 8) & 0xFF)});
            f.insert(f.end(), {0xF8, 0xB2});
            const uint32_t it0 = icon_base;
            f.insert(f.end(), {(uint8_t)((it0 >> 16) & 0xFF), 0xE3, 0x08, 0xA2,
                               (uint8_t)(it0 & 0xFF), (uint8_t)((it0 >> 8) & 0xFF)});
            const uint32_t it2 = icon_base + 2;
            f.insert(f.end(), {(uint8_t)((it2 >> 16) & 0xFF), 0xE3, 0x08, 0x90,
                               (uint8_t)(it2 & 0xFF), (uint8_t)((it2 >> 8) & 0xFF)});
            const std::vector<uint8_t> b2 = B(a.menu_hook + 0x12);
            f.insert(f.end(), b2.begin(), b2.end());
            if (!w.emit(f)) { result.error = w.error; return result; }
        }

        if (extended) {
            for (uint32_t site : {a.menu_hook + 0x92u, a.menu_hook + 0xB0u, a.menu_hook + 0xEEu,
                                  a.menu_hook + 0x10Eu}) {
                const uint32_t addr = w.logical(w.cursor);
                if (!w.write_at(site, BL(addr))) {
                    result.error = w.error;
                    return result;
                }
            }
            std::vector<uint8_t> f;
            f.insert(f.end(), {0x0F, 0x7A, 0x03, 0xC0, 0xC8, 0x92,
                               (uint8_t)((a.sys_str_table + 2) & 0xFF),
                               (uint8_t)(((a.sys_str_table + 2) >> 8) & 0xFF)});
            f.insert(f.end(), {0x1F, 0xFE, 0x10, 0x92, 0xBB, 0x91, 0x00, 0x03, 0x2B, 0x93,
                               0x2A, 0x92});
            const uint32_t str_base = w.logical(str_table);
            const uint32_t st0 = str_base;
            f.insert(f.end(), {(uint8_t)((st0 >> 16) & 0xFF), 0xE3, 0x28, 0xA0,
                               (uint8_t)(st0 & 0xFF), (uint8_t)((st0 >> 8) & 0xFF)});
            const uint32_t st2 = str_base + 2;
            f.insert(f.end(), {(uint8_t)((st2 >> 16) & 0xFF), 0xE3, 0x28, 0x92,
                               (uint8_t)(st2 & 0xFF), (uint8_t)((st2 >> 8) & 0xFF)});
            f.insert(f.end(), {0xA0, 0x8C, 0x00, 0x0D, 0x2B, 0x9D, 0x2A, 0x9C, 0x06, 0xFC,
                               0x2F, 0x90, 0xC2, 0x90, 0x66, 0xF0, 0x2F, 0x90, 0x02, 0xD2,
                               0x1F, 0xFE});
            if (!w.emit(f)) { result.error = w.error; return result; }
        }

        {
            const uint32_t addr = w.logical(w.cursor);
            for (uint32_t site : {a.mode_hook_1, a.mode_hook_2, a.mode_hook_2 + 0x12u})
                if (!w.write_at(site, BL(addr))) {
                    result.error = w.error;
                    return result;
                }
        }
        {
            std::vector<uint8_t> f;
            f.insert(f.end(), {(uint8_t)(8 * old_modes), 0x70, 0x03, 0xC2, 0x08, 0x90,
                               (uint8_t)((a.sys_mode_table + 7) & 0xFF),
                               (uint8_t)(((a.sys_mode_table + 7) >> 8) & 0xFF), 0x1F, 0xFE});
            const uint32_t it7 = icon_base + 7;
            f.insert(f.end(), {(uint8_t)((it7 >> 16) & 0xFF), 0xE3, 0x08, 0x90,
                               (uint8_t)(it7 & 0xFF), (uint8_t)((it7 >> 8) & 0xFF), 0x1F, 0xFE});
            if (!w.emit(f)) { result.error = w.error; return result; }
        }

        result.icon_table = icon_table;
        result.str_table = str_table;
    } else {
        // ---- 580: icon table, string table and routines
        uint32_t icon_table = (uint32_t)w.cursor;
        for (uint32_t i = 0; i < new_modes; ++i) {
            if (!w.emit({(uint8_t)(pbmp[i] & 0xFF), (uint8_t)((pbmp[i] >> 8) & 0xFF),
                         (uint8_t)((pbmp[i] >> 16) & 0xFF), 0},
                        false)) {
                result.error = w.error;
                return result;
            }
        }
        uint32_t ppstr[2] = {0, 0};
        for (int i = 0; i < 2; ++i) {
            const int64_t p = (int64_t)w.cursor - 6 * (int64_t)(old_modes + 1);
            if (p < 0) {
                result.error = "string table underflow";
                return result;
            }
            ppstr[i] = (uint32_t)p;
            for (uint32_t j = 0; j < new_modes; ++j) {
                if (!w.emit({(uint8_t)(pstr[j][(size_t)i] & 0xFF),
                             (uint8_t)((pstr[j][(size_t)i] >> 8) & 0xFF),
                             (uint8_t)((pstr[j][(size_t)i] >> 16) & 0xFF), 0,
                             (uint8_t)(i == 0 ? codes[j] : 0), 0},
                            false)) {
                    result.error = w.error;
                    return result;
                }
            }
        }
        const uint32_t mode_table = ppstr[0] + 4;
        const uint32_t str_table = (uint32_t)w.cursor;
        for (int i = 0; i < 2; ++i) {
            if (!w.emit({(uint8_t)(ppstr[i] & 0xFF), (uint8_t)((ppstr[i] >> 8) & 0xFF)}, false)) {
                result.error = w.error;
                return result;
            }
        }

        {
            if (!w.write_at(a.main_hook, B((uint32_t)w.cursor))) {
                result.error = w.error;
                return result;
            }
        }
        for (uint32_t i = 0; i < new_modes; ++i) {
            if (!w.emit({codes[i], 0x70,
                         (uint8_t)(4 + i + 2 * (new_modes - 1)), 0xC9},
                        false)) {
                result.error = w.error;
                return result;
            }
        }
        if (!w.emit(BL(a.comp_func), false)) { result.error = w.error; return result; }
        if (!w.emit(B(a.main_hook + 4), false)) { result.error = w.error; return result; }
        for (uint32_t i = 0; i < new_modes; ++i) {
            if (!w.emit(BL(pfunc[i]), false)) { result.error = w.error; return result; }
            if (!w.emit({(uint8_t)(uint32_t)(-3 * (int)(i + 1) - 2), 0xCE}, false)) {
                result.error = w.error;
                return result;
            }
        }

        if (icon_table < 4 * old_modes + 4) {
            result.error = "icon table underflow";
            return result;
        }
        icon_table -= 4 * old_modes + 4;

        {
            if (!w.write_at(a.menu_hook, B((uint32_t)w.cursor))) {
                result.error = w.error;
                return result;
            }
        }
        {
            std::vector<uint8_t> f;
            f.insert(f.end(), {(uint8_t)old_modes, 0x70, 0x04, 0xC2});
            const std::vector<uint8_t> mul = BL(a.mul_func);
            f.insert(f.end(), mul.begin(), mul.end());
            const std::vector<uint8_t> b1 = B(a.menu_hook + 4);
            f.insert(f.end(), b1.begin(), b1.end());
            f.insert(f.end(), {0x80, 0xE1, 0xF6, 0xE1, 0x1A, 0xA2, 0x6E, 0xF4, 0x00, 0x01,
                               0x2B, 0x91, 0x2A, 0x90});
            f.insert(f.end(), {(uint8_t)((icon_table >> 16) & 0xFF), 0xE3, 0x08, 0xA4,
                               (uint8_t)(icon_table & 0xFF), (uint8_t)((icon_table >> 8) & 0xFF)});
            f.insert(f.end(), {(uint8_t)((icon_table >> 16) & 0xFF), 0xE3, 0x08, 0x96,
                               (uint8_t)((icon_table + 2) & 0xFF),
                               (uint8_t)(((icon_table + 2) >> 8) & 0xFF)});
            f.insert(f.end(), {0x8A, 0x01, 0x6F, 0x90, 0x40, 0x90, 0x21, 0x90, 0x81, 0xE4,
                               0x81, 0xE2, 0xFF, 0x11, 0xF9, 0xC8, 0x2E, 0xF4, 0x1A, 0xA0});
            const std::vector<uint8_t> b2 = B(a.menu_hook + 8);
            f.insert(f.end(), b2.begin(), b2.end());
            if (!w.emit(f)) { result.error = w.error; return result; }
        }

        {
            if (!w.write_at(a.menu_hook + 0x36, B((uint32_t)w.cursor))) {
                result.error = w.error;
                return result;
            }
        }
        {
            std::vector<uint8_t> f;
            const std::vector<uint8_t> unk = BL(a.unknown_func);
            f.insert(f.end(), unk.begin(), unk.end());
            f.insert(f.end(), {(uint8_t)old_modes, 0x7C, 0x02, 0xC2});
            const std::vector<uint8_t> b = B(a.menu_hook + 0x3A);
            f.insert(f.end(), b.begin(), b.end());
            f.insert(f.end(), {0x40, 0xE1, 0x4A, 0xE1, 0xFB, 0xCE});
            if (!w.emit(f)) { result.error = w.error; return result; }
        }

        {
            if (!w.write_at(a.menu_hook + 0xF4, B((uint32_t)w.cursor))) {
                result.error = w.error;
                return result;
            }
        }
        {
            std::vector<uint8_t> f;
            const uint8_t dsr = (uint8_t)((str_table >> 16) & 0xFF);
            f.insert(f.end(), {(uint8_t)(6 * old_modes), 0x70, 0x04, 0xC2, 0x08, 0xA2,
                               (uint8_t)(a.sys_str_table & 0xFF),
                               (uint8_t)(((a.sys_str_table + 2) >> 8) & 0xFF)});
            const std::vector<uint8_t> b1 = B(a.menu_hook + 0xF8);
            f.insert(f.end(), b1.begin(), b1.end());
            f.insert(f.end(), {0xF2, 0xE1, 0x1A, 0xA2, 0x2A, 0xF0, 0x10, 0x92, 0x2B, 0xD1,
                               0x00, 0x03, 0x26, 0xF2, dsr, 0xE3, 0x28, 0xA2,
                               (uint8_t)(str_table & 0xFF), (uint8_t)((str_table >> 8) & 0xFF),
                               0x26, 0xF0, dsr, 0xE3, 0x02, 0x92, 0x82, 0xE0, dsr, 0xE3, 0x00,
                               0x91, 0x02, 0xCE, 0x81, 0xE2, 0x51, 0x90, 0x1F, 0x90, 0x20, 0x90,
                               0xFB, 0xC8, 0x31, 0x90, 0x33, 0x01, 0x16, 0x00, 0x1A, 0xA2});
            const std::vector<uint8_t> lp = BL(a.line_print_func);
            f.insert(f.end(), lp.begin(), lp.end());
            f.insert(f.end(), {0x0E, 0xE1});
            const std::vector<uint8_t> b2 = B(a.menu_hook + 0x100);
            f.insert(f.end(), b2.begin(), b2.end());
            if (!w.emit(f)) { result.error = w.error; return result; }
        }

        {
            const uint32_t addr = (uint32_t)w.cursor;
            for (uint32_t site : {a.mode_hook_1, a.mode_hook_2, a.mode_hook_2 + 0x1Eu})
                if (!w.write_at(site, BL(addr))) {
                    result.error = w.error;
                    return result;
                }
        }
        {
            std::vector<uint8_t> f;
            f.insert(f.end(), {(uint8_t)(6 * old_modes), 0x70, 0x03, 0xC2, 0x08, 0x90,
                               (uint8_t)(a.sys_mode_table & 0xFF),
                               (uint8_t)(((a.sys_mode_table + 2) >> 8) & 0xFF), 0x1F, 0xFE});
            f.insert(f.end(), {(uint8_t)((mode_table >> 16) & 0xFF), 0xE3, 0x08, 0x90,
                               (uint8_t)(mode_table & 0xFF), (uint8_t)((mode_table >> 8) & 0xFF),
                               0x1F, 0xFE});
            if (!w.emit(f)) { result.error = w.error; return result; }
        }

        result.icon_table = icon_table;
        result.str_table = str_table;
    }

    if (!w.ok) {
        result.error = w.error;
        return result;
    }

    // The reference writes the old count + new modes as a byte (wrapping).  We
    // validated the sum fits, so this is exact.
    w.rom[a.mode_num_addr] = (uint8_t)(old_modes + new_modes);

    if (w.cursor <= target) {
        result.error = "internal error: patcher wrote nothing";
        return result;
    }
    if (w.cursor > a.window) {
        result.error = "the generated block does not fit in the ROM";
        return result;
    }

    // Free-space validation: every byte the generated block occupies must have
    // been erased (0xFF) in the original image.  Hooks inside the firmware are
    // not part of this range.
    if (!options.force) {
        for (uint64_t i = target; i < w.cursor; ++i) {
            if (input[(size_t)i] != 0xFF) {
                result.error = "target range is not blank at 0x" + std::to_string(i) +
                               " (expected 0xFF); pass --force to overwrite";
                return result;
            }
        }
        // The icon/text write regions were cleared in the working copy above;
        // make sure a rejected non-blank byte did not get silently zeroed.
    }

    result.ok = true;
    result.rom = std::move(w.rom);
    result.target = target;
    result.final_cursor = (uint32_t)w.cursor;
    result.mode_count_after = old_modes + new_modes;
    result.log = std::move(w.log);

    // Diff spans.
    uint32_t span_start = 0;
    bool in_span = false;
    for (uint32_t i = 0; i < (uint32_t)result.rom.size(); ++i) {
        if (result.rom[i] != input[i]) {
            if (!in_span) {
                span_start = i;
                in_span = true;
            }
        } else if (in_span) {
            result.changed.emplace_back(span_start, i);
            in_span = false;
        }
    }
    if (in_span) result.changed.emplace_back(span_start, (uint32_t)result.rom.size());
    return result;
}

}  // namespace cwmode
}  // namespace wiz
