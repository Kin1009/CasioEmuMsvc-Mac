// cwlink.cpp - .cwapplink project builder (see cwlink.hpp).
#include "cwlink.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "build.hpp"
#include "common.hpp"
#include "cpre.hpp"
#include "cwmode.hpp"
#include "patch.hpp"

namespace wiz {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kCodeStart = 0x30E40;
constexpr uint32_t kRomSize = 0x40000;
constexpr int kLegacySlots = 12;
constexpr int kMenuBase = 0x7FF8;
constexpr int kHook = 0x25E1E;
constexpr uint8_t kHookNew[4] = {0x00, 0xF3, 0x40, 0x0E};  // B 3:0E40
constexpr int kBootDispatch = 0x29448;  // BL main function 2:E234 in stock

std::string basename_of(const std::string& path) {
    return fs::path(path).filename().string();
}

std::string resolve_relative(const std::string& base_file, const std::string& path) {
    fs::path p(path);
    if (p.is_absolute()) return p.lexically_normal().string();
    fs::path dir = fs::path(base_file).parent_path();
    return (dir / p).lexically_normal().string();
}

std::string canonical_path(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(fs::absolute(fs::path(path), ec), ec);
    if (ec) return fs::path(path).lexically_normal().string();
    return p.string();
}

std::vector<uint8_t> branch(uint8_t opcode, uint32_t addr) {
    return {opcode, (uint8_t)(0xF0 | ((addr >> 16) & 0x0F)), (uint8_t)(addr & 0xFF),
            (uint8_t)((addr >> 8) & 0xFF)};
}

// `cmp r8, #imm8` followed by `bne +4`: the exact bytes the firmware template
// uses in its single-app trampoline.
void append_compare(std::vector<uint8_t>& out, uint8_t id) {
    out.push_back(id);
    out.push_back(0x78);
    out.push_back(0x02);
    out.push_back(0xC8);
}

bool parse_manifest_line(const std::string& raw, std::string& path, bool& quoted) {
    std::string text = strip(raw);
    quoted = false;
    if (text.empty()) return false;
    if (text[0] == '#') return false;
    if (text[0] == '"') {
        quoted = true;
        std::string out;
        size_t i = 1;
        bool closed = false;
        while (i < text.size()) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                out += text[i + 1];
                i += 2;
                continue;
            }
            if (text[i] == '"') {
                i++;
                closed = true;
                break;
            }
            out += text[i++];
        }
        if (!closed) {
            path = text;
            return true;
        }
        path = out;
        return true;
    }
    path = text;
    return true;
}

struct Prepared {
    CwappLinkApp app;
    BuildResult built;       // build at 0x30E40 (size probe)
    cwmode::Image icon;      // extra form
    cwmode::Image icon_sel;
};

bool same_image(const cwmode::Image& a, const cwmode::Image& b) {
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}

}  // namespace

bool is_cwapplink_path(const std::string& path) {
    return ends_with(to_lower(path), ".cwapplink");
}

std::string slot_key_label(int slot) {
    static const char* const labels[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "A", "B", "C", "D", "E", "F",
        "x", "y", "z", "M", "+", "-", "*", "/", "."};
    if (slot < 1 || slot > (int)(sizeof(labels) / sizeof(labels[0]))) return "";
    return labels[slot - 1];
}

std::string slot_key_legend(int slot) {
    static const char* const legends[] = {
        "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "A (-)", "B deg", "C x^-1", "D sin", "E cos", "F tan",
        "x (", "y )", "z s<>d", "M M+", "+", "-", "*", "/", "."};
    if (slot < 1 || slot > (int)(sizeof(legends) / sizeof(legends[0]))) return "";
    return legends[slot - 1];
}

bool parse_cwapplink_manifest(const std::string& manifest_path, std::vector<CwappLinkEntry>& out,
                              std::string& error, int& error_line) {
    out.clear();
    std::ifstream in(manifest_path, std::ios::binary);
    if (!in) {
        error = "cannot read the project file";
        error_line = 0;
        return false;
    }
    std::set<std::string> seen;
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        number++;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (number == 1 && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);
        std::string path;
        bool quoted = false;
        if (!parse_manifest_line(line, path, quoted)) continue;
        if (path.empty()) {
            error = "empty source path";
            error_line = number;
            return false;
        }
        const std::string resolved = canonical_path(resolve_relative(manifest_path, path));
        std::error_code ec;
        if (!fs::is_regular_file(resolved, ec)) {
            error = "source file not found: " + path;
            error_line = number;
            return false;
        }
        const std::string ext = to_lower(fs::path(resolved).extension().string());
        if (ext != ".cwiz" && ext != ".wz") {
            error = "not a .cwiz or .wz source: " + path;
            error_line = number;
            return false;
        }
        if (!seen.insert(resolved).second) {
            error = "duplicate source in the project: " + path;
            error_line = number;
            return false;
        }
        out.push_back(CwappLinkEntry{resolved, number});
    }
    if (out.empty()) {
        error = "the project lists no source files";
        error_line = 0;
        return false;
    }
    return true;
}

CwappLinkResult build_cwapplink(const std::string& manifest_path,
                                const std::vector<uint8_t>& base_rom, bool force) {
    CwappLinkResult out;
    out.error_file = manifest_path;

    std::vector<CwappLinkEntry> entries;
    std::string parse_error;
    int parse_line = 0;
    if (!parse_cwapplink_manifest(manifest_path, entries, parse_error, parse_line)) {
        out.error = parse_error;
        out.error_line = parse_line;
        return out;
    }

    if (base_rom.size() != kRomSize) {
        out.error = fmt("the base ROM is %d bytes; the .cwapplink builder needs a 262144-byte "
                        "fx-580VN X image (pass --rom)",
                        (int)base_rom.size());
        return out;
    }
    const std::string rom_error = validate_580_rom(base_rom, true);
    if (!rom_error.empty()) {
        out.error = rom_error;
        return out;
    }
    const cwmode::Analysis base_analysis = cwmode::analyze(base_rom, cwmode::Model::Cw580);
    if (!base_analysis.ok) {
        out.error = base_analysis.error;
        return out;
    }

    std::vector<Prepared> prepared;
    std::map<int, std::string> identities;
    for (const CwappLinkEntry& entry : entries) {
        Prepared p;
        p.app.path = entry.path;
        p.app.line = entry.line;
        try {
            p.built = build_path(entry.path, "cwapp", "").r;
        } catch (const SectionError& e) {
            out.error = std::string("not a #cwapp source: ") + e.what();
            out.error_file = entry.path;
            return out;
        } catch (const CompileError& e) {
            out.error = e.what();
            if (ends_with(to_lower(entry.path), ".cwiz")) {
                CwizPos pos = cwiz_error_pos(entry.path, e.line);
                out.error_file = pos.file.empty() ? entry.path : pos.file;
                out.error_line = pos.line;
            } else {
                out.error_file = entry.path;
                out.error_line = e.line;
            }
            return out;
        } catch (const std::exception& e) {
            out.error = e.what();
            out.error_file = entry.path;
            return out;
        }
        if (!p.built.cwapp) {
            out.error = "not a #cwapp source: add '#for 580' and '#cwapp'";
            out.error_file = entry.path;
            return out;
        }
        p.app.legacy = !p.built.cwapp_extra;
        p.app.slot = p.app.legacy ? p.built.slot : p.built.cwapp_slot;
        p.app.legacy_slot = p.app.legacy ? p.app.slot : 0;
        if (p.app.legacy) {
            if (p.app.slot < 1 || p.app.slot > kLegacySlots) {
                out.error = fmt("legacy #cwapp slots are 1..%d, got %d", kLegacySlots, p.app.slot);
                out.error_file = entry.path;
                return out;
            }
        } else if (p.app.slot <= 0) {
            out.error = "the #cwapp app slot must be a positive integer";
            out.error_file = entry.path;
            return out;
        }
        auto dup = identities.find(p.app.slot);
        if (dup != identities.end()) {
            out.error = fmt("duplicate app slot %d (also used by %s)", p.app.slot,
                            basename_of(dup->second).c_str());
            out.error_file = entry.path;
            return out;
        }
        identities[p.app.slot] = entry.path;

        if (!p.app.legacy) {
            p.app.text = p.built.cwapp_text;
            if (p.app.text.empty()) {
                out.error = "the #cwapp app text is empty";
                out.error_file = entry.path;
                return out;
            }
            p.app.icon = resolve_relative(entry.path, p.built.cwapp_icon);
            std::string image_error;
            if (!cwmode::load_image(p.app.icon, p.icon, image_error)) {
                out.error = image_error;
                out.error_file = entry.path;
                return out;
            }
            // Apply the stock tile decoration (border + shortcut-key box) to
            // the unselected image.  Pre-decorated images are detected and are
            // not cropped a second time.
            p.icon = cwmode::prepare_mode_icon(p.icon, cwmode::Model::Cw580);
            if (p.built.cwapp_icon_selected.empty()) {
                // Three-argument form: the selected variant is the exact
                // inverse of the decorated unselected image.
                p.app.icon_selected.clear();
                p.icon_sel = cwmode::invert_image(p.icon);
            } else {
                p.app.icon_selected =
                    resolve_relative(entry.path, p.built.cwapp_icon_selected);
                if (!cwmode::load_image(p.app.icon_selected, p.icon_sel, image_error)) {
                    out.error = image_error;
                    out.error_file = entry.path;
                    return out;
                }
                p.icon_sel = cwmode::prepare_mode_icon(p.icon_sel, cwmode::Model::Cw580);
            }
            if (!same_image(p.icon, p.icon_sel)) {
                out.warnings.push_back(fmt(
                    "%s: the firmware stores one 48x23 icon per mode; using the unselected "
                    "image (the menu draws its own selection highlight, the selected image "
                    "cannot be stored separately)",
                    basename_of(entry.path).c_str()));
            }
        }

        if (p.built.rom.size() <= kCodeStart) {
            out.error = "the compiled app has no code at 3:0E40";
            out.error_file = entry.path;
            return out;
        }
        p.app.size = (uint32_t)(p.built.rom.size() - kCodeStart);
        auto app_sym = p.built.syms.find("_APP");
        if (app_sym == p.built.syms.end()) {
            out.error = "the compiled app has no _APP entry symbol";
            out.error_file = entry.path;
            return out;
        }
        p.app.entry = (uint32_t)app_sym->second;
        prepared.push_back(std::move(p));
    }

    // The app slot is the shortcut key position.  Build, place and append in
    // slot order, not manifest order.
    std::sort(prepared.begin(), prepared.end(),
              [](const Prepared& a, const Prepared& b) { return a.app.slot < b.app.slot; });

    // Stock menu slots 1..12 own the first twelve shortcut keys.  An extra app
    // cannot take one without a legacy #slot replacement.
    for (const Prepared& p : prepared) {
        if (!p.app.legacy && p.app.slot >= 1 && p.app.slot <= kLegacySlots) {
            out.error = fmt(
                "slot %d is the stock menu shortcut key %s; use slots 13..24 for extra "
                "apps, or a legacy '#cwapp' with '#slot %d' to replace that menu slot",
                p.app.slot, slot_key_label(p.app.slot).c_str(), p.app.slot);
            out.error_file = p.app.path;
            return out;
        }
    }
    // The firmware labels appended modes in order.  For the keyed slots
    // 13..24, require a contiguous run from 13 so the label equals the slot.
    int expected_key_slot = 13;
    for (const Prepared& p : prepared) {
        if (p.app.legacy || p.app.slot < 13 || p.app.slot > 24) continue;
        if (p.app.slot != expected_key_slot) {
            out.error = fmt(
                "app slot %d (key %s) needs slot %d first: the firmware labels appended "
                "modes by order, so keyed slots 13..24 must be contiguous from 13",
                p.app.slot, slot_key_label(p.app.slot).c_str(), expected_key_slot);
            out.error_file = p.app.path;
            return out;
        }
        expected_key_slot++;
    }

    // ---- assign code blocks
    int legacy_count = 0;
    for (const Prepared& p : prepared)
        if (p.app.legacy) legacy_count++;
    const bool have_extras = legacy_count < (int)prepared.size();
    // 8 bytes per app (cmp+bne, B) plus the 12-byte replay tail, prefixed by
    // the 6-byte clear of the boot-path flag.
    const uint32_t dispatcher_size =
        legacy_count > 1 ? (uint32_t)(8 * legacy_count + 12 + 6) : 0;
    // Multi-legacy ROMs also need a boot dispatcher: after ON the firmware
    // reaches the main hook at 2:9448 with the running mode id in R0.  Each
    // match sets the boot-path flag (clobbering R0) and calls the app.
    const uint32_t boot_size = legacy_count > 1 ? (uint32_t)(18 * legacy_count + 4) : 0;
    uint32_t cursor = kCodeStart + dispatcher_size + boot_size;
    if (cursor & 1) cursor++;

    std::vector<std::pair<uint32_t, uint32_t>> used;  // base, size
    auto place = [&](Prepared& p) {
        if (p.app.legacy && legacy_count == 1) {
            p.app.base = kCodeStart;
            cursor = kCodeStart + p.app.size;
            if (cursor & 1) cursor++;
        } else {
            p.app.base = cursor;
            cursor += p.app.size;
            if (cursor & 1) cursor++;
        }
        if (p.app.base + p.app.size > kRomSize) {
            out.error = fmt("the project does not fit: %s starts at %05X and needs %u bytes",
                            basename_of(p.app.path).c_str(), p.app.base, p.app.size);
            out.error_file = p.app.path;
            return false;
        }
        used.push_back({p.app.base, p.app.size});
        return true;
    };

    // The single legacy app must stay at 3:0E40 (the firmware hook target);
    // place everything else in manifest order.
    for (Prepared& p : prepared)
        if (p.app.legacy && legacy_count == 1)
            if (!place(p)) return out;
    for (Prepared& p : prepared)
        if (!(p.app.legacy && legacy_count == 1))
            if (!place(p)) return out;

    std::sort(used.begin(), used.end());
    for (size_t i = 1; i < used.size(); i++) {
        if (used[i - 1].first + used[i - 1].second > used[i].first) {
            out.error = fmt("code blocks overlap at %05X", used[i].first);
            return out;
        }
    }

    // ---- mode ids: safe A0..BF range, avoiding the stock ids (E0, E1, 02,
    // 06, 07, 03, 0C, 88, 45, 4B, 89, 4A) and the legacy trampoline's E0.
    const int id_limit = 0xC0;  // A0..BF
    int next_id = 0xA0;
    if (legacy_count == 1) {
        for (Prepared& p : prepared)
            if (p.app.legacy) p.app.mode_code = 0xE0;  // exact legacy bytes
    } else {
        for (Prepared& p : prepared)
            if (p.app.legacy) {
                if (next_id >= id_limit) {
                    out.error = "too many apps: the safe mode-id range A0..BF is full";
                    return out;
                }
                p.app.mode_code = (uint8_t)next_id++;
            }
    }
    for (Prepared& p : prepared)
        if (!p.app.legacy) {
            if (next_id >= id_limit) {
                out.error = "too many apps: the safe mode-id range A0..BF is full (32 modes)";
                return out;
            }
            p.app.mode_code = (uint8_t)next_id++;
        }

    // ---- rebuild each app at its final base and extract its block
    std::map<std::string, std::vector<uint8_t>> blocks;
    for (Prepared& p : prepared) {
        BuildResult built;
        if (p.app.base == kCodeStart) {
            built = p.built;  // exact legacy build
        } else {
            try {
                built = build_path(p.app.path, "cwapp", "", p.app.base).r;
            } catch (const CompileError& e) {
                out.error = e.what();
                out.error_file = p.app.path;
                out.error_line = e.line;
                return out;
            } catch (const std::exception& e) {
                out.error = e.what();
                out.error_file = p.app.path;
                return out;
            }
            if (built.rom.size() <= p.app.base ||
                (uint32_t)(built.rom.size() - p.app.base) != p.app.size) {
                out.error = "the code block size changed when the app was relocated";
                out.error_file = p.app.path;
                return out;
            }
            auto sym = built.syms.find("_APP");
            if (sym == built.syms.end() || (uint32_t)sym->second < p.app.base ||
                (uint32_t)sym->second >= p.app.base + p.app.size) {
                out.error = "the relocated app has no usable _APP entry";
                out.error_file = p.app.path;
                return out;
            }
            p.app.entry = (uint32_t)sym->second;
        }
        blocks[p.app.path] =
            std::vector<uint8_t>(built.rom.begin() + p.app.base, built.rom.end());
    }

    // ---- write the code blocks into a copy of the base ROM
    std::vector<uint8_t> rom = base_rom;
    auto blank_or_force = [&](uint32_t base, uint32_t size, const std::string& path) {
        if (force) return true;
        for (uint32_t i = 0; i < size; i++) {
            if (rom[(size_t)base + i] != 0xFF) {
                out.error = fmt("%05X is not free flash (not 0xFF); use --force to overwrite",
                                base + i);
                out.error_file = path;
                return false;
            }
        }
        return true;
    };

    Prepared* legacy_one = nullptr;
    if (legacy_count == 1) {
        for (Prepared& p : prepared)
            if (p.app.legacy) legacy_one = &p;
    }

    if (legacy_one) {
        RomPatchReport pr = patch_rom_image(rom, legacy_one->built, legacy_one->app.slot, force,
                                            true, prepared.size() == 1);
        for (const std::string& warning : pr.warnings) out.warnings.push_back(warning);
        if (!pr.ok) {
            out.error = pr.error;
            out.error_file = legacy_one->app.path;
            out.error_line = pr.error_line;
            return out;
        }
        rom = std::move(pr.rom);
        legacy_one->app.entry = pr.app_entry;
        if ((uint32_t)pr.code_size != legacy_one->app.size) {
            out.error = "internal error: legacy app size changed while patching";
            return out;
        }
        if (legacy_one->app.mode_code != 0xE0) {
            out.error = "internal error: a single legacy app must use mode id E0";
            return out;
        }
    }

    for (Prepared& p : prepared) {
        if (&p == legacy_one) continue;
        const std::vector<uint8_t>& code = blocks[p.app.path];
        if (!blank_or_force(p.app.base, (uint32_t)code.size(), p.app.path)) return out;
        std::memcpy(rom.data() + p.app.base, code.data(), code.size());
    }

    if (legacy_count > 1) {
        // Dispatcher at 3:0E40: compare R8 with each legacy mode id, otherwise
        // replay the two instructions the hook replaced.  Clear the boot-path
        // flag first: these apps are entered at _APP, not through _TRAMP.
        std::vector<uint8_t> disp = {0x00, 0x00, 0x11, 0x90, 0xF4, 0xE7};
        for (Prepared& p : prepared) {
            if (!p.app.legacy) continue;
            append_compare(disp, p.app.mode_code);
            std::vector<uint8_t> b = branch(0x00, p.app.entry);
            disp.insert(disp.end(), b.begin(), b.end());
        }
        append_compare(disp, 0x03);
        std::vector<uint8_t> b88 = branch(0x00, 0x25E88);
        disp.insert(disp.end(), b88.begin(), b88.end());
        std::vector<uint8_t> b22 = branch(0x00, 0x25E22);
        disp.insert(disp.end(), b22.begin(), b22.end());
        if (disp.size() != dispatcher_size) {
            out.error = "internal error: dispatcher size mismatch";
            return out;
        }
        if (!blank_or_force(kCodeStart, (uint32_t)disp.size(), manifest_path)) return out;
        std::memcpy(rom.data() + kCodeStart, disp.data(), disp.size());
        std::memcpy(rom.data() + kHook, kHookNew, 4);
        for (Prepared& p : prepared) {
            if (!p.app.legacy) continue;
            const int off = kMenuBase + 6 * (p.app.slot - 1) + 2;
            rom[(size_t)off] = p.app.mode_code;
            rom[(size_t)off + 1] = 0;
        }

        // Boot dispatcher: 2:9448 calls the main function with R0 = D111 (the
        // running mode id).  Relaunch that app; other ids tail-call the stock
        // menu function, whose return address is already on the stack.  When
        // extras exist cwmode overwrites the hook, so that case is handled
        // after the cwmode patch below.
        if (!have_extras) {
            std::vector<uint8_t> boot;
            for (Prepared& p : prepared) {
                if (!p.app.legacy) continue;
                boot.push_back(p.app.mode_code);
                boot.push_back(0x70);  // cmp r0, #id
                boot.push_back(0x07);
                boot.push_back(0xC8);  // bne +7 words (skip flag set, BL and B)
                boot.insert(boot.end(), {0x01, 0x00, 0x11, 0x90, 0xF4, 0xE7});
                std::vector<uint8_t> bl = branch(0x01, p.app.entry);
                boot.insert(boot.end(), bl.begin(), bl.end());
                std::vector<uint8_t> fb = branch(0x00, 0x2E234);
                boot.insert(boot.end(), fb.begin(), fb.end());
            }
            std::vector<uint8_t> boot_fb = branch(0x00, 0x2E234);
            boot.insert(boot.end(), boot_fb.begin(), boot_fb.end());
            if (boot.size() != boot_size) {
                out.error = "internal error: boot dispatcher size mismatch";
                return out;
            }
            const uint32_t boot_addr = kCodeStart + dispatcher_size;
            if (!blank_or_force(boot_addr, (uint32_t)boot.size(), manifest_path)) return out;
            std::memcpy(rom.data() + boot_addr, boot.data(), boot.size());
            std::vector<uint8_t> hook_boot = branch(0x01, boot_addr);
            std::memcpy(rom.data() + kBootDispatch, hook_boot.data(), hook_boot.size());
        }
    }

    // ---- append the extra apps as firmware modes
    std::vector<cwmode::ModeSpec> modes;
    uint32_t after_code = kCodeStart;
    for (const Prepared& p : prepared) after_code = std::max(after_code, p.app.base + p.app.size);
    if (after_code & 1) after_code++;
    // The cwmode analysis treats the first long 0xFF run as the firmware code
    // end; after our blocks that run starts a couple of bytes later.  Keep the
    // append point at or after it so the "inside the firmware code" guard is
    // satisfied.
    const cwmode::Analysis written = cwmode::analyze(rom, cwmode::Model::Cw580);
    if (written.ok) after_code = std::max(after_code, written.code_end);
    for (Prepared& p : prepared) {
        if (p.app.legacy) continue;
        cwmode::ModeSpec spec;
        spec.code = p.app.mode_code;
        spec.icon = p.icon;
        spec.names = {p.app.text, p.app.text};
        spec.entry = p.app.entry;
        modes.push_back(std::move(spec));
    }
    if (!modes.empty()) {
        cwmode::Options options;
        options.model = cwmode::Model::Cw580;
        options.at = after_code;
        options.force = force;
        cwmode::PatchResult cr = cwmode::patch(rom, modes, options);
        if (!cr.ok) {
            out.error = cr.error;
            return out;
        }
        rom = std::move(cr.rom);
        if (legacy_count > 0) {
            // cwmode took over the main hook.  Put a small legacy dispatcher in
            // front of its target so ON also relaunches the legacy app.
            const uint32_t cw_fallback = ((uint32_t)(rom[kBootDispatch + 1] & 0x0F) << 16) |
                                         rom[kBootDispatch + 2] |
                                         ((uint32_t)rom[kBootDispatch + 3] << 8);
            std::vector<uint8_t> boot;
            for (Prepared& p : prepared) {
                if (!p.app.legacy) continue;
                boot.push_back(p.app.mode_code);
                boot.push_back(0x70);  // cmp r0, #id
                boot.push_back(0x07);
                boot.push_back(0xC8);  // bne +7 words (skip flag set, BL and B)
                boot.insert(boot.end(), {0x01, 0x00, 0x11, 0x90, 0xF4, 0xE7});
                std::vector<uint8_t> bl = branch(0x01, p.app.entry);
                boot.insert(boot.end(), bl.begin(), bl.end());
                std::vector<uint8_t> fb = branch(0x00, cw_fallback);
                boot.insert(boot.end(), fb.begin(), fb.end());
            }
            std::vector<uint8_t> fb = branch(0x00, cw_fallback);
            boot.insert(boot.end(), fb.begin(), fb.end());
            uint32_t boot_addr = (cr.final_cursor + 1) & ~1u;
            if (boot_addr + boot.size() > kRomSize) {
                out.error = "the boot dispatcher does not fit after the appended modes";
                return out;
            }
            if (!blank_or_force(boot_addr, (uint32_t)boot.size(), manifest_path)) return out;
            std::memcpy(rom.data() + boot_addr, boot.data(), boot.size());
            std::vector<uint8_t> hook_boot = branch(0x00, boot_addr);
            std::memcpy(rom.data() + kBootDispatch, hook_boot.data(), hook_boot.size());
        }
    }

    // ---- report
    for (Prepared& p : prepared) {
        const std::string key = slot_key_label(p.app.slot);
        const std::string legend = slot_key_legend(p.app.slot);
        p.app.key = key;
        p.app.key_legend = legend;
        out.apps.push_back(p.app);
        out.summary.push_back(fmt(
            "  %-6s slot %-3d key %-4s %-16s base %05X entry %05X %5u bytes  %s",
            p.app.legacy ? "legacy" : "mode", p.app.slot,
            key.empty() ? "-" : key.c_str(),
            p.app.text.empty() ? "<menu slot>" : p.app.text.c_str(), p.app.base, p.app.entry,
            p.app.size, basename_of(p.app.path).c_str()));
        if (!legend.empty() && legend != key)
            out.summary.push_back("         legend: " + legend);
    }

    // ---- changed spans
    uint32_t span_start = 0;
    bool in_span = false;
    for (uint32_t i = 0; i < (uint32_t)rom.size(); i++) {
        if (rom[i] != base_rom[i]) {
            if (!in_span) {
                span_start = i;
                in_span = true;
            }
        } else if (in_span) {
            out.changed.emplace_back(span_start, i);
            in_span = false;
        }
    }
    if (in_span) out.changed.emplace_back(span_start, (uint32_t)rom.size());

    out.ok = true;
    out.rom = std::move(rom);
    return out;
}

namespace {
CwappLinkResult build_single_from_image(const BuildResult& built,
                                        const std::string& source_path,
                                        const std::vector<uint8_t>& base_rom, bool force) {
    CwappLinkResult out;
    out.error_file = source_path;
    if (!built.cwapp) {
        out.error = "not a #cwapp source: add '#for 580' and '#cwapp'";
        return out;
    }
    if (!built.cwapp_extra) {
        out.error = "this is a legacy #cwapp source; use patch_menu or the legacy build path";
        return out;
    }
    if (base_rom.size() != kRomSize) {
        out.error = fmt("the base ROM is %d bytes; need a 262144-byte fx-580VN X image",
                        (int)base_rom.size());
        return out;
    }
    const std::string rom_error = validate_580_rom(base_rom, true);
    if (!rom_error.empty()) {
        out.error = rom_error;
        return out;
    }
    const cwmode::Analysis base_analysis = cwmode::analyze(base_rom, cwmode::Model::Cw580);
    if (!base_analysis.ok) {
        out.error = base_analysis.error;
        return out;
    }

    cwmode::Image icon, icon_sel;
    std::string image_error;
    const std::string icon_path = resolve_relative(source_path, built.cwapp_icon);
    std::string sel_path;
    if (!cwmode::load_image(icon_path, icon, image_error)) {
        out.error = image_error;
        return out;
    }
    icon = cwmode::prepare_mode_icon(icon, cwmode::Model::Cw580);
    if (built.cwapp_icon_selected.empty()) {
        icon_sel = cwmode::invert_image(icon);
    } else {
        sel_path = resolve_relative(source_path, built.cwapp_icon_selected);
        if (!cwmode::load_image(sel_path, icon_sel, image_error)) {
            out.error = image_error;
            return out;
        }
        icon_sel = cwmode::prepare_mode_icon(icon_sel, cwmode::Model::Cw580);
    }
    if (!same_image(icon, icon_sel)) {
        out.warnings.push_back(
            "the firmware stores one 48x23 icon per mode; using the unselected image "
            "(the menu draws its own selection highlight)");
    }

    auto app_sym = built.syms.find("_APP");
    if (app_sym == built.syms.end() || built.rom.size() <= kCodeStart) {
        out.error = "the compiled app has no _APP entry";
        return out;
    }
    CwappLinkApp app;
    app.path = source_path;
    app.line = 0;
    app.legacy = false;
    app.slot = built.cwapp_slot;
    if (app.slot >= 1 && app.slot <= kLegacySlots) {
        out.error = fmt(
            "slot %d is the stock menu shortcut key %s; use a legacy '#cwapp' with "
            "'#slot %d' to replace that slot, or an extra app slot 13..24",
            app.slot, slot_key_label(app.slot).c_str(), app.slot);
        return out;
    }
    if (app.slot >= 13 && app.slot <= 24 && app.slot != 13) {
        out.error = fmt(
            "a single extra app must use slot 13 (key D); slot %d (key %s) needs the "
            "keyed slots 13..%d present in a .cwapplink project",
            app.slot, slot_key_label(app.slot).c_str(), app.slot - 1);
        return out;
    }
    app.text = built.cwapp_text;
    app.icon = icon_path;
    app.icon_selected = sel_path;
    app.base = kCodeStart;
    app.entry = (uint32_t)app_sym->second;
    app.size = (uint32_t)(built.rom.size() - kCodeStart);
    app.mode_code = 0xA0;

    std::vector<uint8_t> rom = base_rom;
    if (!force) {
        for (uint32_t i = 0; i < app.size; i++) {
            if (rom[(size_t)kCodeStart + i] != 0xFF) {
                out.error = fmt("%05X is not free flash (not 0xFF); use --force",
                                kCodeStart + i);
                return out;
            }
        }
    }
    std::memcpy(rom.data() + kCodeStart, built.rom.data() + kCodeStart, app.size);

    cwmode::ModeSpec spec;
    spec.code = app.mode_code;
    spec.icon = icon;
    spec.names = {app.text, app.text};
    spec.entry = app.entry;
    cwmode::Options options;
    options.model = cwmode::Model::Cw580;
    options.at = kCodeStart + app.size + ((app.size & 1) ? 1 : 0);
    const cwmode::Analysis written = cwmode::analyze(rom, cwmode::Model::Cw580);
    if (written.ok) options.at = std::max(options.at, written.code_end);
    options.force = force;
    cwmode::PatchResult cr = cwmode::patch(rom, {spec}, options);
    if (!cr.ok) {
        out.error = cr.error;
        return out;
    }
    rom = std::move(cr.rom);

    app.key = slot_key_label(app.slot);
    app.key_legend = slot_key_legend(app.slot);
    out.apps.push_back(app);
    out.summary.push_back(fmt(
        "  mode   slot %-3d key %-4s %-16s base %05X entry %05X %5u bytes  %s",
        app.slot, slot_key_label(app.slot).empty() ? "-" : slot_key_label(app.slot).c_str(),
        app.text.c_str(), app.base, app.entry, app.size,
        basename_of(source_path).c_str()));
    uint32_t span_start = 0;
    bool in_span = false;
    for (uint32_t i = 0; i < (uint32_t)rom.size(); i++) {
        if (rom[i] != base_rom[i]) {
            if (!in_span) {
                span_start = i;
                in_span = true;
            }
        } else if (in_span) {
            out.changed.emplace_back(span_start, i);
            in_span = false;
        }
    }
    if (in_span) out.changed.emplace_back(span_start, (uint32_t)rom.size());
    out.ok = true;
    out.rom = std::move(rom);
    return out;
}
}

CwappLinkResult build_cwapp_single(const std::string& source_path,
                                   const std::vector<uint8_t>& base_rom, bool force) {
    CwappLinkResult out;
    out.error_file = source_path;
    std::error_code ec;
    if (!fs::is_regular_file(source_path, ec)) {
        out.error = "source file not found";
        return out;
    }
    // Reuse the manifest builder with a synthetic one-entry project?  A
    // temporary file is avoidable: compile once and take the same path.
    BuildResult built;
    try {
        built = build_path(source_path, "cwapp", "").r;
    } catch (const CompileError& e) {
        out.error = e.what();
        if (ends_with(to_lower(source_path), ".cwiz")) {
            CwizPos pos = cwiz_error_pos(source_path, e.line);
            out.error_file = pos.file.empty() ? source_path : pos.file;
            out.error_line = pos.line;
        } else {
            out.error_line = e.line;
        }
        return out;
    } catch (const std::exception& e) {
        out.error = e.what();
        return out;
    }
    return build_single_from_image(built, source_path, base_rom, force);
}

CwappLinkResult build_cwapp_single_image(const BuildResult& built,
                                        const std::string& source_path,
                                        const std::vector<uint8_t>& base_rom, bool force) {
    return build_single_from_image(built, source_path, base_rom, force);
}

}  // namespace wiz
