// patch.cpp - the ROM patch engine (see patch.hpp).
#include "patch.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "build.hpp"
#include "common.hpp"

namespace wiz {
namespace {

constexpr int kRomSize = 262144;
constexpr int kMenuBase = 0x7FF8;  // menu table
constexpr int kNameBase = 0x5516;
constexpr int kMenuSlots = 12;
constexpr int kHook = 0x25E1E;
const uint8_t kHookOld[4] = {0x03, 0xE8, 0x30, 0x68};
const uint8_t kHookNew[4] = {0x00, 0xF3, 0x40, 0x0E};  // B 3:0E40
constexpr int kBase = 0x30E40;
const char* kSlotChars = "123456789ABC";

int u16(const std::vector<uint8_t>& r, int o) {
    return r[(size_t)o] | (r[(size_t)o + 1] << 8);
}

std::string cstr(const std::vector<uint8_t>& r, int a) {
    std::string s;
    while (a < (int)r.size() && r[(size_t)a] && s.size() < 40) {
        s += (char)r[(size_t)a];
        a++;
    }
    return s;
}

std::string hex_bytes(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (i) s += ' ';
        s += fmt("%02x", p[i]);
    }
    return s;
}

std::string check_menu(const std::vector<uint8_t>& r) {
    for (int k = 0; k < kMenuSlots; k++) {
        int o = kMenuBase + 6 * k;
        int ip = u16(r, o), dp = u16(r, o + 4);
        if (!(0x100 <= ip && ip <= kRomSize - 138) || r[(size_t)o + 3] ||
            r[(size_t)dp] != 2 || r[(size_t)dp + 2] || r[(size_t)dp + 3])
            return fmt("menu entry %c does not look right", kSlotChars[k]);
        std::string s = cstr(r, u16(r, kNameBase + 2 * (r[(size_t)dp + 1] - 2)));
        bool ok = s.size() >= 3;
        for (char c : s)
            if (!((unsigned char)c >= 32 && (unsigned char)c < 127)) ok = false;
        if (!ok) return fmt("menu entry %c: cannot read its name", kSlotChars[k]);
    }
    return "";
}

std::string slot_name(const std::vector<uint8_t>& r, int k) {
    int o = kMenuBase + 6 * k;
    return cstr(r, u16(r, kNameBase + 2 * (r[(size_t)u16(r, o + 4) + 1] - 2)));
}

std::string slot_error(int slot) {
    return fmt("slot must be 1..%d", kMenuSlots);
}

// Size, menu shape and hook checks shared by both entry points.
bool validate_rom(RomPatchReport& out, const std::vector<uint8_t>& rom, bool allow_hook_mismatch) {
    if ((int)rom.size() != kRomSize) {
        out.error = fmt("ROM is %d bytes; an fx-580VN X dump is %d", (int)rom.size(), kRomSize);
        return false;
    }
    std::string bad = check_menu(rom);
    if (!bad.empty()) {
        out.error = "this does not look like the ROM these addresses were read from: " + bad;
        return false;
    }
    if (std::memcmp(&rom[(size_t)kHook], kHookOld, 4) != 0) {
        if (!allow_hook_mismatch) {
            out.error = fmt("set_mode at %05X is %s, expected %s (different firmware?)", kHook,
                            hex_bytes(&rom[(size_t)kHook], 4).c_str(),
                            hex_bytes(kHookOld, 4).c_str());
            return false;
        }
        out.hook_forced = true;
        out.warnings.push_back(fmt("set_mode at %05X is %s; writing the hook anyway", kHook,
                                   hex_bytes(&rom[(size_t)kHook], 4).c_str()));
    }

    // Warn about slots whose mode id jumps into seg-3 stubs (0xFF) in this ROM.
    const int broken_modes[] = {0x06, 0x07, 0x0C, 0x88};
    std::vector<std::string> broken;
    for (int k = 0; k < kMenuSlots; k++) {
        int m = rom[(size_t)(kMenuBase + 6 * k + 2)];
        for (int b : broken_modes)
            if (b == m) broken.push_back(std::string(1, kSlotChars[k]));
    }
    if (!broken.empty())
        out.warnings.push_back(fmt("menu slots %s use firmware modes that jump to unimplemented "
                                   "seg-3 stubs in this ROM; selecting them on the real calculator "
                                   "will crash (a ROM issue, not the patch)",
                                   join(broken, ",").c_str()));
    return true;
}

// Write a compiled cwapp image into `rom` and select `slot`.
void apply_program(RomPatchReport& out, std::vector<uint8_t>& rom, const BuildResult& br,
                   int slot, bool force) {
    out.slot = slot;
    std::vector<uint8_t> code;
    if (br.rom.size() > (size_t)kBase) code.assign(br.rom.begin() + kBase, br.rom.end());
    out.code_size = (int)code.size();

    if ((long)kBase + (long)code.size() > kRomSize) {
        out.error = fmt("program is %d bytes, only %d fit", (int)code.size(), kRomSize - kBase);
        return;
    }
    if (!force) {
        for (size_t i = 0; i < code.size(); i++)
            if (rom[(size_t)kBase + i] != 0xFF) {
                out.error = fmt("%05X.. is not free flash (not all FF); use force to overwrite anyway",
                                kBase);
                return;
            }
    }

    for (size_t i = 0; i < code.size(); i++) rom[(size_t)kBase + i] = code[i];
    out.changes.push_back(fmt("%05X  program (%d bytes)", kBase, (int)code.size()));
    for (int i = 0; i < 4; i++) rom[(size_t)kHook + i] = kHookNew[i];
    out.changes.push_back(fmt("%05X  hook  %s -> %s", kHook,
                              hex_bytes(kHookOld, 4).c_str(), hex_bytes(kHookNew, 4).c_str()));

    int o = kMenuBase + 6 * (slot - 1) + 2;
    std::string old = hex_bytes(&rom[(size_t)o], 2);
    std::string name = slot_name(rom, slot - 1);
    rom[(size_t)o] = 0xE0;
    rom[(size_t)o + 1] = 0;
    out.changes.push_back(
        fmt("%05X  menu slot %c (%s) mode id %s -> E0 00", o, kSlotChars[slot - 1], name.c_str(),
            old.c_str()));

    auto app_it = br.syms.find("_APP");
    out.app_entry = app_it == br.syms.end() ? 0 : (uint32_t)app_it->second;
    out.rom = std::move(rom);
    out.ok = true;
}

}  // namespace

RomPatchReport patch_rom(std::vector<uint8_t> rom, const std::string& source,
                         const std::string& source_name, int slot, bool force,
                         bool allow_hook_mismatch) {
    RomPatchReport out;
    out.slot = slot;

    if (!validate_rom(out, rom, allow_hook_mismatch)) return out;

    BuildResult br;
    try {
        if (ends_with(source_name, ".cwiz"))
            br = build_cwiz(source, "cwapp", source_name);
        else
            br = build_source(source, "cwapp");
    } catch (const CompileError& e) {
        out.error = e.what();
        out.error_line = e.line;
        out.error_col = e.col;
        return out;
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }

    if (slot <= 0) slot = br.slot;
    out.slot = slot;
    if (!(1 <= slot && slot <= kMenuSlots)) {
        out.error = slot_error(slot);
        return out;
    }
    apply_program(out, rom, br, slot, force);
    return out;
}

RomPatchReport patch_rom_image(std::vector<uint8_t> rom, const BuildResult& image, int slot,
                               bool force, bool allow_hook_mismatch) {
    RomPatchReport out;
    out.slot = slot;

    if (!validate_rom(out, rom, allow_hook_mismatch)) return out;

    if (slot <= 0) slot = image.slot;
    out.slot = slot;
    if (!(1 <= slot && slot <= kMenuSlots)) {
        out.error = slot_error(slot);
        return out;
    }
    apply_program(out, rom, image, slot, force);
    return out;
}

}  // namespace wiz
