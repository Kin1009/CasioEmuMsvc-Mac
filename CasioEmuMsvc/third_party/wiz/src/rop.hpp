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
    // A ROP program is entered indirectly: a short launcher at its own address
    // sets SP to the payload and lets the gadget chain continue there.
    std::vector<uint8_t> launcher;
    uint32_t launcher_base = 0x8154;
    std::string launcher_hex;   // space-separated bytes, for pasting into a program
    std::string model;   // "580" or "880"
};

// Compile CWiz-syntax ROP source. `model` is "580" or "880".
RopResult build_rop(const std::string& source, const std::string& model);

// Compatibility entry point used by the debugger.  The vendored compiler
// predates the Wiz-to-CWiz ROP conversion, so the language hint is accepted but
// the source must already use CWiz syntax.
RopResult build_rop(const std::string& source, const std::string& model,
                    const std::string& language);

// True when the source asks for the ROP backend (contains a `#rop` line).
bool source_is_rop(const std::string& source);

} // namespace wiz
