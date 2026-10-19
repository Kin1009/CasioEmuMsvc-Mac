// wiz.cpp - the Wiz language: lexer, parser and code generator.
#include "wiz.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "font6x8.hpp"
#include "font8x8.hpp"

namespace wiz {

namespace {  // internal linkage: the Wiz AST / lexer / parser / code generator

// ------------------------------------------------------------------ key tables
static const std::map<std::string, int> KEY_CONSTS = {
    {"K_NONE", -1},
    {"K_0", 0x0B}, {"K_1", 0x3F}, {"K_2", 0x37}, {"K_3", 0x2F}, {"K_4", 0x3E},
    {"K_5", 0x36}, {"K_6", 0x2E}, {"K_7", 0x3D}, {"K_8", 0x35}, {"K_9", 0x2D},
    {"K_UP", 0x20}, {"K_DOWN", 0x22}, {"K_LEFT", 0x29}, {"K_RIGHT", 0x19}, {"K_OK", 0x21},
    {"K_HOME", 0x30}, {"K_BACK", 0x31}, {"K_SETTINGS", 0x39}, {"K_SHIFT", 0x3A},
    {"K_VAR", 0x32}, {"K_FUNC", 0x2A}, {"K_CATALOG", 0x1A}, {"K_TOOLS", 0x12},
    {"K_PGUP", 0x10}, {"K_PGDOWN", 0x11},
    {"K_X", 0x3B}, {"K_FRAC", 0x33}, {"K_SQRT", 0x2B}, {"K_POWER", 0x23}, {"K_SQUARED", 0x1B},
    {"K_LOGAB", 0x13}, {"K_ANS", 0x3C}, {"K_SIN", 0x34}, {"K_COS", 0x2C}, {"K_TAN", 0x24},
    {"K_LPAREN", 0x1C}, {"K_RPAREN", 0x14},
    {"K_DEL", 0x25}, {"K_AC", 0x1D}, {"K_MUL", 0x26}, {"K_DIV", 0x1E},
    {"K_PLUS", 0x27}, {"K_MINUS", 0x1F},
    {"K_DOT", 0x0C}, {"K_SCI", 0x0D}, {"K_FORMAT", 0x0E}, {"K_EXE", 0x0F},
};

static const std::map<std::string, int> CW_KEY_CONSTS = {
    {"K_NONE", -1},
    {"K_SHIFT", 56}, {"K_ALPHA", 48}, {"K_UP", 40}, {"K_DOWN", 33}, {"K_LEFT", 41}, {"K_RIGHT", 32},
    {"K_MENU", 24}, {"K_OPTN", 57}, {"K_CALC", 49}, {"K_INTEGRAL", 25}, {"K_X", 17},
    {"K_FRAC", 58}, {"K_SQRT", 50}, {"K_SQUARED", 42}, {"K_POWER", 34}, {"K_LOG", 26}, {"K_LN", 18},
    {"K_NEG", 59}, {"K_DEG", 51}, {"K_INV", 43}, {"K_SIN", 35}, {"K_COS", 27}, {"K_TAN", 19},
    {"K_STO", 60}, {"K_ENG", 52}, {"K_LPAREN", 44}, {"K_RPAREN", 36}, {"K_SD", 28}, {"K_MPLUS", 20},
    {"K_7", 61}, {"K_8", 53}, {"K_9", 45}, {"K_DEL", 37}, {"K_AC", 29},
    {"K_4", 62}, {"K_5", 54}, {"K_6", 46}, {"K_MUL", 38}, {"K_DIV", 30},
    {"K_1", 63}, {"K_2", 55}, {"K_3", 47}, {"K_PLUS", 39}, {"K_MINUS", 31},
    {"K_0", 11}, {"K_DOT", 12}, {"K_EXP", 13}, {"K_ANS", 14}, {"K_EXE", 15},
};

}  // namespace

const std::map<std::string, int>& key_consts(const std::string& target_name) {
    return (target_name == "cw" || target_name == "cwapp") ? CW_KEY_CONSTS : KEY_CONSTS;
}

namespace {

// ------------------------------------------------------------------ builtins
static const std::map<std::string, std::pair<int, int>> BUILTINS = {
    {"pixel", {2, 3}}, {"clear", {0, 0}}, {"key", {0, 0}}, {"delay", {1, 1}},
    {"peek", {1, 1}}, {"poke", {2, 2}}, {"_hspan", {4, 4}},
    {"key_down", {1, 1}}, {"key_hit", {1, 1}}, {"buffered", {1, 1}}, {"flip", {0, 0}},
    {"_rpeek", {1, 1}}, {"exit", {0, 0}},
};

static const std::set<std::string> KEYWORDS = {
    "def", "if", "elif", "else", "while", "for", "in", "range", "return", "break",
    "continue", "pass", "global", "and", "or", "not", "True", "False", "const", "rom", "rom8"};

// ------------------------------------------------------------------ prelude
static const std::vector<std::pair<std::string, std::string>> PRELUDE = {
    {"math_abs", "\ndef abs(n):\n    if n < 0:\n        return 0 - n\n    return n\n"},
    {"math_min", "\ndef min(a, b):\n    if a < b:\n        return a\n    return b\n"},
    {"math_max", "\ndef max(a, b):\n    if a > b:\n        return a\n    return b\n"},
    {"math_clamp",
     "\ndef clamp(v, lo, hi):\n    if v < lo:\n        return lo\n    if v > hi:\n        return hi\n"
     "    return v\n"},
    {"math_sign",
     "\ndef sign(n):\n    if n < 0:\n        return -1\n    if n > 0:\n        return 1\n    return 0\n"},
    {"rand",
     "\n_seed = 12345\ndef srand(s):\n    global _seed\n    _seed = s\ndef rand(n):\n"
     "    global _seed\n    _seed = _seed * 25173 + 13849\n    return ((_seed >> 1) & 0x7FFF) % n\n"},
    {"wait_key",
     "\ndef wait_key():\n    while key() != -1:\n        pass\n    while True:\n        k = key()\n"
     "        if k != -1:\n            return k\n"},
    {"hline",
     "\ndef hline(x0, x1, y, c=1):\n    if y < 0 or y > @LAST@:\n        return 0\n    if x0 < 0:\n"
     "        x0 = 0\n    if x1 > 192:\n        x1 = 192\n    if x0 >= x1:\n        return 0\n"
     "    _hspan(x0, x1, y, c != 0)\n"},
    {"vline",
     "\ndef vline(x, y0, y1, c=1):\n    if x < 0 or x > 191:\n        return 0\n    if y0 < 0:\n"
     "        y0 = 0\n    if y1 > @ROWS@:\n        y1 = @ROWS@\n    for y in range(y0, y1):\n"
     "        pixel(x, y, c)\n"},
    {"rect",
     "\ndef rect(x, y, w, h, c=1):\n    hline(x, x + w, y, c)\n    hline(x, x + w, y + h - 1, c)\n"
     "    vline(x, y, y + h, c)\n    vline(x + w - 1, y, y + h, c)\n"},
    {"fillrect",
     "\ndef fillrect(x, y, w, h, c=1):\n    for j in range(y, y + h):\n        hline(x, x + w, j, c)\n"},
    {"line",
     "\ndef line(x0, y0, x1, y1, c=1):\n    dx = abs(x1 - x0)\n    dy = 0 - abs(y1 - y0)\n    sx = 1\n"
     "    if x0 > x1:\n        sx = -1\n    sy = 1\n    if y0 > y1:\n        sy = -1\n    err = dx + dy\n"
     "    while True:\n        pixel(x0, y0, c)\n        if x0 == x1 and y0 == y1:\n            break\n"
     "        e2 = err + err\n        if e2 >= dy:\n            err += dy\n            x0 += sx\n"
     "        if e2 <= dx:\n            err += dx\n            y0 += sy\n"},
    {"circle",
     "\ndef circle(cx, cy, r, c=1):\n    x = r\n    y = 0\n    err = 1 - r\n    while x >= y:\n"
     "        pixel(cx + x, cy + y, c)\n        pixel(cx + y, cy + x, c)\n        pixel(cx - y, cy + x, c)\n"
     "        pixel(cx - x, cy + y, c)\n        pixel(cx - x, cy - y, c)\n        pixel(cx - y, cy - x, c)\n"
     "        pixel(cx + y, cy - x, c)\n        pixel(cx + x, cy - y, c)\n        y += 1\n        if err < 0:\n"
     "            err += y + y + 1\n        else:\n            x -= 1\n            err += (y - x) * 2 + 1\n"},
    {"disc",
     "\ndef disc(cx, cy, r, c=1):\n    r2 = r * r\n    for dy in range(0 - r, r + 1):\n"
     "        for dx in range(0 - r, r + 1):\n            if dx * dx + dy * dy <= r2:\n"
     "                pixel(cx + dx, cy + dy, c)\n"},
    {"font", ""},  // filled in lazily (see prelude_font()) — CW Mono 6x8 (FONT6X8)
    {"font_large", ""},  // CW Mono 7 px advance / 8 px tall (FONT_LARGE)
    {"font_mini", ""},  // CW Text proportional / 8 px tall (FONT_MINI + FONT_MINI_ADV)
    {"char",
     "\ndef char(x, y, c):\n    if c < 32 or c > 126:\n        c = 63\n    base = (c - 32) * 8\n"
     "    for r in range(0, 8):\n        bits = _font[base + r]\n        for k in range(0, 6):\n"
     "            pixel(x + k, y + r, (bits & 128) != 0)\n            bits = bits + bits\n"},
    {"text",
     "\ndef text(x, y, s):\n    c = _rpeek(s)\n    while c != 0:\n        char(x, y, c)\n        x += 6\n"
     "        s += 1\n        c = _rpeek(s)\n    return x\n"},
    {"num",
     "\ndef num(x, y, n):\n    if n == -32768:\n        return text(x, y, \"-32768\")\n    if n < 0:\n"
     "        char(x, y, 45)\n        x += 6\n        n = 0 - n\n    d = 10000\n    shown = 0\n"
     "    while d > 0:\n        q = n / d\n        n = n - q * d\n        if q > 0 or shown or d == 1:\n"
     "            char(x, y, 48 + q)\n            x += 6\n            shown = 1\n        d = d / 10\n    return x\n"},
    {"sprite",
     "\ndef sprite(x, y, w, h, d):\n    bw = (w + 7) >> 3\n    for r in range(0, h):\n"
     "        for k in range(0, w):\n            b = _rpeek(d + r * bw + (k >> 3))\n"
     "            if b & (128 >> (k & 7)):\n                pixel(x + k, y + r)\n"},
    {"print_large_fonts",
     "\ndef _lchar(x, y, c):\n    if c < 32 or c > 126: c = 63\n    base=(c-32)*8\n    for r in range(0,8):\n        bits=_font_large[base+r]\n        for k in range(0,7):\n            pixel(x+k,y+r,(bits&(128>>k))!=0)\n"
     "\ndef _ltext(x, y, s):\n    c=_rpeek(s)\n    while c!=0:\n        _lchar(x,y,c)\n        x+=7\n        s+=1\n        c=_rpeek(s)\n    return x\n"
     "\ndef _lnum(x, y, n):\n    if n==-32768: return _ltext(x,y,\"-32768\")\n"
     "    if n<0: _lchar(x,y,45); x+=7; n=0-n\n"
     "    d=10000; shown=0\n    while d>0:\n        q=n/d; n=n-q*d\n"
     "        if q>0 or shown or d==1: _lchar(x,y,48+q); x+=7; shown=1\n"
     "        d=d/10\n    return x\n"},
    {"print_mini_fonts",
     "\ndef _mchar(x, y, c):\n    if c < 32 or c > 126: c = 63\n    base=(c-32)*8\n    w=_adv_mini[c-32]\n    for r in range(0,8):\n        bits=_font_mini[base+r]\n        for k in range(0,w):\n            pixel(x+k,y+r,(bits&(128>>k))!=0)\n"
     "\ndef _mtext(x, y, s):\n    c=_rpeek(s)\n    while c!=0:\n        x=x+_mchar(x,y,c)\n        s+=1\n        c=_rpeek(s)\n    return x\n"
     "\ndef _mnum(x, y, n):\n    if n==-32768: return _mtext(x,y,\"-32768\")\n"
     "    if n<0: x=x+_mchar(x,y,45); n=0-n\n"
     "    d=10000; shown=0\n    while d>0:\n        q=n/d; n=n-q*d\n"
     "        if q>0 or shown or d==1: x=x+_mchar(x,y,48+q); shown=1\n"
     "        d=d/10\n    return x\n"},
};

static std::string prelude_array(const char* name, const unsigned char* data, int len) {
    std::string s = std::string("rom8 ") + name + " = [";
    for (int i = 0; i < len; i++) {
        if (i) s += ", ";
        s += std::to_string(data[i]);
    }
    s += "]\n";
    return s;
}

static std::string prelude_font() {
    return prelude_array("_font", FONT6X8, FONT6X8_LEN);
}

static std::string prelude_font_mini() {
    return prelude_array("_font_mini", FONT_MINI, FONT8X8_LEN) +
           prelude_array("_adv_mini", FONT_MINI_ADV, FONT_ADV_LEN);
}

static std::string prelude_font_large() {
    return prelude_array("_font_large", FONT_LARGE, FONT8X8_LEN);
}

// ------------------------------------------------------------------ helpers
long long wrap16(long long v) {
    v &= 0xFFFF;
    return (v & 0x8000) ? v - 0x10000 : v;
}

static std::optional<long long> fold(const std::string& op, long long a, long long b) {
    if (op == "+") return wrap16(a + b);
    if (op == "-") return wrap16(a - b);
    if (op == "*") return wrap16(a * b);
    if (op == "&") return wrap16(a & b);
    if (op == "|") return wrap16(a | b);
    if (op == "^") return wrap16(a ^ b);
    if (op == "<<" && b >= 0 && b <= 15) return wrap16(a << b);
    if (op == ">>" && b >= 0 && b <= 15) return a >> b;
    if (op == "/" || op == "%") {
        if (b == 0) return 0;
        long long q = std::llabs(a) / std::llabs(b);
        if ((a < 0) != (b < 0)) q = -q;
        return wrap16(op == "/" ? q : a - q * b);
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ AST
struct Node;
using NodeP = std::shared_ptr<Node>;

struct Node {
    std::string k;
    int line = 0;
    std::string name;   // def/rom/arr/var/index/call/assign/aassign/for-var; op for bin/un/cmp
    std::string kind;   // rom kind
    long long num = 0;  // num literal / for step
    NodeP a, b;
    std::vector<NodeP> args;
    std::vector<NodeP> body;
    std::vector<std::string> params;
    std::vector<std::string> names;  // 'global' statement
    std::vector<long long> defaults;
    std::vector<std::pair<NodeP, std::vector<NodeP>>> arms;  // if-arms
    std::vector<NodeP> els;
    std::vector<int> data;   // rom/arr values
    std::vector<int> bytes;  // str
};

static NodeP mk(const std::string& k, int line = 0) {
    auto n = std::make_shared<Node>();
    n->k = k;
    n->line = line;
    return n;
}

// visit every Node child of a node
template <typename F>
static void each_child(const NodeP& n, F f) {
    if (n->a) f(n->a);
    if (n->b) f(n->b);
    for (auto& x : n->args) f(x);
    for (auto& x : n->body) f(x);
    for (auto& x : n->els) f(x);
    for (auto& arm : n->arms) {
        f(arm.first);
        for (auto& x : arm.second) f(x);
    }
}

// ------------------------------------------------------------------ lexer
struct Tok {
    std::string kind;
    std::string s;
    long long num = 0;
    std::vector<int> bytes;
    int line = 0;
};

static std::string strip_comment(const std::string& raw) {
    bool inq = false;
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (inq) {
            if (c == '\\') i++;
            else if (c == '"') inq = false;
        } else if (c == '"') {
            inq = true;
        } else if (c == '#') {
            return raw.substr(0, i);
        }
    }
    return raw;
}

static const std::map<char, int> ESC = {{'n', 10}, {'t', 9}, {'0', 0}, {'\\', 92}, {'"', 34}};

static std::vector<int> decode_str(const std::string& lit, int ln) {
    std::vector<int> out;
    for (size_t i = 1; i + 1 < lit.size(); i++) {
        char c = lit[i];
        if (c == '\\') {
            i++;
            char e = lit[i];
            auto it = ESC.find(e);
            if (it == ESC.end()) throw WizError(fmt("unknown escape \\%c in string", e), ln);
            out.push_back(it->second);
        } else {
            int o = (unsigned char)c;
            if (o > 126 || o < 32)
                throw WizError("only printable ASCII is allowed in strings", ln);
            out.push_back(o);
        }
    }
    return out;
}

static bool match_op(const std::string& line, size_t pos, std::string& op) {
    static const char* three[] = {"<<=", ">>="};
    static const char* two[] = {"==", "!=", "<=", ">=", "<<", ">>", "//", "+=",
                                "-=", "*=", "/=", "%=", "&=", "|=", "^="};
    static const std::string one = "-+*/%&|^~<>=():,[]";
    for (auto* o : three)
        if (line.compare(pos, 3, o) == 0) { op = o; return true; }
    for (auto* o : two)
        if (line.compare(pos, 2, o) == 0) { op = o; return true; }
    if (pos < line.size() && one.find(line[pos]) != std::string::npos) {
        op = std::string(1, line[pos]);
        return true;
    }
    return false;
}

static std::vector<Tok> lex(const std::string& src) {
    std::vector<Tok> toks;
    std::vector<int> indents = {0};
    std::string s = replace_all(src, "\r", "");
    auto lines = split(s, '\n');
    for (size_t li = 0; li < lines.size(); li++) {
        int ln = (int)li + 1;
        std::string line = rstrip(strip_comment(lines[li]));
        if (strip(line).empty()) continue;
        size_t lead = 0;
        while (lead < line.size() && line[lead] == ' ') lead++;
        if (line.substr(0, lead).find('\t') != std::string::npos)
            throw WizError("tabs are not allowed for indentation, use spaces", ln);
        int ind = (int)(line.size() - lstrip(line, " ").size());
        if (ind > indents.back()) {
            indents.push_back(ind);
            toks.push_back({"INDENT", "", 0, {}, ln});
        }
        while (ind < indents.back()) {
            indents.pop_back();
            toks.push_back({"DEDENT", "", 0, {}, ln});
            if (ind > indents.back()) throw WizError("inconsistent indentation", ln);
        }
        size_t pos = (size_t)ind;
        while (pos < line.size()) {
            char c = line[pos];
            if (c == ' ') { pos++; continue; }
            if (std::isdigit((unsigned char)c)) {
                size_t j = pos;
                long long v;
                if (c == '0' && pos + 1 < line.size() && (line[pos + 1] == 'x' || line[pos + 1] == 'X') &&
                    pos + 2 < line.size() && std::isxdigit((unsigned char)line[pos + 2])) {
                    j = pos + 2;
                    while (j < line.size() && std::isxdigit((unsigned char)line[j])) j++;
                    v = (long long)std::strtoull(line.substr(pos, j - pos).c_str(), nullptr, 16);
                } else {
                    j = pos;
                    while (j < line.size() && std::isdigit((unsigned char)line[j])) j++;
                    v = (long long)std::strtoll(line.substr(pos, j - pos).c_str(), nullptr, 10);
                }
                toks.push_back({"num", "", v, {}, ln});
                pos = j;
                continue;
            }
            if (std::isalpha((unsigned char)c) || c == '_') {
                size_t j = pos + 1;
                while (j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '_')) j++;
                std::string name = line.substr(pos, j - pos);
                toks.push_back({KEYWORDS.count(name) ? "kw" : "name", name, 0, {}, ln});
                pos = j;
                continue;
            }
            if (c == '"') {
                size_t j = pos + 1;
                while (j < line.size() && line[j] != '"') {
                    if (line[j] == '\\') j++;
                    j++;
                }
                if (j >= line.size()) throw WizError("unterminated string", ln);
                std::string lit = line.substr(pos, j - pos + 1);
                toks.push_back({"str", "", 0, decode_str(lit, ln), ln});
                pos = j + 1;
                continue;
            }
            std::string op;
            if (match_op(line, pos, op)) {
                toks.push_back({"op", op, 0, {}, ln});
                pos += op.size();
                continue;
            }
            throw WizError(fmt("unexpected character '%c'", c), ln);
        }
        toks.push_back({"NEWLINE", "", 0, {}, ln});
    }
    while (indents.size() > 1) {
        indents.pop_back();
        toks.push_back({"DEDENT", "", 0, {}, 0});
    }
    toks.push_back({"EOF", "", 0, {}, 0});
    return toks;
}

// ------------------------------------------------------------------ parser
class Parser {
public:
    Parser(std::vector<Tok> toks, std::map<std::string, long long>* consts)
        : t(std::move(toks)), consts(consts) {
        for (auto& kv : *consts) builtin.insert(kv.first);
    }

    std::vector<NodeP> program() {
        std::vector<NodeP> body;
        while (peek().kind != "EOF") body.push_back(stmt(true));
        return body;
    }

private:
    std::vector<Tok> t;
    size_t i = 0;
    std::map<std::string, long long>* consts;
    std::set<std::string> overridden;
    std::set<std::string> builtin;

    const Tok& peek() const { return t[i]; }
    const Tok& next() { return t[i++]; }
    bool is_op(const std::string& s) const { return peek().kind == "op" && peek().s == s; }
    bool is_kw(const std::string& s) const { return peek().kind == "kw" && peek().s == s; }
    void expect_op(const std::string& s) {
        const Tok& tk = next();
        if (tk.kind != "op" || tk.s != s) throw WizError(fmt("expected '%s'", s.c_str()), tk.line);
    }
    Tok expect_kind(const std::string& k) {
        Tok tk = next();
        if (tk.kind != k) throw WizError("expected " + to_lower(k), tk.line);
        return tk;
    }

    std::vector<NodeP> block() {
        expect_op(":");
        if (peek().kind != "NEWLINE") {
            NodeP s = simple_stmt();
            expect_kind("NEWLINE");
            return {s};
        }
        next();
        expect_kind("INDENT");
        std::vector<NodeP> out;
        while (peek().kind != "DEDENT") out.push_back(stmt(false));
        next();
        return out;
    }

    NodeP stmt(bool top) {
        const Tok& tk = peek();
        int ln = tk.line;
        if (tk.kind == "kw") {
            const std::string k = tk.s;
            if (k == "def") {
                if (!top) throw WizError("def only allowed at top level", ln);
                next();
                std::string name = expect_kind("name").s;
                expect_op("(");
                NodeP n = mk("def", ln);
                n->name = name;
                while (!is_op(")")) {
                    Tok pn = expect_kind("name");
                    if (consts->count(pn.s))
                        throw WizError(fmt("'%s' is a constant, cannot be a parameter", pn.s.c_str()),
                                       pn.line);
                    n->params.push_back(pn.s);
                    if (is_op("=")) {
                        next();
                        NodeP dv = expr();
                        if (dv->k != "num") throw WizError("default value must be a constant", ln);
                        n->defaults.push_back(dv->num);
                    } else if (!n->defaults.empty()) {
                        throw WizError("parameter without default after one with default", ln);
                    }
                    if (is_op(",")) next();
                }
                expect_op(")");
                n->body = block();
                return n;
            }
            if (k == "rom" || k == "rom8") {
                if (!top) throw WizError(k + " only allowed at top level", ln);
                next();
                Tok dn = expect_kind("name");
                expect_op("=");
                NodeP n = mk("rom", ln);
                n->name = dn.s;
                n->kind = k;
                n->data = data_list(k, ln);
                expect_kind("NEWLINE");
                return n;
            }
            if (k == "const") {
                if (!top) throw WizError("const only allowed at top level", ln);
                next();
                Tok cn = expect_kind("name");
                expect_op("=");
                NodeP cv = expr();
                expect_kind("NEWLINE");
                if (cv->k != "num")
                    throw WizError("const value must be a constant expression", ln);
                if (consts->count(cn.s) && (!builtin.count(cn.s) || overridden.count(cn.s)))
                    throw WizError(fmt("constant '%s' already defined", cn.s.c_str()), ln);
                overridden.insert(cn.s);
                (*consts)[cn.s] = cv->num;
                return mk("pass", ln);
            }
            if (k == "if") {
                next();
                NodeP n = mk("if", ln);
                NodeP c = expr();
                n->arms.push_back({c, block()});
                while (is_kw("elif")) {
                    next();
                    NodeP c2 = expr();
                    n->arms.push_back({c2, block()});
                }
                if (is_kw("else")) {
                    next();
                    n->els = block();
                }
                return n;
            }
            if (k == "while") {
                next();
                NodeP c = expr();
                NodeP n = mk("while", ln);
                n->a = c;
                n->body = block();
                return n;
            }
            if (k == "for") {
                next();
                std::string v = expect_kind("name").s;
                if (!is_kw("in")) throw WizError("expected 'in'", ln);
                next();
                if (!is_kw("range")) throw WizError("only `for x in range(...)` is supported", ln);
                next();
                expect_op("(");
                std::vector<NodeP> args = {expr()};
                while (is_op(",")) {
                    next();
                    args.push_back(expr());
                }
                expect_op(")");
                NodeP start, stop;
                long long step = 1;
                if (args.size() == 1) {
                    start = mk("num");
                    start->num = 0;
                    stop = args[0];
                } else if (args.size() == 2) {
                    start = args[0];
                    stop = args[1];
                } else if (args.size() == 3) {
                    start = args[0];
                    stop = args[1];
                    if (args[2]->k != "num") throw WizError("range step must be a constant", ln);
                    step = args[2]->num;
                    if (step == 0) throw WizError("range step cannot be 0", ln);
                } else {
                    throw WizError("range takes 1-3 arguments", ln);
                }
                NodeP n = mk("for", ln);
                n->name = v;
                n->a = start;
                n->b = stop;
                n->num = step;
                n->body = block();
                return n;
            }
        }
        NodeP s = simple_stmt();
        expect_kind("NEWLINE");
        return s;
    }

    std::vector<int> data_list(const std::string& kind, int ln) {
        const Tok& tk = peek();
        if (tk.kind == "str") {
            if (kind != "rom8") throw WizError("string data needs rom8", ln);
            std::vector<int> v = tk.bytes;
            next();
            v.push_back(0);
            return v;
        }
        expect_op("[");
        std::vector<int> vals;
        while (!is_op("]")) {
            NodeP e = expr();
            if (e->k != "num") throw WizError("table entries must be constants", ln);
            vals.push_back((int)e->num);
            if (is_op(",")) next();
            else if (!is_op("]")) throw WizError("expected , or ]", ln);
        }
        next();
        if (is_op("*")) {
            next();
            NodeP cnt = expr();
            if (cnt->k != "num" || vals.size() != 1)
                throw WizError("use [value] * COUNT with a constant COUNT", ln);
            std::vector<int> rep(vals.begin(), vals.end());
            vals.clear();
            for (int i = 0; i < (int)cnt->num; i++) vals.insert(vals.end(), rep.begin(), rep.end());
        }
        if (vals.empty()) throw WizError("empty table", ln);
        if (kind == "rom8")
            for (int v : vals)
                if (v < 0 || v > 255)
                    throw WizError(fmt("rom8 entries must be 0..255, got %d", v), ln);
        return vals;
    }

    static bool has_call(const NodeP& node) {
        if (node->k == "call") return true;
        bool found = false;
        each_child(node, [&](const NodeP& c) { if (!found && has_call(c)) found = true; });
        return found;
    }

    NodeP simple_stmt() {
        const Tok& tk = peek();
        int ln = tk.line;
        if (tk.kind == "kw") {
            const std::string k = tk.s;
            if (k == "return") {
                next();
                NodeP n = mk("return", ln);
                if (peek().kind != "NEWLINE") n->a = expr();
                return n;
            }
            if (k == "break" || k == "continue" || k == "pass") {
                next();
                return mk(k, ln);
            }
            if (k == "global") {
                next();
                NodeP n = mk("global", ln);
                n->names.push_back(expect_kind("name").s);
                while (is_op(",")) {
                    next();
                    n->names.push_back(expect_kind("name").s);
                }
                return n;
            }
        }
        if (tk.kind == "name") {
            const Tok& n1 = t[i + 1];
            const Tok& n2 = t[i + 2];
            if (n1.kind == "op" && n1.s == "=" && n2.kind == "op" && n2.s == "[") {
                next();
                next();
                NodeP n = mk("arr", ln);
                n->name = tk.s;
                n->data = data_list("rom", ln);
                return n;
            }
            if (n1.kind == "op" && n1.s == "[") {
                next();
                next();
                NodeP n = mk("aassign", ln);
                n->name = tk.s;
                n->a = expr();
                expect_op("]");
                Tok op = next();
                static const std::set<std::string> OPS = {"=", "+=", "-=", "*=", "/=", "%=",
                                                          "&=", "|=", "^=", "<<=", ">>="};
                if (op.kind != "op" || !OPS.count(op.s))
                    throw WizError("expected assignment after ]", ln);
                n->b = expr();
                if (op.s != "=") {
                    if (has_call(n->a))
                        throw WizError("index with a function call cannot be used with " + op.s, ln);
                    NodeP idx = mk("index", ln);
                    idx->name = tk.s;
                    idx->a = n->a;
                    NodeP bin = mk("bin");
                    bin->name = op.s.substr(0, op.s.size() - 1);
                    bin->a = idx;
                    bin->b = n->b;
                    n->b = bin;
                }
                return n;
            }
            bool assign_op = n1.kind == "op" &&
                             (n1.s == "=" || (n1.s.size() > 1 && ends_with(n1.s, "=") && n1.s != "==" &&
                                              n1.s != "!=" && n1.s != "<=" && n1.s != ">="));
            if (consts->count(tk.s) && assign_op)
                throw WizError(fmt("cannot assign to constant '%s'", tk.s.c_str()), ln);
            if (n1.kind == "op" && n1.s == "=") {
                next();
                next();
                NodeP n = mk("assign", ln);
                n->name = tk.s;
                n->a = expr();
                return n;
            }
            if (n1.kind == "op" && ends_with(n1.s, "=") && n1.s.size() > 1 && n1.s != "==" &&
                n1.s != "!=" && n1.s != "<=" && n1.s != ">=") {
                next();
                next();
                NodeP v = mk("var", ln);
                v->name = tk.s;
                NodeP bin = mk("bin");
                bin->name = n1.s.substr(0, n1.s.size() - 1);
                bin->a = v;
                bin->b = expr();
                NodeP n = mk("assign", ln);
                n->name = tk.s;
                n->a = bin;
                return n;
            }
        }
        NodeP e = expr();
        if (e->k != "call") throw WizError("expression statement has no effect", ln);
        NodeP n = mk("expr", ln);
        n->a = e;
        return n;
    }

    NodeP expr() { return or_e(); }

    NodeP or_e() {
        NodeP l = and_e();
        while (is_kw("or")) {
            next();
            NodeP n = mk("or");
            n->a = l;
            n->b = and_e();
            l = n;
        }
        return l;
    }
    NodeP and_e() {
        NodeP l = not_e();
        while (is_kw("and")) {
            next();
            NodeP n = mk("and");
            n->a = l;
            n->b = not_e();
            l = n;
        }
        return l;
    }
    NodeP not_e() {
        if (is_kw("not")) {
            next();
            NodeP n = mk("not");
            n->a = not_e();
            return n;
        }
        return cmp_e();
    }
    static bool is_cmp(const Tok& t) {
        return t.kind == "op" &&
               (t.s == "==" || t.s == "!=" || t.s == "<" || t.s == ">" || t.s == "<=" || t.s == ">=");
    }
    NodeP cmp_e() {
        NodeP l = bin_level(0);
        const Tok& t = peek();
        if (is_cmp(t)) {
            std::string op = t.s;
            next();
            NodeP r = bin_level(0);
            const Tok& n = peek();
            if (is_cmp(n)) throw WizError("chained comparisons are not supported", n.line);
            NodeP c = mk("cmp");
            c->name = op;
            c->a = l;
            c->b = r;
            return c;
        }
        return l;
    }

    static const std::vector<std::vector<std::string>>& levels() {
        static const std::vector<std::vector<std::string>> L = {
            {"|"}, {"^"}, {"&"}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%", "//"}};
        return L;
    }
    NodeP bin_level(size_t lv) {
        if (lv == levels().size()) return unary();
        NodeP l = bin_level(lv + 1);
        while (peek().kind == "op" && contains(levels()[lv], peek().s)) {
            std::string op = next().s;
            if (op == "//") op = "/";
            NodeP r = bin_level(lv + 1);
            std::optional<long long> fv;
            if (l->k == "num" && r->k == "num") fv = fold(op, l->num, r->num);
            if (fv) {
                NodeP n = mk("num");
                n->num = *fv;
                l = n;
            } else {
                NodeP n = mk("bin");
                n->name = op;
                n->a = l;
                n->b = r;
                l = n;
            }
        }
        return l;
    }
    NodeP unary() {
        const Tok& t = peek();
        if (t.kind == "op" && (t.s == "-" || t.s == "+" || t.s == "~")) {
            std::string op = t.s;
            next();
            NodeP e = unary();
            if (op == "+") return e;
            if (e->k == "num") {
                NodeP n = mk("num");
                n->num = wrap16(op == "-" ? -e->num : ~e->num);
                return n;
            }
            NodeP n = mk("un");
            n->name = op;
            n->a = e;
            return n;
        }
        return primary();
    }
    NodeP primary() {
        Tok tk = next();
        if (tk.kind == "num") {
            NodeP n = mk("num");
            n->num = wrap16(tk.num);
            return n;
        }
        if (tk.kind == "kw" && (tk.s == "True" || tk.s == "False")) {
            NodeP n = mk("num");
            n->num = tk.s == "True" ? 1 : 0;
            return n;
        }
        if (tk.kind == "str") {
            NodeP n = mk("str", tk.line);
            n->bytes = tk.bytes;
            return n;
        }
        if (tk.kind == "name" && is_op("[")) {
            next();
            NodeP n = mk("index", tk.line);
            n->name = tk.s;
            n->a = expr();
            expect_op("]");
            return n;
        }
        if (tk.kind == "name") {
            if (is_op("(")) {
                next();
                NodeP n = mk("call", tk.line);
                n->name = tk.s;
                while (!is_op(")")) {
                    n->args.push_back(expr());
                    if (is_op(",")) next();
                    else if (!is_op(")")) throw WizError("expected , or )", tk.line);
                }
                next();
                return n;
            }
            auto it = consts->find(tk.s);
            if (it != consts->end()) {
                NodeP n = mk("num");
                n->num = it->second;
                return n;
            }
            NodeP n = mk("var", tk.line);
            n->name = tk.s;
            return n;
        }
        if (tk.kind == "op" && tk.s == "(") {
            NodeP e = expr();
            expect_op(")");
            return e;
        }
        throw WizError(fmt("unexpected token '%s'", tk.s.c_str()), tk.line);
    }
};

// ------------------------------------------------------------------ analysis
static void assigned_names(const std::vector<NodeP>& body, std::set<std::string>& acc) {
    for (auto& s : body) {
        if (s->k == "assign") acc.insert(s->name);
        else if (s->k == "for") {
            acc.insert(s->name);
            assigned_names(s->body, acc);
        } else if (s->k == "if") {
            for (auto& arm : s->arms) assigned_names(arm.second, acc);
            assigned_names(s->els, acc);
        } else if (s->k == "while") {
            assigned_names(s->body, acc);
        }
    }
}

static void calls_in_node(const NodeP& n, std::set<std::string>& acc) {
    if (n->k == "call") acc.insert(n->name);
    each_child(n, [&](const NodeP& c) { calls_in_node(c, acc); });
}
static void calls_in_list(const std::vector<NodeP>& ns, std::set<std::string>& acc) {
    for (auto& n : ns) calls_in_node(n, acc);
}

static void refs_in_node(const NodeP& n, std::set<std::string>& acc) {
    if (n->k == "call" || n->k == "var" || n->k == "index") acc.insert(n->name);
    each_child(n, [&](const NodeP& c) { refs_in_node(c, acc); });
}
static void refs_in_list(const std::vector<NodeP>& ns, std::set<std::string>& acc) {
    for (auto& n : ns) refs_in_node(n, acc);
}

// ------------------------------------------------------------------ codegen
struct ArrEntry {
    int addr;
    int size;
    std::vector<int> vals;
};
struct RomEntry {
    std::string label;
    std::string kind;
    std::vector<int> vals;
};

static const std::map<std::string, std::string> INV_JUMP = {
    {"==", "JNE"}, {"!=", "JEQ"}, {"<", "JGES"}, {">", "JLES"}, {"<=", "JGTS"}, {">=", "JLTS"}};
static const std::map<std::string, std::string> TRUE_JUMP = {
    {"==", "JEQ"}, {"!=", "JNE"}, {"<", "JLTS"}, {">", "JGTS"}, {"<=", "JLES"}, {">=", "JGES"}};

class Compiler {
public:
    explicit Compiler(const Target& t) : target(t), var_base(t.var_base), var_limit(t.var_limit) {
        next_addr = var_base;
    }

    std::string compile(const std::string& src);

    std::map<std::string, int> vars;
    std::vector<std::string> notes;

private:
    const Target& target;
    int var_base, var_limit;
    std::vector<std::string> out;
    int nlabel = 0;
    int next_addr;
    std::vector<std::pair<std::string, NodeP>> funcs;
    std::set<std::string> globals;
    std::string fn;  // empty = none
    std::set<std::string> locals;
    std::set<std::string> declared_global;
    std::vector<std::pair<std::string, std::string>> loops;  // (break, continue)
    std::vector<std::pair<std::string, ArrEntry>> arrays;
    std::vector<std::pair<std::string, RomEntry>> roms;
    std::vector<std::pair<std::vector<int>, std::string>> strings;
    std::map<std::string, std::set<std::string>> reach;

    void emit(const std::string& s) { out.push_back("    " + s); }
    void label(const std::string& name) { out.push_back(name + ":"); }
    std::string new_label() { return "L" + std::to_string(++nlabel); }

    NodeP find_func(const std::string& name) const {
        for (auto& kv : funcs)
            if (kv.first == name) return kv.second;
        return nullptr;
    }
    ArrEntry* find_arr(const std::string& n) {
        for (auto& kv : arrays)
            if (kv.first == n) return &kv.second;
        return nullptr;
    }
    RomEntry* find_rom(const std::string& n) {
        for (auto& kv : roms)
            if (kv.first == n) return &kv.second;
        return nullptr;
    }

    int alloc(const std::string& qname) {
        auto it = vars.find(qname);
        if (it == vars.end()) {
            if (next_addr + 2 > var_limit) throw WizError("out of RAM for variables");
            vars[qname] = next_addr;
            next_addr += 2;
        }
        return vars[qname];
    }

    int var_addr(const std::string& name, int ln) {
        if (!fn.empty() && locals.count(name) && !declared_global.count(name))
            return alloc(fn + "." + name);
        if (globals.count(name)) return alloc(name);
        throw WizError(fmt("undefined variable '%s'", name.c_str()), ln);
    }

    std::vector<NodeP> add_prelude(std::vector<NodeP> prog, std::map<std::string, long long>& consts);
    void rewrite_print(NodeP& n, const std::set<std::string>& data_names);

    void init_arrays();
    void declare(const NodeP& d);
    std::string str_label(const std::vector<int>& bs);
    void expand_frames();
    void emit_data();
    void func(const NodeP& d);
    void walk(const std::vector<NodeP>& body, std::vector<NodeP>& out_stmts);

    void block(const std::vector<NodeP>& body);
    void stmt(const NodeP& s);
    void ex(const NodeP& e);
    void call(const NodeP& e);
    void branch(const NodeP& e, const std::string& lbl, bool jump_if);
    void const_(int reg, long long v);
    void load_label(int reg, const std::string& lbl);
    void load(int reg, const NodeP& e);
    void operands(const NodeP& l, const NodeP& r);
    static bool is_leaf(const NodeP& e) {
        return e->k == "num" || e->k == "var" || e->k == "str";
    }
};

std::vector<NodeP> Compiler::add_prelude(std::vector<NodeP> prog,
                                         std::map<std::string, long long>& consts) {
    std::set<std::string> user_defs;
    for (auto& x : prog)
        if (x->k == "def" || x->k == "rom" || x->k == "arr") user_defs.insert(x->name);

    std::vector<std::pair<std::string, std::vector<NodeP>>> snippets;
    std::map<std::string, std::string> provider;
    for (auto& kv : PRELUDE) {
        std::string text = kv.second;
        if (kv.first == "font") text = prelude_font();
        else if (kv.first == "font_mini") text = prelude_font_mini();
        else if (kv.first == "font_large") text = prelude_font_large();
        text = replace_all(text, "@ROWS@", std::to_string(target.rows));
        text = replace_all(text, "@LAST@", std::to_string(target.rows - 1));
        if (target.rom_seg == 0) text = replace_all(text, "_rpeek(", "peek(");
        auto sp = Parser(lex(text), &consts).program();
        snippets.push_back({kv.first, sp});
        for (auto& x : sp)
            if (x->k == "def" || x->k == "rom" || x->k == "arr") provider[x->name] = kv.first;
    }

    std::set<std::string> need;
    refs_in_list(prog, need);
    std::vector<std::string> included;
    std::set<std::string> done;
    while (!need.empty()) {
        std::string n = *need.begin();
        need.erase(need.begin());
        if (done.count(n)) continue;
        done.insert(n);
        auto it = provider.find(n);
        if (it == provider.end() || user_defs.count(n) || contains(included, it->second)) continue;
        included.push_back(it->second);
        for (auto& kv : snippets)
            if (kv.first == it->second)
                for (auto& x : kv.second) refs_in_node(x, need);
    }
    std::vector<NodeP> result = prog;
    for (auto& sid : included)
        for (auto& kv : snippets)
            if (kv.first == sid)
                for (auto& x : kv.second) {
                    if ((x->k == "def" || x->k == "rom" || x->k == "arr") && user_defs.count(x->name))
                        continue;
                    result.push_back(x);
                }
    return result;
}

void Compiler::rewrite_print(NodeP& n, const std::set<std::string>& data_names) {
    if (n->a) rewrite_print(n->a, data_names);
    if (n->b) rewrite_print(n->b, data_names);
    for (auto& x : n->args) rewrite_print(x, data_names);
    for (auto& x : n->body) rewrite_print(x, data_names);
    for (auto& x : n->els) rewrite_print(x, data_names);
    for (auto& arm : n->arms) {
        rewrite_print(arm.first, data_names);
        for (auto& x : arm.second) rewrite_print(x, data_names);
    }
    if (n->k == "call" && (n->name == "print" || n->name == "printLarge" || n->name == "printMini")) {
        if (n->args.size() < 3)
            throw WizError(n->name + "(x, y, value, ...) needs at least 3 arguments", n->line);
        const bool mini = (n->name == "printMini");
        const bool large = (n->name == "printLarge");
        NodeP cur = n->args[0];
        for (size_t i = 2; i < n->args.size(); i++) {
            const NodeP& a = n->args[i];
            bool is_text = a->k == "str" || (a->k == "var" && data_names.count(a->name));
            NodeP c = mk("call", n->line);
            if (mini)
                c->name = is_text ? "_mtext" : "_mnum";
            else
                c->name = is_text ? "_ltext" : "_lnum";
            c->args = {cur, n->args[1], a};
            cur = c;
        }
        n = cur;
    }
}

void Compiler::init_arrays() {
    for (auto& kv : arrays) {
        const ArrEntry& e = kv.second;
        std::set<int> uniq(e.vals.begin(), e.vals.end());
        if (uniq.size() == 1 && e.size > 6) {
            std::string lp = new_label();
            const_(2, e.addr);
            const_(4, e.size);
            const_(0, e.vals[0]);
            label(lp);
            emit("ST ER0, [ER2]");
            emit("ADD ER2, #2");
            emit("ADD ER4, #-1");
            emit("BNE " + lp);
        } else {
            for (size_t i = 0; i < e.vals.size(); i++) {
                const_(0, e.vals[i]);
                emit(fmt("ST ER0, 0x%04X", e.addr + 2 * (int)i));
            }
        }
    }
}

void Compiler::declare(const NodeP& d) {
    if (d->k == "arr") {
        if (find_arr(d->name) || find_rom(d->name) || find_func(d->name))
            throw WizError(fmt("'%s' already defined", d->name.c_str()), d->line);
        if (next_addr + 2 * (int)d->data.size() > var_limit)
            throw WizError(fmt("out of RAM for array '%s'", d->name.c_str()), d->line);
        arrays.push_back({d->name, ArrEntry{next_addr, (int)d->data.size(), d->data}});
        next_addr += 2 * (int)d->data.size();
    } else {
        if (find_arr(d->name) || find_rom(d->name) || find_func(d->name))
            throw WizError(fmt("'%s' already defined", d->name.c_str()), d->line);
        roms.push_back({d->name, RomEntry{"__d_" + d->name, d->kind, d->data}});
    }
}

std::string Compiler::str_label(const std::vector<int>& bs) {
    for (auto& kv : strings)
        if (kv.first == bs) return kv.second;
    std::string lbl = "__s" + std::to_string(strings.size());
    strings.push_back({bs, lbl});
    return lbl;
}

void Compiler::expand_frames() {
    std::vector<std::string> res;
    for (auto& line : out) {
        std::string t = strip(line);
        if (starts_with(t, ";;SAVE ") || starts_with(t, ";;REST ")) {
            std::string f = split(t, ' ')[1];
            std::vector<std::pair<int, std::string>> fv;
            for (auto& kv : vars)
                if (starts_with(kv.first, f + ".")) fv.push_back({kv.second, kv.first});
            std::sort(fv.begin(), fv.end());
            if (starts_with(t, ";;SAVE ")) {
                for (auto& p : fv) {
                    res.push_back(fmt("    L ER2, 0x%04X", p.first));
                    res.push_back("    PUSH ER2");
                }
            } else {
                for (auto it = fv.rbegin(); it != fv.rend(); ++it) {
                    res.push_back("    POP ER2");
                    res.push_back(fmt("    ST ER2, 0x%04X", it->first));
                }
            }
        } else {
            res.push_back(line);
        }
    }
    out = res;
    for (auto& kv : funcs) {
        const std::string& f = kv.first;
        bool rec = reach[f].count(f) != 0;
        if (!rec)
            for (const auto& g : reach[f])
                if (reach.count(g) && reach[g].count(f)) { rec = true; break; }
        if (rec) {
            int n = 0;
            for (auto& v : vars)
                if (starts_with(v.first, f + ".")) n++;
            notes.push_back(fmt("recursive %s(): ~%d bytes of stack per call level, about %d levels "
                                "max (stack is ~2000 bytes)",
                                f.c_str(), 4 + 2 * n + 4, 2000 / (4 + 2 * n + 4)));
        }
    }
}

void Compiler::emit_data() {
    int total = 0;
    for (auto& kv : roms) {
        RomEntry& e = kv.second;
        if (e.kind == "rom") {
            out.push_back(e.label + ":");
            for (size_t i = 0; i < e.vals.size(); i += 8) {
                std::string s = "    DW ";
                for (size_t j = i; j < e.vals.size() && j < i + 8; j++) {
                    if (j > i) s += ", ";
                    s += std::to_string(e.vals[j]);
                }
                out.push_back(s);
            }
            total += 2 * (int)e.vals.size();
        }
    }
    for (auto& kv : roms) {
        RomEntry& e = kv.second;
        if (e.kind == "rom8") {
            out.push_back(e.label + ":");
            for (size_t i = 0; i < e.vals.size(); i += 12) {
                std::string s = "    DB ";
                for (size_t j = i; j < e.vals.size() && j < i + 12; j++) {
                    if (j > i) s += ", ";
                    s += std::to_string(e.vals[j]);
                }
                out.push_back(s);
            }
            total += (int)e.vals.size();
        }
    }
    for (auto& kv : strings) {
        out.push_back(kv.second + ":");
        std::vector<int> data = kv.first;
        data.push_back(0);
        for (size_t i = 0; i < data.size(); i += 12) {
            std::string s = "    DB ";
            for (size_t j = i; j < data.size() && j < i + 12; j++) {
                if (j > i) s += ", ";
                s += std::to_string(data[j]);
            }
            out.push_back(s);
        }
        total += (int)data.size();
    }
    if (total & 1) out.push_back("    DB 0");
}

void Compiler::func(const NodeP& d) {
    std::string name = d->name;
    fn = name;
    std::set<std::string> names(d->params.begin(), d->params.end());
    assigned_names(d->body, names);
    for (auto& n : names) {
        if (find_arr(n) || find_rom(n))
            throw WizError(
                fmt("'%s' is a table/array and cannot be used as a local variable", n.c_str()),
                d->line);
    }
    declared_global.clear();
    std::vector<NodeP> all;
    walk(d->body, all);
    for (auto& s : all) {
        if (s->k == "global") {
            for (auto& g : s->names) {
                declared_global.insert(g);
                if (!globals.count(g))
                    throw WizError(fmt("global '%s' is never assigned at top level", g.c_str()),
                                   s->line);
            }
        }
    }
    locals = names;
    for (auto& p : d->params) alloc(name + "." + p);
    label("f_" + name);
    emit("PUSH LR");
    block(d->body);
    emit("MOV ER0, #0");
    label("f_" + name + "__ret");
    emit("POP PC");
    fn.clear();
    locals.clear();
    declared_global.clear();
}

void Compiler::walk(const std::vector<NodeP>& body, std::vector<NodeP>& acc) {
    for (auto& s : body) {
        acc.push_back(s);
        if (s->k == "if") {
            for (auto& arm : s->arms) walk(arm.second, acc);
            walk(s->els, acc);
        } else if (s->k == "while") {
            walk(s->body, acc);
        } else if (s->k == "for") {
            walk(s->body, acc);
        }
    }
}

void Compiler::block(const std::vector<NodeP>& body) {
    for (auto& s : body) stmt(s);
}

void Compiler::stmt(const NodeP& s) {
    const std::string& k = s->k;
    if (k == "assign") {
        ex(s->a);
        emit(fmt("ST ER0, 0x%04X", var_addr(s->name, s->line)));
    } else if (k == "expr") {
        ex(s->a);
    } else if (k == "aassign") {
        ArrEntry* arr = find_arr(s->name);
        if (!arr)
            throw WizError(
                fmt("'%s' is not a RAM array (rom tables are read-only)", s->name.c_str()),
                s->line);
        int addr = arr->addr;
        ex(s->b);
        const NodeP& idx = s->a;
        if (idx->k == "num") {
            emit(fmt("ST ER0, 0x%04X", (addr + 2 * (int)idx->num) & 0xFFFF));
        } else {
            emit("PUSH ER0");
            ex(idx);
            emit("ADD ER0, ER0");
            const_(2, addr);
            emit("ADD ER0, ER2");
            emit("MOV ER2, ER0");
            emit("POP ER0");
            emit("ST ER0, [ER2]");
        }
    } else if (k == "pass" || k == "global") {
        // nothing
    } else if (k == "if") {
        std::string end = new_label();
        for (auto& arm : s->arms) {
            std::string nxt = new_label();
            branch(arm.first, nxt, false);
            block(arm.second);
            emit("B " + end);
            label(nxt);
        }
        block(s->els);
        label(end);
    } else if (k == "while") {
        std::string top = new_label(), end = new_label();
        label(top);
        branch(s->a, end, false);
        loops.push_back({end, top});
        block(s->body);
        loops.pop_back();
        emit("B " + top);
        label(end);
    } else if (k == "for") {
        int addr = var_addr(s->name, s->line);
        int endv = alloc((fn.empty() ? std::string() : fn) + ".__end" + std::to_string(nlabel));
        std::string top = new_label(), cont = new_label(), end = new_label();
        ex(s->a);
        emit(fmt("ST ER0, 0x%04X", addr));
        ex(s->b);
        emit(fmt("ST ER0, 0x%04X", endv));
        label(top);
        emit(fmt("L ER0, 0x%04X", addr));
        emit(fmt("L ER2, 0x%04X", endv));
        emit("CMP R0, R2");
        emit("CMPC R1, R3");
        emit(std::string(s->num > 0 ? "JGES " : "JLES ") + end);
        loops.push_back({end, cont});
        block(s->body);
        loops.pop_back();
        label(cont);
        emit(fmt("L ER0, 0x%04X", addr));
        if (s->num >= -64 && s->num <= 63)
            emit(fmt("ADD ER0, #%d", (int)s->num));
        else {
            const_(2, s->num);
            emit("ADD ER0, ER2");
        }
        emit(fmt("ST ER0, 0x%04X", addr));
        emit("B " + top);
        label(end);
    } else if (k == "return") {
        if (fn.empty()) throw WizError("return outside function", s->line);
        if (!s->a) emit("MOV ER0, #0");
        else ex(s->a);
        emit("B f_" + fn + "__ret");
    } else if (k == "break" || k == "continue") {
        if (loops.empty()) throw WizError(k + " outside loop", s->line);
        emit("B " + (k == "break" ? loops.back().first : loops.back().second));
    } else {
        throw WizError("internal: unknown statement " + k);
    }
}

void Compiler::const_(int reg, long long v) {
    v = wrap16(v);
    if (v >= -64 && v <= 63) {
        emit(fmt("MOV ER%d, #%lld", reg, v));
    } else {
        emit(fmt("MOV R%d, #%lld", reg, v & 0xFF));
        emit(fmt("MOV R%d, #%lld", reg + 1, (v >> 8) & 0xFF));
    }
}

void Compiler::load_label(int reg, const std::string& lbl) {
    emit(fmt("MOV R%d, #(%s & 255)", reg, lbl.c_str()));
    emit(fmt("MOV R%d, #((%s >> 8) & 255)", reg + 1, lbl.c_str()));
}

void Compiler::load(int reg, const NodeP& e) {
    if (e->k == "num") {
        const_(reg, e->num);
    } else if (e->k == "str") {
        load_label(reg, str_label(e->bytes));
    } else {
        ArrEntry* arr = find_arr(e->name);
        RomEntry* rom = find_rom(e->name);
        if (arr) const_(reg, arr->addr);
        else if (rom) load_label(reg, rom->label);
        else emit(fmt("L ER%d, 0x%04X", reg, var_addr(e->name, e->line)));
    }
}

void Compiler::operands(const NodeP& l, const NodeP& r) {
    if (is_leaf(r)) {
        ex(l);
        load(2, r);
    } else {
        ex(l);
        emit("PUSH ER0");
        ex(r);
        emit("MOV ER2, ER0");
        emit("POP ER0");
    }
}

void Compiler::ex(const NodeP& e) {
    const std::string& k = e->k;
    if (k == "num" || k == "var" || k == "str") {
        load(0, e);
    } else if (k == "index") {
        ArrEntry* arr = find_arr(e->name);
        RomEntry* rom = find_rom(e->name);
        bool word;
        bool base_const;
        int base_addr = 0;
        std::string base_label;
        if (arr) {
            if (e->a->k == "num") {
                emit(fmt("L ER0, 0x%04X", (arr->addr + 2 * (int)e->a->num) & 0xFFFF));
                return;
            }
            word = true;
            base_const = true;
            base_addr = arr->addr;
        } else if (rom) {
            word = (rom->kind == "rom");
            base_const = false;
            base_label = rom->label;
        } else {
            throw WizError(fmt("'%s' is not an array or table", e->name.c_str()), e->line);
        }
        ex(e->a);
        if (word) emit("ADD ER0, ER0");
        if (base_const) const_(2, base_addr);
        else load_label(2, base_label);
        emit("ADD ER0, ER2");
        emit("MOV ER2, ER0");
        if (rom && target.rom_seg) emit(fmt("DSRI %d", target.rom_seg));
        if (word) {
            emit("L ER0, [ER2]");
        } else {
            emit("L R0, [ER2]");
            emit("MOV R1, #0");
        }
    } else if (k == "bin") {
        const std::string& op = e->name;
        operands(e->a, e->b);
        if (op == "+") emit("ADD ER0, ER2");
        else if (op == "-") { emit("SUB R0, R2"); emit("SUBC R1, R3"); }
        else if (op == "&" || op == "|" || op == "^") {
            std::string m = op == "&" ? "AND" : (op == "|" ? "OR" : "XOR");
            emit(m + " R0, R2");
            emit(m + " R1, R3");
        } else if (op == "*") emit("BL __mul16");
        else if (op == "/") emit("BL __div16");
        else if (op == "%") { emit("BL __div16"); emit("MOV ER0, ER2"); }
        else if (op == "<<") emit("BL __shl16");
        else if (op == ">>") emit("BL __sra16");
        else throw WizError("internal: operator " + op);
    } else if (k == "un") {
        ex(e->a);
        if (e->name == "-") {
            emit("MOV ER2, ER0");
            emit("MOV ER0, #0");
            emit("SUB R0, R2");
            emit("SUBC R1, R3");
        } else {
            emit("XOR R0, #255");
            emit("XOR R1, #255");
        }
    } else if (k == "cmp" || k == "and" || k == "or" || k == "not") {
        std::string lf = new_label(), le = new_label();
        branch(e, lf, false);
        emit("MOV ER0, #1");
        emit("B " + le);
        label(lf);
        emit("MOV ER0, #0");
        label(le);
    } else if (k == "call") {
        call(e);
    } else {
        throw WizError("internal: expr " + k);
    }
}

void Compiler::call(const NodeP& e) {
    const std::string& name = e->name;
    auto bit = BUILTINS.find(name);
    if (bit != BUILTINS.end()) {
        int lo = bit->second.first, hi = bit->second.second;
        if (!((int)e->args.size() >= lo && (int)e->args.size() <= hi))
            throw WizError(fmt("%s() takes %d-%d arguments", name.c_str(), lo, hi), e->line);
        for (auto& a : e->args) {
            ex(a);
            emit("PUSH ER0");
        }
        for (int i = (int)e->args.size() - 1; i >= 0; i--) emit(fmt("POP ER%d", 2 * i));
        if (name == "exit" && target.name != "cwapp")
            throw WizError(
                "exit() exists only for the cwapp target (a program patched into the firmware)",
                e->line);
        if (name == "pixel") {
            if (e->args.size() == 2) emit("MOV ER4, #1");
            emit("BL __pixel");
        } else {
            std::string bare = name;
            while (!bare.empty() && bare[0] == '_') bare.erase(bare.begin());
            emit("BL __" + bare);
        }
        return;
    }
    NodeP d = find_func(name);
    if (!d) throw WizError(fmt("unknown function '%s'", name.c_str()), e->line);
    int required = (int)d->params.size() - (int)d->defaults.size();
    if (!((int)e->args.size() >= required && (int)e->args.size() <= (int)d->params.size())) {
        if (!d->defaults.empty())
            throw WizError(fmt("%s() takes %d-%d arguments, got %d", name.c_str(), required,
                               (int)d->params.size(), (int)e->args.size()),
                           e->line);
        throw WizError(fmt("%s() takes %d arguments, got %d", name.c_str(), (int)d->params.size(),
                           (int)e->args.size()),
                       e->line);
    }
    std::vector<NodeP> args = e->args;
    for (size_t i = e->args.size() - required; i < d->defaults.size(); i++) {
        NodeP n = mk("num");
        n->num = d->defaults[i];
        args.push_back(n);
    }
    auto rn = reach.find(name);
    bool save = !fn.empty() && (name == fn || (rn != reach.end() && rn->second.count(fn) != 0));
    if (save) emit(";;SAVE " + fn);
    for (auto& a : args) {
        ex(a);
        emit("PUSH ER0");
    }
    for (auto it = d->params.rbegin(); it != d->params.rend(); ++it) {
        emit("POP ER0");
        emit(fmt("ST ER0, 0x%04X", alloc(name + "." + *it)));
    }
    emit("BL f_" + name);
    if (save) emit(";;REST " + fn);
}

void Compiler::branch(const NodeP& e, const std::string& lbl, bool jump_if) {
    const std::string& k = e->k;
    if (k == "cmp") {
        operands(e->a, e->b);
        emit("CMP R0, R2");
        emit("CMPC R1, R3");
        emit((jump_if ? TRUE_JUMP : INV_JUMP).at(e->name) + " " + lbl);
    } else if (k == "not") {
        branch(e->a, lbl, !jump_if);
    } else if (k == "and") {
        if (!jump_if) {
            branch(e->a, lbl, false);
            branch(e->b, lbl, false);
        } else {
            std::string skip = new_label();
            branch(e->a, skip, false);
            branch(e->b, lbl, true);
            label(skip);
        }
    } else if (k == "or") {
        if (jump_if) {
            branch(e->a, lbl, true);
            branch(e->b, lbl, true);
        } else {
            std::string skip = new_label();
            branch(e->a, skip, true);
            branch(e->b, lbl, false);
            label(skip);
        }
    } else if (k == "num") {
        if ((e->num != 0) == jump_if) emit("B " + lbl);
    } else {
        ex(e);
        emit("OR R0, R1");
        emit(std::string(jump_if ? "JNE " : "JEQ ") + lbl);
    }
}

std::string Compiler::compile(const std::string& src) {
    std::map<std::string, long long> consts;
    for (auto& kv : key_consts(target.name)) consts[kv.first] = kv.second;
    std::vector<NodeP> prog = Parser(lex(src), &consts).program();

    std::set<std::string> data_names;
    for (auto& x : prog)
        if (x->k == "rom" || x->k == "arr") data_names.insert(x->name);
    for (auto& x : prog) rewrite_print(x, data_names);
    prog = add_prelude(prog, consts);

    std::vector<NodeP> top;
    for (auto& s : prog) {
        if (s->k == "def") {
            if (find_func(s->name) || BUILTINS.count(s->name))
                throw WizError(fmt("function '%s' already defined", s->name.c_str()), s->line);
            std::set<std::string> ps(s->params.begin(), s->params.end());
            if (ps.size() != s->params.size()) throw WizError("duplicate parameter name", s->line);
            funcs.push_back({s->name, s});
        } else {
            top.push_back(s);
        }
    }
    std::vector<NodeP> decls;
    std::vector<NodeP> rest;
    for (auto& x : top) {
        if (x->k == "arr" || x->k == "rom") decls.push_back(x);
        else rest.push_back(x);
    }
    top = rest;
    for (auto& dcl : decls) declare(dcl);

    std::set<std::string> gl;
    assigned_names(top, gl);
    for (auto& g : gl) {
        if (find_arr(g) || find_rom(g))
            throw WizError(fmt("'%s' is a table/array, cannot assign to the name itself", g.c_str()));
    }
    globals = gl;

    std::map<std::string, std::set<std::string>> graph;
    for (auto& kv : funcs) {
        std::set<std::string> c;
        calls_in_list(kv.second->body, c);
        for (auto& cc : c) {
            if (!find_func(cc) && !BUILTINS.count(cc))
                throw WizError(fmt("call to unknown function '%s'", cc.c_str()), kv.second->line);
        }
        std::set<std::string> g2;
        for (auto& x : c)
            if (find_func(x)) g2.insert(x);
        graph[kv.first] = g2;
    }
    for (auto& kv : funcs) {
        std::set<std::string> seen;
        std::vector<std::string> todo(graph[kv.first].begin(), graph[kv.first].end());
        while (!todo.empty()) {
            std::string m = todo.back();
            todo.pop_back();
            if (seen.count(m)) continue;
            seen.insert(m);
            for (auto& x : graph[m]) todo.push_back(x);
        }
        reach[kv.first] = seen;
    }
    std::set<std::string> tc;
    calls_in_list(top, tc);
    for (auto& cc : tc)
        if (!find_func(cc) && !BUILTINS.count(cc))
            throw WizError(fmt("call to unknown function '%s'", cc.c_str()));
    for (auto& kv : funcs)
        if (kv.first == "main" && !kv.second->params.empty())
            throw WizError("main() takes no parameters", kv.second->line);

    label("__wiz_entry");
    emit("BL __rt_init");
    emit("BL __init");
    if (find_func("main")) emit("BL f_main");
    label("__wiz_halt");
    emit("B __wiz_halt");
    fn.clear();
    label("__init");
    emit("PUSH LR");
    init_arrays();
    block(top);
    emit("MOV ER0, #0");
    emit("POP PC");
    for (auto& kv : funcs) func(kv.second);
    expand_frames();
    emit_data();
    std::string body;
    for (auto& l : out) { body += l; body += "\n"; }
    return body;
}

// ------------------------------------------------------------------ entry
static std::vector<std::string> find_k_names(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s.compare(i, 2, "K_") == 0) {
            size_t j = i;
            while (j < s.size() &&
                   (std::isupper((unsigned char)s[j]) || std::isdigit((unsigned char)s[j]) || s[j] == '_'))
                j++;
            if (j - i > 2) out.push_back(s.substr(i, j - i));
            i = j - 1;
        }
    }
    return out;
}

static std::vector<std::string> const_k_names(const std::string& s) {
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = s.find("const", p)) != std::string::npos) {
        size_t j = p + 5;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) j++;
        if (s.compare(j, 2, "K_") == 0) {
            size_t k = j;
            while (k < s.size() &&
                   (std::isupper((unsigned char)s[k]) || std::isdigit((unsigned char)s[k]) || s[k] == '_'))
                k++;
            out.push_back(s.substr(j, k - j));
        }
        p += 5;
    }
    return out;
}

static std::vector<std::string> g_notes;

}  // namespace

const std::vector<std::string>& last_notes() { return g_notes; }

WizResult compile_to_asm(const std::string& src, const Target& target) {
    Compiler c(target);
    const auto& table = key_consts(target.name);
    std::set<std::string> defined;
    for (auto& n : const_k_names(src)) defined.insert(n);
    auto lines = split(src, '\n');
    for (size_t li = 0; li < lines.size(); li++) {
        std::string line = lines[li];
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        for (auto& name : find_k_names(line)) {
            if (!table.count(name) && !defined.count(name)) {
                std::vector<std::string> keys;
                for (auto& kv : table)
                    if (kv.first != "K_NONE") keys.push_back(kv.first.substr(2));
                std::sort(keys.begin(), keys.end());
                throw WizError(fmt("%s is not a key on %s (keys of %s: %s)", name.c_str(),
                                   target.name.c_str(), target.name.c_str(), join(keys, " ").c_str()),
                               (int)li + 1);
            }
        }
    }
    WizResult r;
    r.asm_body = c.compile(src);
    r.variables = c.vars;
    g_notes = c.notes;
    return r;
}

}  // namespace wiz
