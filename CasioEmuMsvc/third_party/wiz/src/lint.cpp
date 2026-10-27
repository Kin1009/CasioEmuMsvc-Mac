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
    std::string code;     // text with comments and literals blanked
    std::string comment;  // the comment characters only (for TODO detection)
    int indent = 0;
    bool blank = true;
    bool directive = false;  // first token is a preprocessor directive
};

bool ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && is_space(s[a])) ++a;
    while (b > a && is_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Split `text` on `sep`, ignoring separators nested in (), [] or {}.
std::vector<std::string> split_top_level(const std::string& text, char sep) {
    std::vector<std::string> out;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '(' || c == '[' || c == '{') ++depth;
        else if (c == ')' || c == ']' || c == '}') depth = std::max(0, depth - 1);
        else if (c == sep && depth == 0) {
            out.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    out.push_back(text.substr(start));
    return out;
}

// ---------------------------------------------------------------- line scanner
std::vector<Line> split_lines(const std::string& source, bool cwiz) {
    std::vector<Line> lines;
    std::string current;
    bool in_block = false;

    auto push = [&] {
        Line line;
        line.text = current;
        line.indent = 0;
        while (line.indent < (int)line.text.size() && is_space(line.text[(size_t)line.indent]))
            line.indent++;
        line.blank = line.text.find_first_not_of(" \t\r") == std::string::npos;
        line.code.assign(line.text.size(), ' ');
        line.comment.assign(line.text.size(), ' ');

        const std::string& text = line.text;
        bool in_string = false, in_char = false, in_line_comment = false;
        bool was_block = in_block;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            char next = i + 1 < text.size() ? text[i + 1] : '\0';
            if (in_line_comment) {
                line.comment[i] = c;
                continue;
            }
            if (in_block) {
                line.comment[i] = c;
                if (c == '*' && next == '/') {
                    line.comment[i + 1] = '/';
                    ++i;
                    in_block = false;
                }
                continue;
            }
            if (in_string) {
                if (c == '\\' && i + 1 < text.size()) ++i;
                else if (c == '"') in_string = false;
                continue;
            }
            if (in_char) {
                if (c == '\\' && i + 1 < text.size()) ++i;
                else if (c == '\'') in_char = false;
                continue;
            }
            if (cwiz && c == '/' && next == '/') {
                line.comment[i] = c;
                line.comment[i + 1] = next;
                line.code[i] = line.code[i + 1] = ' ';
                ++i;
                in_line_comment = true;
                continue;
            }
            if (cwiz && c == '/' && next == '*') {
                line.comment[i] = c;
                line.comment[i + 1] = next;
                line.code[i] = line.code[i + 1] = ' ';
                ++i;
                in_block = true;
                continue;
            }
            if (c == '"') {
                in_string = true;
                continue;
            }
            if (cwiz && c == '\'') {
                in_char = true;
                continue;
            }
            // Wiz has no preprocessor: '#' always starts a comment.  CWiz
            // directives are handled below and are not comments.
            if (!cwiz && c == '#') {
                line.comment[i] = c;
                in_line_comment = true;
                continue;
            }
            line.code[i] = c;
        }

        // A CWiz line whose first non-space token is '#' is a directive.  Blank
        // it for declaration/identifier analysis so macro names do not look
        // like variables; the text is still available to the preproc rules.
        size_t first = line.text.find_first_not_of(" \t\r");
        line.directive = !was_block && first != std::string::npos && line.text[first] == '#' &&
                         (cwiz || first == 0);
        if (cwiz && line.directive) line.code.assign(line.code.size(), ' ');

        lines.push_back(std::move(line));
        current.clear();
    };

    for (char c : source) {
        if (c == '\n') push();
        else if (c != '\r') current.push_back(c);
    }
    if (!current.empty() || source.empty() || source.back() != '\n') push();
    return lines;
}

// Count whole-word occurrences in code (comments/literals already blanked).
int count_word(const std::vector<Line>& lines, const std::string& name) {
    int n = 0;
    for (const Line& line : lines) {
        size_t i = 0;
        while (i < line.code.size()) {
            if (!ident_start(line.code[i])) { ++i; continue; }
            size_t s = i;
            while (i < line.code.size() && ident_char(line.code[i])) ++i;
            if (line.code.compare(s, i - s, name) == 0) ++n;
        }
    }
    return n;
}

// ---------------------------------------------------------------- declarations
const std::set<std::string>& base_types() {
    static const std::set<std::string> types = {"int", "char", "void", "rom", "rom8"};
    return types;
}
const std::set<std::string>& qualifiers() {
    static const std::set<std::string> q = {"const", "static", "volatile", "register",
                                            "extern", "inline", "unsigned", "signed",
                                            "short", "long"};
    return q;
}
const std::set<std::string>& statement_keywords() {
    static const std::set<std::string> k = {
        "if", "else", "elif", "while", "do", "for", "switch", "case", "default",
        "return", "break", "continue", "goto", "def", "fn", "class", "struct",
        "enum", "union", "typedef", "sizeof"};
    return k;
}
const std::set<std::string>& builtin_names() {
    static const std::set<std::string> b = {
        "print", "print_cw", "text", "text_cw", "num", "num_cw", "char", "glyph",
        "char_cw", "glyph_cw", "pixel", "clear", "clearRender", "render",
        "renderStatus", "key", "key_down", "key_hit", "wait_key", "delay", "peek",
        "poke", "sprite", "hline", "vline", "rect", "fillrect", "line", "circle",
        "disc", "abs", "min", "max", "clamp", "sign", "srand", "rand", "exit",
        "flip", "buffered", "wipe", "_hspan", "_rpeek"};
    return b;
}

struct NewDecl {
    std::string name;
    int col = 0;         // 0-based
    int array_size = -1;
};

// Parse the declaration(s) on one code line.  Returns an empty list for
// function definitions/prototypes, control statements and plain assignments to
// an existing Wiz name (the caller de-duplicates those).
std::vector<NewDecl> declarations_on_line(const Line& line, bool cwiz) {
    const std::string& code = line.code;
    size_t n = code.size();
    size_t i = 0;
    while (i < n && is_space(code[i])) ++i;
    if (i == n || code[i] == '#') return {};

    auto read_ident = [&](size_t& pos) {
        if (pos >= n || !ident_start(code[pos])) return std::string();
        size_t s = pos;
        while (pos < n && ident_char(code[pos])) ++pos;
        return code.substr(s, pos - s);
    };
    auto skip_ws = [&](size_t& pos) {
        while (pos < n && is_space(code[pos])) ++pos;
    };

    if (!cwiz) {
        size_t p = i;
        std::string first = read_ident(p);
        if (first.empty() || statement_keywords().count(first)) {
            // `for i in range(...)` declares the loop variable.  Do not treat
            // the target header `for cwapp` / `for 580` as a declaration.
            if (first == "for") {
                size_t q = p;
                skip_ws(q);
                size_t var_at = q;
                std::string var = read_ident(q);
                skip_ws(q);
                if (!var.empty() && read_ident(q) == "in")
                    return {{var, (int)var_at, -1}};
            }
            return {};
        }
        if (first == "def") return {};
        if (first == "const") {
            size_t q = p;
            skip_ws(q);
            std::string name = read_ident(q);
            return name.empty() ? std::vector<NewDecl>{}
                                : std::vector<NewDecl>{{name, (int)(q - name.size()), -1}};
        }
        if (first == "rom" || first == "rom8") {
            size_t q = p;
            skip_ws(q);
            std::string name = read_ident(q);
            return name.empty() ? std::vector<NewDecl>{}
                                : std::vector<NewDecl>{{name, (int)(q - name.size()), -1}};
        }
        // NAME = value / NAME[index] = value; ignore comparisons and compound
        // assignment operators.
        size_t q = p;
        skip_ws(q);
        if (q < n && code[q] == '[') {
            int depth = 0;
            while (q < n) {
                if (code[q] == '[') ++depth;
                else if (code[q] == ']' && --depth == 0) { ++q; break; }
                ++q;
            }
            skip_ws(q);
        }
        if (q < n && code[q] == '=' && (q + 1 >= n || code[q + 1] != '='))
            return {{first, (int)(i), -1}};
        return {};
    }

    // CWiz: [qualifiers] base-type [base-type] [*] declarator [, declarator]...
    size_t p = i;
    bool saw_base = false;
    while (p < n) {
        std::string word = read_ident(p);
        if (word.empty()) break;
        if (qualifiers().count(word)) {
            skip_ws(p);
            continue;
        }
        if (base_types().count(word)) {
            saw_base = true;
            size_t save = p;
            skip_ws(p);
            std::string next = read_ident(p);
            if ((word == "rom" || word == "rom8") &&
                (next == "int" || next == "char")) {
                // `rom int table[...]`: the second word is the element type.
                continue;
            }
            p = save;
            break;
        }
        break;
    }
    if (!saw_base) return {};

    std::vector<NewDecl> out;
    while (p < n) {
        skip_ws(p);
        while (p < n && code[p] == '*') { ++p; skip_ws(p); }
        if (p >= n || code[p] == '(') break;  // function pointer / function
        size_t name_at = p;
        std::string name = read_ident(p);
        if (name.empty()) break;
        if (statement_keywords().count(name) || base_types().count(name)) break;
        skip_ws(p);

        int size = -1;
        if (p < n && code[p] == '[') {
            size_t open = p++;
            int depth = 1;
            while (p < n && depth > 0) {
                if (code[p] == '[') ++depth;
                else if (code[p] == ']') --depth;
                ++p;
            }
            std::string inner = trim(code.substr(open + 1, p - open - 2));
            if (!inner.empty() &&
                inner.find_first_not_of("0123456789") == std::string::npos) {
                size = std::atoi(inner.c_str());
            }
            skip_ws(p);
        }

        // A '(' after the name means this is a function declaration.
        if (p < n && code[p] == '(') return {};
        out.push_back({name, (int)name_at, size});

        // Skip an initializer up to the next top-level comma.
        if (p < n && code[p] == '=') {
            ++p;
            int depth = 0;
            while (p < n) {
                char c = code[p];
                if (c == '(' || c == '[' || c == '{') ++depth;
                else if (c == ')' || c == ']' || c == '}') depth = std::max(0, depth - 1);
                else if (c == ',' && depth == 0) break;
                ++p;
            }
        }
        skip_ws(p);
        if (p < n && code[p] == ',') { ++p; continue; }
        break;
    }
    return out;
}

struct ParamInfo {
    std::string name;
    int line = 0;
    int col = 0;
};

struct FunctionInfo {
    std::string name;
    std::string ret;
    int line = 0;
    int col = 0;
    std::vector<ParamInfo> params;
    bool has_body = false;
    bool has_return = false;
};

// Parse a parameter list from text that is known to be between '(' and ')'.
void parse_params(const std::string& text, int line, int base_col, std::vector<ParamInfo>& out) {
    for (const std::string& raw : split_top_level(text, ',')) {
        std::string part = trim(raw);
        if (part.empty() || part == "void") continue;
        // Drop a default value.
        size_t eq = part.find('=');
        if (eq != std::string::npos) part = trim(part.substr(0, eq));
        // Remove array suffix and pointer stars, then take the last identifier.
        std::string name;
        size_t i = 0;
        while (i < part.size()) {
            if (!ident_start(part[i])) { ++i; continue; }
            size_t s = i;
            while (i < part.size() && ident_char(part[i])) ++i;
            std::string word = part.substr(s, i - s);
            if (!base_types().count(word) && !qualifiers().count(word)) name = word;
        }
        if (!name.empty())
            out.push_back({name, line, base_col + (int)raw.find(name) + 1});
    }
}

// Collect function signatures, parameters and (for CWiz) whether a non-void
// function contains a return statement.
std::vector<FunctionInfo> collect_functions(const std::vector<Line>& lines, bool cwiz) {
    std::vector<FunctionInfo> out;
    std::set<std::string> seen;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& code = lines[i].code;
        if (code.find('(') == std::string::npos) continue;

        FunctionInfo fn;
        size_t open = std::string::npos;
        if (!cwiz) {
            std::string s = trim(code);
            if (s.rfind("def ", 0) != 0) continue;
            size_t p = 4;
            while (p < s.size() && is_space(s[p])) ++p;
            size_t ns = p;
            while (p < s.size() && ident_char(s[p])) ++p;
            fn.name = s.substr(ns, p - ns);
            fn.ret = "int";
            fn.line = (int)i + 1;
            open = s.find('(', p);
            if (open == std::string::npos) continue;
            fn.col = (int)open + 1;
        } else {
            std::string s = trim(code);
            size_t p = 0;
            std::string word;
            while (p < s.size()) {
                if (!ident_start(s[p])) { ++p; continue; }
                size_t ws = p;
                while (p < s.size() && ident_char(s[p])) ++p;
                std::string w = s.substr(ws, p - ws);
                if (qualifiers().count(w)) continue;
                if (!base_types().count(w)) { word.clear(); break; }
                word = w;
                size_t q = p;
                while (q < s.size() && is_space(s[q])) ++q;
                std::string next;
                size_t r = q;
                while (r < s.size() && ident_char(s[r])) next += s[r++];
                if ((w == "rom" || w == "rom8") && (next == "int" || next == "char")) {
                    p = r;
                    word = next;
                    continue;
                }
                // After the base type, find `name(`.
                size_t np = q;
                while (np < s.size() && is_space(s[np])) ++np;
                size_t nstart = np;
                while (np < s.size() && ident_char(s[np])) ++np;
                if (nstart == np) break;
                fn.name = s.substr(nstart, np - nstart);
                fn.ret = w;
                open = np;
                while (open < s.size() && is_space(s[open])) ++open;
                if (open >= s.size() || s[open] != '(') { fn.name.clear(); break; }
                fn.line = (int)i + 1;
                fn.col = (int)open + 1;
                break;
            }
            if (fn.name.empty() || open == std::string::npos) continue;
            if (seen.count(fn.name + "@" + std::to_string(fn.line))) continue;
        }

        // Gather the parameter list, which may span lines.
        std::string sig;
        int depth = 0;
        size_t start = open + 1;
        size_t close = std::string::npos;
        size_t last_line = i;
        for (size_t j = i; j < lines.size(); ++j) {
            const std::string& c = lines[j].code;
            size_t from = (j == i) ? start : 0;
            for (size_t k = from; k < c.size(); ++k) {
                if (c[k] == '(') ++depth;
                else if (c[k] == ')') {
                    if (depth == 0) { close = k; last_line = j; break; }
                    --depth;
                }
                if (close == std::string::npos) sig += c[k];
            }
            if (close != std::string::npos) break;
            sig += ' ';
            last_line = j;
        }
        if (close == std::string::npos) continue;
        parse_params(sig, fn.line, fn.col, fn.params);
        seen.insert(fn.name + "@" + std::to_string(fn.line));

        // For CWiz, locate a `{` after the parameter list and scan the body.
        if (cwiz) {
            size_t b = last_line;
            size_t pos = close + 1;
            while (b < lines.size()) {
                const std::string& c = lines[b].code;
                if (b == last_line) {
                    if (pos < c.size() && c[pos] == '{') { fn.has_body = true; break; }
                    if (c.find('{', pos) != std::string::npos) {
                        pos = c.find('{', pos);
                        fn.has_body = true;
                        break;
                    }
                } else if (c.find('{') != std::string::npos) {
                    pos = c.find('{');
                    fn.has_body = true;
                    break;
                }
                ++b;
            }
            if (fn.has_body) {
                int brace = 0;
                bool started = false;
                for (size_t j = b; j < lines.size(); ++j) {
                    const std::string& c = lines[j].code;
                    size_t from = (j == b) ? pos : 0;
                    for (size_t k = from; k < c.size(); ++k) {
                        if (c[k] == '{') { ++brace; started = true; }
                        else if (c[k] == '}') {
                            --brace;
                            if (started && brace == 0) { j = lines.size(); break; }
                        }
                    }
                    if (std::regex_search(c, std::regex("\\breturn\\b"))) fn.has_return = true;
                    if (started && brace == 0) break;
                }
            }
        }
        out.push_back(std::move(fn));
    }
    return out;
}

// ---------------------------------------------------------------- helpers
void add(std::vector<LintDiagnostic>& out, LintSeverity sev, int line, int col, int end_col,
         const std::string& rule, const std::string& message, const std::string& hint = "") {
    LintDiagnostic d;
    d.severity = sev;
    d.line = line;
    d.col = std::max(1, col);
    d.end_col = end_col > 0 ? end_col : d.col + 1;
    d.rule = rule;
    d.message = message;
    d.hint = hint;
    out.push_back(std::move(d));
}

LintSeverity severity_of(const std::string& rule) {
    static const std::map<std::string, LintSeverity> sev = {
        {"preproc/cwapp-order", LintSeverity::Error},
        {"preproc/slot-order", LintSeverity::Error},
        {"preproc/slot-range", LintSeverity::Error},
        {"preproc/unknown-directive", LintSeverity::Error},
        {"preproc/unbalanced-conditional", LintSeverity::Error},
        {"style/todo", LintSeverity::Note},
        {"readability/empty-block", LintSeverity::Note},
        {"readability/redundant-boolean", LintSeverity::Note},
        {"correctness/constant-condition", LintSeverity::Note},
    };
    auto it = sev.find(rule);
    return it == sev.end() ? LintSeverity::Warning : it->second;
}

// Return the position of a plain '=' inside the condition of `if (...)` or
// `while (...)` on this line, or npos.
size_t assignment_in_condition(const std::string& code) {
    static const std::regex kw("\\b(if|while)\\b");
    std::smatch m;
    std::string::const_iterator begin = code.cbegin();
    while (std::regex_search(begin, code.cend(), m, kw)) {
        size_t base = (size_t)(m[0].second - code.cbegin());
        size_t open = code.find('(', base);
        if (open == std::string::npos) return std::string::npos;
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t i = open; i < code.size(); ++i) {
            if (code[i] == '(') ++depth;
            else if (code[i] == ')') {
                if (--depth == 0) { close = i; break; }
            }
        }
        if (close == std::string::npos) return std::string::npos;
        for (size_t i = open + 1; i < close; ++i) {
            if (code[i] != '=') continue;
            char prev = i > 0 ? code[i - 1] : '\0';
            char next = i + 1 < code.size() ? code[i + 1] : '\0';
            if (next == '=' || prev == '=' || prev == '!' || prev == '<' || prev == '>' ||
                prev == '+' || prev == '-' || prev == '*' || prev == '/' || prev == '%' ||
                prev == '&' || prev == '|' || prev == '^')
                continue;
            return i;
        }
        begin = m[0].second;
    }
    return std::string::npos;
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
        {"preproc/unknown-directive", "a directive is misspelled or unsupported"},
        {"preproc/unbalanced-conditional", "#if/#endif (or #else/#elif) do not nest"},
        {"readability/unused-variable", "a declared variable is never read"},
        {"readability/unused-parameter", "a function parameter is never used"},
        {"readability/empty-block", "a control-flow body is empty"},
        {"readability/redundant-boolean", "a comparison with true/false is redundant"},
        {"correctness/shadowed-variable", "a local hides a global with the same name"},
        {"correctness/shadowed-builtin", "a declaration hides a built-in function"},
        {"correctness/unreachable-code", "a statement follows return/break/continue"},
        {"correctness/assignment-in-condition", "an assignment is used as a condition"},
        {"correctness/self-assignment", "a variable is assigned to itself"},
        {"correctness/division-by-zero", "a literal zero is used as a divisor"},
        {"correctness/constant-condition", "a condition is always false"},
        {"correctness/out-of-bounds", "a constant index is outside the array"},
        {"correctness/missing-return", "a non-void function has no return statement"},
        {"style/line-length", "line is longer than the configured limit"},
        {"style/trailing-whitespace", "line ends with spaces or tabs"},
        {"style/tabs", "indentation uses a tab"},
        {"style/todo", "TODO/FIXME/XXX marker"},
    };
    return rules;
}

std::vector<LintDiagnostic> lint_source(const std::string& source,
                                        const std::string& file_name,
                                        const std::string& target,
                                        int line_limit) {
    std::vector<LintDiagnostic> out;
    const bool cwiz = ends_with(file_name, ".cwiz");
    const std::vector<Line> lines = split_lines(source, cwiz);
    std::set<int> compile_error_lines;

    // ---------------------------------------------------------------- compiler
    try {
        if (source_is_rop(source)) {
            const std::string rop_model = target == "880" || target == "cwii" ? "880" : "580";
            const RopResult result = build_rop(source, rop_model, cwiz ? "cwiz" : "wiz");
            if (!result.ok) {
                add(out, LintSeverity::Error, 0, 0, 0, "compile",
                    result.error.empty() ? "ROP compilation failed" : result.error);
            }
        } else if (target == "cwapp" || target == "580" || target == "880" || target == "cw" ||
                   target == "cwii" || target == "fwnx") {
            if (cwiz)
                build_cwiz(source, target, file_name);
            else
                build_source(source, target);
        }
    } catch (const CompileError& e) {
        LintDiagnostic d;
        d.severity = LintSeverity::Error;
        d.line = e.line;
        d.col = e.col;
        d.end_col = e.end_col;
        d.rule = "compile";
        d.message = e.what();
        d.hint = e.hint;
        d.code = e.code;
        out.push_back(std::move(d));
        if (e.line > 0) compile_error_lines.insert(e.line);
    } catch (const std::exception& e) {
        add(out, LintSeverity::Error, 0, 0, 0, "compile", e.what());
    }

    // ---------------------------------------------------------------- preprocessor
    static const std::set<std::string> known_directives = {
        "if", "ifdef", "ifndef", "elif", "else", "endif", "define", "undef", "pragma",
        "error", "include", "for", "cwapp", "org", "binary", "slot", "rop", "launcher",
        "pad", "nopad", "__asm__", "__hex__", "line"};
    std::string last_for;
    std::set<std::string> seen_for;
    bool in_cwapp = false;
    bool slot_seen = false;
    std::vector<int> conditional_stack;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& raw = lines[i].text;
        const std::string t = trim(raw);
        if (t.empty()) continue;

        if (cwiz && lines[i].directive && t[0] == '#') {
            size_t p = 1;
            while (p < t.size() && is_space(t[p])) ++p;
            size_t s = p;
            while (p < t.size() && ident_char(t[p])) ++p;
            std::string name = t.substr(s, p - s);
            if (!name.empty() && !known_directives.count(name)) {
                std::vector<std::string> candidates(known_directives.begin(), known_directives.end());
                std::string hint = did_you_mean(name, candidates);
                add(out, severity_of("preproc/unknown-directive"), (int)i + 1, (int)s + 1,
                    (int)p + 1, "preproc/unknown-directive",
                    "unknown directive '#" + name + "'", hint);
            }
            if (name == "if" || name == "ifdef" || name == "ifndef") conditional_stack.push_back((int)i + 1);
            else if (name == "elif" || name == "else") {
                if (conditional_stack.empty())
                    add(out, severity_of("preproc/unbalanced-conditional"), (int)i + 1, 1,
                        (int)t.size() + 1, "preproc/unbalanced-conditional",
                        "#" + name + " without a matching #if",
                        "add #if/#ifdef before it, or remove this directive");
            } else if (name == "endif") {
                if (conditional_stack.empty())
                    add(out, severity_of("preproc/unbalanced-conditional"), (int)i + 1, 1,
                        (int)t.size() + 1, "preproc/unbalanced-conditional",
                        "#endif without a matching #if", "remove it or add the missing #if");
                else conditional_stack.pop_back();
            }
        }

        // Target headers: `#for 580` (CWiz) or `for 580` (Wiz).  Do not treat
        // `for i in range(...)` / `for (...)` as a header.  `#cwapp` and
        // `for cwapp` are handled separately below.
        std::string header;
        bool is_header = false;
        if (cwiz) {
            if (t.rfind("#for", 0) == 0) {
                header = t.substr(4);
                is_header = true;
            }
        } else if (t.rfind("for ", 0) == 0 && t.find(" in ") == std::string::npos) {
            header = t.substr(4);
            is_header = true;
        }
        if (is_header) {
            std::string first = trim(header).substr(0, trim(header).find_first_of(", \t#"));
            if (!first.empty()) {
                if (seen_for.count(first))
                    add(out, severity_of("preproc/duplicate-target"), (int)i + 1, 1,
                        (int)raw.size() + 1, "preproc/duplicate-target",
                        "#for " + first + " is already listed earlier in this file",
                        "remove the duplicate section header");
                seen_for.insert(first);
                last_for = first;
                in_cwapp = false;
                slot_seen = false;
                if (first != "580" && first != "880" && first != "cwapp" && first != "all")
                    add(out, severity_of("preproc/unknown-target"), (int)i + 1, 1,
                        (int)raw.size() + 1, "preproc/unknown-target",
                        "unknown target '#for " + first + "' (use 580 or 880)",
                        "write 580 (fx-580VN X) or 880 (fx-880BTG)");
            }
        }
        if (t == "----") {
            last_for.clear();
            in_cwapp = false;
            slot_seen = false;
        }
        const bool cwapp = cwiz && t.rfind("#cwapp", 0) == 0;
        if (cwapp && last_for != "580") {
            if (!compile_error_lines.count((int)i + 1))
                add(out, severity_of("preproc/cwapp-order"), (int)i + 1, 1, (int)raw.size() + 1,
                    "preproc/cwapp-order",
                    "#cwapp needs #for 580 before it" +
                        (last_for.empty() ? std::string(" (no target section is active)")
                                          : " (the current section is #for " + last_for + ")"),
                    "add '#for 580' before '#cwapp'");
        }
        if (cwapp && last_for == "580") {
            in_cwapp = true;
            slot_seen = false;
        }
        const bool slot_line = cwiz && (t == "#slot" || t.rfind("#slot ", 0) == 0 ||
                                        t.rfind("#slot\t", 0) == 0);
        if (slot_line) {
            if (!in_cwapp) {
                add(out, severity_of("preproc/slot-order"), (int)i + 1, 1, (int)raw.size() + 1,
                    "preproc/slot-order",
                    "#slot is only available after #cwapp (inside a #for 580 section)",
                    "move '#slot n' after a '#cwapp' line");
            } else if (slot_seen) {
                add(out, severity_of("preproc/duplicate-slot"), (int)i + 1, 1, (int)raw.size() + 1,
                    "preproc/duplicate-slot",
                    "this #cwapp section already has a #slot",
                    "keep a single '#slot n' per #cwapp section");
            } else {
                slot_seen = true;
                std::string rest = trim(t.size() > 5 ? t.substr(5) : "");
                char* end = nullptr;
                long n = rest.empty() ? 0 : std::strtol(rest.c_str(), &end, 10);
                if (rest.empty() || !end || *end != '\0' || n < 1 || n > 12)
                    add(out, severity_of("preproc/slot-range"), (int)i + 1, 1,
                        (int)raw.size() + 1, "preproc/slot-range",
                        "#slot needs a slot number from 1 to 12",
                        "for example '#slot 2'");
            }
        }
    }
    if (!conditional_stack.empty()) {
        int line = conditional_stack.back();
        add(out, severity_of("preproc/unbalanced-conditional"), line, 1, 1,
            "preproc/unbalanced-conditional", "unterminated #if (missing #endif)",
            "add '#endif' at the end of the conditional block");
    }

    // ---------------------------------------------------------------- declarations
    struct Decl {
        std::string name;
        int line;
        int col;
        int indent;
        bool global;
    };
    std::vector<Decl> decls;
    std::map<std::string, int> global_line;
    std::map<std::string, int> array_size;
    std::map<std::string, std::set<int>> array_decl_lines;
    int brace_depth = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].blank || lines[i].directive) continue;
        const int line_depth = brace_depth;
        const std::vector<NewDecl> names = declarations_on_line(lines[i], cwiz);
        const bool global = cwiz ? line_depth == 0 : lines[i].indent == 0;
        for (const NewDecl& item : names) {
            decls.push_back({item.name, (int)i + 1, item.col, lines[i].indent, global});
            if (global && !global_line.count(item.name)) global_line[item.name] = (int)i + 1;
            if (item.array_size >= 0) {
                array_size[item.name] = item.array_size;
                array_decl_lines[item.name].insert((int)i + 1);
            }
        }
        for (char c : lines[i].code) {
            if (c == '{') ++brace_depth;
            else if (c == '}') brace_depth = std::max(0, brace_depth - 1);
        }
    }

    // De-duplicate declarations: Wiz `x = 1` is an assignment after the first
    // declaration.  For Wiz, keep the first global and the first local for each
    // name (the local one is what exposes shadowing).
    std::set<std::string> seen_pos;
    std::set<std::string> seen_wiz_global;
    std::set<std::string> seen_wiz_local;
    std::vector<Decl> unique;
    for (const Decl& d : decls) {
        if (d.name.empty()) continue;
        std::string key = d.name + "@" + std::to_string(d.line) + "@" + std::to_string(d.col);
        if (!seen_pos.insert(key).second) continue;
        if (!cwiz) {
            auto& seen = d.global ? seen_wiz_global : seen_wiz_local;
            if (!seen.insert(d.name).second) continue;
        }
        unique.push_back(d);
    }
    decls.swap(unique);

    for (const Decl& d : decls) {
        if (d.name.empty() || d.name[0] == '_') continue;
        if (!d.global && global_line.count(d.name) && global_line[d.name] != d.line) {
            add(out, severity_of("correctness/shadowed-variable"), d.line, d.col + 1,
                d.col + (int)d.name.size() + 1, "correctness/shadowed-variable",
                "local '" + d.name + "' hides the global declared on line " +
                    std::to_string(global_line[d.name]),
                "rename the local, or use the 'global " + d.name + "' statement in Wiz");
        }
        if (builtin_names().count(d.name)) {
            add(out, severity_of("correctness/shadowed-builtin"), d.line, d.col + 1,
                d.col + (int)d.name.size() + 1, "correctness/shadowed-builtin",
                "'" + d.name + "' hides the built-in function of the same name",
                "rename the variable; built-ins are called like functions");
        }
        if (count_word(lines, d.name) <= 1) {
            add(out, severity_of("readability/unused-variable"), d.line, d.col + 1,
                d.col + (int)d.name.size() + 1, "readability/unused-variable",
                "'" + d.name + "' is declared but never read",
                "remove it, use it, or prefix it with '_' to silence this warning");
        }
    }

    // ---------------------------------------------------------------- functions
    const std::vector<FunctionInfo> functions = collect_functions(lines, cwiz);
    for (const FunctionInfo& fn : functions) {
        for (const ParamInfo& p : fn.params) {
            if (p.name.empty() || p.name[0] == '_') continue;
            if (count_word(lines, p.name) <= 1) {
                add(out, severity_of("readability/unused-parameter"), p.line, p.col,
                    p.col + (int)p.name.size(), "readability/unused-parameter",
                    "parameter '" + p.name + "' of " + fn.name + "() is never used",
                    "remove the parameter or prefix it with '_' to silence this warning");
            }
        }
        if (cwiz && fn.has_body && fn.ret != "void" && !fn.has_return) {
            add(out, severity_of("correctness/missing-return"), fn.line, fn.col,
                fn.col + (int)fn.name.size(), "correctness/missing-return",
                "non-void function '" + fn.name + "' has no return statement",
                "add a return value, or declare the function as void");
        }
    }

    // ---------------------------------------------------------------- line rules
    bool reported_tabs = false;
    const int limit = std::max(20, line_limit);
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& text = lines[i].text;
        if (text.size() > (size_t)limit)
            add(out, severity_of("style/line-length"), (int)i + 1, limit + 1, (int)text.size() + 1,
                "style/line-length", fmt("line is %zu columns (limit %d)", text.size(), limit),
                "wrap the line, or raise wiz.lint.lineLength");
        if (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
            add(out, severity_of("style/trailing-whitespace"), (int)i + 1, (int)text.size(),
                (int)text.size() + 1, "style/trailing-whitespace", "trailing whitespace",
                "delete the spaces at the end of the line");
        if (!reported_tabs && text.find('\t') != std::string::npos) {
            add(out, severity_of("style/tabs"), (int)i + 1, (int)text.find('\t') + 1,
                (int)text.find('\t') + 2, "style/tabs",
                "tab character in the source (the examples use spaces)",
                "replace tabs with four spaces");
            reported_tabs = true;
        }
        // TODO markers only count inside comments.
        for (const char* marker : {"TODO", "FIXME", "XXX"}) {
            size_t pos = 0;
            while ((pos = text.find(marker, pos)) != std::string::npos) {
                bool in_comment = pos < lines[i].comment.size() && lines[i].comment[pos] != ' ';
                if (in_comment) {
                    add(out, LintSeverity::Note, (int)i + 1, (int)pos + 1,
                        (int)pos + (int)std::strlen(marker) + 1, "style/todo",
                        "unfinished marker in a comment",
                        "finish the work or remove the marker");
                    break;
                }
                pos += std::strlen(marker);
            }
        }
    }

    // ---------------------------------------------------------------- code rules
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& code = lines[i].code;
        if (trim(code).empty()) continue;
        const int line = (int)i + 1;

        // x = x;
        {
            static const std::regex re(
                "\\b([A-Za-z_][A-Za-z0-9_]*)[ \\t]*=[ \\t]*\\1[ \\t]*;");
            std::smatch m;
            if (std::regex_search(code, m, re)) {
                size_t at = (size_t)m.position(0);
                if (at == 0 || code[at - 1] != '+') {
                    add(out, severity_of("correctness/self-assignment"), line,
                        (int)m.position(1) + 1,
                        (int)m.position(1) + (int)m[1].str().size() + 1,
                        "correctness/self-assignment",
                        "'" + m[1].str() + "' is assigned to itself",
                        "remove the statement, or use the value you meant to assign");
                }
            }
        }

        // Assignment in an if/while condition.
        if (cwiz) {
            size_t at = assignment_in_condition(code);
            if (at != std::string::npos)
                add(out, severity_of("correctness/assignment-in-condition"), line, (int)at + 1,
                    (int)at + 2, "correctness/assignment-in-condition",
                    "assignment used as a condition (did you mean '=='?)",
                    "use '==' for comparison; if assignment is intended, put it on its own line");
        }

        // Division / remainder by a literal zero.
        {
            static const std::regex re("([/%])[ \\t]*0(?![0-9xX])");
            std::smatch m;
            if (std::regex_search(code, m, re)) {
                add(out, severity_of("correctness/division-by-zero"), line,
                    (int)m.position(1) + 1, (int)m.position(1) + 2,
                    "correctness/division-by-zero",
                    "division by zero in a constant expression",
                    "check the divisor; guard the operation when it can be zero");
            }
        }

        // if (0) / while (false).
        {
            static const std::regex re("\\b(if|while)[ \\t]*\\([ \\t]*(0|false)[ \\t]*\\)");
            std::smatch m;
            if (std::regex_search(code, m, re))
                add(out, severity_of("correctness/constant-condition"), line,
                    (int)m.position(0) + 1, (int)m.position(0) + (int)m[0].str().size() + 1,
                    "correctness/constant-condition",
                    "the condition is always false",
                    "remove the block, or replace the constant with the intended test");
        }

        // x == true / x != false.
        {
            static const std::regex re("(==|!=)[ \\t]*(true|false)\\b");
            std::smatch m;
            if (std::regex_search(code, re) && std::regex_search(code, m, re))
                add(out, severity_of("readability/redundant-boolean"), line,
                    (int)m.position(0) + 1, (int)m.position(0) + (int)m[0].str().size() + 1,
                    "readability/redundant-boolean",
                    "comparison with '" + m[2].str() + "' is redundant",
                    m[1].str() == "==" ? "use the value directly, or '!' for negation"
                                       : "use the value directly");
        }

        // if (...) {} / while (...) {} / else {}.
        {
            static const std::regex re(
                "\\b(if|while|for)[ \\t]*\\([^)]*\\)[ \\t]*\\{[ \\t]*\\}");
            static const std::regex else_re("\\belse[ \\t]*\\{[ \\t]*\\}");
            std::smatch m;
            if (std::regex_search(code, m, re) || std::regex_search(code, m, else_re))
                add(out, severity_of("readability/empty-block"), line,
                    (int)m.position(0) + 1, (int)m.position(0) + (int)m[0].str().size() + 1,
                    "readability/empty-block", "empty control-flow body",
                    "add a comment explaining why it is empty, or remove the block");
        }

        // Constant index outside a known array size.
        {
            static const std::regex re("\\b([A-Za-z_][A-Za-z0-9_]*)[ \\t]*\\[[ \\t]*([0-9]+)[ \\t]*\\]");
            std::sregex_iterator it(code.begin(), code.end(), re), end;
            for (; it != end; ++it) {
                const std::string name = (*it)[1].str();
                auto sz = array_size.find(name);
                if (sz == array_size.end()) continue;
                if (array_decl_lines[name].count(line)) continue;
                long idx = std::strtol((*it)[2].str().c_str(), nullptr, 10);
                if (idx < 0 || idx < sz->second) continue;
                add(out, severity_of("correctness/out-of-bounds"), line,
                    (int)(*it).position(0) + 1,
                    (int)(*it).position(0) + (int)(*it)[0].str().size() + 1,
                    "correctness/out-of-bounds",
                    fmt("index %ld is outside '%s' (size %d)", idx, name.c_str(), sz->second),
                    "use an index from 0 to " + std::to_string(sz->second - 1));
            }
        }
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
                    lines[j].indent + 1, lines[j].indent + 2, "correctness/unreachable-code",
                    "unreachable: the previous statement transfers control",
                    "remove the statement, or move it before the return/break/continue");
            break;
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const LintDiagnostic& a, const LintDiagnostic& b) {
        if (a.line != b.line) return a.line < b.line;
        if (a.col != b.col) return a.col < b.col;
        return a.rule < b.rule;
    });
    return out;
}

}  // namespace wiz
