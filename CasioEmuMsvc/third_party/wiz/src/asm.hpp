// asm.hpp - assembler for the nX-U8/100 and nX-U16/100 cores (ML620909 / ML620906).
//
// Opcode table from the OKI nX-U8/100 Core Instruction Manual ch.4, cross-checked
// against the LAPIS nX-U16/100 manual. Both cores share one encoder.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common.hpp"

namespace wiz {

std::string resolve_core(const std::string& name);

class Assembler {
public:
    explicit Assembler(const std::string& core = "nX-U8/100");

    std::map<uint32_t, uint8_t> assemble(const std::string& text, int base = 0);

    std::string core;
    std::map<std::string, int> syms;

private:
    int ev(const std::string& s, bool final) const;
    std::vector<uint16_t> encode(const std::string& mn, const std::vector<std::string>& ops,
                                 int pc, bool final) const;
};

std::vector<uint8_t> to_bin(const std::map<uint32_t, uint8_t>& image, int size = 0);

}  // namespace wiz
