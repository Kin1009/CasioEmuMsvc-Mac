#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wiz {

// One annotated item of a compiled payload, used to re-emit it as RAC source.
// `offset` is relative to the payload's injection address.
struct RopRscItem {
    enum class Kind { Label, Word, Call, Literal };
    Kind kind = Kind::Label;
    size_t offset = 0;
    std::string text;   // label name, RAC expression, call address, or source text
    size_t length = 0;  // Literal: encoded byte length (0 = NUL terminated text)
};

// Result of compiling a CWiz-syntax ROP payload.
struct RopResult {
    bool ok = false;
    std::string error;   // non-empty when ok == false
    std::string log;     // emission log / diagnostics
    std::vector<uint8_t> bytes;
    uint32_t base = 0;   // #org / injection (backup) address
    // The address the chain executes at.  It differs from `base` for the 580
    // common launcher, which copies the payload from E9E0 to D730 first.
    uint32_t exec = 0;
    // A ROP program is entered indirectly: a short launcher at its own address
    // sets SP to the payload and lets the gadget chain continue there.
    std::vector<uint8_t> launcher;
    uint32_t launcher_base = 0x8154;
    std::string launcher_hex;   // space-separated bytes, for pasting into a program
    std::string model;   // "580" or "880"
    // Labels, label-relative address words and string literals, for --emit-rsc.
    std::vector<RopRscItem> rsc;
};

// Compile CWiz-syntax ROP source. `model` is "580" or "880".
RopResult build_rop(const std::string& source, const std::string& model,
                    const std::string& language = "cwiz");

// True when the source asks for the ROP backend (contains a `#rop` line).
bool source_is_rop(const std::string& source);

// Assembly of the direct key-matrix scanner used by ROP `key()`.  It is the
// same loop as the cwapp runtime `__key`, with no OS state.
std::string rop_key_routine_asm();

// Re-emit a compiled payload as RAC-Compiler source (.rsc).  The output uses
// real RAC syntax where it is byte-safe (`@section.main`, `lbl`, `hex`,
// `"..."`, `eval(adr(...))`) and hex blocks everywhere else.  `rac_launcher`
// asks for the launcher section to be emitted in RAC source form (the RAC
// launcher) instead of the compiler's default launcher bytes; it is only
// available on the 580.  On failure `error` is set and the result is empty.
std::string rop_to_rsc(const RopResult& rop, bool rac_launcher, std::string& error);

// RAC-Compiler location: $RAC_COMPILER, else tools/rac-compiler/rac.py.
std::string default_rac_path();
bool rac_available(const std::string& path);

// Result of the optional external RAC backend.
struct RacRopResult {
    bool ok = false;
    bool ran = false;              // the RAC compiler was invoked successfully
    bool launcher_from_rac = false;
    std::string error;
    std::string log;
    RopResult direct;              // the direct build (also the fallback path)
    std::vector<uint8_t> payload;
    uint32_t payload_base = 0;
    std::vector<uint8_t> launcher;
    uint32_t launcher_base = 0;
    std::string rsc_path;
    std::string rac_path;
    size_t payload_diff = 0;       // bytes that differ from the direct payload
};

// Build a ROP source through the external RAC compiler.  `rsc_path` is where
// the emitted .rsc is written (no temporary directory is used).  The direct
// compiler remains the fallback: when RAC is missing or fails, `ok` is false
// but `direct` holds a complete direct build.
RacRopResult build_rop_rac(const std::string& source, const std::string& model,
                           const std::string& language, const std::string& rsc_path,
                           const std::string& rac_path = std::string(),
                           bool rac_launcher = false);

} // namespace wiz
