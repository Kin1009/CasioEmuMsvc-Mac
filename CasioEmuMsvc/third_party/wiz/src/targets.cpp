// targets.cpp - target table + `for <target>` source sections.
#include "targets.hpp"

#include <cstdlib>
#include <unordered_map>

namespace wiz {

const Target TARGET_CWII{"cwii", "ML620909", "template_ML620909.asm", 0x9000, 0x9000,
                         0x9000, 0xE000, 2};
const Target TARGET_CW{"cw", "ML620906", "template_ML620906.asm", 0xD000, 0xD000, 0xD000,
                       0xE000, 1, 0, 63, "nX-U16/100", 1};
const Target TARGET_CWAPP{"cwapp", "ML620906", "template_cwapp.asm", 0xD000, 0xD000, 0xD000,
                          0xE000, 1, 3, 63, "nX-U16/100", 1};

const std::string DEFAULT_TARGET = [] {
    const char* e = std::getenv("WIZ_TARGET");
    return std::string(e && *e ? e : "cwii");
}();

static const char* const MODELS = "580, 880";

std::optional<std::string> canonical(const std::string& name) {
    std::string key;
    for (char c : name) {
        if (c == ' ' || c == '-' || c == '_') continue;
        key += (char)std::tolower((unsigned char)c);
    }
    static const std::unordered_map<std::string, std::string> ALIASES = {
        {"580", "cw"}, {"cw", "cw"}, {"880", "cwii"}, {"cwii", "cwii"}, {"cwapp", "cwapp"}};
    auto it = ALIASES.find(key);
    if (it == ALIASES.end()) return std::nullopt;
    return it->second;
}

const Target& get_target(const std::string& t) {
    auto name = canonical(t);
    if (!name) throw std::runtime_error(fmt("unknown target '%s' (use %s)", t.c_str(), MODELS));
    if (*name == "cw") return TARGET_CW;
    if (*name == "cwii") return TARGET_CWII;
    return TARGET_CWAPP;
}

const Target& get_target(const Target& t) { return t; }

// ------------------------------------------------------------------ scanning
static bool next_cp(const std::string& s, size_t& i, uint32_t& cp) {
    if (i >= s.size()) return false;
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { cp = c; i += 1; return true; }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        cp = ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu); i += 2; return true;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) |
             ((unsigned char)s[i + 2] & 0x3Fu); i += 3; return true;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        cp = ((c & 0x07u) << 18) | (((unsigned char)s[i + 1] & 0x3Fu) << 12) |
             (((unsigned char)s[i + 2] & 0x3Fu) << 6) | ((unsigned char)s[i + 3] & 0x3Fu);
        i += 4; return true;
    }
    cp = c; i += 1; return true;
}

static bool is_sep_char(uint32_t cp) {
    return cp == '-' || cp == '=' || cp == '_' || cp == 0x2014 || cp == 0x2013 ||
           cp == 0x2500 || cp == 0x2501 || cp == 0x2550;
}

// Parse the comma-separated name list of a header, starting at `i`.
static bool parse_header_names(const std::string& raw, size_t i, std::vector<std::string>& names) {
    auto skip_ws = [&] { while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) i++; };
    auto is_ident = [](char c) {
        return std::isalnum((unsigned char)c) || c == '_';
    };
    skip_ws();
    while (true) {
        if (i >= raw.size() || !is_ident(raw[i])) return false;
        size_t s = i;
        while (i < raw.size() && is_ident(raw[i])) i++;
        names.push_back(raw.substr(s, i - s));
        skip_ws();
        if (i < raw.size() && raw[i] == ',') {
            i++;
            skip_ws();
            continue;
        }
        break;
    }
    if (i < raw.size() && raw[i] == '#') i = raw.size();
    return i == raw.size();
}

// Parse `for A, B`, the CWiz target directive `#for A, B` (A, B are calculators:
// 580 / 880), or the CWiz `#cwapp` marker, which marks the section that follows
// as the fx-580VN X firmware app rather than a ROM file.
// Returns false if the line is not a header.
static bool parse_header(const std::string& raw, std::vector<std::string>& names) {
    size_t base = (raw.size() > 1 && raw[0] == '#') ? 1 : 0;
    if (raw.compare(base, 3, "for") == 0 && raw.size() > base + 3 &&
        (raw[base + 3] == ' ' || raw[base + 3] == '\t')) {
        return parse_header_names(raw, base + 4, names);
    }
    if (base == 0) return false;
    // `#cwapp`: the app flavour of the 580, optionally with a trailing comment.
    std::string rest = rstrip(raw.substr(1));
    size_t hash = rest.find('#');
    if (hash != std::string::npos) rest = rstrip(rest.substr(0, hash));
    if (rest != "cwapp") return false;
    names.push_back(rest);
    return true;
}

static bool is_separator(const std::string& raw) {
    size_t i = 0;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) i++;
    size_t count = 0;
    while (i < raw.size()) {
        size_t save = i;
        uint32_t cp = 0;
        if (!next_cp(raw, i, cp)) break;
        if (!is_sep_char(cp)) { i = save; break; }
        count++;
    }
    if (count < 3) return false;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) i++;
    if (i < raw.size() && raw[i] == '#') i = raw.size();
    return i == raw.size();
}

struct ScanLine {
    int line;
    std::string raw;
    char kind;  // 'h' header, 's' sep, 'c' code
    std::vector<std::string> names;
};

static std::vector<ScanLine> scan(const std::string& src) {
    std::vector<ScanLine> out;
    std::string s = replace_all(src, "\r", "");
    auto lines = split(s, '\n');
    for (size_t k = 0; k < lines.size(); k++) {
        const std::string& raw = lines[k];
        ScanLine sl;
        sl.line = (int)k + 1;
        sl.raw = raw;
        if (parse_header(raw, sl.names)) {
            sl.kind = 'h';
        } else if (is_separator(raw)) {
            sl.kind = 's';
        } else {
            sl.kind = 'c';
        }
        out.push_back(std::move(sl));
    }
    return out;
}

std::vector<std::string> declared_targets(const std::string& src) {
    std::vector<std::string> out;
    for (const auto& sl : scan(src)) {
        if (sl.kind != 'h') continue;
        for (const auto& n : sl.names) {
            if (n == "all") continue;
            auto c = canonical(n);
            if (!c) throw SectionError(fmt("line %d: unknown target '%s' (use %s, all)",
                                           sl.line, n.c_str(), MODELS));
            if (!contains(out, *c)) out.push_back(*c);
        }
    }
    return out;
}

std::string select_sections(const std::string& src, const Target& target) {
    auto slines = scan(src);
    std::vector<std::string> declared;
    for (const auto& sl : slines) {
        if (sl.kind != 'h') continue;
        for (const auto& n : sl.names) {
            if (n == "all") continue;
            auto c = canonical(n);
            if (c && !contains(declared, *c)) declared.push_back(*c);
        }
    }
    bool has_all = false;
    for (const auto& sl : slines)
        if (sl.kind == 'h' && contains(sl.names, "all")) has_all = true;

    if (!declared.empty() && !contains(declared, target.name) && !has_all)
        throw SectionError(fmt("this file has no 'for %s' section (it has: %s)",
                               target.name.c_str(), join(declared, ", ").c_str()));

    std::vector<std::string> out;
    bool active = true;
    for (const auto& sl : slines) {
        if (sl.kind == 'h') {
            active = contains(sl.names, "all");
            if (!active)
                for (const auto& n : sl.names) {
                    auto c = canonical(n);
                    if (c && *c == target.name) { active = true; break; }
                }
            out.push_back("");
        } else if (sl.kind == 's') {
            active = true;
            out.push_back("");
        } else {
            out.push_back(active ? sl.raw : "");
        }
    }
    return join(out, "\n");
}

}  // namespace wiz
