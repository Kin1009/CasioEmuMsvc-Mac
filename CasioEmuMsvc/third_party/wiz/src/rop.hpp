#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wiz {

// Result of compiling a CWiz-syntax ROP payload.
struct RopResult {
    bool ok = false;
    std::string error;   // non-empty when ok == false
    std::string log;     // emission log / diagnostics
    std::vector<uint8_t> bytes;
    uint32_t base = 0;   // #org / opn address (home)
    std::string model;   // "580" or "880"
};

// Compile CWiz-syntax ROP source. `model` is "580" or "880".
RopResult build_rop(const std::string& source, const std::string& model);

// True when the source asks for the ROP backend (contains a `#rop` line).
bool source_is_rop(const std::string& source);

} // namespace wiz
