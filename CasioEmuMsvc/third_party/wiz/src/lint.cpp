// lint.cpp - static checks for Wiz (.wz) and CWiz (.cwiz) sources (see lint.hpp).
#include "lint.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "build.hpp"
#include "common.hpp"
#include "rop.hpp"
#include "targets.hpp"

namespace wiz {
namespace {

struct Line {
    std::string text;
    std::string code;  // text with comments and string/char literals blanked
    int indent = 0;
    bool blank = true;
};

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::vector<Line> split_lines(const std::string& source) {
    std::vector<Line> lines;
    std::string current;
    auto push = [&] {
        Line line;
        line.text = current;
        line.indent = 0;
        while (line.indent < (int)line.text.size() && is_space(line.text[(size_t)line.indent]))
            line.indent++;
        line.blank = line.text.find_first_not_of(" \t\r") == std::string::npos;
        // Blank comments and literals so they cannot create false references.
        std::string code = line.text;
        bool in_string = false, in_char = false, in_line_comment = false, in_block = false;
        for (size_t i = 0; i < code.size(); ++i) {
            char c = code[i];
            char next = i + 1 < code.size() ? code[i + 1] : '\0';
            if (in_line_comment) {
                code[i] = ' ';
                continue;
            }
            if (in_block) {
                if (c == '*' && next == '/') {
                    code[i] = code[i + 1] = ' ';
                    ++i;
                    in_block = false;
                } else {
                    code[i] = ' ';
                }
                continue;
            }
            if (in_string) {
                code[i] = ' ';
                if (c == '\\' && i + 1 < code.size()) code[++i] = ' ';
                else if (c == '"') in_string = false;
                continue;
            }
            if (in_char) {
                code[i] = ' ';
                if (c == '\\' && i + 1 < code.size()) code[++i] = ' ';
                else if (c == '\'') in_char = false;
                continue;
            }
            if (c == '/' && next == '/') {
                code[i] = code[i + 1] = ' ';
                ++i;
                in_line_comment = true;
            } else if (c == '/' && next == '*') {
                code[i] = code[i + 1] = ' ';
                ++i;
                in_block = true;
            } else if (c == '"') {
                code[i] = ' ';
                in_string = true;
            } else if (c == '\'') {
                code[i] = ' ';
                in_char = true;
            }
        }
        line.code = code;
        lines.push_back(line);
        current.clear();
    };
    for (char c : source) {
        if (c == '\n') push();
        else if (c != '\r') current.push_back(c);
    }
    if (!current.empty() || source.empty() || source.back() != '\n') push();
    return lines;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Count identifier occurrences in code (word boundaries).
int count_word(const std::vector<Line>& lines, const std::string& name) {
    const std::regex re("(^|[^A-Za-z0-9_])" + name + "([^A-Za-z0-9_]|$)");
    int n = 0;
    for (const Line& line : lines) {
        for (std::sregex_iterator it(line.code.begin(), line.code.end(), re), end; it != end; ++it)
            ++n;
    }
    return n;
}

bool is_type_word(const std::string& w) {
    static const std::set<std::string> types = {
        "int", "char", "bool", "byte", "rom8", "rom16", "int8", "int16", "arr", "void"};
    return types.count(w) != 0;
}

// Parse a typed declaration (CWiz) or an assignment (Wiz).  Returns the declared
// name and whether it is a function.
bool declaration_name(const Line& line, bool cwiz, std::string& name) {
    std::string s = trim(line.code);
    if (s.empty() || starts_with(s, "#") || starts_with(s, "for ") || starts_with(s, "for(") ||
        starts_with(s, "if ") || starts_with(s, "while ") || starts_with(s, "return") ||
        starts_with(s, "switch") || starts_with(s, "case") || starts_with(s, "else"))
        return false;

    if (cwiz) {
        std::regex re("^([A-Za-z_][A-Za-z0-9_]*)[ \\t]*\\*?[ \\t]+([A-Za-z_][A-Za-z0-9_]*)");
        std::smatch m;
        if (!std::regex_search(s, m, re))
            return false;
        if (!is_type_word(m[1].str()))
            return false;
        // A function definition: the name is followed by '('.
        size_t pos = (size_t)m.position(2) + (size_t)m.length(2);
        while (pos < s.size() && is_space(s[pos])) ++pos;
        if (pos < s.size() && s[pos] == '(')
            return false;
        name = m[2].str();
        return true;
    }

    // Wiz: NAME = value / NAME[..] = value; ignore comparisons.
    std::regex re("^([A-Za-z_][A-Za-z0-9_]*)[ \\t]*(\\[[^\\]]*\\])?[ \\t]*=(?!=)");
    std::smatch m;
    if (!std::regex_search(s, m, re))
        return false;
    name = m[1].str();
    if (name == "if" || name == "while" || name == "return" || name == "def")
        return false;
    return true;
}

void add(std::vector<LintDiagnostic>& out, LintSeverity sev, int line, int col,
         const std::string& rule, const std::string& message) {
    out.push_back({sev, line, col, rule, message});
}

LintSeverity severity_of(const std::string& rule) {
    if (rule == "readability/unused-variable" || rule == "correctness/shadowed-variable" ||
        rule == "correctness/unreachable-code" || rule == "style/line-length")
        return LintSeverity::Warning;
    return LintSeverity::Warning;
}

}  // namespace

const std::vector<LintRuleInfo>& lint_rules() {
    static const std::vector<LintRuleInfo> rules = {
        {"compile", "the compiler's own syntax and semantic diagnostics"},
        {"preproc/cwapp-order", "#cwapp must be preceded by #for 580"},
        {"preproc/slot-order", "#slot is only valid after #cwapp"},
        {"preproc/slot-range", "#slot must be a number from 1 to 12"},
        {"preproc/duplicate-slot", "a #cwapp section has more than one #slot"},
        {"preproc/duplicate-target", "a #for target is listed more than once"},
        {"preproc/unknown-target", "#for names a target the toolchain does not know"},
        {"readability/unused-variable", "a declared variable is never read"},
        {"correctness/shadowed-variable", "a local hides a global with the same name"},
        {"correctness/unreachable-code", "a statement follows return/break/continue"},
        {"style/line-length", "line is longer than 100 columns"},
        {"style/trailing-whitespace", "line ends with spaces or tabs"},
        {"style/tabs", "indentation uses a tab"},
        {"style/todo", "TODO/FIXME/XXX marker"},
    };
    return rules;
}

std::vector<LintDiagnostic> lint_source(const std::string& source,
                                        const std::string& file_name,
                                        const std::string& target) {
    std::vector<LintDiagnostic> out;
    const bool cwiz = ends_with(file_name, ".cwiz");
    const std::vector<Line> lines = split_lines(source);
    std::set<int> compile_error_lines;

    // ---------------------------------------------------------------- compiler
    try {
        if (source_is_rop(source)) {
            const std::string rop_model = target == "880" || target == "cwii" ? "880" : "580";
            const RopResult result = build_rop(source, rop_model);
            if (!result.ok)
                add(out, LintSeverity::Error, 0, 0, "compile", result.error);
        } else if (target == "cwapp" || target == "580" || target == "880" || target == "cw" ||
                   target == "cwii") {
            if (cwiz)
                build_cwiz(source, target, file_name);
            else
                build_source(source, target);
        }
    } catch (const CompileError& e) {
        add(out, LintSeverity::Error, e.line, e.col, "compile", e.what());
        if (e.line > 0) compile_error_lines.insert(e.line);
    } catch (const std::exception& e) {
        add(out, LintSeverity::Error, 0, 0, "compile", e.what());
    }

    // ---------------------------------------------------------------- preprocessor
    std::string last_for;
    std::set<std::string> seen_for;
    bool in_cwapp = false;
    bool slot_seen = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i].text);
        if (starts_with(t, "#for ")) {
            std::string rest = trim(t.substr(5));
            std::string first = rest.substr(0, rest.find_first_of(", \t"));
            if (!first.empty()) {
                if (seen_for.count(first))
                    add(out, severity_of("preproc/duplicate-target"), (int)i + 1, 1,
                        "preproc/duplicate-target",
                        "#for " + first + " is already listed earlier in this file");
                seen_for.insert(first);
                last_for = first;
                in_cwapp = false;
                slot_seen = false;
                if (first != "580" && first != "880")
                    add(out, severity_of("preproc/unknown-target"), (int)i + 1, 1,
                        "preproc/unknown-target",
                        "unknown target '#for " + first + "' (use 580 or 880)");
            }
        }
        if (starts_with(t, "#cwapp") && last_for != "580" &&
            !compile_error_lines.count((int)i + 1))
            add(out, severity_of("preproc/cwapp-order"), (int)i + 1, 1,
                "preproc/cwapp-order",
                "#cwapp needs #for 580 immediately before it (the current section is " +
                    (last_for.empty() ? std::string("global") : "#for " + last_for) + ")");
        if (starts_with(t, "#cwapp") && last_for == "580") {
            in_cwapp = true;
            slot_seen = false;
        }
        const bool slot_line = t == "#slot" || starts_with(t, "#slot ") || starts_with(t, "#slot\t");
        if (slot_line) {
            if (!in_cwapp) {
                add(out, severity_of("preproc/slot-order"), (int)i + 1, 1, "preproc/slot-order",
                    "#slot is only available after #cwapp (inside a #for 580 section)");
            } else if (slot_seen) {
                add(out, severity_of("preproc/duplicate-slot"), (int)i + 1, 1,
                    "preproc/duplicate-slot", "this #cwapp section already has a #slot");
            } else {
                slot_seen = true;
                std::string rest = trim(t.size() > 5 ? t.substr(5) : "");
                char* end = nullptr;
                long n = rest.empty() ? 0 : std::strtol(rest.c_str(), &end, 10);
                if (rest.empty() || !end || *end != '\0' || n < 1 || n > 12)
                    add(out, severity_of("preproc/slot-range"), (int)i + 1, 1,
                        "preproc/slot-range", "#slot needs a slot number from 1 to 12");
            }
        }
    }

    // ---------------------------------------------------------------- declarations
    struct Decl {
        std::string name;
        int line;
        int indent;
        bool global;
    };
    std::vector<Decl> decls;
    std::map<std::string, int> global_line;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].blank) continue;
        std::string name;
        if (!declaration_name(lines[i], cwiz, name)) continue;
        const bool global = lines[i].indent == 0;
        decls.push_back({name, (int)i + 1, lines[i].indent, global});
        if (global && !global_line.count(name)) global_line[name] = (int)i + 1;
    }

    for (const Decl& d : decls) {
        if (d.name.empty() || d.name[0] == '_') continue;
        if (!d.global && global_line.count(d.name)) {
            add(out, severity_of("correctness/shadowed-variable"), d.line,
                lines[(size_t)d.line - 1].indent + 1, "correctness/shadowed-variable",
                "local '" + d.name + "' hides the global declared on line " +
                    std::to_string(global_line[d.name]));
        }
        if (count_word(lines, d.name) <= 1) {
            add(out, severity_of("readability/unused-variable"), d.line,
                lines[(size_t)d.line - 1].indent + 1, "readability/unused-variable",
                "'" + d.name + "' is declared but never read");
        }
    }

    // ---------------------------------------------------------------- line rules
    bool reported_tabs = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& text = lines[i].text;
        if (text.size() > 100)
            add(out, severity_of("style/line-length"), (int)i + 1, 101, "style/line-length",
                fmt("line is %zu columns (limit 100)", text.size()));
        if (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            add(out, severity_of("style/trailing-whitespace"), (int)i + 1, (int)text.size(),
                "style/trailing-whitespace", "trailing whitespace");
        if (!reported_tabs && text.find('\t') != std::string::npos) {
            add(out, severity_of("style/tabs"), (int)i + 1, (int)text.find('\t') + 1, "style/tabs",
                "tab character in the source (the examples use spaces)");
            reported_tabs = true;
        }
        const std::string upper = [&] {
            std::string u = text;
            for (char& c : u) c = (char)std::toupper((unsigned char)c);
            return u;
        }();
        if (upper.find("TODO") != std::string::npos || upper.find("FIXME") != std::string::npos ||
            upper.find("XXX") != std::string::npos)
            add(out, LintSeverity::Note, (int)i + 1, 1, "style/todo", "unfinished marker in a comment");
    }

    // ---------------------------------------------------------------- unreachable
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string t = trim(lines[i].code);
        if (t.empty()) continue;
        const bool jump = starts_with(t, "return") || starts_with(t, "break") ||
                          starts_with(t, "continue") || starts_with(t, "goto ");
        if (!jump) continue;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            const std::string n = trim(lines[j].code);
            if (n.empty()) continue;
            if (n == "}" || starts_with(n, "case ") || starts_with(n, "default:") ||
                starts_with(n, "#") || starts_with(n, "else") || starts_with(n, "elif"))
                break;
            if (lines[j].indent >= lines[i].indent)
                add(out, severity_of("correctness/unreachable-code"), (int)j + 1,
                    lines[j].indent + 1, "correctness/unreachable-code",
                    "unreachable: the previous statement transfers control");
            break;
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const LintDiagnostic& a, const LintDiagnostic& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.col < b.col;
    });
    return out;
}

}  // namespace wiz
