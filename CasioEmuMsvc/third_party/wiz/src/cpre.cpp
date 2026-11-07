// cpre.cpp - the CWiz preprocessor (see cpre.hpp).
#include "cpre.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common.hpp"
#include "cwiz.hpp"
#include "directives.hpp"
#include "targets.hpp"
#include "vn_f4.hpp"

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

// The freestanding headers supported by this target.  They are compiled into
// the binary: the toolchain has no host include path and no libc.
std::string builtin_header(const std::string& name) {
    static const std::map<std::string, std::string> headers = {
        {"stdbool.h",
         "#define true 1\n#define false 0\n"
         "#define __bool_true_false_are_defined 1\n"},
        {"stddef.h",
         "typedef unsigned int size_t;\ntypedef int ptrdiff_t;\n"
         "typedef int max_align_t;\n#define NULL ((void*)0)\n"},
        {"stdalign.h", "#define alignas _Alignas\n#define alignof _Alignof\n"},
        {"stdarg.h",
         "typedef int va_list;\n"
         "#define va_start(ap, last) ((ap) = __va_start())\n"
         "#define va_arg(ap, type) __va_arg(&(ap))\n"
         "#define va_end(ap) ((void)0)\n"
         "#define va_copy(dst, src) ((dst) = (src))\n"},
        {"stdnoreturn.h", "#define noreturn _Noreturn\n"},
        {"stdint.h",
         "typedef signed char int8_t;\ntypedef unsigned char uint8_t;\n"
         "typedef int int16_t;\ntypedef unsigned int uint16_t;\n"
         "typedef signed char int_least8_t;\ntypedef unsigned char uint_least8_t;\n"
         "typedef int int_least16_t;\ntypedef unsigned int uint_least16_t;\n"
         "typedef signed char int_fast8_t;\ntypedef unsigned char uint_fast8_t;\n"
         "typedef int int_fast16_t;\ntypedef unsigned int uint_fast16_t;\n"
         "typedef int intptr_t;\ntypedef unsigned int uintptr_t;\n"
         "typedef int intmax_t;\ntypedef unsigned int uintmax_t;\n"
         "#define INT8_MIN (-128)\n#define INT8_MAX 127\n#define UINT8_MAX 255\n"
         "#define INT16_MIN (-32768)\n#define INT16_MAX 32767\n#define UINT16_MAX 65535\n"
         "#define INT8_C(v) v\n#define INT16_C(v) v\n#define UINT8_C(v) v\n#define UINT16_C(v) v\n"},
        {"limits.h",
         "#define CHAR_BIT 8\n#define SCHAR_MIN (-128)\n#define SCHAR_MAX 127\n"
         "#define UCHAR_MAX 255\n#define CHAR_MIN (-128)\n#define CHAR_MAX 127\n"
         "#define SHRT_MIN (-32768)\n#define SHRT_MAX 32767\n#define USHRT_MAX 65535\n"
         "#define INT_MIN (-32768)\n#define INT_MAX 32767\n#define UINT_MAX 65535\n"
         "#define LONG_MIN (-32768)\n#define LONG_MAX 32767\n#define ULONG_MAX 65535\n"},
        {"string.h",
         "#include <stddef.h>\n"
         "/* memcpy/memset/memcmp/memccpy/strdup/strndup/strlen/strcmp/strcpy/\n"
         "   strcat/strchr are provided by the CWiz prelude. */\n"},
        {"stdckdint.h",
         "/* C23 checked arithmetic subset: ckd_add/ckd_sub are prelude\n"
         "   functions returning nonzero on overflow.  ckd_mul is not\n"
         "   available on the 16-bit model. */\n"},
    };
    auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
}

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

// ------------------------------------------------------- __normalized__
// `__normalized__ char s[] = "Tiếng Việt";` is rewritten at compile time to
// the same bytes normalizeText() would produce: ASCII unchanged and each
// Vietnamese codepoint replaced by 0xF4,<column>.  The marker applies to the
// string literals on its own logical line; escape sequences are decoded first
// so both "\xE1\xBA\xBF" and "ế" work.
bool utf8_next(const std::string& s, size_t& i, uint32_t& cp) {
    if (i >= s.size()) return false;
    unsigned char c = (unsigned char)s[i++];
    if (c < 0x80) {
        cp = c;
        return true;
    }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    if (extra < 0 || i + (size_t)extra > s.size()) {
        cp = c;
        return true;
    }
    cp = c & (0x3F >> extra);
    for (int k = 0; k < extra; k++) cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return true;
}

std::string normalized_literal(const std::string& body, int line) {
    // Decode C escapes to bytes first.
    std::string bytes;
    for (size_t i = 0; i < body.size(); i++) {
        char c = body[i];
        if (c != '\\') {
            bytes += c;
            continue;
        }
        if (++i >= body.size())
            throw CwizError("unterminated escape in __normalized__ string", line);
        char e = body[i];
        switch (e) {
            case 'n': bytes += '\n'; break;
            case 't': bytes += '\t'; break;
            case 'r': bytes += '\r'; break;
            case '0': bytes += '\0'; break;
            case '\\': bytes += '\\'; break;
            case '\'': bytes += '\''; break;
            case '"': bytes += '"'; break;
            case 'a': bytes += '\a'; break;
            case 'b': bytes += '\b'; break;
            case 'f': bytes += '\f'; break;
            case 'v': bytes += '\v'; break;
            case 'x': {
                std::string hex;
                while (i + 1 < body.size() && std::isxdigit((unsigned char)body[i + 1]))
                    hex += body[++i];
                if (hex.empty())
                    throw CwizError("\\x needs at least one hex digit in __normalized__ string",
                                    line);
                bytes += (char)(std::strtoul(hex.c_str(), nullptr, 16) & 0xFF);
                break;
            }
            default: bytes += e; break;
        }
    }

    std::string out = "\"";
    size_t i = 0;
    while (i < bytes.size()) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == 0xF4 && i + 1 < bytes.size()) {
            out += "\\xf4";
            out += "\\x";
            out += fmt("%02x", (unsigned char)bytes[i + 1]);
            i += 2;
            continue;
        }
        if (c < 0x80) {
            out += "\\x";
            out += fmt("%02x", c);
            i += 1;
            continue;
        }
        uint32_t cp = 0;
        utf8_next(bytes, i, cp);
        uint8_t code = vn_f4_code(cp);
        if (code) {
            out += "\\xf4";
            out += "\\x";
            out += fmt("%02x", code);
        } else {
            out += "\\x3f";
        }
    }
    out += "\"";
    return out;
}

std::string apply_normalized(const std::string& line, int line_no) {
    size_t marker = std::string::npos;
    bool in_str = false, in_chr = false;
    for (size_t i = 0; i + 14 <= line.size(); i++) {
        char c = line[i];
        if (in_str || in_chr) {
            if (c == '\\') { i++; continue; }
            if (in_str && c == '"') in_str = false;
            if (in_chr && c == '\'') in_chr = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '\'') { in_chr = true; continue; }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') break;
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') break;
        if (line.compare(i, 14, "__normalized__") == 0) {
            bool prev_ok = i == 0 || !ident_char(line[i - 1]);
            bool next_ok = i + 14 >= line.size() || !ident_char(line[i + 14]);
            if (prev_ok && next_ok) {
                marker = i;
                break;
            }
        }
    }
    if (marker == std::string::npos) return line;

    std::string before = line.substr(0, marker);
    if (!strip(before).empty())
        throw CwizError("__normalized__ must be the first token on the declaration",
                        line_no, 0,
                        "write '__normalized__ rom8 char name[N] = \"...\";'",
                        "preproc.attribute-placement");
    std::string work = line.substr(0, marker) + line.substr(marker + 14);
    const std::string head = strip(work);
    const bool char_array = head.rfind("rom8 char ", 0) == 0 || head.rfind("char ", 0) == 0;
    if (!char_array || head.find('[') == std::string::npos ||
        head.find('=') == std::string::npos || head.find('"') == std::string::npos)
        throw CwizError("__normalized__ applies only to a char/rom8 char array initialized from a string",
                        line_no, 0,
                        "write '__normalized__ rom8 char name[N] = \"...\";'",
                        "preproc.attribute-placement");
    std::string out;
    in_str = false;
    in_chr = false;
    for (size_t i = 0; i < work.size(); i++) {
        char c = work[i];
        if (in_str || in_chr) {
            if (c == '\\' && i + 1 < work.size()) {
                out += c;
                out += work[++i];
                continue;
            }
            out += c;
            if (in_str && c == '"') in_str = false;
            if (in_chr && c == '\'') in_chr = false;
            continue;
        }
        if (c == '/' && i + 1 < work.size() && (work[i + 1] == '/' || work[i + 1] == '*')) {
            out += work.substr(i);
            break;
        }
        if (c == '"') {
            size_t j = i + 1;
            std::string body;
            while (j < work.size() && work[j] != '"') {
                if (work[j] == '\\' && j + 1 < work.size()) body += work[j++];
                body += work[j++];
            }
            out += normalized_literal(body, line_no);
            i = j;
            continue;
        }
        if (c == '\'') {
            in_chr = true;
            out += c;
            continue;
        }
        out += c;
    }
    return out;
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

// Split a directive argument list on whitespace, honouring double quotes and
// backslash escapes inside them.  Used by `#cwapp <slot> <text> <icon> <icon>`.
static std::vector<std::string> split_quoted_args(const std::string& text, std::string& err) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace((unsigned char)text[i])) i++;
        if (i >= text.size()) break;
        std::string token;
        if (text[i] == '"') {
            i++;
            bool closed = false;
            while (i < text.size()) {
                if (text[i] == '\\' && i + 1 < text.size()) {
                    token += text[i + 1];
                    i += 2;
                    continue;
                }
                if (text[i] == '"') {
                    i++;
                    closed = true;
                    break;
                }
                token += text[i++];
            }
            if (!closed) {
                err = "unterminated quoted argument";
                return {};
            }
        } else {
            while (i < text.size() && !std::isspace((unsigned char)text[i])) token += text[i++];
        }
        out.push_back(token);
    }
    return out;
}

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
        preserve_.clear();
        slot_ = 2;
        slot_seen_ = false;
        has_org_ = false;
        org_ = 0;
        app_extra_ = false;
        app_slot_ = 0;
        app_text_.clear();
        app_icon_.clear();
        app_icon_sel_.clear();
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
    bool has_org() const { return has_org_; }
    unsigned long org() const { return org_; }
    std::string binary_path() const { return binary_path_; }
    bool cwapp_extra() const { return app_extra_; }
    int cwapp_slot() const { return app_slot_; }
    std::string cwapp_text() const { return app_text_; }
    std::string cwapp_icon() const { return app_icon_; }
    std::string cwapp_icon_selected() const { return app_icon_sel_; }
    const std::vector<PreserveRange>& preserve() const { return preserve_; }

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
    std::vector<PreserveRange> preserve_;
    int slot_ = 2;
    bool slot_seen_ = false;
    bool has_org_ = false;
    unsigned long org_ = 0;
    std::string binary_path_ = "";
    bool app_extra_ = false;
    int app_slot_ = 0;
    std::string app_text_, app_icon_, app_icon_sel_;
    int file_line = 0;
    int line_offset_ = 0;          // #line: reported = physical + offset
    std::string logical_file_;     // #line "file" (empty = physical file)

    bool active() const { return active_stack.back().active; }

    void push_out(const std::string& text, const std::string& file, int line) {
        out.push_back(text);
        origins().push_back(
            CwizOrigin{logical_file_.empty() ? file : logical_file_, line + line_offset_});
    }

    void emit_lines(const std::string& src, const std::string& file) {
        const int save_offset = line_offset_;
        const std::string save_file = logical_file_;
        line_offset_ = 0;
        logical_file_.clear();
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
                push_out(apply_normalized(expand(joined, 0), (int)i + 1), file, (int)i + 1);
            } else {
                push_out("", file, (int)i + 1);
            }
            for (int k = 0; k < consumed; k++) push_out("", file, (int)i + 1);
        }
        line_offset_ = save_offset;
        logical_file_ = save_file;
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
        if (name == "elif" || name == "elifdef" || name == "elifndef") {
            if (active_stack.size() <= 1)
                throw CwizError("#" + name + " without #if", line, 0,
                                "add an #if/#ifdef before this #elif",
                                "preproc.unbalanced-conditional");
            Frame& f = active_stack.back();
            if (f.seen_else)
                throw CwizError("#" + name + " after #else", line, 0,
                                "move the #elif branch before #else",
                                "preproc.unbalanced-conditional");
            if (!f.parent || f.taken) {
                f.active = false;
            } else if (name == "elifdef") {
                f.active = macros.count(rest) != 0;
                f.taken = f.active;
            } else if (name == "elifndef") {
                f.active = macros.count(rest) == 0;
                f.taken = f.active;
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
            throw CwizError(rest.empty() ? "#error" : rest, line, 0,
                            "fix the condition that produced this #error, or remove it",
                            "preproc.error");
        }
        if (name == "include") {
            push_out("", file, line);
            const std::string rs = strip(rest);
            if (!rs.empty() && rs[0] == '<') {
                std::string hn, err;
                if (!parse_directive_string(rest, hn, err, true))
                    throw CwizError("#include: " + err, line, 0,
                                    "write #include <stdint.h>", "preproc.directive-argument");
                std::string hs = builtin_header(hn);
                if (hs.empty())
                    throw CwizError("#include <" + hn + ">: no system include path", line, 0,
                                    "only the freestanding headers are available; quote a project file",
                                    "preproc.include-not-found");
                include_stack.push_back("<" + hn + ">");
                emit_lines(hs, "<" + hn + ">");
                include_stack.pop_back();
                return;
            }
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
            std::vector<std::string> wanted;
            for (const std::string& part : split(rest, ',')) {
                std::string p = strip(part);
                if (p.empty()) continue;
                if (to_lower(p) == "all") { wanted.push_back("all"); continue; }
                std::string canon;
                if (!canonical_target_name(p, canon))
                    throw CwizError("#for: unknown target '" + p + "'", line, 0,
                                    "use '#for 580' or '#for 880'",
                                    "preproc.directive-argument");
                wanted.push_back(canon);
            }
            if (wanted.empty())
                throw CwizError("#for needs a target", line, 0,
                                "write '#for 580' or '#for 880, 580'",
                                "preproc.directive-argument");
            prev_target_for = join(wanted, ", ");
            has_cwapp_ = false;
            push_out("for " + prev_target_for, file, line);
            return;
        }
        if (name == "cwapp") {
            // `#cwapp` only valid after `#for 580` (the app flavour of the 580).
            if (prev_target_for.find("580") == std::string::npos) {
                throw CwizError("#cwapp requires #for 580 before it (found: #for " +
                                    prev_target_for + ")",
                                line, 0, "add '#for 580' before '#cwapp'",
                                "preproc.directive-order");
            }
            has_cwapp_ = true;
            preserve_.clear();
            slot_seen_ = false;
            app_extra_ = false;
            app_slot_ = 0;
            app_text_.clear();
            app_icon_.clear();
            app_icon_sel_.clear();
            const std::string args = strip(rest);
            if (!args.empty()) {
                std::string terr;
                std::vector<std::string> toks = split_quoted_args(args, terr);
                if (!terr.empty())
                    throw CwizError("#cwapp: " + terr, line, 0,
                                    "write '#cwapp 1 \"Name\" icon.png selected.png'",
                                    "preproc.directive-argument");
                if (toks.size() < 3)
                    throw CwizError(
                        "#cwapp with arguments needs a slot, a text and one or two icon paths",
                        line, 0,
                        "write '#cwapp 1 \"Name\" icon.png [selected.png]' (or use a bare "
                        "'#cwapp' with '#slot n')",
                        "preproc.directive-argument");
                unsigned long n = 0;
                std::string nerr;
                if (!parse_directive_number(toks[0], n, nerr) || n == 0 || n > 0x7FFFFFFFul)
                    throw CwizError("#cwapp: the first argument must be a positive app slot",
                                    line, 0,
                                    "write '#cwapp 1 \"Name\" icon.png selected.png'",
                                    "preproc.directive-argument");
                app_extra_ = true;
                app_slot_ = (int)n;
                if (toks.size() == 3) {
                    app_text_ = toks[1];
                    app_icon_ = toks[2];
                    app_icon_sel_.clear();
                } else {
                    std::vector<std::string> text_parts(toks.begin() + 1, toks.end() - 2);
                    app_text_ = join(text_parts, " ");
                    app_icon_ = toks[toks.size() - 2];
                    app_icon_sel_ = toks.back();
                }
                if (app_text_.empty())
                    throw CwizError("#cwapp: the mode text is empty", line, 0,
                                    "put the menu name between the slot and the icon paths",
                                    "preproc.directive-argument");
            }
            push_out("for cwapp", file, line);
            return;
        }
        if (name == "org") {
            // #org is the ROP/firmware origin directive.  A normal CWiz image
            // starts at 0 and a #cwapp image is placed by its patch slot, so
            // here the value is metadata rather than an assembler ORG.
            if (!has_cwapp_)
                throw CwizError("#org is only meaningful in a #cwapp or #rop source",
                                line, 0, "add '#cwapp' after '#for 580', or remove '#org'",
                                "preproc.directive-context");
            unsigned long val = 0;
            std::string err;
            bool bare = false;
            if (!parse_directive_number(rest, val, err, true, &bare))
                throw CwizError("#org: " + err, line, 0,
                                "write '#org 0x25000'", "preproc.directive-argument");
            org_ = val;
            has_org_ = true;
            push_out("", file, line);
            return;
        }
        if (name == "binary") {
            std::string bpath, err;
            if (!parse_directive_string(rest, bpath, err))
                throw CwizError("#binary: " + err, line, 0,
                                "write #binary \"firmware.bin\"", "preproc.directive-argument");
            binary_path_ = bpath;
            // emit nothing; build_cwiz injects DB directly for fwnx
            push_out("", file, line);
            return;
        }
        if (name == "line") {
            std::string rest_s = strip(rest);
            size_t sp = rest_s.find_first_of(" \t");
            std::string num = sp == std::string::npos ? rest_s : rest_s.substr(0, sp);
            std::string filepart = sp == std::string::npos ? std::string() : strip(rest_s.substr(sp));
            unsigned long n = 0;
            std::string err;
            if (!parse_directive_number(num, n, err) || n == 0)
                throw CwizError("#line: " + (err.empty() ? "expected a positive line number" : err),
                                line, 0, "write '#line 100' or '#line 100 \"file.cwiz\"'",
                                "preproc.directive-argument");
            std::string logical;
            if (!filepart.empty()) {
                if (!parse_directive_string(filepart, logical, err))
                    throw CwizError("#line: " + err, line, 0,
                                    "write '#line 100 \"file.cwiz\"'",
                                    "preproc.directive-argument");
            }
            push_out("", file, line);
            line_offset_ = (int)n - (line + 1);
            logical_file_ = logical;
            return;
        }
        if (name == "embed") {
            std::string path, err;
            if (!parse_directive_string(rest, path, err))
                throw CwizError("#embed: " + err, line, 0,
                                "write '#embed \"data.bin\"'", "preproc.directive-argument");
            std::string resolved = path;
            if (!path.empty() && path[0] != '/') {
                std::string cand = dir_name(file) + path;
                bool ok = false;
                read_binary_file(cand, &ok);
                if (ok) resolved = cand;
            }
            bool ok = false;
            std::vector<uint8_t> bytes = read_binary_file(resolved, &ok);
            if (!ok)
                throw CwizError("#embed: file not found '" + path + "'", line, 0,
                                "place the file next to the source, or use an absolute path",
                                "preproc.directive-argument");
            std::string list;
            for (size_t k = 0; k < bytes.size(); k++) {
                if (k) list += ", ";
                list += std::to_string((int)bytes[k]);
            }
            push_out(list, file, line);
            return;
        }
        if (name == "preserve") {
            if (!has_cwapp_)
                throw CwizError("#preserve is only available after #cwapp", line, 0,
                                "move '#preserve <addr> <len>' below the '#cwapp' line",
                                "preproc.directive-order");
            std::string terr;
            std::vector<std::string> toks = split_quoted_args(rest, terr);
            if (!terr.empty() || toks.size() != 2)
                throw CwizError("#preserve needs exactly an address and a length", line, 0,
                                "write '#preserve 0xD111 2' (hex or decimal)",
                                "preproc.directive-argument");
            unsigned long addr = 0, len = 0;
            std::string err;
            if (!parse_directive_number(toks[0], addr, err) || addr > 0xFFFFul)
                throw CwizError("#preserve: bad address '" + toks[0] + "'",
                                line, 0, "use a 16-bit address such as 0xD111",
                                "preproc.directive-argument");
            if (!parse_directive_number(toks[1], len, err) || len == 0 || len > 0xFFFFul)
                throw CwizError("#preserve: bad length '" + toks[1] + "'",
                                line, 0, "use a positive length such as 2",
                                "preproc.directive-argument");
            preserve_.push_back(PreserveRange{line, addr, len});
            return;
        }
        if (name == "slot") {
            // `#slot n` - choose the fx-580VN X menu slot (1..12) that the
            // compiler patches.  Only meaningful in a legacy #cwapp section.
            if (!has_cwapp_)
                throw CwizError("#slot is only available after #cwapp", line, 0,
                                "move '#slot n' below the '#cwapp' line",
                                "preproc.directive-order");
            if (app_extra_)
                throw CwizError("#slot is not used with '#cwapp <slot> <text> <image> [<selected>]'",
                                line, 0,
                                "the slot is already the first argument of this #cwapp line; remove #slot",
                                "preproc.directive-conflict");
            if (slot_seen_)
                throw CwizError("#slot is already set for this #cwapp section", line, 0,
                                "keep a single '#slot n' per #cwapp section",
                                "preproc.directive-duplicate");
            unsigned long n = 0;
            std::string err;
            if (!parse_directive_number(rest, n, err) || n < 1 || n > 12)
                throw CwizError("#slot needs a slot number from 1 to 12" +
                                    (err.empty() ? std::string() : " (" + err + ")"),
                                line, 0, "write '#slot 2'", "preproc.directive-argument");
            slot_ = (int)n;
            slot_seen_ = true;
            push_out("", file, line);
            return;
        }
        {
            const DirectiveInfo* info = find_directive(name);
            if (info && info->context == DirectiveContext::Rop)
                throw CwizError("#" + name + " is only available in a #rop source", line, 0,
                                "ROP directives require '#rop' at the top of the file",
                                "preproc.directive-context");
            throw CwizError("unknown directive '#" + name + "'", line, 0,
                            directive_suggestion(name, DirectiveContext::Preprocess),
                            "preproc.unknown-directive");
        }
    }

    static std::string canon_file(const std::string& p) {
        std::string r = p;
        while (r.size() >= 2 && r.compare(0, 2, "./") == 0) r = r.substr(2);
        return r;
    }

    std::string include_path(const std::string& rest, const std::string& file, int line) {
        std::string name, err;
        if (!parse_directive_string(rest, name, err))
            throw CwizError("#include: " + err, line, 0,
                            "write #include \"file.cwiz\"", "preproc.directive-argument");
        if (name.empty())
            throw CwizError("#include needs \"file\"", line, 0,
                            "write #include \"file.cwiz\"", "preproc.directive-argument");
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
                            "preproc.directive-argument");
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
                                    "preproc.directive-argument");
                if (rest[j] == '.') {  // variadic tail: ... -> __VA_ARGS__
                    m.params.push_back("__VA_ARGS__");
                    while (j < rest.size() && rest[j] != ')') j++;
                    if (j < rest.size()) j++;
                    break;
                }
                if (!ident_start(rest[j]))
                    throw CwizError("bad macro parameter", line, 0,
                                    "parameter names use letters, digits and '_'",
                                    "preproc.directive-argument");
                size_t k = j;
                while (k < rest.size() && ident_char(rest[k])) k++;
                m.params.push_back(rest.substr(j, k - j));
                j = k;
                while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) j++;
                if (j < rest.size() && rest[j] == ',') { j++; continue; }
                if (j < rest.size() && rest[j] == ')') { j++; break; }
                throw CwizError("expected ',' or ')' in macro parameters", line, 0,
                                "separate parameters with ',' and close with ')'",
                                "preproc.directive-argument");
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

    std::string subst_has(const std::string& s) {
        std::string res;
        size_t i = 0;
        auto read_balanced = [&](size_t open, std::string& inner, size_t& end) {
            int depth = 0;
            size_t k = open;
            for (; k < s.size(); k++) {
                if (s[k] == '(') depth++;
                else if (s[k] == ')') {
                    depth--;
                    if (depth == 0) { inner = s.substr(open + 1, k - open - 1); end = k; return true; }
                }
            }
            return false;
        };
        while (i < s.size()) {
            if (s.compare(i, 13, "__has_include") == 0) {
                size_t p = i + 13;
                while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
                std::string inner;
                size_t end = p;
                if (p < s.size() && s[p] == '(' && read_balanced(p, inner, end)) {
                    std::string name, err;
                    bool found = false;
                    if (parse_directive_string(inner, name, err, true) && !name.empty()) {
                        std::string cand = name;
                        if (name[0] != '/') {
                            std::string c = dir_name(root_file) + name;
                            bool ok = false;
                            read_text_file(c, &ok);
                            if (ok) cand = c;
                        }
                        bool ok = false;
                        read_text_file(cand, &ok);
                        found = ok;
                    }
                    res += found ? "1" : "0";
                    i = end + 1;
                    continue;
                }
            }
            if (s.compare(i, 17, "__has_c_attribute") == 0) {
                size_t p = i + 17;
                while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
                std::string inner;
                size_t end = p;
                if (p < s.size() && s[p] == '(' && read_balanced(p, inner, end)) {
                    std::string n = strip(inner);
                    n.erase(std::remove_if(n.begin(), n.end(), [](char c) {
                                return c == '[' || c == ']' || c == ' ' || c == '\t';
                            }), n.end());
                    static const std::set<std::string> known = {
                        "nodiscard", "maybe_unused", "deprecated", "fallthrough", "noreturn",
                        "rom", "rom8", "normalized", "at"};
                    res += known.count(n) ? "1" : "0";
                    i = end + 1;
                    continue;
                }
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
        std::string expanded = expand(subst_defined(subst_has(e0)), 0);
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
                    std::string va;
                    int va_index = -1;
                    for (size_t pi = 0; pi < m.params.size(); pi++)
                        if (m.params[pi] == "__VA_ARGS__") va_index = (int)pi;
                    if (va_index >= 0)
                        for (size_t z = (size_t)va_index; z < args.size(); z++) {
                            if (z > (size_t)va_index) va += ", ";
                            va += args[z];
                        }
                    std::string sub = m.body;
                    // C23 __VA_OPT__(content): keep content only when the
                    // variadic tail has at least one token.
                    for (;;) {
                        size_t vp = sub.find("__VA_OPT__");
                        if (vp == std::string::npos) break;
                        size_t p = vp + 10;
                        while (p < sub.size() && (sub[p] == ' ' || sub[p] == '\t')) p++;
                        if (p >= sub.size() || sub[p] != '(') { sub.erase(vp, 10); continue; }
                        int dep = 0;
                        size_t e = p;
                        for (; e < sub.size(); e++) {
                            if (sub[e] == '(') dep++;
                            else if (sub[e] == ')') {
                                dep--;
                                if (dep == 0) break;
                            }
                        }
                        if (e >= sub.size()) { sub.erase(vp, 10); continue; }
                        std::string inner = sub.substr(p + 1, e - p - 1);
                        sub.replace(vp, e - vp + 1, strip(va).empty() ? "" : inner);
                    }
                    // Stringizing: #param uses the unexpanded argument.
                    {
                        std::string out2;
                        bool in_s = false;
                        for (size_t z = 0; z < sub.size(); z++) {
                            char ch = sub[z];
                            if (in_s) {
                                out2 += ch;
                                if (ch == '\\' && z + 1 < sub.size()) { out2 += sub[++z]; continue; }
                                if (ch == '"') in_s = false;
                                continue;
                            }
                            if (ch == '"') { in_s = true; out2 += ch; continue; }
                            if (ch == '#' && (z == 0 || sub[z - 1] != '#') &&
                                (z + 1 >= sub.size() || sub[z + 1] != '#')) {
                                size_t j2 = z + 1;
                                while (j2 < sub.size() && (sub[j2] == ' ' || sub[j2] == '\t')) j2++;
                                size_t k2 = j2;
                                while (k2 < sub.size() && ident_char(sub[k2])) k2++;
                                std::string nm = sub.substr(j2, k2 - j2);
                                int idx = -1;
                                for (size_t pi = 0; pi < m.params.size(); pi++)
                                    if (m.params[pi] == nm) idx = (int)pi;
                                if (idx >= 0) {
                                    std::string val = idx < (int)args.size() ? args[(size_t)idx]
                                                                             : std::string();
                                    std::string quoted = "\"";
                                    for (char vc : strip(val)) {
                                        if (vc == '"' || vc == '\\') quoted += '\\';
                                        quoted += vc;
                                    }
                                    quoted += "\"";
                                    out2 += quoted;
                                    z = k2 - 1;
                                    continue;
                                }
                            }
                            out2 += ch;
                        }
                        sub = out2;
                    }
                    for (size_t pi = 0; pi < m.params.size(); pi++) {
                        std::string val = pi < args.size() ? args[pi] : std::string();
                        if (m.params[pi] == "__VA_ARGS__") val = va;
                        sub = replace_ident(sub, m.params[pi], val);
                    }
                    // Token pasting: remove ## and the whitespace around it.
                    for (;;) {
                        size_t pp = sub.find("##");
                        if (pp == std::string::npos) break;
                        size_t a2 = pp;
                        while (a2 > 0 && (sub[a2 - 1] == ' ' || sub[a2 - 1] == '\t')) a2--;
                        size_t b2 = pp + 2;
                        while (b2 < sub.size() && (sub[b2] == ' ' || sub[b2] == '\t')) b2++;
                        sub = sub.substr(0, a2) + sub.substr(b2);
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
    info.has_org = pre.has_org();
    info.org = pre.org();
    info.binary_path = pre.binary_path();
    info.cwapp_extra = pre.cwapp_extra();
    info.cwapp_slot = pre.cwapp_slot();
    info.cwapp_text = pre.cwapp_text();
    info.cwapp_icon = pre.cwapp_icon();
    info.cwapp_icon_selected = pre.cwapp_icon_selected();
    info.preserve = pre.preserve();
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
