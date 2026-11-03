// directives.cpp - shared directive metadata and argument grammars.
#include "directives.hpp"

#include <cctype>
#include <cstdlib>

#include "common.hpp"

namespace wiz {
namespace {

const std::vector<DirectiveInfo> kDirectives = {
    // preprocessor conditionals and file directives
    {"if", DirectiveArg::Expr, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#if EXPR - conditional compilation"},
    {"ifdef", DirectiveArg::Name, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#ifdef NAME - true when NAME is defined"},
    {"ifndef", DirectiveArg::Name, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#ifndef NAME - true when NAME is not defined"},
    {"elif", DirectiveArg::Expr, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#elif EXPR - else-if branch of a conditional"},
    {"elifdef", DirectiveArg::Name, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#elifdef NAME - C23 else-if defined(NAME)"},
    {"elifndef", DirectiveArg::Name, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#elifndef NAME - C23 else-if !defined(NAME)"},
    {"else", DirectiveArg::None, DirectiveContext::Preprocess, "preproc.unbalanced-conditional",
     "#else - final branch of a conditional"},
    {"endif", DirectiveArg::None, DirectiveContext::Preprocess, "preproc.unbalanced-conditional",
     "#endif - close a conditional"},
    {"define", DirectiveArg::Macro, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#define NAME value - object/function-like macro"},
    {"undef", DirectiveArg::Name, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#undef NAME - remove a macro"},
    {"pragma", DirectiveArg::Text, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#pragma once - file directive"},
    {"error", DirectiveArg::Text, DirectiveContext::Preprocess, "preproc.error",
     "#error text - stop compilation with a message"},
    {"include", DirectiveArg::Include, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#include \"file\" - inline another source file"},
    {"line", DirectiveArg::Text, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#line N [\"file\"] - adjust diagnostic line/file mapping"},
    {"embed", DirectiveArg::Path, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#embed \"file\" - expand a binary file to a byte initializer list"},
    {"for", DirectiveArg::Targets, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#for 580, 880 - select the target section"},
    {"cwapp", DirectiveArg::None, DirectiveContext::Preprocess, "preproc.directive-order",
     "#cwapp - compile as the fx-580VN X firmware app (after #for 580)"},
    {"slot", DirectiveArg::Slot, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#slot 1..12 - menu slot for a #cwapp build"},
    {"binary", DirectiveArg::Path, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#binary \"file.bin\" - full firmware image (fwnx target)"},
    {"org", DirectiveArg::Number, DirectiveContext::Preprocess, "preproc.directive-argument",
     "#org 0x25000 - firmware/ROP origin (metadata for #cwapp)"},

    // ROP backend directives
    {"rop", DirectiveArg::None, DirectiveContext::Rop, "preproc.directive-context",
     "#rop - select the ROP gadget-chain backend"},
    {"org", DirectiveArg::Number, DirectiveContext::Rop, "preproc.directive-argument",
     "#org 0xE9E0 - payload execution address"},
    {"pad", DirectiveArg::PadSize, DirectiveContext::Rop, "preproc.directive-argument",
     "#pad 256 - trailing null-buffer size"},
    {"nopad", DirectiveArg::None, DirectiveContext::Rop, "preproc.directive-context",
     "#nopad - no trailing null buffer"},
    {"launcher", DirectiveArg::Launcher, DirectiveContext::Rop, "preproc.directive-argument",
     "#launcher common|simple [...] - launcher stub"},
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' ||
                     s[b - 1] == '\n'))
        b--;
    return s.substr(a, b - a);
}

bool all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

bool all_hex(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

}  // namespace

const std::vector<DirectiveInfo>& directive_table() { return kDirectives; }

const DirectiveInfo* find_directive(const std::string& name) {
    for (const auto& d : kDirectives)
        if (name == d.name) return &d;
    return nullptr;
}

const DirectiveInfo* find_directive(const std::string& name, DirectiveContext ctx) {
    for (const auto& d : kDirectives)
        if (name == d.name && d.context == ctx) return &d;
    return nullptr;
}

bool is_preprocess_directive(const std::string& name) {
    for (const auto& d : kDirectives)
        if (name == d.name && d.context == DirectiveContext::Preprocess) return true;
    return false;
}

bool is_rop_directive(const std::string& name) {
    for (const auto& d : kDirectives)
        if (name == d.name && d.context == DirectiveContext::Rop) return true;
    return false;
}

std::vector<std::string> directive_names(DirectiveContext ctx) {
    std::vector<std::string> out;
    for (const auto& d : kDirectives)
        if (d.context == ctx && !contains(out, d.name)) out.push_back(d.name);
    return out;
}

std::string directive_suggestion(const std::string& name, DirectiveContext ctx) {
    std::string hit = closest_match(name, directive_names(ctx));
    return hit.empty() ? std::string() : "did you mean '#" + hit + "'?";
}

bool parse_directive_number(const std::string& text, unsigned long& out, std::string& err,
                            bool allow_bare_hex, bool* used_bare_hex) {
    std::string s = trim(text);
    if (used_bare_hex) *used_bare_hex = false;
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') s = trim(s.substr(1, s.size() - 2));
    if (s.empty()) {
        err = "expected a number";
        return false;
    }
    bool hex = false;
    std::string digits = s;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        hex = true;
        digits = s.substr(2);
        if (!all_hex(digits)) {
            err = "expected a decimal or 0x-prefixed number, got '" + s + "'";
            return false;
        }
    } else if (all_digits(s)) {
        hex = false;
    } else if (allow_bare_hex && all_hex(s)) {
        hex = true;
        if (used_bare_hex) *used_bare_hex = true;
    } else {
        err = "expected a decimal or 0x-prefixed number, got '" + s + "'";
        return false;
    }
    char* end = nullptr;
    out = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
    if (!end || *end != '\0') {
        err = "expected a decimal or 0x-prefixed number, got '" + s + "'";
        return false;
    }
    return true;
}

bool parse_directive_string(const std::string& text, std::string& out, std::string& err,
                            bool allow_angle) {
    std::string s = trim(text);
    if (s.empty()) {
        err = "expected a quoted string";
        return false;
    }
    if (s[0] == '<') {
        if (!allow_angle) {
            err = "angle-bracket includes are not supported; use #include \"file\"";
            return false;
        }
        size_t end = s.find('>', 1);
        if (end == std::string::npos) {
            err = "unterminated <...> path";
            return false;
        }
        out = s.substr(1, end - 1);
        if (!trim(s.substr(end + 1)).empty()) {
            err = "unexpected text after the closing '>'";
            return false;
        }
        return true;
    }
    if (s[0] != '"') {
        err = "expected a quoted string, got '" + s + "'";
        return false;
    }
    size_t end = 1;
    while (end < s.size() && s[end] != '"') end++;
    if (end >= s.size()) {
        err = "unterminated string";
        return false;
    }
    out = s.substr(1, end - 1);
    if (!trim(s.substr(end + 1)).empty()) {
        err = "unexpected text after the closing quote";
        return false;
    }
    return true;
}

bool canonical_target_name(const std::string& text, std::string& out) {
    std::string key;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '-' || c == '_') continue;
        key += (char)std::tolower((unsigned char)c);
    }
    if (key == "580" || key == "cw" || key == "580vnx" || key == "fx580vn" ||
        key == "fx580vnx") {
        out = "580";
        return true;
    }
    if (key == "880" || key == "cwii" || key == "880btg" || key == "880btg2") {
        out = "880";
        return true;
    }
    return false;
}

std::string canonical_address(unsigned long v) { return fmt("0x%lX", v); }
std::string canonical_decimal(unsigned long v) { return fmt("%lu", v); }

}  // namespace wiz
