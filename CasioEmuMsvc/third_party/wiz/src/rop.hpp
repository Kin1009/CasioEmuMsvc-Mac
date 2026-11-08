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
    std::string name;   // Call: gadget name (for the RAC mnemonic)
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
    bool launcher_simple = false;   // #launcher simple / explicit stub address
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
// real RAC syntax where possible (`@section.main`, `lbl`, `call`, `hex`,
// `"..."`, `eval(adr(...))`) and always derives the launcher from the RAC
// `@section.launcher` shape.  On failure `error` is set and the result is empty.
std::string rop_to_rsc(const RopResult& rop, std::string& error);

// Generate the RAC source for a #rop source without running RAC (used by
// --emit-rsc).  Handles `#raw-rsc` and the high-level emitter.
std::string rop_source_to_rsc(const std::string& source, const std::string& model,
                              const std::string& language, std::string& error);

// Extract a verbatim `#raw-rsc { ... }` block from a #rop source.  The returned
// text is used as the complete RAC source, unchanged.  Returns false when the
// source has no such block; on a malformed block `error` is set.
bool extract_raw_rsc(const std::string& source, std::string& out, std::string& error);

// RAC-Compiler location: $RAC_COMPILER, else tools/rac-compiler/rac.py.
std::string default_rac_path();
bool rac_available(const std::string& path);

// Result of the RAC compiler (the only production ROP backend).  The direct
// `RopResult` is kept for diagnostics and for the RAC-source metadata; the
// payload and launcher in this structure are the bytes RAC produced.
struct RacRopResult {
    bool ok = false;
    bool ran = false;              // the RAC compiler was invoked successfully
    std::string error;
    std::string log;
    RopResult direct;              // parsed metadata (not an output path)
    std::vector<uint8_t> payload;
    uint32_t payload_base = 0;
    std::vector<uint8_t> launcher;
    uint32_t launcher_base = 0;
    std::string rsc_path;
    std::string rsc_text;
    std::string rac_path;
};

// Build a ROP source through the external RAC compiler.  `rsc_path` is where
// the emitted .rsc is written (no temporary directory is used).  RAC is
// mandatory: when it is missing or fails, `ok` is false and no image exists.
RacRopResult build_rop_rac(const std::string& source, const std::string& model,
                           const std::string& language, const std::string& rsc_path,
                           const std::string& rac_path = std::string());

} // namespace wiz
