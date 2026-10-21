// targets.hpp - hardware description of each calculator + `for <target>` sections.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"

namespace wiz {

struct Target {
    std::string name;
    std::string chip;
    std::string tmpl;      // boot template file
    int rom_limit;
    int ram_lo;
    int var_base;
    int var_limit;
    int planes;            // LCD bitplanes
    int rom_seg = 0;       // data segment (DSR) of ROM tables; 3 for 'cwapp'
    int rows = 64;         // visible LCD rows (width is always 192)
    std::string core = "nX-U8/100";
    int row_off = 0;       // VRAM row of user y=0 (cw: 1)
};

extern const Target TARGET_CWII;
extern const Target TARGET_CW;
extern const Target TARGET_CWAPP;

extern const std::string DEFAULT_TARGET;

std::optional<std::string> canonical(const std::string& name);
const Target& get_target(const std::string& t);
const Target& get_target(const Target& t);

struct SectionError : std::runtime_error {
    explicit SectionError(const std::string& m) : std::runtime_error(m) {}
};

std::vector<std::string> declared_targets(const std::string& src);
std::string select_sections(const std::string& src, const Target& target);

}  // namespace wiz
