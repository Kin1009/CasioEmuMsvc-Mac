// cpre.cpp - the CWiz preprocessor (see cpre.hpp).
#include "cpre.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"
#include "cwiz.hpp"
#include "targets.hpp"

namespace wiz {
namespace {

static std::string prev_target_for;  // enforce #cwapp only after #for 580

std::vector<CwizOrigin>& origins() {
    static std::vector<CwizOrigin> v;
    return v;
}

bool ident_start(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
bool ident_char(char c) { return std::isalnum((unsigned char)c) || c == '_'; }

std::string lstrip(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
std::string rstrip(const std::string& s) {
    size_t n = s.size();
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n--;
    return s.substr(0, n);
}
std::string strip(const std::string& s) { return rstrip(lstrip(s)); }

std::string dir_name(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Remove a trailing `//` comment and any `/* ... */` comment, ignoring quotes.
// Directives are text, so a comment on a `#define` line must not become part of
// the macro body.
std::string strip_comment(const std::string& s) {
    std::string out;
    bool in_str = false;
    char q = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (in_str) {
            out += c;
            if (c == '\\' && i + 1 < s.size()) {
                out += s[++i];
                continue;
            }
            if (c == q) in_str = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_str = true;
            q = c;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') return rstrip(out);
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            if (e == std::string::npos) return rstrip(out);
            out += ' ';
            i = e + 1;
            continue;
        }
        out += c;
    }
    return rstrip(out);
}

struct Macro {
    bool func = false;
    std::vector<std::string> params;
    std::string body;
};

struct IfTok {
    std::string kind;  // "num" | "op" (name holds the operator) | "id" (name holds the name)
    long long num = 0;
    std::string name;
};

IfTok tok_num(long long v) {
    IfTok t;
    t.kind = "num";
    t.num = v;
    return t;
}
IfTok tok_op(const std::string& o) {
    IfTok t;
    t.kind = "op";
    t.name = o;
    return t;
}
IfTok tok_id(const std::string& n) {
    IfTok t;
    t.kind = "id";
    t.name = n;
    return t;
}

// ---------------------------------------------------------------- #if evaluator
class IfEval {
public:
    IfEval(std::vector<IfTok> toks, const std::map<std::string, Macro>& macros, int line)
        : t(std::move(toks)), macros(macros), line(line) {}

    long long run() {
        long long v = ternary();
        if (p != t.size()) throw CwizError("junk in #if expression", line);
        return v;
    }

private:
    std::vector<IfTok> t;
    size_t p = 0;
    const std::map<std::string, Macro>& macros;
    int line;

    bool is_op(const std::string& s) const {
        return p < t.size() && t[p].kind == "op" && t[p].name == s;
    }

    long long ternary() {
        long long c = lor();
        if (is_op("?")) {
            p++;
            long long a = ternary();
            if (!is_op(":")) throw CwizError("expected ':' in #if", line);
            p++;
            long long b = ternary();
            return c ? a : b;
        }
        return c;
    }
    long long lor() {
        long long a = land();
        while (is_op("||")) { p++; long long b = land(); a = (a || b) ? 1 : 0; }
        return a;
    }
    long long land() {
        long long a = bor();
        while (is_op("&&")) { p++; long long b = bor(); a = (a && b) ? 1 : 0; }
        return a;
    }
    long long bor() {
        long long a = bxor();
        while (is_op("|")) { p++; a = a | bxor(); }
        return a;
    }
    long long bxor() {
        long long a = band();
        while (is_op("^")) { p++; a = a ^ band(); }
        return a;
    }
    long long band() {
        long long a = eq();
        while (is_op("&")) { p++; a = a & eq(); }
        return a;
    }
    long long eq() {
        long long a = rel();
        while (is_op("==") || is_op("!=")) {
            std::string o = t[p].name;
            p++;
            long long b = rel();
            a = (o == "==") ? (a == b) : (a != b);
        }
        return a;
    }
    long long rel() {
        long long a = shift();
        while (is_op("<") || is_op(">") || is_op("<=") || is_op(">=")) {
            std::string o = t[p].name;
            p++;
            long long b = shift();
            if (o == "<") a = a < b;
            else if (o == ">") a = a > b;
            else if (o == "<=") a = a <= b;
            else a = a >= b;
        }
        return a;
    }
    long long shift() {
        long long a = add();
        while (is_op("<<") || is_op(">>")) {
            std::string o = t[p].name;
            p++;
            long long b = add();
            if (b < 0 || b > 63) b = 0;
            unsigned long long u = (unsigned long long)a;
            a = (long long)((o == "<<") ? (u << b) : (u >> b));
        }
        return a;
    }
    long long add() {
        long long a = mul();
        while (is_op("+") || is_op("-")) {
            std::string o = t[p].name;
            p++;
            long long b = mul();
            a = (o == "+") ? a + b : a - b;
        }
        return a;
    }
    long long mul() {
        long long a = un();
        while (is_op("*") || is_op("/") || is_op("%")) {
            std::string o = t[p].name;
            p++;
            long long b = un();
            if ((o == "/" || o == "%") && b == 0) throw CwizError("division by zero in #if", line);
            a = (o == "*") ? a * b : (o == "/") ? a / b : a % b;
        }
        return a;
    }
    long long un() {
        if (is_op("!")) { p++; return un() ? 0 : 1; }
        if (is_op("~")) { p++; return ~un(); }
        if (is_op("-")) { p++; return -un(); }
        if (is_op("+")) { p++; return un(); }
        if (is_op("(")) {
            p++;
            long long v = ternary();
            if (!is_op(")")) throw CwizError("expected ')' in #if", line);
            p++;
            return v;
        }
        if (p >= t.size()) throw CwizError("unexpected end of #if expression", line);
        if (t[p].kind == "num") return t[p++].num;
        std::string name = t[p++].name;
        auto it = macros.find(name);
        if (it != macros.end() && !it->second.func) {
            std::string b = strip(it->second.body);
            if (!b.empty()) {
                char* end = nullptr;
                long long v = std::strtoll(b.c_str(), &end, 0);
                if (end && *end == '\0') return v;
            }
        }
        return 0;
    }
};

// ---------------------------------------------------------------- preprocessor
class Pre {
public:
    explicit Pre(const std::string& target) {
        macros["__CWIZ__"] = Macro{false, {}, "1"};
        if (target == "cw" || target == "cwapp") macros["__CW__"] = Macro{false, {}, "1"};
        else macros["__CWII__"] = Macro{false, {}, "1"};
        if (target == "cwapp") macros["__CWAPP__"] = Macro{false, {}, "1"};
        macros["__ROWS__"] = Macro{false, {}, std::to_string(get_target(target).rows)};
    }

    std::string run(const std::string& src, const std::string& file) {
        root_file = file;
        prev_target_for.clear();
        has_cwapp_ = false;
        slot_ = 2;
        slot_seen_ = false;
        active_stack.push_back(Frame{true, true, true, false});
        emit_lines(src, file);
        if (active_stack.size() != 1)
            throw CwizError("unterminated #if (missing #endif)", file_line, 0,
                            "add '#endif' at the end of the conditional block",
                            "preproc.unbalanced-conditional");
        return join(out, "\n");
    }

    bool has_cwapp() const { return has_cwapp_; }
    int slot() const { return slot_; }
    std::string binary_path() const { return binary_path_; }

private:
    struct Frame {
        bool parent;
        bool active;
        bool taken;
        bool seen_else;
    };

    std::map<std::string, Macro> macros;
    std::vector<Frame> active_stack;
    std::vector<std::string> out;
    std::set<std::string> once_files;
    std::vector<std::string> include_stack;
    std::string root_file;
    bool in_block = false;  // inside a /* ... */ comment
    bool has_cwapp_ = false;
    int slot_ = 2;
    bool slot_seen_ = false;
    std::string binary_path_ = "";
    int file_line = 0;

    bool active() const { return active_stack.back().active; }

    void push_out(const std::string& text, const std::string& file, int line) {
        out.push_back(text);
        origins().push_back(CwizOrigin{file, line});
    }

    void emit_lines(const std::string& src, const std::string& file) {
        std::string s = replace_all(src, "\r", "");
        std::vector<std::string> lines = split(s, '\n');

        for (size_t i = 0; i < lines.size(); i++) {
            file_line = (int)i + 1;
            std::string joined = lines[i];
            int consumed = 0;
            std::string probe = rstrip(joined);
            while (!probe.empty() && probe.back() == '\\' && i + 1 < lines.size()) {
                joined = rstrip(joined.substr(0, joined.size() - 1)) + " " + lstrip(lines[++i]);
                consumed++;
                probe = rstrip(joined);
            }

            std::string t = lstrip(joined);
            if (!in_block && !t.empty() && t[0] == '#') {
                try {
                    handle_directive(t, file, (int)i + 1);
                } catch (const CwizError& e) {
                    if (file != root_file) throw CwizError(file + ": " + e.what(), (int)i + 1);
                    throw;
                }
            } else if (active()) {
                push_out(expand(joined, 0), file, (int)i + 1);
            } else {
                push_out("", file, (int)i + 1);
            }
            for (int k = 0; k < consumed; k++) push_out("", file, (int)i + 1);
        }
    }

    void handle_directive(const std::string& t, const std::string& file, int line) {
        size_t i = 1;
        while (i < t.size() && (t[i] == ' ' || t[i] == '\t')) i++;
        size_t s = i;
        while (i < t.size() && ident_char(t[i])) i++;
        std::string name = t.substr(s, i - s);
        std::string rest = strip_comment(strip(t.substr(i)));

        if (name.empty()) {  // a lone '#'
            push_out("", file, line);
            return;
        }

        if (name == "ifdef" || name == "ifndef" || name == "if") {
            bool parent = active();
            bool cond = false;
            if (parent) {
                if (name == "ifdef") cond = macros.count(rest) != 0;
                else if (name == "ifndef") cond = macros.count(rest) == 0;
                else cond = eval_if(rest, line) != 0;
            }
            active_stack.push_back(Frame{parent, parent && cond, parent && cond, false});
            push_out("", file, line);
            return;
        }
        if (name == "elif") {
            if (active_stack.size() <= 1)
                throw CwizError("#elif without #if", line, 0,
                                "add an #if/#ifdef before this #elif",
                                "preproc.unbalanced-conditional");
            Frame& f = active_stack.back();
            if (f.seen_else)
                throw CwizError("#elif after #else", line, 0,
                                "move the #elif branch before #else",
                                "preproc.unbalanced-conditional");
            if (!f.parent || f.taken) {
                f.active = false;
            } else {
                f.active = eval_if(rest, line) != 0;
                f.taken = f.active;
            }
            push_out("", file, line);
            return;
        }
        if (name == "else") {
            if (active_stack.size() <= 1)
                throw CwizError("#else without #if", line, 0,
                                "add an #if/#ifdef before this #else",
                                "preproc.unbalanced-conditional");
            Frame& f = active_stack.back();
            if (f.seen_else)
                throw CwizError("duplicate #else", line, 0,
                                "a conditional block can have only one #else",
                                "preproc.unbalanced-conditional");
            f.seen_else = true;
            f.active = f.parent && !f.taken;
            f.taken = f.taken || f.active;
            push_out("", file, line);
            return;
        }
        if (name == "endif") {
            if (active_stack.size() <= 1)
                throw CwizError("#endif without #if", line, 0,
                                "remove it, or add the missing #if/#ifdef",
                                "preproc.unbalanced-conditional");
            active_stack.pop_back();
            push_out("", file, line);
            return;
        }

        if (!active()) {
            push_out("", file, line);
            return;
        }

        if (name == "define") {
            define_macro(rest, line);
            push_out("", file, line);
            return;
        }
        if (name == "undef") {
            macros.erase(rest);
            push_out("", file, line);
            return;
        }
        if (name == "pragma") {
            if (rest == "once") once_files.insert(canon_file(file));
            push_out("", file, line);
            return;
        }
        if (name == "error") {
            throw CwizError(rest.empty() ? "#error" : rest, line);
        }
        if (name == "include") {
            push_out("", file, line);
            std::string inc = include_path(rest, file, line);
            std::string canon = canon_file(inc);
            if (once_files.count(canon)) return;
            if (contains(include_stack, canon))
                throw CwizError("#include cycle: " + inc, line, 0,
                                "remove the include that points back to a file already being read",
                                "preproc.include-cycle");
            bool ok = false;
            std::string text = read_text_file(inc, &ok);
            if (!ok)
                throw CwizError("#include: file not found '" + inc +
                                    "' (looked relative to source '" + file + "':" +
                                    std::to_string(line) + ")",
                                line, 0,
                                "check the file name and place the included file next to the source",
                                "preproc.include-not-found");
            include_stack.push_back(canon);
            emit_lines(text, inc);
            include_stack.pop_back();
            return;
        }
        if (name == "for") {
            // `#for 580, 880` - the target directive (which calculator).
            prev_target_for = rest;
            has_cwapp_ = false;
            push_out("for " + rest, file, line);
            return;
        }
        if (name == "cwapp") {
            // `#cwapp` only valid after `#for 580` (the app flavour of the 580).
            if (prev_target_for.find("580") == std::string::npos) {
                throw CwizError("#cwapp requires #for 580 before it (found: #for " +
                                    prev_target_for + ")",
                                line, 0, "add '#for 580' before '#cwapp'",
                                "preproc.cwapp-order");
            }
            has_cwapp_ = true;
            slot_seen_ = false;
            push_out("for cwapp", file, line);
            return;
        }
        if (name == "org") {
            std::string rest_o = strip(rest);
            push_out("ORG " + rest_o, file, line);
            return;
        }
        if (name == "binary") {
            std::string rest_s = strip(rest);
            size_t q1 = rest_s.find('"');
            // #org directive: set assembly origin
            if (name == "org") {
                std::string rest_o = strip(rest);
                push_out("ORG " + rest_o, file, line);
                return;
            }
            if (q1 == std::string::npos)
                throw CwizError("#binary needs \"path\"", line, 0,
                                "write #binary \"firmware.bin\"", "preproc.binary");
            size_t q2 = rest_s.find('"', q1 + 1);
            if (q2 == std::string::npos)
                throw CwizError("#binary unterminated string", line, 0,
                                "add the closing '\"' after the path", "preproc.binary");
            std::string bpath = rest_s.substr(q1 + 1, q2 - q1 - 1);
            binary_path_ = bpath;
            // emit nothing; build_cwiz will inject DB directly
            push_out("", file, line);
            return;
        }
        if (name == "slot") {
            // `#slot n` - choose the fx-580VN X menu slot (1..12) that the
            // compiler patches.  It is only meaningful in a #cwapp section.
            if (!has_cwapp_)
                throw CwizError("#slot is only available after #cwapp", line, 0,
                                "move '#slot n' below the '#cwapp' line", "preproc.slot-order");
            if (slot_seen_)
                throw CwizError("#slot is already set for this #cwapp section", line, 0,
                                "keep a single '#slot n' per #cwapp section",
                                "preproc.slot-duplicate");
            char* end = nullptr;
            long n = std::strtol(rest.c_str(), &end, 10);
            if (rest.empty() || !end || *end != '\0' || n < 1 || n > 12)
                throw CwizError("#slot needs a slot number from 1 to 12", line, 0,
                                "for example '#slot 2'", "preproc.slot-range");
            slot_ = (int)n;
            slot_seen_ = true;
            push_out("", file, line);
            return;
        }
        {
            static const std::vector<std::string> known = {
                "if", "ifdef", "ifndef", "elif", "else", "endif", "define", "undef",
                "pragma", "error", "include", "for", "cwapp", "org", "binary", "slot"};
            std::string hint = did_you_mean(name, known);
            throw CwizError("unknown directive '#" + name + "'", line, 0, hint,
                            "preproc.unknown-directive");
        }
    }

    static std::string canon_file(const std::string& p) {
        std::string r = p;
        while (r.size() >= 2 && r.compare(0, 2, "./") == 0) r = r.substr(2);
        return r;
    }

    std::string include_path(const std::string& rest, const std::string& file, int line) {
        std::string r = strip(rest);
        if (r.size() < 2)
            throw CwizError("#include needs \"file\"", line, 0,
                            "write #include \"file.cwiz\"", "preproc.include");
        char open = r[0];
        char close = (open == '<') ? '>' : '"';
        if (open != '"' && open != '<')
            throw CwizError("#include needs \"file\"", line, 0,
                            "write #include \"file.cwiz\"", "preproc.include");
        size_t end = r.find(close, 1);
        if (end == std::string::npos)
            throw CwizError("unterminated #include", line, 0,
                            "add the closing quote or '>'", "preproc.include");
        std::string name = r.substr(1, end - 1);
        if (!name.empty() && name[0] == '/') return name;
        std::string cand = dir_name(file) + name;
        bool ok = false;
        read_text_file(cand, &ok);
        return ok ? cand : name;
    }

    void define_macro(const std::string& rest, int line) {
        if (rest.empty() || !ident_start(rest[0]))
            throw CwizError("#define needs a name", line, 0,
                            "write #define NAME value or #define NAME(a) value",
                            "preproc.define");
        size_t i = 0;
        while (i < rest.size() && ident_char(rest[i])) i++;
        std::string name = rest.substr(0, i);
        Macro m;
        if (i < rest.size() && rest[i] == '(') {
            m.func = true;
            size_t j = i + 1;
            while (true) {
                while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) j++;
                if (j < rest.size() && rest[j] == ')') { j++; break; }
                if (j >= rest.size())
                    throw CwizError("unterminated macro parameter list", line, 0,
                                    "add the closing ')' after the parameters",
                                    "preproc.define");
                if (rest[j] == '.') {  // variadic tail: ... -> __VA_ARGS__
                    m.params.push_back("__VA_ARGS__");
                    while (j < rest.size() && rest[j] != ')') j++;
                    if (j < rest.size()) j++;
                    break;
                }
                if (!ident_start(rest[j]))
                    throw CwizError("bad macro parameter", line, 0,
                                    "parameter names use letters, digits and '_'",
                                    "preproc.define");
                size_t k = j;
                while (k < rest.size() && ident_char(rest[k])) k++;
                m.params.push_back(rest.substr(j, k - j));
                j = k;
                while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) j++;
                if (j < rest.size() && rest[j] == ',') { j++; continue; }
                if (j < rest.size() && rest[j] == ')') { j++; break; }
                throw CwizError("expected ',' or ')' in macro parameters", line, 0,
                                "separate parameters with ',' and close with ')'",
                                "preproc.define");
            }
            i = j;
        }
        m.body = strip(rest.substr(i));
        macros[name] = m;
    }

    // `defined X` / `defined(X)` -> 1 / 0, before macro expansion (which would
    // otherwise replace the operand).
    std::string subst_defined(const std::string& s) {
        std::string res;
        size_t i = 0;
        while (i < s.size()) {
            if (ident_start(s[i])) {
                size_t k = i;
                while (k < s.size() && ident_char(s[k])) k++;
                std::string id = s.substr(i, k - i);
                if (id == "defined") {
                    size_t j = k;
                    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
                    bool paren = j < s.size() && s[j] == '(';
                    if (paren) j++;
                    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
                    size_t a = j;
                    while (j < s.size() && ident_char(s[j])) j++;
                    std::string nm = s.substr(a, j - a);
                    if (paren) {
                        while (j < s.size() && s[j] != ')') j++;
                        if (j < s.size()) j++;
                    }
                    res += macros.count(nm) ? " 1 " : " 0 ";
                    i = j;
                    continue;
                }
                res += id;
                i = k;
                continue;
            }
            res += s[i++];
        }
        return res;
    }

    long long eval_if(const std::string& expr, int line) {
        std::vector<IfTok> toks;
        std::string e0 = expr;
        size_t cmt = e0.find("//");
        if (cmt != std::string::npos) e0 = e0.substr(0, cmt);
        std::string expanded = expand(subst_defined(e0), 0);
        const std::string& s = expanded;
        size_t i = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t') { i++; continue; }
            if (ident_start(c)) {
                size_t k = i;
                while (k < s.size() && ident_char(s[k])) k++;
                std::string id = s.substr(i, k - i);
                if (id == "defined") {
                    size_t j = k;
                    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
                    bool paren = j < s.size() && s[j] == '(';
                    if (paren) j++;
                    while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
                    size_t a = j;
                    while (j < s.size() && ident_char(s[j])) j++;
                    std::string nm = s.substr(a, j - a);
                    if (paren) {
                        while (j < s.size() && s[j] != ')') j++;
                        if (j < s.size()) j++;
                    }
                    toks.push_back(tok_num(macros.count(nm) ? 1 : 0));
                    i = j;
                    continue;
                }
                if (id == "true") { toks.push_back(tok_num(1)); i = k; continue; }
                if (id == "false") { toks.push_back(tok_num(0)); i = k; continue; }
                toks.push_back(tok_id(id));
                i = k;
                continue;
            }
            if (std::isdigit((unsigned char)c)) {
                size_t k = i;
                if (s.size() > i + 1 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                    k = i + 2;
                    while (k < s.size() && std::isxdigit((unsigned char)s[k])) k++;
                } else {
                    while (k < s.size() && std::isdigit((unsigned char)s[k])) k++;
                }
                std::string num = s.substr(i, k - i);
                toks.push_back(tok_num(std::strtoll(num.c_str(), nullptr, 0)));
                i = k;
                continue;
            }
            if (c == '\'') {
                size_t k = i + 1;
                long long v = 0;
                if (k < s.size() && s[k] == '\\') {
                    k++;
                    char e = k < s.size() ? s[k++] : 0;
                    v = (e == 'n') ? 10 : (e == 't') ? 9 : (e == '0') ? 0 : e;
                } else if (k < s.size()) {
                    v = (unsigned char)s[k++];
                }
                while (k < s.size() && s[k] != '\'') k++;
                if (k < s.size()) k++;
                toks.push_back(tok_num(v));
                i = k;
                continue;
            }
            static const char* ops2[] = {"<<", ">>", "<=", ">=", "==", "!=", "&&", "||", nullptr};
            bool matched = false;
            for (int o = 0; ops2[o]; o++) {
                if (s.compare(i, 2, ops2[o]) == 0) {
                    toks.push_back(tok_op(ops2[o]));
                    i += 2;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            if (std::string("+-*/%&|^~!<>()?:").find(c) != std::string::npos) {
                toks.push_back(tok_op(std::string(1, c)));
                i++;
                continue;
            }
            throw CwizError(std::string("bad character in #if: '") + c + "'", line);
        }
        IfEval ev(std::move(toks), macros, line);
        return ev.run();
    }

    // ---- macro expansion ------------------------------------------------
    std::string expand(const std::string& text, int depth) {
        if (depth > 32) return text;
        std::string res;
        size_t i = 0;
        while (i < text.size()) {
            char c = text[i];
            if (in_block) {
                if (c == '*' && i + 1 < text.size() && text[i + 1] == '/') {
                    res += "*/";
                    i += 2;
                    in_block = false;
                    continue;
                }
                res += c;
                i++;
                continue;
            }
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
                in_block = true;
                res += "/*";
                i += 2;
                continue;
            }
            if (c == '"' || c == '\'') {
                char q = c;
                res += c;
                i++;
                while (i < text.size()) {
                    if (text[i] == '\\' && i + 1 < text.size()) {
                        res += text[i];
                        res += text[i + 1];
                        i += 2;
                        continue;
                    }
                    res += text[i];
                    if (text[i] == q) { i++; break; }
                    i++;
                }
                continue;
            }
            if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
                res += text.substr(i);  // a comment: copy verbatim
                break;
            }
            if (ident_start(c)) {
                size_t k = i;
                while (k < text.size() && ident_char(text[k])) k++;
                std::string id = text.substr(i, k - i);
                auto it = macros.find(id);
                if (it == macros.end()) {
                    res += id;
                    i = k;
                    continue;
                }
                const Macro& m = it->second;
                if (m.func) {
                    size_t j = k;
                    while (j < text.size() && (text[j] == ' ' || text[j] == '\t')) j++;
                    if (j >= text.size() || text[j] != '(') {
                        res += id;
                        i = k;
                        continue;
                    }
                    std::vector<std::string> args;
                    std::string cur;
                    int depthp = 1;
                    size_t q = j + 1;
                    bool in_str = false;
                    char sq = 0;
                    bool closed = false;
                    for (; q < text.size(); q++) {
                        char d = text[q];
                        if (in_str) {
                            cur += d;
                            if (d == '\\' && q + 1 < text.size()) {
                                cur += text[++q];
                                continue;
                            }
                            if (d == sq) in_str = false;
                            continue;
                        }
                        if (d == '"' || d == '\'') {
                            in_str = true;
                            sq = d;
                            cur += d;
                            continue;
                        }
                        if (d == '(') { depthp++; cur += d; continue; }
                        if (d == ')') {
                            depthp--;
                            if (depthp == 0) { closed = true; break; }
                            cur += d;
                            continue;
                        }
                        if (d == ',' && depthp == 1) { args.push_back(cur); cur.clear(); continue; }
                        cur += d;
                    }
                    if (!closed) {
                        res += id;
                        i = k;
                        continue;
                    }
                    args.push_back(cur);
                    if (args.size() == 1 && strip(args[0]).empty() && m.params.empty()) args.clear();
                    std::string sub = m.body;
                    for (size_t pi = 0; pi < m.params.size(); pi++) {
                        std::string val = pi < args.size() ? args[pi] : std::string();
                        if (m.params[pi] == "__VA_ARGS__") {
                            val.clear();
                            for (size_t z = pi; z < args.size(); z++) {
                                if (z > pi) val += ", ";
                                val += args[z];
                            }
                        }
                        sub = replace_ident(sub, m.params[pi], val);
                    }
                    res += expand(sub, depth + 1);
                    i = q + 1;
                    continue;
                }
                res += expand(m.body, depth + 1);
                i = k;
                continue;
            }
            res += c;
            i++;
        }
        return res;
    }

    static std::string replace_ident(const std::string& text, const std::string& name,
                                     const std::string& val) {
        std::string res;
        size_t i = 0;
        while (i < text.size()) {
            char c = text[i];
            if (c == '"' || c == '\'') {
                char q = c;
                res += c;
                i++;
                while (i < text.size()) {
                    if (text[i] == '\\' && i + 1 < text.size()) {
                        res += text[i];
                        res += text[i + 1];
                        i += 2;
                        continue;
                    }
                    res += text[i];
                    if (text[i] == q) { i++; break; }
                    i++;
                }
                continue;
            }
            if (ident_start(c)) {
                size_t k = i;
                while (k < text.size() && ident_char(text[k])) k++;
                std::string id = text.substr(i, k - i);
                res += (id == name) ? val : id;
                i = k;
                continue;
            }
            res += c;
            i++;
        }
        return res;
    }
};

}  // namespace

CwizPreprocessInfo preprocess_cwiz_info(const std::string& src, const std::string& file,
                                        const std::string& target) {
    origins().clear();
    Pre pre(target);
    CwizPreprocessInfo info;
    info.text = pre.run(src, file);
    info.has_cwapp = pre.has_cwapp();
    info.slot = pre.slot();
    info.binary_path = pre.binary_path();
    return info;
}

std::string preprocess_cwiz(const std::string& src, const std::string& file,
                            const std::string& target) {
    return preprocess_cwiz_info(src, file, target).text;
}

CwizOrigin cwiz_line_origin(int line) {
    if (line >= 1 && line <= (int)origins().size()) return origins()[(size_t)(line - 1)];
    return CwizOrigin{};
}

CwizPos cwiz_error_pos(const std::string& fallback_file, int line) {
    CwizOrigin o = cwiz_line_origin(line);
    if (o.file.empty()) return CwizPos{fallback_file, line};
    return CwizPos{o.file, o.line};
}

}  // namespace wiz
