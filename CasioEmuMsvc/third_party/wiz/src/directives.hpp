// directives.hpp - one grammar for every #directive.
//
// The preprocessor (cpre.cpp), the ROP backend (rop.cpp) and the linter
// (lint.cpp) share this table and the two argument parsers.  Keeping the
// metadata in one place is what makes the front ends agree on:
//   * which directives exist and where they are allowed,
//   * the number grammar (decimal or 0x, never bare hex except as a
//     documented compatibility alias),
//   * the string grammar ("..." only), and
//   * the diagnostic code shape.
#pragma once

#include <string>
#include <vector>

namespace wiz {

enum class DirectiveArg {
    None,      // #cwapp, #rop, #nopad, #else, #endif
    Name,      // #ifdef NAME, #undef NAME
    Expr,      // #if EXPR, #elif EXPR
    Include,   // #include "file"
    Targets,   // #for 580, 880
    Slot,      // #slot 1..12
    Path,      // #binary "file"
    Number,    // #org 0xE9E0
    PadSize,   // #pad 256
    Launcher,  // #launcher common [short|medium|long] [backup] [main]
    Text,      // #error text
    Macro,     // #define / #pragma
};

enum class DirectiveContext { Preprocess, Rop };

struct DirectiveInfo {
    const char* name;
    DirectiveArg arg;
    DirectiveContext context;
    const char* code;  // diagnostic code used for argument/ordering failures
    const char* doc;   // one-line reference text
};

const std::vector<DirectiveInfo>& directive_table();
const DirectiveInfo* find_directive(const std::string& name);
const DirectiveInfo* find_directive(const std::string& name, DirectiveContext ctx);

bool is_preprocess_directive(const std::string& name);
bool is_rop_directive(const std::string& name);

// Names valid in a context, for suggestions and the linter.
std::vector<std::string> directive_names(DirectiveContext ctx);
std::string directive_suggestion(const std::string& name, DirectiveContext ctx);

// Decimal (including 0x...) only.  `allow_bare_hex` accepts the legacy
// E9E0 spelling and reports it through `canonical` (the caller may warn).
bool parse_directive_number(const std::string& text, unsigned long& out, std::string& err,
                            bool allow_bare_hex = false, bool* used_bare_hex = nullptr);

// One quoted-string grammar.  Angle brackets are rejected with a hint.
bool parse_directive_string(const std::string& text, std::string& out, std::string& err,
                            bool allow_angle = false);

// 580 / 880 canonical target names.  Aliases are accepted for compatibility.
bool canonical_target_name(const std::string& text, std::string& out);

std::string canonical_address(unsigned long v);  // "0xE9E0"
std::string canonical_decimal(unsigned long v);   // "256"

}  // namespace wiz
