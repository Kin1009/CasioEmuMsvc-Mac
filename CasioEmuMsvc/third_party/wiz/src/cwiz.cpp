// cwiz.cpp - the CWiz compiler: lexer, parser, semantic analyser, code generator.
#include "cwiz.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>

#include "font6x8.hpp"
#include "font_cw.hpp"
#include "font_cw_vn.hpp"
#include "vn_f4.hpp"
#include "wiz.hpp"  // key_consts()

namespace wiz {

namespace {  // internal linkage: the CWiz lexer / AST / parser / sema / code generator

static long long wrap16(long long v) {
    v &= 0xFFFF;
    return (v & 0x8000) ? v - 0x10000 : v;
}

// =================================================================== lexer
static const std::set<std::string> CW_KEYWORDS = {
    "int", "char", "void", "bool", "const", "static", "volatile", "register",
    "extern", "inline", "signed", "unsigned", "short", "long", "rom", "rom8",
    "struct", "union", "enum", "typedef",
    "restrict", "_Atomic", "_Thread_local", "_Noreturn", "noreturn",
    "constexpr", "typeof", "typeof_unqual", "auto", "char8_t",
    "_Alignas", "alignas", "_Alignof", "alignof",
    "_Static_assert", "static_assert", "_Generic", "nullptr", "_BitInt",
    "if", "else", "while", "do", "for", "return", "break", "continue", "true",
    "false", "switch", "case", "default", "goto", "sizeof"};

// multi-character operators, longest first
static const std::vector<std::string> OP_TABLE = {
    "[[", "]]", "...", "<<=", ">>=", "->", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "==",  "!=",  "<=", ">=", "<<", ">>", "&&", "||", "+",  "-",  "*",  "/",
    "%",   "&",   "|",  "^",  "~",  "!",  "=",  "<",  ">",  "?",  ":",  ";",
    ",",   ".",   "(",  ")",  "{",  "}",  "[",  "]"};

struct Token {
    std::string kind;  // num, str, char, ident, kw, op, eof
    long long num = 0;
    std::vector<uint32_t> cps;  // for 'str'
    std::string sval;           // for ident/kw/op
    int line = 1, col = 1;
};

static bool ident_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_';
}
static bool ident_cont(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex(char c) { return std::isxdigit((unsigned char)c) != 0; }

// Decode one UTF-8 code point at byte position i of s.
static uint32_t utf8_at(const std::string& s, size_t& i) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { i += 1; return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        uint32_t cp = ((c & 0x1Fu) << 6) | ((unsigned char)s[i + 1] & 0x3Fu);
        i += 2; return cp;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        uint32_t cp = ((c & 0x0Fu) << 12) | (((unsigned char)s[i + 1] & 0x3Fu) << 6) |
                      ((unsigned char)s[i + 2] & 0x3Fu);
        i += 3; return cp;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        uint32_t cp = ((c & 0x07u) << 18) | (((unsigned char)s[i + 1] & 0x3Fu) << 12) |
                      (((unsigned char)s[i + 2] & 0x3Fu) << 6) | ((unsigned char)s[i + 3] & 0x3Fu);
        i += 4; return cp;
    }
    i += 1;
    return c;
}

// Append `cp` as UTF-8 bytes.  String literals keep their byte-string model:
// a source character such as "ế" becomes the three UTF-8 bytes that
// normalizeText() later turns into an F4 pair, while \xNN still inserts one
// raw byte.
static void append_utf8_bytes(std::vector<uint32_t>& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(cp);
    } else if (cp < 0x800) {
        out.push_back(0xC0 | (cp >> 6));
        out.push_back(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out.push_back(0xE0 | (cp >> 12));
        out.push_back(0x80 | ((cp >> 6) & 0x3F));
        out.push_back(0x80 | (cp & 0x3F));
    } else {
        out.push_back(0xF0 | (cp >> 18));
        out.push_back(0x80 | ((cp >> 12) & 0x3F));
        out.push_back(0x80 | ((cp >> 6) & 0x3F));
        out.push_back(0x80 | (cp & 0x3F));
    }
}

static std::vector<uint32_t> decode_escapes(const std::string& text, int tok_line,
                                             bool utf8_text = false) {
    std::vector<uint32_t> out;
    size_t i = 0, n = text.size();
    while (i < n) {
        char c = text[i];
        if (c != '\\') {
            uint32_t cp = utf8_at(text, i);
            if (utf8_text && cp >= 0x80) append_utf8_bytes(out, cp);
            else out.push_back(cp);
            continue;
        }
        i++;
        if (i >= n)
            throw CwizError("unterminated escape sequence", tok_line, 0,
                            "add the escaped character after '\\'", "lex.unterminated-escape");
        char e = text[i];
        i++;
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case '0': out.push_back(0); break;
            case '\\': out.push_back('\\'); break;
            case '\'': out.push_back('\''); break;
            case '"': out.push_back('"'); break;
            case 'a': out.push_back(7); break;
            case 'b': out.push_back(8); break;
            case 'f': out.push_back(12); break;
            case 'v': out.push_back(11); break;
            case 'x': {
                std::string hexs;
                while (i < n && is_hex(text[i])) { hexs += text[i]; i++; }
                if (hexs.empty())
                    throw CwizError("\\x needs at least one hex digit", tok_line, 0,
                                    "write two hex digits, for example \\x41", "lex.bad-escape");
                out.push_back((uint32_t)(std::strtoul(hexs.c_str(), nullptr, 16) & 0xFF));
                break;
            }
            default:
                throw CwizError(fmt("unknown escape sequence \\%c", e), tok_line, 0,
                                "valid escapes are \\n \\t \\r \\0 \\\\ \\' \\\" and \\xNN",
                                "lex.bad-escape");
        }
    }
    return out;
}

static std::vector<Token> lex(const std::string& src) {
    std::vector<Token> toks;
    size_t i = 0, n = src.size();
    int line = 1, col = 1;
    auto adv = [&](size_t k = 1) { i += k; col += (int)k; };
    auto push = [&](Token t) { toks.push_back(std::move(t)); };

    while (i < n) {
        char c = src[i];
        if (c == '\n') { i++; line++; col = 1; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { adv(); continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') adv();
            continue;
        }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            adv(2);
            while (i < n && !(src[i] == '*' && i + 1 < n && src[i + 1] == '/')) {
                if (src[i] == '\n') { line++; col = 1; i++; }
                else adv();
            }
            if (i >= n)
                throw CwizError("unterminated block comment", line, col,
                                "add '*/' to close the comment", "lex.unterminated-comment");
            adv(2);
            continue;
        }
        if (is_digit(c)) {
            int sl = line, sc = col;
            std::string text;
            auto sep = [&](bool (*pred)(char)) {
                if (i < n && src[i] == '\'' && i + 1 < n && pred(src[i + 1])) {
                    adv();
                    return true;
                }
                return false;
            };
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                text += src[i]; adv();
                text += src[i]; adv();
                std::string digits;
                while (i < n && (is_hex(src[i]) || sep(is_hex))) {
                    if (src[i] != '\'') digits += src[i];
                    adv();
                }
                if (digits.empty())
                    throw CwizError("0x needs at least one hex digit", sl, sc,
                                    "write a hex value, for example 0x1F", "lex.bad-number");
                Token t; t.kind = "num";
                t.num = (long long)std::strtoull(digits.c_str(), nullptr, 16);
                t.line = sl; t.col = sc;
                push(t);
            } else if (c == '0' && i + 1 < n && (src[i + 1] == 'b' || src[i + 1] == 'B')) {
                adv(); adv();
                std::string digits;
                auto is_bin = [](char x) { return x == '0' || x == '1'; };
                while (i < n && (is_bin(src[i]) || sep(is_bin))) {
                    if (src[i] != '\'') digits += src[i];
                    adv();
                }
                if (digits.empty())
                    throw CwizError("0b needs at least one binary digit", sl, sc,
                                    "write a binary value, for example 0b1010", "lex.bad-number");
                Token t; t.kind = "num";
                t.num = (long long)std::strtoull(digits.c_str(), nullptr, 2);
                t.line = sl; t.col = sc;
                push(t);
            } else {
                std::string digits;
                auto is_dec = [](char x) { return x >= '0' && x <= '9'; };
                while (i < n && (is_dec(src[i]) || sep(is_dec))) {
                    if (src[i] != '\'') digits += src[i];
                    adv();
                }
                if (i + 1 < n && src[i] == '.' && is_digit(src[i + 1]))
                    throw CwizError("floating-point literals are not supported", sl, sc,
                                    "values are 16-bit integers; use fixed point such as 15/10",
                                    "lex.float");
                Token t; t.kind = "num";
                t.num = (long long)std::strtoll(digits.c_str(), nullptr, 10);
                t.line = sl; t.col = sc;
                push(t);
            }
            continue;
        }
        if (c == '"') {
            int sl = line, sc = col;
            adv();
            std::string body;
            while (i < n && src[i] != '"') {
                if (src[i] == '\n')
                    throw CwizError("unterminated string literal", sl, sc,
                                    "add a closing '\"' before the end of the line",
                                    "lex.unterminated-string");
                if (src[i] == '\\') {
                    body += src[i]; adv();
                    if (i < n) { body += src[i]; adv(); }
                    continue;
                }
                body += src[i];
                adv();
            }
            if (i >= n)
                throw CwizError("unterminated string literal", sl, sc,
                                "add a closing '\"' at the end of the string",
                                "lex.unterminated-string");
            adv();
            Token t; t.kind = "str"; t.cps = decode_escapes(body, sl, true); t.line = sl;
            t.col = sc;
            push(t);
            continue;
        }
        if (c == '\'') {
            int sl = line, sc = col;
            adv();
            std::string body;
            while (i < n && src[i] != '\'') {
                if (src[i] == '\\') {
                    body += src[i]; adv();
                    if (i < n) { body += src[i]; adv(); }
                    continue;
                }
                body += src[i];
                adv();
            }
            if (i >= n)
                throw CwizError("unterminated character literal", sl, sc,
                                "add a closing single quote after the character",
                                "lex.unterminated-char");
            adv();
            std::vector<uint32_t> chars = decode_escapes(body, sl);
            if (chars.size() != 1)
                throw CwizError("character literal must hold exactly one character", sl, sc,
                                "use a string literal (\"...\") for text", "lex.bad-char");
            Token t; t.kind = "char"; t.num = (long long)(chars[0] & 0xFFFF); t.line = sl; t.col = sc;
            push(t);
            continue;
        }
        if (c == 'u' && i + 2 < n && src[i + 1] == '8' &&
            (src[i + 2] == '"' || src[i + 2] == '\'')) {
            // C23 u8"..." / u8'...' - the payload is bytes in this toolchain.
            adv(2);
            continue;
        }
        if (ident_start(c)) {
            int sl = line, sc = col;
            std::string text;
            while (i < n && ident_cont(src[i])) { text += src[i]; adv(); }
            Token t;
            t.kind = CW_KEYWORDS.count(text) ? "kw" : "ident";
            t.sval = text;
            t.line = sl; t.col = sc;
            push(t);
            continue;
        }
        if (c == '.' && i + 1 < n && is_digit(src[i + 1]))
            throw CwizError("floating-point literals are not supported", line, col,
                            "values are 16-bit integers; use fixed point such as 15/10",
                            "lex.float");
        bool matched = false;
        for (const auto& op : OP_TABLE) {
            if (src.compare(i, op.size(), op) == 0) {
                Token t; t.kind = "op"; t.sval = op; t.line = line; t.col = col;
                push(t);
                adv(op.size());
                matched = true;
                break;
            }
        }
        if (matched) continue;
        {
            std::string hint;
            unsigned char uc = (unsigned char)c;
            if (uc >= 0x80)
                hint = "source files must be UTF-8/ASCII; non-ASCII characters are not identifiers";
            else if (c == '`')
                hint = "use a double quote (\") for strings";
            throw CwizError(fmt("unexpected character '%c'", c), line, col, hint,
                            "lex.unexpected-character");
        }
    }
    Token t; t.kind = "eof"; t.line = line; t.col = col;
    toks.push_back(t);
    return toks;
}

// =================================================================== AST
enum class NK {
    Num, Str, Ident, Index, Call, Unary, IncDec, Bin, Cond, Assign, Sizeof, Member, Cast,
    Alignof, Generic, Assoc, Designator, CompoundLit,
    Block, DeclStmt, DeclGroup, If, While, DoWhile, For, Switch, Case, Return, Break,
    Continue, ExprStmt, Goto, Label, StaticAssert, ConstDecl, VarDecl, ArrayDecl, Param,
    FuncDecl, Program
};

struct Node;
struct Func;
using NodeP = std::shared_ptr<Node>;

struct Sym {
    std::string kind;
    std::string name;
    std::string vtype = "int";  // declared type: "int", "char", "int*", ...
    long long value = 0;
    int size = 0;
    int elem_bytes = 2;
    std::vector<long long> dims;
    std::string scope = "global";
    std::string qname;
    int addr = 0;
    int decl_line = 0;
    int align = 1;
    Func* func = nullptr;
    int index = 0;
    // initialiser (globals/arrays): 0 = none, 1 = expression node, 2 = list, 3 = string
    int init_kind = 0;
    NodeP init_node;
    std::vector<NodeP> init_list;
    std::vector<uint32_t> init_cps;
};

struct ParamInfo {
    std::string name;
    std::string type;
    bool has_default = false;
    NodeP default_expr;
    long long default_val = 0;
    bool has_default_val = false;
};

struct Func {
    std::string name;
    std::vector<NodeP> params;  // Param nodes
    NodeP body;
    std::string ret;
    std::string label;
    std::vector<Sym*> locals;
    bool recursive = false;
    bool variadic = false;
    Sym* va_count = nullptr;
    Sym* va_array = nullptr;
};

struct Node {
    NK k;
    int line = 0, col = 0;
    long long value = 0;
    std::string sval;   // name / Str(/string) / ops
    std::string type;   // VarDecl/Param type, FuncDecl ret, ArrayDecl elem
    std::string kind;   // ArrayDecl kind, Call kind
    std::string builtin;
    NodeP a, b, c, d;
    std::vector<NodeP> list;         // stmts / args / decls / params / init list
    std::vector<uint32_t> cps;       // Str value / rom8 string init
    bool init_is_str = false;
    Sym* sym = nullptr;
    Func* func = nullptr;
    bool prefix = true;
    bool via_ptr = false;   // Index node: the base is a pointer, not an array
    std::string etype;      // static type of the expression ("int", "char", "int*", ...)
    // Numbered declarations
    NodeP size_expr;
    std::vector<NodeP> dim_exprs;  // array dimensions, outermost first
    std::vector<long long> dims;   // resolved dimensions (analyser)
    int stride = 0;                // bytes advanced by an Index at this level
    int align = 1;                 // _Alignas/alignas request
    bool has_size = false;
    long long size = 0;
    bool variadic = false;
    bool has_default = false;
    long long default_val = 0;
    bool has_default_val = false;
};

static NodeP mk(NK k, int line = 0, int col = 0) {
    auto n = std::make_shared<Node>();
    n->k = k;
    n->line = line;
    n->col = col;
    return n;
}

template <typename F>
static void each_child(const NodeP& n, F f) {
    if (n->a) f(n->a);
    if (n->b) f(n->b);
    if (n->c) f(n->c);
    if (n->d) f(n->d);
    for (auto& x : n->list) f(x);
    if (n->size_expr) f(n->size_expr);
}

static void walk(const NodeP& n, std::vector<NodeP>& out) {
    out.push_back(n);
    each_child(n, [&](const NodeP& c) { walk(c, out); });
}

static std::set<std::string> referenced_names(const NodeP& n) {
    std::set<std::string> out;
    std::vector<NodeP> all;
    walk(n, all);
    for (auto& x : all) {
        if (x->k == NK::Ident || x->k == NK::Call) out.insert(x->sval);
    }
    return out;
}

// =================================================================== parser
static const std::set<std::string> TYPES = {"int", "char", "void", "bool", "char8_t"};
// Storage classes / width words are accepted and ignored: every value is a
// 16-bit cell in this toolchain, so `unsigned int` and `int` are the same.
static const std::set<std::string> DECL_QUALIFIERS = {
    "const", "static", "volatile", "register", "extern", "inline", "signed",
    "unsigned", "short", "long", "restrict", "_Atomic", "_Thread_local",
    "_Noreturn", "noreturn"};
static const std::set<std::string> ASSIGN_OPS = {"=",  "+=",  "-=",  "*=",  "/=",  "%=",
                                                 "&=", "|=",  "^=",  "<<=", ">>="};

// -- aggregate types -------------------------------------------------------
struct StructField {
    std::string name;
    std::string type;
    int offset = 0;
    int size = 2;
    int bits = 0;        // bitfield width (0 = ordinary member)
    bool flexible = false;
    bool is_group = false;             // anonymous struct/union member
    std::vector<StructField> group;    // its flattened members
};
struct TypeDef {
    std::string kind;  // "struct" or "union"
    std::vector<StructField> fields;
    int size = 0;
    int align = 1;
    bool complete = false;
};
struct TypeTable {
    std::map<std::string, TypeDef> types;   // "struct Name" -> layout
    std::map<std::string, std::string> aliases;  // typedef name -> canonical type
    std::map<std::string, long long> enums;      // enum constant -> value
    int anon = 0;
};
static TypeTable* g_types = nullptr;
static int align_up(int v, int a) { return a <= 1 ? v : ((v + a - 1) / a) * a; }

// -- pointer types ---------------------------------------------------------
// A pointer type is a base type with one or more trailing '*' ("int*", "char**").
// Every pointer value is a 16-bit RAM address (or a ROM offset, see docs/GUIDE.md).
static bool is_ptr_type(const std::string& t) { return !t.empty() && t.back() == '*'; }
static std::string ptr_base(const std::string& t) {
    return is_ptr_type(t) ? t.substr(0, t.size() - 1) : t;
}
static std::string ptr_to(const std::string& t) { return t + "*"; }
static bool is_aggregate_type(const std::string& t) {
    return !is_ptr_type(t) &&
           (t.rfind("struct ", 0) == 0 || t.rfind("union ", 0) == 0);
}
static const TypeDef* type_def(const std::string& t) {
    if (!g_types || !is_aggregate_type(t)) return nullptr;
    auto it = g_types->types.find(t);
    return it == g_types->types.end() ? nullptr : &it->second;
}
static const StructField* find_field(const std::string& t, const std::string& name) {
    const TypeDef* d = type_def(t);
    if (!d) return nullptr;
    for (const auto& f : d->fields)
        if (f.name == name) return &f;
    return nullptr;
}
// Size in bytes of a value of type `t` when stored in memory (a pointer is 2).
static int type_bytes(const std::string& t) {
    if (is_ptr_type(t)) return 2;
    if (t == "char" || t == "char8_t") return 1;
    if (const TypeDef* d = type_def(t)) return d->size;
    return 2;
}
// How far `p + 1` moves for a pointer of type `t` (the element size).
static int ptr_step(const std::string& t) { return type_bytes(ptr_base(t)); }
static int type_alignment(const std::string& t) {
    if (is_ptr_type(t) || t == "int" || t == "bool") return 2;
    if (t == "char" || t == "char8_t") return 1;
    if (const TypeDef* d = type_def(t)) return d->align;
    return 2;
}
// The element type of an array whose elements are `elem_bytes` wide.
static std::string elem_type(int elem_bytes) { return elem_bytes == 1 ? "char" : "int"; }

class CwizParser {
public:
    CwizParser(std::vector<Token> toks, TypeTable& types) : t(std::move(toks)), types_(types) {}

    NodeP parse() {
        NodeP prog = mk(NK::Program, 1, 1);
        while (!at("eof")) prog->list.push_back(decl(true));
        return prog;
    }

private:
    std::vector<Token> t;
    size_t p = 0;
    TypeTable& types_;
    std::vector<NodeP> pending_type_nodes_;  // enum constants produced by a type specifier
    int pending_align_ = 1;                  // _Alignas on the next declaration
    bool params_variadic_ = false;           // last param_list() ended with ...

    const Token& cur() const { return t[p]; }
    bool at(const std::string& kind, const std::string& value = "", bool have_value = false) const {
        const Token& tk = cur();
        return tk.kind == kind && (!have_value || tk.sval == value || (tk.kind == "num" && false));
    }
    bool at_kind(const std::string& kind) const { return cur().kind == kind; }
    bool at_op1(const std::string& op) const { return cur().kind == "op" && cur().sval == op; }
    bool at_op(const std::set<std::string>& ops) const {
        return cur().kind == "op" && ops.count(cur().sval);
    }
    bool at_kw1(const std::string& kw) const { return cur().kind == "kw" && cur().sval == kw; }
    bool at_kw(const std::set<std::string>& kws) const {
        return cur().kind == "kw" && kws.count(cur().sval);
    }
    bool is_type_start(const Token& tk) const {
        if (tk.kind == "kw") {
            return TYPES.count(tk.sval) || DECL_QUALIFIERS.count(tk.sval) ||
                   tk.sval == "rom" || tk.sval == "rom8" || tk.sval == "struct" ||
                   tk.sval == "union" || tk.sval == "enum" || tk.sval == "constexpr" ||
                   tk.sval == "auto" || tk.sval == "typeof" ||
                   tk.sval == "typeof_unqual" || tk.sval == "_BitInt";
        }
        return tk.kind == "ident" && types_.aliases.count(tk.sval) != 0;
    }
    bool at_decl_start() const { return is_type_start(cur()); }
    bool type_start_at(size_t idx) const { return idx < t.size() && is_type_start(t[idx]); }
    const Token& next() { return t[p++]; }

    static std::string show(const Token& tk) {
        if (tk.kind == "eof") return "end of file";
        if (tk.kind == "str") return "a string";
        return "'" + (tk.kind == "num" ? std::to_string(tk.num) : tk.sval) + "'";
    }

    void expect_op(const std::string& op) {
        if (!at_op1(op)) {
            const Token& tk = cur();
            std::string hint;
            std::string code = "syntax.expected-token";
            std::string before = show(tk);
            if (op == ";") {
                hint = "add ';' at the end of the statement";
                code = "syntax.missing-semicolon";
            } else if (op == ")") {
                hint = "add a closing ')'";
                if (tk.kind == "eof" || (tk.kind == "op" && (tk.sval == ";" || tk.sval == "}"))) {
                    hint = "add ')' before this point (an opening '(' is not closed)";
                    code = "syntax.unbalanced-paren";
                }
            } else if (op == "}") {
                hint = "add a closing '}' (or check the braces above)";
            } else if (op == "{") {
                hint = "add '{' to start the block";
            } else if (op == "(") {
                hint = "add '(' after the keyword";
            } else if (op == "]") {
                hint = "add a closing ']'";
                if (tk.kind == "eof" || (tk.kind == "op" && (tk.sval == ";" || tk.sval == "}"))) {
                    hint = "add ']' before this point (an opening '[' is not closed)";
                    code = "syntax.unbalanced-bracket";
                }
            }
            throw CwizError(fmt("expected '%s' before %s", op.c_str(), before.c_str()),
                            tk.line, tk.col, hint, code);
        }
        next();
    }
    Token expect_ident() {
        if (cur().kind != "ident") {
            const Token& tk = cur();
            std::string hint;
            if (tk.kind == "kw")
                hint = "'" + tk.sval + "' is a keyword; choose another name";
            else
                hint = "a name may contain letters, digits and '_' and cannot start with a digit";
            throw CwizError("expected an identifier before " + show(tk), tk.line, tk.col, hint,
                            "syntax.expected-identifier");
        }
        return next();
    }

    // C23 attributes: [[nodiscard]], [[maybe_unused]], ...  They are accepted
    // and recorded only insofar as they suppress lint findings; codegen is
    // unchanged on this target.
    void skip_attributes() {
        while (at_op1("[[")) {
            Token open = next();
            int depth = 1;
            while (depth > 0) {
                if (at_kind("eof"))
                    throw CwizError("unterminated attribute", open.line, open.col,
                                    "close the attribute with ']]'", "syntax.unterminated-attribute");
                if (at_op1("[[")) { depth++; next(); continue; }
                if (at_op1("]]")) { depth--; next(); continue; }
                next();
            }
        }
    }

    int parse_alignas_arg() {
        next();  // _Alignas / alignas
        expect_op("(");
        int a = 1;
        if (at_decl_start()) {
            std::string t = type_name();
            a = type_align(t);
        } else {
            long long v = const_int_expr();
            if (v <= 0 || (v & (v - 1)) != 0)
                throw CwizError("alignment must be a positive power of two",
                                cur().line, cur().col, "use 1, 2, 4, ...",
                                "sema.alignment");
            a = (int)v;
        }
        expect_op(")");
        return a;
    }

    // Small constant evaluator usable while parsing (enum values, alignas).
    long long const_int_expr() {
        long long v = const_int_unary();
        while (at_op1("+") || at_op1("-") || at_op1("*") || at_op1("/") || at_op1("%") ||
               at_op1("<<") || at_op1(">>") || at_op1("&") || at_op1("|") || at_op1("^")) {
            std::string op = next().sval;
            long long r = const_int_unary();
            if (op == "+") v += r;
            else if (op == "-") v -= r;
            else if (op == "*") v *= r;
            else if (op == "/" && r) v /= r;
            else if (op == "%" && r) v %= r;
            else if (op == "<<") v <<= (r & 15);
            else if (op == ">>") v >>= (r & 15);
            else if (op == "&") v &= r;
            else if (op == "|") v |= r;
            else if (op == "^") v ^= r;
        }
        return wrap16(v);
    }
    long long const_int_unary() {
        if (at_op1("-")) { next(); return wrap16(-const_int_unary()); }
        if (at_op1("+")) { next(); return const_int_unary(); }
        if (at_op1("~")) { next(); return wrap16(~const_int_unary()); }
        if (at_op1("(")) { next(); long long v = const_int_expr(); expect_op(")"); return v; }
        if (cur().kind == "num") return wrap16(next().num);
        if (cur().kind == "char") return wrap16(next().num);
        if (cur().kind == "ident") {
            std::string n = next().sval;
            auto it = types_.enums.find(n);
            if (it != types_.enums.end()) return it->second;
        }
        throw CwizError("expected an integer constant", cur().line, cur().col,
                        "use a literal or an earlier enum value", "sema.constant");
    }

    // Skip storage-class / width keywords and return the base type.
    std::string base_type_name() {
        bool had_qualifier = false;
        while (at_kw(DECL_QUALIFIERS)) { had_qualifier = true; next(); }
        const Token& tk = cur();
        if (had_qualifier &&
            (tk.kind == "eof" || tk.kind == "ident" ||
             (tk.kind == "op" && std::string(");,=[]{}").find(tk.sval[0]) != std::string::npos)))
            return "int";  // `unsigned x`, `sizeof(long)`, `long long` ...
        if (tk.kind == "kw" && (tk.sval == "struct" || tk.sval == "union"))
            return aggregate_specifier();
        if (tk.kind == "kw" && tk.sval == "enum") {
            enum_specifier();
            return "int";
        }
        if (tk.kind == "kw" && (tk.sval == "typeof" || tk.sval == "typeof_unqual")) {
            next();
            expect_op("(");
            if (at_decl_start()) {
                std::string t = type_name();
                expect_op(")");
                return t;
            }
            throw CwizError("typeof(expression) is not supported", tk.line, tk.col,
                            "use typeof(type), or declare the variable explicitly",
                            "sema.typeof-expression");
        }
        if (tk.kind == "kw" && tk.sval == "_BitInt") {
            next();
            expect_op("(");
            long long bits = const_int_expr();
            expect_op(")");
            if (bits <= 0 || bits > 16)
                throw CwizError("_BitInt(N) with N > 16 is not supported by the 16-bit target",
                                tk.line, tk.col, "use _BitInt(8) or _BitInt(16)",
                                "sema.bitint-width");
            return bits <= 8 ? "char" : "int";
        }
        if (tk.kind == "kw" && tk.sval == "auto") {
            next();
            return "auto";
        }
        if (tk.kind == "kw" && TYPES.count(tk.sval)) {
            std::string v = tk.sval;
            next();
            return v;
        }
        if (tk.kind == "ident" && types_.aliases.count(tk.sval)) {
            std::string v = types_.aliases[tk.sval];
            next();
            return v;
        }
        std::string hint;
        if (tk.kind == "ident") {
            std::vector<std::string> candidates(TYPES.begin(), TYPES.end());
            for (const auto& kv : types_.aliases) candidates.push_back(kv.first);
            hint = did_you_mean(tk.sval, candidates);
        }
        throw CwizError("expected a type before " + show(tk), tk.line, tk.col, hint,
                        "syntax.expected-type");
    }

    std::string type_name() {
        std::string v = base_type_name();
        while (at_op1("*")) {  // int *p / char **q
            next();
            v += "*";
        }
        return v;
    }

    static int type_align(const std::string& t) {
        if (is_ptr_type(t) || t == "int" || t == "bool") return 2;
        if (t == "char" || t == "char8_t") return 1;
        if (const TypeDef* d = type_def(t)) return d->align;
        return 2;
    }

    // `struct Name { ... }` / `union Name { ... }`, or a reference to a tag.
    std::string aggregate_specifier() {
        Token kw = next();
        const std::string kind = kw.sval;
        std::string tag;
        if (cur().kind == "ident") tag = next().sval;
        else if (!at_op1("{")) {
            const Token& tk = cur();
            throw CwizError("expected a struct/union tag or '{' before " + show(tk), tk.line,
                            tk.col, "write 'struct Name { ... }' or 'struct Name'",
                            "syntax.expected-tag");
        }
        if (tag.empty()) tag = "__anon" + std::to_string(++types_.anon);
        const std::string full = kind + " " + tag;
        if (!at_op1("{")) {
            if (!types_.types.count(full)) types_.types[full] = TypeDef{kind, {}, 0, 1, false};
            return full;
        }
        next();  // {
        TypeDef def;
        def.kind = kind;
        def.complete = true;
        types_.types[full] = def;  // make the tag visible to self-referential pointers
        std::set<std::string> names;
        std::vector<StructField> fields;
        while (!at_op1("}")) {
            if (at_kind("eof"))
                throw CwizError("unterminated " + kind, kw.line, kw.col,
                                "add '}' to close the aggregate", "syntax.unterminated-struct");
            std::string fty = type_name();
            if (at_op1(";") && is_aggregate_type(fty)) {
                const TypeDef* inner = type_def(fty);
                if (!inner || !inner->complete)
                    throw CwizError("anonymous member has an incomplete type", kw.line, kw.col,
                                    "define the struct/union first", "sema.incomplete-type");
                StructField g;
                g.type = fty;
                g.size = inner->size;
                g.is_group = true;
                g.group = inner->fields;
                fields.push_back(g);
                next();  // ;
                continue;
            }
            while (true) {
                Token fn = expect_ident();
                StructField f;
                f.name = fn.sval;
                f.type = fty;
                f.size = type_bytes(fty);
                if (at_op1("[")) {
                    Token lb = next();
                    if (!at_op1("]"))
                        throw CwizError("struct array members are not supported", lb.line, lb.col,
                                        "use a flexible member ('name[]') or a pointer",
                                        "sema.struct-array-member");
                    next();
                    f.flexible = true;
                    f.size = 0;
                }
                if (at_op1(":")) {
                    next();
                    long long w = const_int_expr();
                    if (w <= 0 || w > (type_bytes(fty) * 8))
                        throw CwizError("invalid bitfield width", fn.line, fn.col,
                                        "the width must fit the member type",
                                        "sema.bitfield-width");
                    f.bits = (int)w;
                }
                if (!names.insert(fn.sval).second)
                    throw CwizError("duplicate member '" + fn.sval + "'", fn.line, fn.col,
                                    "member names must be unique inside one " + kind,
                                    "sema.duplicate-member");
                if (is_aggregate_type(fty) && type_def(fty) && !type_def(fty)->complete)
                    throw CwizError("member '" + fn.sval + "' has an incomplete type",
                                    fn.line, fn.col, "define the struct before embedding it",
                                    "sema.incomplete-type");
                fields.push_back(f);
                if (at_op1(",")) { next(); continue; }
                break;
            }
            expect_op(";");
        }
        next();  // }
        for (size_t i = 0; i < fields.size(); i++)
            if (fields[i].flexible && i + 1 != fields.size())
                throw CwizError("a flexible array member must be last", kw.line, kw.col,
                                "move '" + fields[i].name + "[]' to the end of the struct",
                                "sema.flexible-member");
        int off = 0, size = 0, align = 1;
        std::vector<StructField> flat;
        std::set<std::string> flat_names;
        for (auto& f : fields) {
            const int a = type_align(f.type);
            if (a > align) align = a;
            if (kind == "union") {
                f.offset = 0;
                if (f.size > size) size = f.size;
            } else {
                off = align_up(off, a);
                f.offset = off;
                off += f.size;
            }
            if (f.is_group) {
                for (StructField sub : f.group) {
                    sub.offset += f.offset;
                    if (!flat_names.insert(sub.name).second)
                        throw CwizError("duplicate member '" + sub.name +
                                            "' (via an anonymous member)",
                                        kw.line, kw.col,
                                        "rename one of the members", "sema.duplicate-member");
                    flat.push_back(sub);
                }
            } else {
                if (!flat_names.insert(f.name).second)
                    throw CwizError("duplicate member '" + f.name + "'", kw.line, kw.col,
                                    "member names must be unique inside one " + kind,
                                    "sema.duplicate-member");
                flat.push_back(f);
            }
        }
        def.fields = flat;
        def.align = align;
        def.size = kind == "union" ? align_up(size, align) : align_up(off, align);
        types_.types[full] = def;
        return full;
    }

    long long enum_value(const NodeP& e) {
        if (e->k == NK::Num) return wrap16(e->value);
        if (e->k == NK::Unary) {
            long long v = enum_value(e->a);
            if (e->sval == "-") return wrap16(-v);
            if (e->sval == "+") return v;
            if (e->sval == "~") return wrap16(~v);
            if (e->sval == "!") return v ? 0 : 1;
        }
        if (e->k == NK::Bin) {
            long long a = enum_value(e->a), b = enum_value(e->b);
            if (e->sval == "+") return wrap16(a + b);
            if (e->sval == "-") return wrap16(a - b);
            if (e->sval == "*") return wrap16(a * b);
            if (e->sval == "/" && b) return wrap16(a / b);
            if (e->sval == "%" && b) return wrap16(a % b);
            if (e->sval == "&") return wrap16(a & b);
            if (e->sval == "|") return wrap16(a | b);
            if (e->sval == "^") return wrap16(a ^ b);
            if (e->sval == "<<") return wrap16(a << (b & 15));
            if (e->sval == ">>") return wrap16(a >> (b & 15));
        }
        if (e->k == NK::Ident) {
            auto it = types_.enums.find(e->sval);
            if (it != types_.enums.end()) return it->second;
        }
        throw CwizError("enum value is not a constant expression", e->line, e->col,
                        "use literals or earlier enumerators", "sema.enum-constant");
    }

    void enum_specifier() {
        Token kw = next();
        if (cur().kind == "ident") next();  // optional tag
        if (at_op1(":")) {                  // C23 fixed underlying type
            next();
            std::string under = type_name();
            (void)under;                    // semantics remain int on this target
        }
        if (!at_op1("{")) return;            // `enum E x;` uses int
        next();
        long long nextv = 0;
        while (!at_op1("}")) {
            Token name = expect_ident();
            long long val = nextv;
            if (at_op1("=")) {
                next();
                val = enum_value(expr());
            }
            NodeP c = mk(NK::ConstDecl, name.line, name.col);
            c->sval = name.sval;
            c->type = "int";
            NodeP num = mk(NK::Num, name.line, name.col);
            num->value = val;
            c->a = num;
            pending_type_nodes_.push_back(c);
            types_.enums[name.sval] = val;
            nextv = val + 1;
            if (at_op1(",")) { next(); continue; }
            break;
        }
        if (!at_op1("}"))
            throw CwizError("expected '}' to close the enum", kw.line, kw.col,
                            "close the enumerator list with '}'", "syntax.unterminated-enum");
        next();
    }

    // Attach enum constants produced while parsing a type to the declaration.
    NodeP flush_pending(NodeP r) {
        if (pending_type_nodes_.empty()) return r;
        NodeP g = mk(NK::DeclGroup, r ? r->line : 1, r ? r->col : 1);
        g->list = pending_type_nodes_;
        pending_type_nodes_.clear();
        if (r) {
            if (r->k == NK::DeclGroup) {
                for (auto& x : r->list) g->list.push_back(x);
            } else if (!(r->k == NK::VarDecl && r->sval.empty())) {
                g->list.push_back(r);
            }
        }
        return g;
    }

    NodeP decl(bool global_scope) {
        NodeP r = decl_one(global_scope);
        return flush_pending(r);
    }

    NodeP static_assert_node() {
        Token kw = next();
        expect_op("(");
        NodeP cond = expr();
        std::string msg;
        if (at_op1(",")) {
            next();
            if (cur().kind != "str")
                throw CwizError("static_assert message must be a string", cur().line, cur().col,
                                "write static_assert(condition, \"message\");",
                                "syntax.static-assert");
            for (uint32_t cp : next().cps)
                if (cp <= 0xFF) msg += (char)cp;
        }
        expect_op(")");
        expect_op(";");
        NodeP n = mk(NK::StaticAssert, kw.line, kw.col);
        n->a = cond;
        n->sval = msg;
        return n;
    }

    NodeP decl_one(bool global_scope) {
        Token tk = cur();
        skip_attributes();
        if (at_kw1("_Static_assert") || at_kw1("static_assert"))
            return static_assert_node();
        if (at_kw1("_Alignas") || at_kw1("alignas")) {
            pending_align_ = parse_alignas_arg();
            return decl_one(global_scope);
        }
        if (at_kw1("constexpr")) {
            if (!global_scope)
                throw CwizError("constexpr is only allowed at file scope", tk.line, tk.col,
                                "move it before the functions", "sema.constexpr-scope");
            return const_decl();
        }
        if (at_kw1("typedef")) {
            if (!global_scope)
                throw CwizError("typedef is only allowed at file scope", tk.line, tk.col,
                                "move the typedef before the functions", "sema.typedef-scope");
            next();
            std::string base = type_name();
            NodeP empty = mk(NK::DeclGroup, tk.line, tk.col);
            if (at_op1("(") && t[p + 1].kind == "op" && t[p + 1].sval == "*") {
                next();
                while (at_op1("*")) next();
                Token name = expect_ident();
                expect_op(")");
                std::vector<NodeP> sig = param_list();
                (void)sig;
                types_.aliases[name.sval] = "fnptr";
                expect_op(";");
                return empty;
            }
            while (true) {
                Token name = expect_ident();
                std::string ty = base;
                if (at_op1("["))
                    throw CwizError("array typedefs are not supported", cur().line, cur().col,
                                    "typedef the element type and declare the array separately",
                                    "sema.typedef-array");
                types_.aliases[name.sval] = ty;
                if (at_op1(",")) { next(); continue; }
                break;
            }
            expect_op(";");
            return empty;
        }
        if (at_kw1("const")) {
            if (!global_scope)
                throw CwizError("const is only allowed at file scope", tk.line, tk.col);
            return const_decl();
        }
        if (at_kw1("rom") || at_kw1("rom8")) {
            if (!global_scope)
                throw CwizError(tk.sval + " tables are only allowed at file scope", tk.line, tk.col);
            return rom_decl();
        }
        if (at_decl_start()) return type_decl(global_scope);
        if (at_op1("}"))
            throw CwizError("unmatched '}'", tk.line, tk.col,
                            "remove the extra '}', or add the '{' that it closes",
                            "syntax.unmatched-brace");
        std::string hint;
        if (tk.kind == "ident") {
            hint = "a declaration starts with a type, for example 'int " + tk.sval + ";'";
        }
        throw CwizError("expected a declaration, found " + show(tk), tk.line, tk.col, hint,
                        "syntax.expected-declaration");
    }

    NodeP const_decl() {
        Token kw = next();
        std::string base = "int";
        if (at_decl_start()) base = base_type_name();
        NodeP group = mk(NK::DeclGroup, kw.line, kw.col);
        while (true) {
            while (at_op1("*")) next();
            Token name = expect_ident();
            expect_op("=");
            NodeP value = expr();
            NodeP n = mk(NK::ConstDecl, name.line, name.col);
            n->sval = name.sval;
            n->type = base;
            n->a = value;
            group->list.push_back(n);
            if (at_op1(",")) { next(); continue; }
            break;
        }
        expect_op(";");
        return group->list.size() == 1 ? group->list[0] : group;
    }

    void array_dims(NodeP n) {
        while (at_op1("[")) {
            next();
            NodeP e;
            if (!at_op1("]")) e = expr();
            expect_op("]");
            n->dim_exprs.push_back(e);
        }
        if (n->dim_exprs.size() == 1) n->size_expr = n->dim_exprs[0];
    }

    NodeP rom_decl() {
        Token kind_tok = next();
        std::string kind = kind_tok.sval;
        std::string elem = "int";
        if (at_kw(TYPES)) elem = next().sval;
        else if (cur().kind == "ident" && types_.aliases.count(cur().sval)) elem = type_name();
        if (kind == "rom" && elem != "int")
            throw CwizError("rom stores 16-bit words; its element type must be int", kind_tok.line,
                            kind_tok.col, "write 'rom int name[N] = { ... };'",
                            "sema.type-placement");
        if (kind == "rom8" && elem != "char")
            throw CwizError("rom8 stores bytes; its element type must be char", kind_tok.line,
                            kind_tok.col, "write 'rom8 char name[N] = \"...\";'",
                            "sema.type-placement");
        NodeP group = mk(NK::DeclGroup, kind_tok.line, kind_tok.col);
        while (true) {
            Token name = expect_ident();
            NodeP n = mk(NK::ArrayDecl, name.line, name.col);
            n->sval = name.sval;
            n->kind = kind;
            n->type = elem;
            array_dims(n);
            if (n->dim_exprs.empty())
                throw CwizError(kind + " requires an array size or initializer", name.line,
                                name.col, "write 'rom int name[4];' or 'rom8 char s[] = \"...\";'",
                                "sema.type-placement");
            if (at_op1("=")) {
                next();
                if (kind == "rom8" && at_kind("str")) {
                    n->cps = next().cps;
                    n->init_is_str = true;
                } else {
                    n->list = init_list();
                }
            }
            group->list.push_back(n);
            if (at_op1(",")) { next(); continue; }
            break;
        }
        expect_op(";");
        return group->list.size() == 1 ? group->list[0] : group;
    }

    std::vector<NodeP> init_list() {
        if (!at_op1("{")) {
            const Token& tk = cur();
            throw CwizError("expected '{' to start an initializer", tk.line, tk.col,
                            "array initializers look like '{1, 2, 3}'", "syntax.expected-token");
        }
        next();
        std::vector<NodeP> items;
        if (!at_op1("}")) {
            while (true) {
                NodeP item;
                if (at_op1(".")) {
                    Token dot = next();
                    Token fld = expect_ident();
                    expect_op("=");
                    item = mk(NK::Designator, dot.line, dot.col);
                    item->sval = fld.sval;
                    item->a = expr();
                } else if (at_op1("[")) {
                    Token lb = next();
                    NodeP idx = expr();
                    expect_op("]");
                    expect_op("=");
                    item = mk(NK::Designator, lb.line, lb.col);
                    item->b = idx;
                    item->a = expr();
                } else {
                    item = expr();
                }
                items.push_back(item);
                if (at_op1(",")) {
                    next();
                    if (at_op1("}")) break;
                    continue;
                }
                break;
            }
        }
        expect_op("}");
        return items;
    }

    NodeP type_decl(bool global_scope) {
        Token start = cur();
        std::string base = base_type_name();
        NodeP group = mk(NK::DeclGroup, start.line, start.col);
        const int decl_align = pending_align_;
        pending_align_ = 1;
        if (at_op1(";")) {  // `struct P { ... };` / `enum E { ... };`
            next();
            return group;
        }

        while (true) {
            if (at_op1("(") && t[p + 1].kind == "op" && t[p + 1].sval == "*") {
                next();  // (
                while (at_op1("*")) next();
                Token name = expect_ident();
                expect_op(")");
                std::vector<NodeP> sig = param_list();
                (void)sig;
                if (!group->list.empty())
                    throw CwizError("a function pointer cannot share a declaration",
                                    name.line, name.col, "put it on its own line",
                                    "syntax.function-in-declaration");
                NodeP n = mk(NK::VarDecl, name.line, name.col);
                n->sval = name.sval;
                n->type = "fnptr";
                n->align = decl_align;
                if (at_op1("=")) { next(); n->a = expr(); }
                group->list.push_back(n);
                if (at_op1(",")) { next(); continue; }
                break;
            }
            int pointers = 0;
            while (at_op1("*")) { next(); ++pointers; }
            Token name = expect_ident();
            std::string ty = base + std::string((size_t)pointers, '*');

            if (at_op1("(")) {
                if (!group->list.empty())
                    throw CwizError("a function cannot appear in a comma-separated declaration",
                                    name.line, name.col,
                                    "declare the function on its own line",
                                    "syntax.function-in-declaration");
                std::vector<NodeP> params = param_list();
                const bool is_variadic = params_variadic_;
                NodeP body = block();
                if (!global_scope)
                    throw CwizError("functions can only be defined at file scope", start.line,
                                    start.col, "move the function out of the enclosing block",
                                    "sema.function-scope");
                NodeP n = mk(NK::FuncDecl, start.line, start.col);
                n->sval = name.sval;
                n->list = params;
                n->b = body;
                n->type = ty;
                n->variadic = is_variadic;
                return n;
            }

            if (ty == "void")
                throw CwizError("variable '" + name.sval + "' cannot have type void",
                                name.line, name.col,
                                "use int, char or bool; void is only for a function's return type",
                                "sema.void-variable");
            if (is_aggregate_type(ty) && (!type_def(ty) || !type_def(ty)->complete))
                throw CwizError("variable '" + name.sval + "' has an incomplete type",
                                name.line, name.col, "define the struct/union first",
                                "sema.incomplete-type");

            if (at_op1("[")) {
                NodeP n = mk(NK::ArrayDecl, name.line, name.col);
                n->sval = name.sval;
                n->kind = "ram";
                n->type = ty;
                n->align = decl_align;
                array_dims(n);
                if (at_op1("=")) {
                    next();
                    if (ty == "char" && at_kind("str")) {  // char buf[N] = "..."
                        n->cps = next().cps;
                        n->init_is_str = true;
                    } else {
                        n->list = init_list();
                    }
                }
                group->list.push_back(n);
            } else {
                NodeP n = mk(NK::VarDecl, name.line, name.col);
                n->sval = name.sval;
                n->type = ty;
                n->align = decl_align;
                if (at_op1("=")) {
                    next();
                    if (is_aggregate_type(ty) && at_op1("{")) {
                        n->list = init_list();
                    } else {
                        n->a = expr();
                    }
                } else if (ty == "auto") {
                    throw CwizError("auto requires an initializer", name.line, name.col,
                                    "write 'auto x = value;'", "sema.auto-no-init");
                }
                group->list.push_back(n);
            }

            if (at_op1(",")) { next(); continue; }
            break;
        }

        expect_op(";");
        return group->list.size() == 1 ? group->list[0] : group;
    }

    std::vector<NodeP> param_list() {
        expect_op("(");
        params_variadic_ = false;
        std::vector<NodeP> params;
        if (!at_op1(")")) {
            while (true) {
                if (at_op1("...")) {
                    next();
                    params_variadic_ = true;
                    if (!at_op1(")"))
                        throw CwizError("'...' must be the last parameter", cur().line, cur().col,
                                        "write 'f(int n, ...)'", "syntax.variadic");
                    break;
                }
                if (at_kw1("void") && t[p + 1].kind == "op" && t[p + 1].sval == ")") {
                    next();
                    break;
                }
                std::string pty = type_name();
                Token pname;
                if (at_op1(",") || at_op1(")")) {
                    // Unnamed parameter in a prototype / function-pointer type.
                    pname.line = cur().line;
                    pname.col = cur().col;
                } else {
                    pname = expect_ident();
                }
                NodeP n = mk(NK::Param, pname.line, pname.col);
                n->sval = pname.sval;
                n->type = pty;
                if (at_op1("=")) {
                    next();
                    n->a = expr();
                    n->has_default = true;
                }
                params.push_back(n);
                if (at_op1(",")) { next(); continue; }
                break;
            }
        }
        expect_op(")");
        return params;
    }

    NodeP block() {
        Token lb = cur();
        expect_op("{");
        NodeP n = mk(NK::Block, lb.line, lb.col);
        while (!at_op1("}")) {
            if (at_kind("eof"))
                throw CwizError("unterminated block", lb.line, lb.col,
                                "add '}' to close the block opened here", "syntax.unterminated-block");
            n->list.push_back(stmt());
        }
        expect_op("}");
        return n;
    }

    NodeP stmt() {
        Token tk = cur();
        skip_attributes();
        tk = cur();
        if (at_kw1("_Static_assert") || at_kw1("static_assert"))
            return static_assert_node();
        if (at_kw1("_Alignas") || at_kw1("alignas")) {
            pending_align_ = parse_alignas_arg();
            return stmt();
        }
        if (at_op1("{")) return block();
        if (at_op1(";")) {  // the empty statement, needed after a label
            next();
            return mk(NK::Block, tk.line, tk.col);
        }
        // `name:` is a label; `name` alone can never start a statement, so the
        // one-token lookahead is unambiguous.
        if (tk.kind == "ident" && t[p + 1].kind == "op" && t[p + 1].sval == ":") {
            next();
            next();
            NodeP n = mk(NK::Label, tk.line, tk.col);
            n->sval = tk.sval;
            return n;
        }
        if (at_kw1("switch")) return switch_stmt();
        if (at_kw1("case") || at_kw1("default"))
            throw CwizError("case/default only make sense inside a switch", tk.line, tk.col,
                            "move this label inside a switch body",
                            "syntax.case-outside-switch");
        if (at_kw1("goto")) {
            next();
            NodeP n = mk(NK::Goto, tk.line, tk.col);
            Token target = expect_ident();
            n->sval = target.sval;
            expect_op(";");
            return n;
        }
        if (at_kw1("if")) return if_stmt();
        if (at_kw1("while")) return while_stmt();
        if (at_kw1("do")) return do_stmt();
        if (at_kw1("for")) return for_stmt();
        if (at_kw1("return")) {
            next();
            NodeP n = mk(NK::Return, tk.line, tk.col);
            if (!at_op1(";")) n->a = expr();
            expect_op(";");
            return n;
        }
        if (at_kw1("break")) { next(); expect_op(";"); return mk(NK::Break, tk.line, tk.col); }
        if (at_kw1("continue")) {
            next(); expect_op(";"); return mk(NK::Continue, tk.line, tk.col);
        }
        if (at_decl_start() && !at_kw1("void")) return local_decl();
        NodeP e = expr();
        expect_op(";");
        NodeP n = mk(NK::ExprStmt, tk.line, tk.col);
        n->a = e;
        return n;
    }

    NodeP local_const_decl() {
        Token start = next();  // const
        std::string base = "int";
        if (at_decl_start()) base = base_type_name();
        NodeP n = mk(NK::DeclStmt, start.line, start.col);
        while (true) {
            while (at_op1("*")) next();
            Token name = expect_ident();
            expect_op("=");
            NodeP c = mk(NK::ConstDecl, name.line, name.col);
            c->sval = name.sval;
            c->type = base;
            c->a = expr();
            n->list.push_back(c);
            if (at_op1(",")) { next(); continue; }
            break;
        }
        expect_op(";");
        return n;
    }

    NodeP local_decl() {
        Token start = cur();
        if (at_kw1("const") || at_kw1("constexpr")) return local_const_decl();
        std::string base = base_type_name();
        const int decl_align = pending_align_;
        pending_align_ = 1;
        NodeP group = mk(NK::DeclGroup, start.line, start.col);

        while (true) {
            if (at_op1("(") && t[p + 1].kind == "op" && t[p + 1].sval == "*") {
                next();
                while (at_op1("*")) next();
                Token name = expect_ident();
                expect_op(")");
                std::vector<NodeP> sig = param_list();
                (void)sig;
                NodeP n = mk(NK::VarDecl, name.line, name.col);
                n->sval = name.sval;
                n->type = "fnptr";
                n->align = decl_align;
                if (at_op1("=")) { next(); n->a = expr(); }
                group->list.push_back(n);
                if (at_op1(",")) { next(); continue; }
                break;
            }
            int pointers = 0;
            while (at_op1("*")) { next(); ++pointers; }
            Token name = expect_ident();
            std::string ty = base + std::string((size_t)pointers, '*');
            if (at_op1("("))
                throw CwizError("functions can only be defined at file scope", name.line,
                                name.col, "move the function out of the enclosing block",
                                "sema.function-scope");
            if (ty == "void")
                throw CwizError("variable '" + name.sval + "' cannot have type void",
                                name.line, name.col,
                                "use int, char or bool; void is only for a function's return type",
                                "sema.void-variable");

            if (is_aggregate_type(ty) && (!type_def(ty) || !type_def(ty)->complete))
                throw CwizError("variable '" + name.sval + "' has an incomplete type",
                                name.line, name.col, "define the struct/union first",
                                "sema.incomplete-type");
            if (at_op1("[")) {
                NodeP n = mk(NK::ArrayDecl, name.line, name.col);
                n->sval = name.sval;
                n->kind = "ram";
                n->type = ty;
                n->align = decl_align;
                array_dims(n);
                if (at_op1("=")) {
                    next();
                    if (ty == "char" && at_kind("str")) {
                        n->cps = next().cps;
                        n->init_is_str = true;
                    } else {
                        n->list = init_list();
                    }
                }
                group->list.push_back(n);
            } else {
                NodeP vd = mk(NK::VarDecl, name.line, name.col);
                vd->sval = name.sval;
                vd->type = ty;
                vd->align = decl_align;
                if (at_op1("=")) {
                    next();
                    if (is_aggregate_type(ty) && at_op1("{")) {
                        vd->list = init_list();
                    } else {
                        vd->a = expr();
                    }
                } else if (ty == "auto") {
                    throw CwizError("auto requires an initializer", name.line, name.col,
                                    "write 'auto x = value;'", "sema.auto-no-init");
                }
                group->list.push_back(vd);
            }

            if (at_op1(",")) { next(); continue; }
            break;
        }

        expect_op(";");
        NodeP n = mk(NK::DeclStmt, start.line, start.col);
        n->list = group->list;
        return n;
    }

    NodeP if_stmt() {
        Token tk = next();
        expect_op("(");
        NodeP cond = expr();
        expect_op(")");
        NodeP then = stmt();
        NodeP other;
        if (at_kw1("else")) { next(); other = stmt(); }
        NodeP n = mk(NK::If, tk.line, tk.col);
        n->a = cond;
        n->b = then;
        n->c = other;
        return n;
    }

    NodeP while_stmt() {
        Token tk = next();
        expect_op("(");
        NodeP cond = expr();
        expect_op(")");
        NodeP body = stmt();
        NodeP n = mk(NK::While, tk.line, tk.col);
        n->a = cond;
        n->b = body;
        return n;
    }

    NodeP do_stmt() {
        Token tk = next();
        NodeP body = stmt();
        if (!at_kw1("while"))
            throw CwizError("expected 'while' after do-block", tk.line, tk.col,
                            "write 'do { ... } while (condition);'", "syntax.expected-while");
        next();
        expect_op("(");
        NodeP cond = expr();
        expect_op(")");
        expect_op(";");
        NodeP n = mk(NK::DoWhile, tk.line, tk.col);
        n->a = body;
        n->b = cond;
        return n;
    }

    // switch (expr) { case C: ... default: ... }
    //
    // A case node carries its constant `value` (or `sval == "default"`) and the
    // statements that follow it.  Falling through is preserved by emitting the
    // bodies in source order; break leaves through the enclosing switch's end.
    NodeP switch_stmt() {
        Token tk = next();
        expect_op("(");
        NodeP cond = expr();
        expect_op(")");
        NodeP n = mk(NK::Switch, tk.line, tk.col);
        n->a = cond;
        expect_op("{");
        NodeP current;
        while (!at_op1("}")) {
            if (at_kind("eof"))
                throw CwizError("unterminated switch", tk.line, tk.col,
                                "add '}' to close the switch opened here",
                                "syntax.unterminated-switch");
            if (at_kw1("case")) {
                Token ct = next();
                NodeP c = mk(NK::Case, ct.line, ct.col);
                c->a = expr();
                // `case A ... B:` (GNU range) is not part of the C subset.
                if (at_op1("..."))
                    throw CwizError("case ranges are not supported", cur().line, cur().col,
                                    "write one case per value", "syntax.case-range");
                expect_op(":");
                n->list.push_back(c);
                current = c;
                continue;
            }
            if (at_kw1("default")) {
                Token dt = next();
                NodeP c = mk(NK::Case, dt.line, dt.col);
                c->sval = "default";
                expect_op(":");
                n->list.push_back(c);
                current = c;
                continue;
            }
            if (!current)
                throw CwizError("a statement inside switch must follow case/default",
                                cur().line, cur().col,
                                "add 'case value:' or 'default:' before this statement",
                                "syntax.switch-statement");
            current->list.push_back(stmt());
        }
        expect_op("}");
        return n;
    }

    NodeP for_stmt() {
        Token tk = next();
        expect_op("(");
        NodeP init;
        if (!at_op1(";")) {
            if (at_decl_start() && !at_kw1("void")) {
                init = local_decl();  // consumes its own ';'
            } else {
                init = mk(NK::ExprStmt, cur().line, cur().col);
                init->a = expr();
                expect_op(";");
            }
        } else {
            expect_op(";");
        }
        NodeP cond;
        if (!at_op1(";")) cond = expr();
        expect_op(";");
        NodeP step;
        if (!at_op1(")")) step = expr();
        expect_op(")");
        NodeP body = stmt();
        NodeP n = mk(NK::For, tk.line, tk.col);
        n->a = init;
        n->b = cond;
        n->c = step;
        n->d = body;
        return n;
    }

    // -- expressions ---------------------------------------------------
    NodeP expr() { return assignment(); }

    NodeP assignment() {
        NodeP left = conditional();
        if (at_op(ASSIGN_OPS)) {
            Token op = next();
            NodeP right = assignment();
            if (left->k != NK::Ident && left->k != NK::Index && left->k != NK::Member &&
                !(left->k == NK::Unary && left->sval == "*"))
                throw CwizError("cannot assign to this expression", op.line, op.col,
                                "the left side must be a variable, array element, member or *pointer",
                                "syntax.not-assignable");
            NodeP n = mk(NK::Assign, op.line, op.col);
            n->sval = op.sval;
            n->a = left;
            n->b = right;
            return n;
        }
        return left;
    }

    NodeP conditional() {
        NodeP cond = logical_or();
        if (at_op1("?")) {
            Token q = next();
            NodeP then = expr();
            expect_op(":");
            NodeP other = conditional();
            NodeP n = mk(NK::Cond, q.line, q.col);
            n->a = cond;
            n->b = then;
            n->c = other;
            return n;
        }
        return cond;
    }

    NodeP logical_or() {
        NodeP left = logical_and();
        while (at_op1("||")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = "||";
            n->a = left;
            n->b = logical_and();
            left = n;
        }
        return left;
    }
    NodeP logical_and() {
        NodeP left = bit_or();
        while (at_op1("&&")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = "&&";
            n->a = left;
            n->b = bit_or();
            left = n;
        }
        return left;
    }
    NodeP bit_or() {
        NodeP left = bit_xor();
        while (at_op1("|")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = "|";
            n->a = left;
            n->b = bit_xor();
            left = n;
        }
        return left;
    }
    NodeP bit_xor() {
        NodeP left = bit_and();
        while (at_op1("^")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = "^";
            n->a = left;
            n->b = bit_and();
            left = n;
        }
        return left;
    }
    NodeP bit_and() {
        NodeP left = equality();
        while (at_op1("&")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = "&";
            n->a = left;
            n->b = equality();
            left = n;
        }
        return left;
    }
    NodeP equality() {
        NodeP left = relational();
        while (at_op1("==") || at_op1("!=")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = op.sval;
            n->a = left;
            n->b = relational();
            left = n;
        }
        return left;
    }
    NodeP relational() {
        NodeP left = shift();
        while (at_op1("<") || at_op1(">") || at_op1("<=") || at_op1(">=")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = op.sval;
            n->a = left;
            n->b = shift();
            left = n;
        }
        return left;
    }
    NodeP shift() {
        NodeP left = additive();
        while (at_op1("<<") || at_op1(">>")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = op.sval;
            n->a = left;
            n->b = additive();
            left = n;
        }
        return left;
    }
    NodeP additive() {
        NodeP left = multiplicative();
        while (at_op1("+") || at_op1("-")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = op.sval;
            n->a = left;
            n->b = multiplicative();
            left = n;
        }
        return left;
    }
    NodeP multiplicative() {
        NodeP left = unary();
        while (at_op1("*") || at_op1("/") || at_op1("%")) {
            Token op = next();
            NodeP n = mk(NK::Bin, op.line, op.col);
            n->sval = op.sval;
            n->a = left;
            n->b = unary();
            left = n;
        }
        return left;
    }

    NodeP unary() {
        Token tk = cur();
        // Cast: `(type)expr`.  Only unambiguous when the token after '(' is a
        // type name (including typedefs and struct/union tags).
        if (at_op1("(") && type_start_at(p + 1)) {
            next();
            NodeP n = mk(NK::Cast, tk.line, tk.col);
            n->type = type_name();
            expect_op(")");
            if (at_op1("{")) {  // C99 compound literal
                n->k = NK::CompoundLit;
                n->list = init_list();
                return postfix_tail(n);
            }
            n->a = unary();
            return postfix_tail(n);
        }
        // sizeof is an expression in C, not a statement.  `sizeof(type)` and
        // `sizeof expr` are both resolved by the analyser, where symbol sizes
        // are known (an array's size is its element count * element width).
        if (at_kw1("sizeof")) {
            next();
            NodeP n = mk(NK::Sizeof, tk.line, tk.col);
            if (at_op1("(") && type_start_at(p + 1)) {
                next();
                n->type = type_name();
                expect_op(")");
            } else {
                n->a = unary();
            }
            return n;
        }
        if (at_kw1("_Alignof") || at_kw1("alignof")) {
            next();
            NodeP n = mk(NK::Alignof, tk.line, tk.col);
            if (at_op1("(") && type_start_at(p + 1)) {
                next();
                n->type = type_name();
                expect_op(")");
            } else {
                n->a = unary();
            }
            return n;
        }
        if (at_kw1("_Generic")) {
            next();
            NodeP n = mk(NK::Generic, tk.line, tk.col);
            expect_op("(");
            n->a = assignment();
            expect_op(",");
            while (true) {
                NodeP a = mk(NK::Assoc, cur().line, cur().col);
                if (at_kw1("default")) {
                    next();
                    a->sval = "default";
                } else {
                    a->type = type_name();
                }
                expect_op(":");
                a->a = assignment();
                n->list.push_back(a);
                if (at_op1(",")) { next(); continue; }
                break;
            }
            expect_op(")");
            return n;
        }
        if (at_op1("-") || at_op1("+") || at_op1("~") || at_op1("!")) {
            next();
            NodeP n = mk(NK::Unary, tk.line, tk.col);
            n->sval = tk.sval;
            n->a = unary();
            return n;
        }
        if (at_op1("&") || at_op1("*")) {
            next();
            NodeP n = mk(NK::Unary, tk.line, tk.col);
            n->sval = tk.sval;
            n->a = unary();
            return n;
        }
        if (at_op1("++") || at_op1("--")) {
            next();
            NodeP n = mk(NK::IncDec, tk.line, tk.col);
            n->sval = tk.sval;
            n->a = unary();
            n->prefix = true;
            return n;
        }
        return postfix();
    }

    NodeP postfix() { return postfix_tail(primary()); }

    NodeP postfix_tail(NodeP e) {
        while (true) {
            if (at_op1("[")) {
                Token lb = next();
                NodeP idx = expr();
                expect_op("]");
                NodeP n = mk(NK::Index, lb.line, lb.col);
                n->a = e;  // an array/table name, or any pointer expression
                n->b = idx;
                e = n;
            } else if (at_op1(".") || at_op1("->")) {
                Token op = next();
                Token name = expect_ident();
                NodeP n = mk(NK::Member, op.line, op.col);
                n->kind = op.sval;
                n->sval = name.sval;
                n->a = e;
                e = n;
            } else if (at_op1("(")) {
                if (e->k != NK::Ident)
                    throw CwizError("call target must be a function name", cur().line, cur().col,
                                    "only a named function can be called directly",
                                    "syntax.call-target");
                next();
                std::vector<NodeP> args;
                if (!at_op1(")")) {
                    while (true) {
                        args.push_back(expr());
                        if (at_op1(",")) { next(); continue; }
                        break;
                    }
                }
                expect_op(")");
                NodeP n = mk(NK::Call, e->line, e->col);
                n->sval = e->sval;
                n->a = e;  // callee; direct calls still resolve by name
                n->list = args;
                e = n;
            } else if (at_op1("++") || at_op1("--")) {
                Token op = next();
                NodeP n = mk(NK::IncDec, op.line, op.col);
                n->sval = op.sval;
                n->a = e;
                n->prefix = false;
                e = n;
            } else {
                break;
            }
        }
        return e;
    }

    NodeP primary() {
        Token tk = cur();
        if (tk.kind == "num" || tk.kind == "char") {
            next();
            NodeP n = mk(NK::Num, tk.line, tk.col);
            n->value = tk.num;
            return n;
        }
        if (tk.kind == "str") {
            next();
            NodeP n = mk(NK::Str, tk.line, tk.col);
            n->cps = tk.cps;
            return n;
        }
        if (at_kw1("true")) { next(); NodeP n = mk(NK::Num, tk.line, tk.col); n->value = 1; return n; }
        if (at_kw1("false")) { next(); NodeP n = mk(NK::Num, tk.line, tk.col); n->value = 0; return n; }
        if (at_kw1("nullptr")) {
            next();
            NodeP n = mk(NK::Num, tk.line, tk.col);
            n->value = 0;
            return n;
        }
        if (tk.kind == "ident") {
            next();
            NodeP n = mk(NK::Ident, tk.line, tk.col);
            n->sval = tk.sval;
            return n;
        }
        if (at_op1("(")) {
            next();
            NodeP e = expr();
            expect_op(")");
            return e;
        }
        {
            std::string hint;
            if (tk.kind == "op" && tk.sval == ";")
                hint = "an expression is missing before ';'";
            else if (tk.kind == "op" && tk.sval == "=")
                hint = "declarations look like 'int x = value;'";
            else if (tk.kind == "kw" && tk.sval == "case")
                hint = "switch/case is not supported; use if/else";
            throw CwizError("unexpected " + show(tk), tk.line, tk.col, hint,
                            "syntax.unexpected-token");
        }
    }
};

// =================================================================== prelude
static std::string font_table_source() {
    std::string body;
    for (int i = 0; i < FONT6X8_LEN; i++) body += fmt("\\x%02x", FONT6X8[i]);
    return fmt("rom8 char _font[760] = \"%s\";", body.c_str());
}

// The CW Display table is a normal prelude declaration so select_prelude() only
// emits its 2.4 KB when a _cw function actually uses it.
static std::string cw_font_table_source() {
    std::string s = fmt("rom _cw_font[%d] = {", FONT_CW_ROWS);
    for (int i = 0; i < FONT_CW_ROWS; i++) {
        if (i) s += ",";
        if (i % 16 == 0) s += "\n";
        s += fmt("0x%X", FONT_CW[i]);
    }
    s += "\n};";
    return s;
}

// Vietnamese glyphs are indexed by the F4 column byte (0x10..0x95), so the
// table is emitted in code order.  Only programs that use a *_cw helper or
// normalizeText() pull it in.
static std::string cw_font_vn_table_source() {
    std::string s = fmt("rom _cw_font_vn[%d] = {", FONT_CW_VN_ROWS);
    for (int i = 0; i < FONT_CW_VN_ROWS; i++) {
        if (i) s += ",";
        if (i % 16 == 0) s += "\n";
        s += fmt("0x%X", FONT_CW_VN[i]);
    }
    s += "\n};";
    return s;
}

// Codepoint -> F4 column map used by the runtime normalizeText() scan.
static std::string vn_f4_table_source() {
    // VN_F4_TABLE is sorted by codepoint, which is what the runtime binary
    // search expects.  Keep the column byte in the parallel byte table.
    std::string cps = fmt("rom _vn_f4_cps[%d] = {", VN_F4_COUNT);
    std::string codes = fmt("rom8 char _vn_f4_codes[%d] = \"", VN_F4_COUNT);
    for (int i = 0; i < VN_F4_COUNT; i++) {
        if (i) cps += ",";
        if (i % 12 == 0) cps += "\n";
        cps += fmt("0x%X", (unsigned)VN_F4_TABLE[i].cp);
        codes += fmt("\\x%02x", VN_F4_TABLE[i].code);
    }
    cps += "\n};";
    codes += "\";";
    return cps + "\n" + codes;
}

static const char* PRELUDE_SRC = R"CWIZ(
// ---- CWiz standard library (auto-included) ----

int _seed = 12345;

int abs(int n) { if (n < 0) { return 0 - n; } return n; }
int min(int a, int b) { if (a < b) { return a; } return b; }
int max(int a, int b) { if (a > b) { return a; } return b; }
int clamp(int v, int lo, int hi) { if (v < lo) { return lo; } if (v > hi) { return hi; } return v; }
int sign(int n) { if (n < 0) { return -1; } if (n > 0) { return 1; } return 0; }

void srand(int s) { _seed = s; }
int rand(int n) { _seed = _seed * 25173 + 13849; return ((_seed >> 1) & 0x7FFF) % n; }

void hline(int x0, int x1, int y, int c = 1) {
    if (y < 0 || y >= _ROWS) { return; }
    if (x0 < 0) { x0 = 0; }
    if (x1 > 192) { x1 = 192; }
    if (x0 >= x1) { return; }
    _hspan(x0, x1, y, c != 0);
}
void vline(int x, int y0, int y1, int c = 1) {
    if (x < 0 || x > 191) { return; }
    if (y0 < 0) { y0 = 0; }
    if (y1 > _ROWS) { y1 = _ROWS; }
    int y = y0;
    while (y < y1) { pixel(x, y, c); y = y + 1; }
}
void rect(int x, int y, int w, int h, int c = 1) {
    hline(x, x + w, y, c);
    hline(x, x + w, y + h - 1, c);
    vline(x, y, y + h, c);
    vline(x + w - 1, y, y + h, c);
}
void fillrect(int x, int y, int w, int h, int c = 1) {
    int j = y;
    while (j < y + h) { hline(x, x + w, j, c); j = j + 1; }
}
void line(int x0, int y0, int x1, int y1, int c = 1) {
    int dx = abs(x1 - x0);
    int dy = 0 - abs(y1 - y0);
    int sx = 1;
    if (x0 > x1) { sx = -1; }
    int sy = 1;
    if (y0 > y1) { sy = -1; }
    int err = dx + dy;
    while (1) {
        pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) { break; }
        int e2 = err + err;
        if (e2 >= dy) { err = err + dy; x0 = x0 + sx; }
        if (e2 <= dx) { err = err + dx; y0 = y0 + sy; }
    }
}
void circle(int cx, int cy, int r, int c = 1) {
    int x = r;
    int y = 0;
    int err = 1 - r;
    while (x >= y) {
        pixel(cx + x, cy + y, c);
        pixel(cx + y, cy + x, c);
        pixel(cx - y, cy + x, c);
        pixel(cx - x, cy + y, c);
        pixel(cx - x, cy - y, c);
        pixel(cx - y, cy - x, c);
        pixel(cx + y, cy - x, c);
        pixel(cx + x, cy - y, c);
        y = y + 1;
        if (err < 0) { err = err + y + y + 1; }
        else { x = x - 1; err = err + (y - x) * 2 + 1; }
    }
}
void disc(int cx, int cy, int r, int c = 1) {
    int r2 = r * r;
    int dy = 0 - r;
    while (dy <= r) {
        int dx = 0 - r;
        while (dx <= r) {
            if (dx * dx + dy * dy <= r2) { pixel(cx + dx, cy + dy, c); }
            dx = dx + 1;
        }
        dy = dy + 1;
    }
}

void glyph(int x, int y, int c) {
    if (c < 32 || c > 126) { c = 63; }
    int base = (c - 32) * 8;
    int r = 0;
    while (r < 8) {
        int bits = _font[base + r];
        int k = 0;
        while (k < 6) {
            pixel(x + k, y + r, (bits & 128) != 0);
            bits = bits + bits;
            k = k + 1;
        }
        r = r + 1;
    }
}
int text(int x, int y, int s) {
    int c = _rpeek(s);
    while (c != 0) {
        glyph(x, y, c);
        x = x + 6;
        s = s + 1;
        c = _rpeek(s);
    }
    return x;
}
int num(int x, int y, int n) {
    if (n == -32768) { return text(x, y, "-32768"); }
    if (n < 0) { glyph(x, y, 45); x = x + 6; n = 0 - n; }
    int d = 10000;
    int shown = 0;
    while (d > 0) {
        int q = n / d;
        n = n - q * d;
        if (q > 0 || shown || d == 1) {
            glyph(x, y, 48 + q);
            x = x + 6;
            shown = 1;
        }
        d = d / 10;
    }
    return x;
}

// ---- ClassWiz CW Display (native 11x13, no scaling) ----
void glyph_cw(int x, int y, int c) {
    if (c < 32 || c > 126) { c = 63; }
    int base = (c - 32) * 13;
    int r = 0;
    while (r < 13) {
        int bits = _cw_font[base + r];
        int k = 0;
        while (k < 11) {
            pixel(x + k, y + r, (bits & 1024) != 0);
            bits = bits + bits;
            k = k + 1;
        }
        r = r + 1;
    }
}
// One Vietnamese glyph, selected by the byte after 0xF4 (0x10..0x95).
void glyph_vn_cw(int x, int y, int c) {
    if (c < 16 || c > 149) { glyph_cw(x, y, 63); return; }
    int base = (c - 16) * 13;
    int r = 0;
    while (r < 13) {
        int bits = _cw_font_vn[base + r];
        int k = 0;
        while (k < 11) {
            pixel(x + k, y + r, (bits & 1024) != 0);
            bits = bits + bits;
            k = k + 1;
        }
        r = r + 1;
    }
}
int text_cw(int x, int y, int s) {
    int c = _rpeek(s);
    while (c != 0) {
        if (c == 0xF4) {
            int code = _rpeek(s + 1);
            if (code == 0) { glyph_cw(x, y, 63); x = x + 11; break; }
            glyph_vn_cw(x, y, code);
            x = x + 11;
            s = s + 2;
        } else {
            glyph_cw(x, y, c);
            x = x + 11;
            s = s + 1;
        }
        c = _rpeek(s);
    }
    return x;
}

// ---- Vietnamese text -------------------------------------------------
// Static result buffer for normalizeText().  It lives in RAM, so callers that
// need two normalized strings at once must copy the first one.
char _normalized_text[256];

// Runtime mapping: Unicode codepoint -> F4 column byte.  A binary search
// keeps the ROM table small (134 words + 134 bytes) at the cost of a few
// comparisons per character.
int _vn_f4_code(int cp) {
    int lo = 0;
    int hi = 133;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int v = _vn_f4_cps[mid];
        if (v == cp) { return _vn_f4_codes[mid]; }
        if (v < cp) { lo = mid + 1; }
        else { hi = mid - 1; }
    }
    return 0;
}
// Convert a byte string containing UTF-8 to CW Display F4 bytes.  Returns
// the address of the static 256-byte buffer (overwritten on each call).
// ASCII is copied, known Vietnamese letters become 0xF4,<code>, an existing
// F4 pair is copied unchanged, and any other non-ASCII codepoint becomes '?'.
int normalizeText(int s) {
    int out = 0;
    int c = _rpeek(s);
    while (c != 0) {
        int cp = c;
        if (c == 0xF4) {
            int f4next = _rpeek(s + 1);
            if (f4next == 0) {
                _normalized_text[out] = 63;
                out = out + 1;
                break;
            }
            _normalized_text[out] = 0xF4;
            _normalized_text[out + 1] = f4next;
            out = out + 2;
            s = s + 2;
            if (out > 250) { break; }
            c = _rpeek(s);
            continue;
        }
        if (c >= 0xC0 && c < 0xE0) {
            cp = ((c & 31) << 6) | (_rpeek(s + 1) & 63);
            s = s + 1;
        } else if (c >= 0xE0 && c < 0xF0) {
            cp = ((c & 15) << 12) | ((_rpeek(s + 1) & 63) << 6) | (_rpeek(s + 2) & 63);
            s = s + 2;
        } else if (c >= 0xF0) {
            cp = ((c & 7) << 18) | ((_rpeek(s + 1) & 63) << 12) |
                 ((_rpeek(s + 2) & 63) << 6) | (_rpeek(s + 3) & 63);
            s = s + 3;
        }
        int code = _vn_f4_code(cp);
        if (code != 0) {
            _normalized_text[out] = 0xF4;
            _normalized_text[out + 1] = code;
            out = out + 2;
        } else if (cp < 128) {
            _normalized_text[out] = cp;
            out = out + 1;
        } else {
            _normalized_text[out] = 63;
            out = out + 1;
        }
        if (out > 250) { break; }
        s = s + 1;
        c = _rpeek(s);
    }
    _normalized_text[out] = 0;
    return _normalized_text;
}
int num_cw(int x, int y, int n) {
    if (n == -32768) { return text_cw(x, y, "-32768"); }
    if (n < 0) { glyph_cw(x, y, 45); x = x + 11; n = 0 - n; }
    int d = 10000;
    int shown = 0;
    while (d > 0) {
        int q = n / d;
        n = n - q * d;
        if (q > 0 || shown || d == 1) {
            glyph_cw(x, y, 48 + q);
            x = x + 11;
            shown = 1;
        }
        d = d / 10;
    }
    return x;
}
void sprite(int x, int y, int w, int h, int d) {
    int bw = (w + 7) >> 3;
    int r = 0;
    while (r < h) {
        int k = 0;
        while (k < w) {
            int b = _rpeek(d + r * bw + (k >> 3));
            if (b & (128 >> (k & 7))) { pixel(x + k, y + r); }
            k = k + 1;
        }
        r = r + 1;
    }
}
int memoryRead(int addr, int len) {
    if (len == 1) { return peek(addr); }
    if (len == 2) { return peek(addr) + (peek(addr + 1) << 8); }
    return 0;
}
void memoryWrite(int addr, int len, int data) {
    if (len == 1) { poke(addr, data); }
    if (len == 2) { poke(addr, data); poke(addr + 1, data >> 8); }
}
void setStatus(int addr, int status) {
    memoryWrite(SB2 + addr - 0xF800, 1, status != LOW);
}

int wait_key() {
    while (key() != -1) { }
    while (1) {
        int k = key();
        if (k != -1) { return k; }
    }
}

// ---- strings / memory (freestanding <string.h> subset) ----------------
char *memcpy(char *dst, char *src, int n) {
    int i = 0;
    while (i < n) { dst[i] = src[i]; i = i + 1; }
    return dst;
}
char *memset(char *dst, int v, int n) {
    int i = 0;
    while (i < n) { dst[i] = (char)v; i = i + 1; }
    return dst;
}
int memcmp(char *a, char *b, int n) {
    int i = 0;
    while (i < n) {
        if (a[i] != b[i]) { return a[i] - b[i]; }
        i = i + 1;
    }
    return 0;
}
char *memccpy(char *dst, char *src, int c, int n) {
    int i = 0;
    while (i < n) {
        dst[i] = src[i];
        i = i + 1;
        if (dst[i - 1] == (char)c) { return dst + i; }
    }
    return 0;
}
char *strcat(char *dst, char *src) {
    int n = strlen(dst);
    int i = 0;
    while (src[i] != 0) { dst[n + i] = src[i]; i = i + 1; }
    dst[n + i] = 0;
    return dst;
}
char *strchr(char *s, int c) {
    while (s[0] != 0) {
        if (s[0] == (char)c) { return s; }
        s = s + 1;
    }
    if (c == 0) { return s; }
    return 0;
}
// C23 <stdckdint.h> subset: add/sub overflow checks.
int ckd_add(int *r, int a, int b) {
    int s = a + b;
    *r = s;
    return (a > 0 && b > 0 && s < 0) || (a < 0 && b < 0 && s >= 0);
}
int ckd_sub(int *r, int a, int b) {
    int s = a - b;
    *r = s;
    return (a >= 0 && b < 0 && s < 0) || (a < 0 && b > 0 && s >= 0);
}
// strdup/strndup subset: a fixed static arena, never freed (documented).
char _dup_buf[512];
int _dup_pos = 0;
char *strdup(char *s) {
    char *out = _dup_buf + _dup_pos;
    int i = 0;
    while (s[i] != 0 && _dup_pos + i < 511) { out[i] = s[i]; i = i + 1; }
    out[i] = 0;
    _dup_pos = _dup_pos + i + 1;
    if (_dup_pos > 500) { _dup_pos = 0; }
    return out;
}
char *strndup(char *s, int n) {
    char *out = _dup_buf + _dup_pos;
    int i = 0;
    while (i < n && s[i] != 0 && _dup_pos + i < 511) { out[i] = s[i]; i = i + 1; }
    out[i] = 0;
    _dup_pos = _dup_pos + i + 1;
    if (_dup_pos > 500) { _dup_pos = 0; }
    return out;
}

int strlen(char *s) {
    int n = 0;
    while (_rpeek(s + n) != 0) { n = n + 1; }
    return n;
}
int strcmp(char *a, char *b) {
    while (1) {
        int ca = _rpeek(a);
        int cb = _rpeek(b);
        if (ca != cb) { return ca - cb; }
        if (ca == 0) { return 0; }
        a = a + 1;
        b = b + 1;
    }
}
char *strcpy(char *dst, char *src) {
    int i = 0;
    while (1) {
        int c = _rpeek(src + i);
        poke(dst + i, c);
        if (c == 0) { break; }
        i = i + 1;
    }
    return dst;
}
int atoi(char *s) {
    int n = 0;
    int neg = 0;
    int c = _rpeek(s);
    if (c == 45) {
        neg = 1;
        s = s + 1;
        c = _rpeek(s);
    }
    while (c >= 48 && c <= 57) {
        n = n * 10 + (c - 48);
        s = s + 1;
        c = _rpeek(s);
    }
    if (neg) { return 0 - n; }
    return n;
}
char *itoa(char *buf, int n, int base) {
    if (n == -32768) {
        poke(buf, 45); poke(buf + 1, 51); poke(buf + 2, 50); poke(buf + 3, 55);
        poke(buf + 4, 54); poke(buf + 5, 56); poke(buf + 6, 0);
        return buf;
    }
    int pos = 0;
    int neg = 0;
    if (n < 0) { neg = 1; n = 0 - n; }
    if (n == 0) { poke(buf, 48); poke(buf + 1, 0); return buf; }
    int d = n;
    while (d > 0) {
        int r = d % base;
        d = d / base;
        if (r < 10) { poke(buf + 7 + pos, 48 + r); }
        else { poke(buf + 7 + pos, 87 + r); }
        pos = pos + 1;
    }
    if (neg) { poke(buf + 7 + pos, 45); pos = pos + 1; }
    int i = 0;
    while (i < pos) {
        poke(buf + i, peek(buf + 7 + pos - 1 - i));
        i = i + 1;
    }
    poke(buf + pos, 0);
    return buf;
}

// C23 <stddef.h> unreachable(): reaching it is undefined behaviour, so this
// subset parks the CPU (the simulator reports a halt).
void unreachable(void) {
    while (1) { }
}

// ---- getpixel --------------------------------------------------------
// Upstream implements this in assembly; here it reads the shadow buffer,
// which is exactly what pixel() writes, so the result is the same before
// and after render().
int getpixel(int x, int y) {
    if (x < 0 || x > 191 || y < 0 || y >= _ROWS) { return 0; }
    int a = SB2 + (y + _ROW_OFF) * 32 + (x >> 3);
    if (peek(a) & (128 >> (x & 7))) { return 1; }
    return 0;
}
void clear_rect(int x, int y, int w, int h, int c = 0) {
    int j = 0;
    while (j < h) {
        int i = 0;
        while (i < w) {
            pixel(x + i, y + j, c);
            i = i + 1;
        }
        j = j + 1;
    }
}
void invert_rect(int x, int y, int w, int h) {
    int j = 0;
    while (j < h) {
        int i = 0;
        while (i < w) {
            pixel(x + i, y + j, !getpixel(x + i, y + j));
            i = i + 1;
        }
        j = j + 1;
    }
}
// Forward copy: the caller must choose an order that is safe for overlap.
void copy_rect(int x, int y, int w, int h, int x2, int y2) {
    int j = 0;
    while (j < h) {
        int i = 0;
        while (i < w) {
            pixel(x2 + i, y2 + j, getpixel(x + i, y + j));
            i = i + 1;
        }
        j = j + 1;
    }
}
)CWIZ";

static std::string prelude_source() {
    return font_table_source() + "\n" + cw_font_table_source() + "\n" +
           cw_font_vn_table_source() + "\n" + vn_f4_table_source() + "\n" + PRELUDE_SRC;
}

static std::string decl_name(const NodeP& d) { return d->sval; }

static std::vector<NodeP> select_prelude(const NodeP& user_prog, const NodeP& prelude_prog) {
    std::map<std::string, NodeP> by_name;
    for (auto& d : prelude_prog->list) {
        std::string n = decl_name(d);
        if (!n.empty()) by_name[n] = d;
    }
    std::set<std::string> user_names;
    for (auto& d : user_prog->list) {
        std::string n = decl_name(d);
        if (!n.empty()) user_names.insert(n);
    }
    std::set<std::string> used = referenced_names(user_prog);
    if (used.count("print")) {
        used.insert("text");
        used.insert("num");
    }
    if (used.count("print_cw")) {
        used.insert("text_cw");
        used.insert("num_cw");
    }
    std::set<std::string> needed;
    std::vector<std::string> stack;
    for (auto& n : used)
        if (by_name.count(n) && !user_names.count(n)) stack.push_back(n);
    while (!stack.empty()) {
        std::string n = stack.back();
        stack.pop_back();
        if (needed.count(n) || user_names.count(n)) continue;
        needed.insert(n);
        for (auto& dep : referenced_names(by_name[n]))
            if (by_name.count(dep) && !needed.count(dep) && !user_names.count(dep))
                stack.push_back(dep);
    }
    std::vector<NodeP> out;
    for (auto& d : prelude_prog->list)
        if (needed.count(decl_name(d))) out.push_back(d);
    return out;
}

// =================================================================== sema
struct BuiltinInfo {
    int lo = 0;
    int hi = 0;
    const char* sig = "";
    // Bit i set = argument i is an address, where a string literal (a ROM
    // address in this language) is meaningful.  Everywhere else a string is a
    // type error, which catches pixel("x", 0) and friends.
    unsigned addr_args = 0;
};

static const std::map<std::string, BuiltinInfo> CW_BUILTINS = {
    {"pixel", {2, 3, "pixel(x, y, c = 1)", 0}},
    {"clear", {0, 0, "clear()", 0}},
    {"clearRender", {0, 0, "clearRender()", 0}},
    {"key", {0, 0, "key()", 0}},
    {"delay", {1, 1, "delay(n)", 0}},
    {"peek", {1, 1, "peek(addr)", 1u << 0}},
    {"poke", {2, 2, "poke(addr, value)", 1u << 0}},
    {"_hspan", {4, 4, "_hspan(x0, x1, y, c)", 0}},
    {"key_down", {1, 1, "key_down(k)", 0}},
    {"key_hit", {1, 1, "key_hit(k)", 0}},
    {"buffered", {1, 1, "buffered(on)", 0}},
    {"flip", {0, 0, "flip()", 0}},
    {"_rpeek", {1, 1, "_rpeek(addr)", 1u << 0}},
    {"wipe", {0, 0, "wipe()", 0}},
    {"recover", {0, 0, "recover()", 0}},
    {"reset", {0, 0, "reset()", 0}},
    {"exit", {0, 0, "exit()", 0}},
    {"render", {0, 0, "render()", 0}},
    {"renderStatus", {0, 0, "renderStatus()", 0}},
    {"ticks", {0, 0, "ticks()", 0}},
    {"delay_ms", {1, 1, "delay_ms(ms)", 0}},
    {"__va_start", {0, 0, "__va_start()", 0}},
    {"__va_arg", {1, 1, "__va_arg(ap)", 1u << 0}},
    {"_hpeek", {1, 1, "_hpeek(addr)", 1u << 0}},
    {"_hpoke", {2, 2, "_hpoke(addr, value)", 1u << 0}}};
static const int PRINT_MIN = 3;

struct Analyzed {
    std::map<std::string, long long> consts;
    std::vector<Sym*> globals;
    std::vector<Sym*> arrays;
    std::vector<Sym*> tables;
    std::map<std::string, Func*> funcs;
    std::vector<std::string> func_order;
    bool has_main = false;
    const Target* target = nullptr;
    std::vector<std::unique_ptr<Sym>> sym_store;
    std::vector<std::unique_ptr<Func>> func_store;
};

class Analyzer {
public:
    Analyzer(NodeP prog, const Target& target, std::map<std::string, long long> key_consts)
        : prog(std::move(prog)), target(target), consts(std::move(key_consts)) {
        an.target = &this->target;
    }

    Analyzed run() {
        for (auto& d : prog->list) global_decl(d);
        an.consts = consts;
        an.has_main = an.funcs.count("main") != 0;
        // Global initialisers are lowered at the very end (their addresses are
        // assigned by the code generator), but they still need type checking.
        for (Sym* sym : an.globals)
            if (sym->init_kind == 1 && sym->init_node) {
                if (expr(sym->init_node) == "void")
                    err("cannot initialize '" + sym->name + "' with a void expression",
                        sym->init_node, "call a function that returns a value",
                        "sema.void-expression");
            } else if (sym->init_kind == 4) {
                const TypeDef* td = type_def(sym->vtype);
                for (const NodeP& item : sym->init_list) {
                    NodeP v = item;
                    if (item->k == NK::Designator) {
                        if (item->sval.empty() || !td || !find_field(sym->vtype, item->sval))
                            err("no such member '" + item->sval + "'", item,
                                "use a member name from the struct/union",
                                "sema.unknown-member");
                        v = item->a;
                    }
                    if (expr(v) == "void")
                        err("cannot initialize a struct member with a void expression", v,
                            "call a value-returning function", "sema.void-expression");
                }
            }
        for (auto& name : an.func_order) function_body(an.funcs[name]);
        mark_recursion();
        return std::move(an);
    }

private:
    NodeP prog;
    const Target& target;
    Analyzed an;
    std::map<std::string, long long> consts;
    std::vector<std::pair<std::string, Sym*>> globals;
    std::vector<std::pair<std::string, Sym*>> arrays;
    std::vector<std::pair<std::string, Sym*>> tables;
    Func* fn = nullptr;
    std::vector<std::pair<std::string, Sym*>> locals;
    int loop_depth = 0;
    int switch_depth = 0;
    std::map<std::string, int> label_lines;  // label -> first declaration line
    std::map<std::string, std::set<std::string>> call_graph;

    Sym* new_sym(const std::string& kind, const std::string& name) {
        an.sym_store.push_back(std::make_unique<Sym>());
        Sym* s = an.sym_store.back().get();
        s->kind = kind;
        s->name = name;
        s->qname = name;
        return s;
    }
    Sym* find_in(std::vector<std::pair<std::string, Sym*>>& v, const std::string& name) {
        for (auto& kv : v)
            if (kv.first == name) return kv.second;
        return nullptr;
    }

    [[noreturn]] void err(const std::string& msg, const NodeP& node,
                          const std::string& hint = "", const std::string& code = "") {
        throw CwizError(msg, node ? node->line : 0, node ? node->col : 0, hint, code);
    }

    // Names a "did you mean" hint can suggest.
    std::vector<std::string> name_candidates() const {
        std::vector<std::string> names;
        for (const auto& kv : locals) names.push_back(kv.first);
        for (const auto& kv : globals) names.push_back(kv.first);
        for (const auto& kv : arrays) names.push_back(kv.first);
        for (const auto& kv : tables) names.push_back(kv.first);
        for (const auto& kv : consts)
            if (kv.first.rfind("K_", 0) != 0 && kv.first != "_ROWS") names.push_back(kv.first);
        for (const auto& kv : an.funcs) names.push_back(kv.first);
        for (const auto& kv : CW_BUILTINS) names.push_back(kv.first);
        names.push_back("print");
        names.push_back("print_cw");
        return names;
    }

    void declare_global_name(const std::string& name, const NodeP& node) {
        // A second global with the same name is a common copy/paste mistake, so
        // it gets its own code and points at the first declaration.
        Sym* prior = find_in(globals, name);
        if (!prior) prior = find_in(arrays, name);
        if (!prior) prior = find_in(tables, name);
        if (prior) {
            std::string where = prior->decl_line > 0
                                    ? fmt(" (first declared at line %d)", prior->decl_line)
                                    : "";
            err(fmt("duplicate global '%s'%s", name.c_str(), where.c_str()), node,
                "rename one of the globals, or remove this declaration",
                "sema.duplicate-global");
        }
        if (an.funcs.count(name) || consts.count(name))
            err(fmt("redefinition of '%s'", name.c_str()), node,
                "rename one of the declarations, or remove this one", "sema.redefinition");
    }

    Sym* resolve(const std::string& name) {
        if (auto* s = find_in(locals, name)) return s;
        if (auto* s = find_in(globals, name)) return s;
        if (auto* s = find_in(arrays, name)) return s;
        if (auto* s = find_in(tables, name)) return s;
        auto it = consts.find(name);
        if (it != consts.end()) {
            Sym* s = new_sym("const", name);
            s->value = it->second;
            return s;
        }
        return nullptr;
    }

    // -- const evaluation ---------------------------------------------
    long long const_eval(const NodeP& node) {
        if (node->k == NK::Num) return wrap16(node->value);
        if (node->k == NK::Unary) {
            long long v = const_eval(node->a);
            if (node->sval == "-") return wrap16(-v);
            if (node->sval == "+") return v;
            if (node->sval == "~") return wrap16(~v);
            if (node->sval == "!") return v ? 0 : 1;
        }
        if (node->k == NK::Bin)
            return fold(node->sval, const_eval(node->a), const_eval(node->b));
        if (node->k == NK::Ident) {
            if (Sym* s = find_in(locals, node->sval))
                if (s->kind == "const") return wrap16(s->value);
            auto it = consts.find(node->sval);
            if (it != consts.end()) return wrap16(it->second);
            err(fmt("'%s' is not a compile-time constant", node->sval.c_str()), node,
                "only literals and other const values can be used here", "sema.constant");
        }
        if (node->k == NK::Cast) return const_eval(node->a);
        if (node->k == NK::Sizeof) {
            if (!node->type.empty()) return type_bytes(node->type);
            if (node->a && node->a->k == NK::Ident) {
                Sym* s = resolve(node->a->sval);
                if (s) {
                    if (s->kind == "array" || s->kind == "rom" || s->kind == "rom8")
                        return (long long)s->elem_bytes * s->size;
                    return type_bytes(s->vtype);
                }
            }
        }
        if (node->k == NK::Alignof) {
            if (!node->type.empty()) return type_alignment(node->type);
            if (node->a && node->a->k == NK::Ident) {
                Sym* s = resolve(node->a->sval);
                if (s) return type_alignment(s->vtype);
            }
        }
        err("initializer is not a constant expression", node,
            "use a literal or a 'const' value; runtime expressions are not allowed here",
            "sema.constant");
    }

    long long fold(const std::string& op, long long a, long long b) {
        if (op == "+") return wrap16(a + b);
        if (op == "-") return wrap16(a - b);
        if (op == "*") return wrap16(a * b);
        if (op == "&") return wrap16(a & b);
        if (op == "|") return wrap16(a | b);
        if (op == "^") return wrap16(a ^ b);
        if (op == "/" || op == "%") {
            if (b == 0) return 0;
            long long q = std::llabs(a) / std::llabs(b);
            if ((a < 0) != (b < 0)) q = -q;
            return wrap16(op == "/" ? q : a - q * b);
        }
        if (op == "<<") return (b >= 0 && b <= 15) ? wrap16(a << (b & 15)) : 0;
        if (op == ">>") return (b >= 0 && b <= 15) ? wrap16(a >> (b & 15)) : 0;
        if (op == "==") return a == b;
        if (op == "!=") return a != b;
        if (op == "<") return a < b;
        if (op == ">") return a > b;
        if (op == "<=") return a <= b;
        if (op == ">=") return a >= b;
        if (op == "&&") return (a && b) ? 1 : 0;
        if (op == "||") return (a || b) ? 1 : 0;
        err(fmt("operator '%s' is not allowed in a constant expression", op.c_str()), nullptr,
            "constant expressions support + - * / % & | ^ << >> and comparisons",
            "sema.constant");
    }

    // -- pass 1: globals ----------------------------------------------
    void global_decl(const NodeP& d) {
        if (d->k == NK::DeclGroup) {
            for (const NodeP& item : d->list) global_decl(item);
            return;
        }
        if (d->k == NK::StaticAssert) {
            if (!const_eval(d->a))
                err(d->sval.empty() ? "static assertion failed"
                                    : "static assertion failed: " + d->sval,
                    d, "make the condition true, or remove the assertion",
                    "sema.static-assert");
            return;
        }
        if (d->k == NK::ConstDecl) {
            long long val = const_eval(d->a);
            if (consts.count(d->sval) && d->sval.rfind("K_", 0) != 0)
                err(fmt("redefinition of constant '%s'", d->sval.c_str()), d);
            consts[d->sval] = val;
            return;
        }
        if (d->k == NK::VarDecl) {
            declare_global_name(d->sval, d);
            if (d->type == "auto") {
                if (!d->a)
                    err("auto requires an initializer", d,
                        "write 'auto x = value;'", "sema.auto-no-init");
                d->type = expr(d->a);
            }
            Sym* sym = new_sym("var", d->sval);
            sym->vtype = d->type.empty() ? "int" : d->type;
            sym->align = d->align;
            sym->decl_line = d->line;
            if (!d->list.empty()) {
                const TypeDef* td = type_def(sym->vtype);
                if (!td)
                    err("only struct/union variables can use a braced initializer", d,
                        "use a scalar expression or remove the braces", "sema.initializer");
                if ((int)d->list.size() > (int)td->fields.size())
                    err(fmt("too many initializers for '%s' (%d values, %d members)",
                            d->sval.c_str(), (int)d->list.size(), (int)td->fields.size()),
                        d);
                sym->init_kind = 4;
                sym->init_list = d->list;
            } else if (d->a) {
                sym->init_kind = 1;
                sym->init_node = d->a;
            }
            globals.push_back({d->sval, sym});
            an.globals.push_back(sym);
            return;
        }
        if (d->k == NK::ArrayDecl) {
            declare_global_name(d->sval, d);
            int elem_bytes = (d->kind == "rom8") ? 1 : type_bytes(d->type);
            std::vector<long long> dims = resolve_dims(d, elem_bytes);
            long long size = 1;
            for (long long v : dims) size *= v;
            Sym* sym = new_sym(d->kind, d->sval);
            sym->size = (int)size;
            sym->elem_bytes = elem_bytes;
            sym->dims = dims;
            sym->align = d->align;
            sym->decl_line = d->line;
            if (d->init_is_str) {
                sym->init_kind = 3;
                sym->init_cps = d->cps;
            } else if (!d->list.empty()) {
                for (const NodeP& item : d->list) {
                    if (item->k != NK::Designator || !item->b) continue;
                    long long idx = const_eval(item->b);
                    if (idx < 0 || idx >= size)
                        err(fmt("array designator [%lld] is outside '%s'", idx,
                                d->sval.c_str()),
                            item, "use an index inside the declared array",
                            "sema.array-designator");
                    item->value = idx;
                }
                sym->init_kind = 2;
                sym->init_list = d->list;
            }
            sym->kind = (d->kind == "ram") ? "array" : d->kind;
            if (d->kind == "ram") {
                arrays.push_back({d->sval, sym});
                an.arrays.push_back(sym);
            } else {
                tables.push_back({d->sval, sym});
                an.tables.push_back(sym);
            }
            return;
        }
        if (d->k == NK::FuncDecl) {
            if (an.funcs.count(d->sval))
                err(fmt("redefinition of function '%s'", d->sval.c_str()), d,
                    "a function name may only be defined once; rename one of them",
                    "sema.redefinition");
            an.func_store.push_back(std::make_unique<Func>());
            Func* f = an.func_store.back().get();
            f->name = d->sval;
            f->params = d->list;
            f->body = d->b;
            f->ret = d->type;
            f->variadic = d->variadic;
            f->label = "f_" + d->sval;
            if (is_aggregate_type(f->ret))
                err("functions cannot return a struct/union by value", d,
                    "return a pointer, or fill a struct passed by pointer",
                    "sema.aggregate-return");
            std::set<std::string> seen;
            bool default_seen = false;
            for (auto& p : d->list) {
                if (is_aggregate_type(p->type))
                    err("struct/union parameters are not supported", p,
                        "pass a pointer to the struct instead", "sema.aggregate-parameter");
                if (seen.count(p->sval))
                    err(fmt("duplicate parameter '%s'", p->sval.c_str()), p,
                        "parameter names must be unique inside a function",
                        "sema.duplicate-parameter");
                seen.insert(p->sval);
                if (p->a) {
                    p->default_val = const_eval(p->a);
                    p->has_default_val = true;
                    default_seen = true;
                } else {
                    if (default_seen)
                        err(fmt("parameter '%s' without a default follows one with a default",
                                p->sval.c_str()),
                            p, "move parameters with defaults to the end of the list",
                            "sema.default-order");
                    p->has_default_val = false;
                }
            }
            an.funcs[d->sval] = f;
            an.func_order.push_back(d->sval);
            return;
        }
        err("unsupported declaration", d);
    }

    // -- pass 2: function bodies --------------------------------------
    void function_body(Func* f) {
        fn = f;
        locals.clear();
        for (size_t i = 0; i < f->params.size(); i++) {
            NodeP& p = f->params[i];
            Sym* sym = new_sym("param", p->sval);
            sym->vtype = p->type.empty() ? "int" : p->type;
            sym->scope = "local";
            sym->qname = f->name + "." + p->sval;
            sym->func = f;
            sym->index = (int)i;
            locals.push_back({p->sval, sym});
            f->locals.push_back(sym);
        }
        if (f->variadic) {
            Sym* count = new_sym("var", "__va_count");
            count->vtype = "int";
            count->scope = "local";
            count->qname = f->name + ".__va_count";
            count->func = f;
            f->va_count = count;
            f->locals.push_back(count);
            Sym* arr = new_sym("array", "__va_args");
            arr->vtype = "int";
            arr->elem_bytes = 2;
            arr->size = 16;
            arr->dims.push_back(16);
            arr->scope = "local";
            arr->qname = f->name + ".__va_args";
            arr->func = f;
            f->va_array = arr;
            f->locals.push_back(arr);
        }
        loop_depth = 0;
        switch_depth = 0;
        std::vector<NodeP> all;
        walk(f->body, all);
        std::set<std::string> calls;
        for (auto& n : all)
            if (n->k == NK::Call) calls.insert(n->sval);
        call_graph[f->name] = calls;

        // Labels are function-scoped: collect them first so forward jumps and
        // jumps from nested blocks both validate, then check every goto.
        label_lines.clear();
        for (auto& n : all) {
            if (n->k != NK::Label) continue;
            auto it = label_lines.find(n->sval);
            if (it != label_lines.end())
                err(fmt("duplicate label '%s' (first defined at line %d)", n->sval.c_str(),
                        it->second),
                    n, "rename one of the labels", "sema.duplicate-label");
            label_lines[n->sval] = n->line;
        }
        for (auto& n : all) {
            if (n->k != NK::Goto) continue;
            if (!label_lines.count(n->sval))
                err(fmt("goto target '%s' is not defined", n->sval.c_str()), n,
                    "define it with '" + n->sval + ":' in this function, or remove the goto",
                    "sema.undefined-label");
        }
        stmt(f->body);
    }

    Sym* declare_local(const std::string& name, const NodeP& node) {
        if (find_in(locals, name))
            err(fmt("redefinition of '%s'", name.c_str()), node,
                "this name already exists in the function; choose another name",
                "sema.redefinition");
        Sym* sym = new_sym("var", name);
        sym->vtype = node->type.empty() ? "int" : node->type;
        sym->scope = "local";
        sym->decl_line = node->line;
        sym->qname = fn->name + "." + name;
        sym->func = fn;
        locals.push_back({name, sym});
        fn->locals.push_back(sym);
        return sym;
    }

    // Resolve `int a[N][M]` into concrete dimensions.  At most one [] may be
    // omitted, and then it is inferred from the initializer.
    std::vector<long long> resolve_dims(const NodeP& node, int elem_bytes) {
        if (is_aggregate_type(node->type))
            err("arrays of structs/unions are not supported", node,
                "use an array of pointers, or a separate array per field",
                "sema.array-of-aggregate");
        std::vector<long long> dims;
        long long known = 1;
        int unknown = -1;
        std::vector<NodeP> exprs = node->dim_exprs;
        if (exprs.empty() && node->size_expr) exprs.push_back(node->size_expr);
        for (size_t i = 0; i < exprs.size(); i++) {
            if (!exprs[i]) {
                if (unknown >= 0)
                    err("only one array dimension may be omitted", node,
                        "give the other dimensions explicitly", "sema.array-size");
                unknown = (int)i;
                dims.push_back(0);
                continue;
            }
            long long v = const_eval(exprs[i]);
            if (v <= 0)
                err(fmt("array '%s' needs a positive size", node->sval.c_str()), node,
                    "use a size of 1 or more, for example 'int a[4];'", "sema.array-size");
            dims.push_back(v);
            known *= v;
        }
        long long init_count = node->init_is_str ? (long long)node->cps.size()
                                                 : (long long)node->list.size();
        if (unknown >= 0) {
            if (init_count == 0)
                err(fmt("array '%s' needs a size or an initializer", node->sval.c_str()), node,
                    "write the dimension explicitly, or add an initializer", "sema.array-size");
            if (known <= 0 || init_count % known != 0)
                err(fmt("cannot infer the size of array '%s'", node->sval.c_str()), node,
                    "write the dimension explicitly, or match the initializer",
                    "sema.array-size");
            dims[(size_t)unknown] = init_count / known;
        }
        long long total = 1;
        for (long long v : dims) total *= v;
        if (init_count > total)
            err(fmt("too many initializers for '%s' (%lld values, %lld elements)",
                    node->sval.c_str(), init_count, total),
                node);
        bool have_size = !dims.empty();
        if (!have_size && init_count == 0)
            err(fmt("array '%s' needs a size or an initializer", node->sval.c_str()), node);
        (void)elem_bytes;
        return dims;
    }

    Sym* declare_local_array(const NodeP& node) {
        if (find_in(locals, node->sval))
            err(fmt("redefinition of '%s'", node->sval.c_str()), node,
                "this name already exists in the function; choose another name",
                "sema.redefinition");
        int elem_bytes = type_bytes(node->type);
        std::vector<long long> dims = resolve_dims(node, elem_bytes);
        long long total = 1;
        for (long long v : dims) total *= v;
        Sym* sym = new_sym("array", node->sval);
        sym->vtype = node->type.empty() ? "int" : node->type;
        sym->size = (int)total;
        sym->elem_bytes = elem_bytes;
        sym->dims = dims;
        sym->align = node->align;
        sym->decl_line = node->line;
        sym->scope = "local";
        sym->qname = fn->name + "." + node->sval;
        sym->func = fn;
        locals.push_back({node->sval, sym});
        fn->locals.push_back(sym);
        return sym;
    }

    Sym* declare_local_const(const NodeP& node) {
        if (find_in(locals, node->sval))
            err(fmt("redefinition of '%s'", node->sval.c_str()), node,
                "this name already exists in the function; choose another name",
                "sema.redefinition");
        Sym* sym = new_sym("const", node->sval);
        sym->vtype = node->type.empty() ? "int" : node->type;
        sym->value = const_eval(node->a);
        sym->scope = "local";
        sym->decl_line = node->line;
        locals.push_back({node->sval, sym});
        return sym;
    }

    void stmt(const NodeP& s) {
        switch (s->k) {
            case NK::Block:
                for (auto& st : s->list) stmt(st);
                return;
            case NK::DeclGroup:
                for (const NodeP& item : s->list) {
                    if (item->k == NK::DeclGroup) stmt(item);
                    else {
                        NodeP one = mk(NK::DeclStmt, item->line, item->col);
                        one->list.push_back(item);
                        stmt(one);
                    }
                }
                return;
            case NK::DeclStmt: {
                for (const NodeP& vd : s->list) {
                    if (vd->k == NK::ConstDecl) {
                        vd->sym = declare_local_const(vd);
                    } else if (vd->k == NK::ArrayDecl) {
                        vd->sym = declare_local_array(vd);
                        for (const NodeP& e : vd->list) {
                            if (e->k == NK::Designator) {
                                if (e->b) {
                                    long long idx = const_eval(e->b);
                                    if (idx < 0 || idx >= vd->sym->size)
                                        err("array designator is outside the array", e,
                                            "use an index inside the declared array",
                                            "sema.array-designator");
                                    e->value = idx;
                                }
                                expr(e->a);
                            } else {
                                expr(e);
                            }
                        }
                    } else {
                        if (vd->type == "auto") {
                            if (!vd->a)
                                err("auto requires an initializer", vd,
                                    "write 'auto x = value;'", "sema.auto-no-init");
                            vd->type = expr(vd->a);
                        }
                        vd->sym = declare_local(vd->sval, vd);
                        vd->sym->align = vd->align;
                        if (!vd->list.empty()) {
                            const TypeDef* d = type_def(vd->sym->vtype);
                            if (!d)
                                err("only struct/union variables can use a braced initializer",
                                    vd, "use a scalar expression or remove the braces",
                                    "sema.initializer");
                            if ((int)vd->list.size() > (int)d->fields.size())
                                err(fmt("too many initializers for '%s' (%d values, %d members)",
                                        vd->sval.c_str(), (int)vd->list.size(),
                                        (int)d->fields.size()),
                                    vd);
                            for (const NodeP& item : vd->list) {
                                NodeP e = item;
                                if (item->k == NK::Designator) {
                                    if (item->sval.empty() || !find_field(vd->sym->vtype, item->sval))
                                        err("no such member '" + item->sval + "'", item,
                                            "use a member name from the struct/union",
                                            "sema.unknown-member");
                                    e = item->a;
                                }
                                expr(e);
                            }
                        } else if (vd->a && expr(vd->a) == "void") {
                            err("cannot initialize a variable with a void expression", vd,
                                "call a function that returns a value", "sema.void-expression");
                        }
                    }
                }
                // Backwards-compatible single-declarator form.
                if (s->a) {
                    s->a->sym = declare_local(s->a->sval, s->a);
                    if (s->a->a) expr(s->a->a);
                }
                return;
            }
            case NK::ExprStmt:
                expr(s->a);
                return;
            case NK::If:
                if (expr(s->a) == "void")
                    err("if condition has type void", s,
                        "use a value-returning expression", "sema.void-expression");
                stmt(s->b);
                if (s->c) stmt(s->c);
                return;
            case NK::While:
                if (expr(s->a) == "void")
                    err("while condition has type void", s,
                        "use a value-returning expression", "sema.void-expression");
                loop_depth++;
                stmt(s->b);
                loop_depth--;
                return;
            case NK::DoWhile:
                loop_depth++;
                stmt(s->a);
                loop_depth--;
                expr(s->b);
                return;
            case NK::For:
                if (s->a) stmt(s->a);
                if (s->b) expr(s->b);
                if (s->c) expr(s->c);
                loop_depth++;
                stmt(s->d);
                loop_depth--;
                return;
            case NK::Switch: {
                if (expr(s->a) == "void")
                    err("switch condition has type void", s,
                        "use a value-returning expression", "sema.void-expression");
                std::map<long long, int> seen;
                int defaults = 0;
                for (const NodeP& c : s->list) {
                    if (c->k != NK::Case) continue;
                    if (c->sval == "default") {
                        if (++defaults > 1)
                            err("duplicate 'default' in switch", c,
                                "a switch may have at most one default label",
                                "sema.duplicate-default");
                        continue;
                    }
                    long long v = const_eval(c->a);
                    c->value = v;  // codegen dispatches on the folded value
                    auto it = seen.find(v);
                    if (it != seen.end())
                        err(fmt("duplicate case value %lld in switch", v), c,
                            fmt("case %lld is already used at line %d; use a different value",
                                v, it->second),
                            "sema.duplicate-case");
                    seen[v] = c->line;
                }
                switch_depth++;
                for (const NodeP& c : s->list)
                    for (const NodeP& st : c->list) stmt(st);
                switch_depth--;
                return;
            }
            case NK::Case:
                err("case/default outside a switch", s,
                    "move this label inside a switch body", "sema.case-outside-switch");
            case NK::Label:
            case NK::Goto:
                return;  // validated in function_body
            case NK::StaticAssert:
                if (!const_eval(s->a))
                    err(s->sval.empty() ? "static assertion failed"
                                        : "static assertion failed: " + s->sval,
                        s, "make the condition true, or remove the assertion",
                        "sema.static-assert");
                return;
            case NK::Return:
                if (s->a) {
                    std::string t = expr(s->a);
                    if (t == "void")
                        err("cannot return a void expression from a function", s,
                            "remove the call, or call a function that returns a value",
                            "sema.void-expression");
                    if (fn && fn->ret == "void")
                        err(fmt("function '%s' is void and cannot return a value",
                                fn->name.c_str()),
                            s, "write 'return;', or declare the function with a return type",
                            "sema.void-return");
                } else if (fn && fn->ret != "void") {
                    err(fmt("function '%s' must return a value", fn->name.c_str()), s,
                        "return a value, or declare the function as void",
                        "sema.missing-return-value");
                }
                return;
            case NK::Break:
                // break is valid inside a loop or a switch; continue only in a loop.
                if (loop_depth == 0 && switch_depth == 0)
                    err("'break' outside a loop or switch", s,
                        "move it inside while/do/for/switch, or remove it",
                        "sema.break-outside-loop");
                return;
            case NK::Continue:
                if (loop_depth == 0)
                    err("'continue' outside a loop", s,
                        "continue skips to the next loop iteration; move it inside "
                        "while/do/for, or remove it",
                        "sema.continue-outside-loop");
                return;
            default:
                err("unsupported statement", s);
        }
    }

    // Type-checks an expression and records its static type in e->etype.
    std::string expr(const NodeP& e) {
        switch (e->k) {
            case NK::Num:
                return e->etype = "int";
            case NK::Str:
                return e->etype = "char*";  // a string literal is a ROM char pointer
            case NK::Ident: {
                Sym* sym = resolve(e->sval);
                if (!sym) {
                    if (an.funcs.count(e->sval))
                        return e->etype = "fnptr";  // function designator decays
                    err(fmt("'%s' is not defined", e->sval.c_str()), e,
                        did_you_mean(e->sval, name_candidates()),
                        "sema.undefined-identifier");
                }
                e->sym = sym;
                if (sym->kind == "const") return e->etype = (is_ptr_type(sym->vtype) ? sym->vtype : "int");
                if (sym->kind == "array" || sym->kind == "rom" || sym->kind == "rom8")
                    return e->etype = ptr_to(elem_type(sym->elem_bytes));  // array decays to a pointer
                if (is_aggregate_type(sym->vtype))
                    err(fmt("'%s' is a struct/union value", e->sval.c_str()), e,
                        "use a member ('.'), '->' on a pointer, or '&' for its address",
                        "sema.aggregate-value");
                return e->etype = (sym->vtype.empty() ? "int" : sym->vtype);
            }
            case NK::Index: {
                analyze_index(e);
                expr(e->b);
                if (!e->dims.empty()) return e->etype = ptr_to(e->type);  // row decay
                return e->etype = e->type;
            }
            case NK::Member: {
                const StructField* f = member_field(e);
                return e->etype = f->type;
            }
            case NK::Cast: {
                if (is_aggregate_type(e->type))
                    err("casts to struct/union types are not supported", e,
                        "cast the member value or a pointer instead", "sema.cast");
                std::string t = expr(e->a);
                if (t == "void")
                    err("cannot cast a void expression", e, "call a value-returning function",
                        "sema.void-expression");
                return e->etype = e->type;
            }
            case NK::Alignof: {
                long long a = 1;
                if (!e->type.empty()) {
                    a = type_alignment(e->type);
                } else {
                    std::string t = expr(e->a);
                    a = type_alignment(t);
                }
                e->k = NK::Num;
                e->value = a;
                e->a.reset();
                return e->etype = "int";
            }
            case NK::CompoundLit: {
                if (!fn)
                    err("compound literals are only supported inside a function", e,
                        "assign the value to a local object first", "sema.compound-literal");
                std::string ty = e->type;
                if (is_aggregate_type(ty)) {
                    const TypeDef* td = type_def(ty);
                    if (!td)
                        err("compound literal has an incomplete type", e,
                            "define the struct/union first", "sema.incomplete-type");
                    Sym* tmp = new_sym("var", "__compound");
                    tmp->vtype = ty;
                    tmp->scope = "local";
                    tmp->qname = fn->name + ".__compound" + std::to_string(an.sym_store.size());
                    tmp->func = fn;
                    fn->locals.push_back(tmp);
                    e->sym = tmp;
                    for (const NodeP& item : e->list) {
                        NodeP v = item;
                        if (item->k == NK::Designator) {
                            if (!item->sval.empty() && !find_field(ty, item->sval))
                                err("no such member '" + item->sval + "'", item,
                                    "use a member name from the struct/union",
                                    "sema.unknown-member");
                            v = item->a;
                        }
                        expr(v);
                    }
                    return e->etype = ty;
                }
                if (e->list.size() != 1)
                    err("a scalar compound literal needs one value", e,
                        "write (int){3}", "sema.compound-literal");
                NodeP v = e->list[0]->k == NK::Designator ? e->list[0]->a : e->list[0];
                std::string t = expr(v);
                if (t == "void")
                    err("compound literal value is void", v,
                        "use a value-returning expression", "sema.void-expression");
                e->k = NK::Cast;
                e->a = v;
                e->list.clear();
                return e->etype = ty;
            }
            case NK::Generic: {
                std::string t = expr(e->a);
                NodeP chosen;
                NodeP fallback;
                for (const NodeP& a : e->list) {
                    if (a->sval == "default") { fallback = a; continue; }
                    if (a->type == t) { chosen = a; break; }
                }
                if (!chosen) chosen = fallback;
                if (!chosen)
                    err("no _Generic association matches type '" + t + "'", e,
                        "add a matching type or a default: association",
                        "sema.generic-no-match");
                std::string rt = expr(chosen->a);
                e->k = NK::Cast;
                e->type.clear();
                e->a = chosen->a;
                return e->etype = rt;
            }
            case NK::Call:
                return e->etype = call(e);
            case NK::Unary:
                if (e->sval == "&") return e->etype = ptr_to(lvalue(e->a, false));
                if (e->sval == "*") {
                    std::string t = expr(e->a);
                    if (!is_ptr_type(t))
                        err("cannot dereference a non-pointer value", e,
                            "only int*/char* values can be dereferenced; use '&x' to take an address",
                            "sema.not-pointer");
                    return e->etype = ptr_base(t);
                }
                if (expr(e->a) == "void")
                    err("unary operator cannot be applied to a void expression", e,
                        "call a function that returns a value", "sema.void-expression");
                return e->etype = "int";
            case NK::IncDec:
                return e->etype = lvalue(e->a, true);
            case NK::Sizeof: {
                long long bytes = 0;
                if (!e->type.empty()) {
                    if (e->type == "void")
                        err("sizeof(void) has no meaning", e,
                            "use sizeof(int), sizeof(char) or sizeof(expr)",
                            "sema.sizeof-void");
                    bytes = type_bytes(e->type);
                } else if (e->a->k == NK::Ident) {
                    Sym* s = resolve(e->a->sval);
                    if (!s)
                        err(fmt("'%s' is not defined", e->a->sval.c_str()), e,
                            did_you_mean(e->a->sval, name_candidates()),
                            "sema.undefined-identifier");
                    if (s->kind == "array" || s->kind == "rom" || s->kind == "rom8")
                        bytes = (long long)s->elem_bytes * s->size;
                    else
                        bytes = type_bytes(s->vtype);
                } else {
                    std::string t = expr(e->a);
                    bytes = type_bytes(t);
                    if (e->a->k == NK::Str)
                        bytes = (long long)e->a->cps.size() + 1;  // the terminating NUL
                    else if (e->a->k == NK::Index && !e->a->dims.empty()) {
                        long long n = 1;
                        for (long long v : e->a->dims) n *= v;
                        bytes = n * type_bytes(e->a->type);
                    }
                }
                // Fold immediately: the rest of the compiler only sees a number.
                e->k = NK::Num;
                e->value = bytes;
                e->a.reset();
                return e->etype = "int";
            }
            case NK::Bin: {
                std::string ta = expr(e->a);
                std::string tb = expr(e->b);
                if (ta == "void" || tb == "void")
                    err("operator '" + e->sval + "' cannot be applied to a void expression", e,
                        "call a function that returns a value", "sema.void-expression");
                const std::string& op = e->sval;
                if (op == "+" || op == "-") {
                    bool pa = is_ptr_type(ta), pb = is_ptr_type(tb);
                    if (pa && pb) return e->etype = (op == "-") ? "int" : ta;
                    if (pa) return e->etype = ta;
                    if (pb) return e->etype = (op == "+") ? tb : "int";
                }
                return e->etype = "int";
            }
            case NK::Cond: {
                if (expr(e->a) == "void")
                    err("conditional expression has a void condition", e,
                        "use a value-returning expression", "sema.void-expression");
                std::string t = expr(e->b);
                std::string u = expr(e->c);
                if (t == "void" || u == "void")
                    err("conditional expression has a void branch", e,
                        "both branches must produce a value", "sema.void-expression");
                return e->etype = t;
            }
            case NK::Assign: {
                std::string t = lvalue(e->a, true);
                if (expr(e->b) == "void")
                    err("cannot assign a void expression", e,
                        "call a function that returns a value", "sema.void-expression");
                return e->etype = t;
            }
            default:
                err("unsupported expression", e);
        }
        return "";
    }

    // Work out the element type, stride and remaining dimensions of `base[i]`.
    // Fills e->type (element), e->dims (remaining dims), e->stride and e->etype.
    void analyze_index(const NodeP& e) {
        std::vector<long long> dims;
        std::string elem;
        int elem_bytes = 2;
        if (e->a->k == NK::Ident) {
            Sym* sym = resolve(e->a->sval);
            if (!sym) {
                if (an.funcs.count(e->a->sval))
                    err(fmt("'%s' is a function, not an array", e->a->sval.c_str()), e,
                        "call it with '" + e->a->sval + "()'", "sema.function-as-value");
                err(fmt("'%s' is not defined", e->a->sval.c_str()), e,
                    did_you_mean(e->a->sval, name_candidates()),
                    "sema.undefined-identifier");
            }
            if (sym->kind == "array" || sym->kind == "rom" || sym->kind == "rom8") {
                e->sym = sym;
                dims = sym->dims;
                if (dims.empty()) dims.push_back(sym->size);
                elem = elem_type(sym->elem_bytes);
                elem_bytes = sym->elem_bytes;
            } else {
                e->sym = sym;
                std::string t = expr(e->a);
                if (!is_ptr_type(t))
                    err(fmt("'%s' is not an array or pointer", e->a->sval.c_str()), e,
                        "index only arrays, pointers and ROM tables", "sema.not-indexable");
                e->via_ptr = true;
                e->type = ptr_base(t);
                e->stride = ptr_step(t);
                e->dims.clear();
                return;
            }
        } else if (e->a->k == NK::Index) {
            analyze_index(e->a);
            e->sym = e->a->sym;
            if (!e->a->dims.empty()) {
                dims = e->a->dims;
                elem = e->a->type;
            } else if (is_ptr_type(e->a->etype)) {
                e->via_ptr = true;
                e->type = ptr_base(e->a->etype);
                e->stride = ptr_step(e->a->etype);
                e->dims.clear();
                return;
            } else {
                err("can only index an array or pointer", e,
                    "too many [] levels for this object", "sema.not-indexable");
            }
            elem_bytes = type_bytes(elem);
        } else {
            std::string t = expr(e->a);
            if (!is_ptr_type(t))
                err("can only index an array or pointer", e,
                    "index only arrays, pointers and ROM tables", "sema.not-indexable");
            e->via_ptr = true;
            e->type = ptr_base(t);
            e->stride = ptr_step(t);
            e->dims.clear();
            return;
        }
        if (dims.empty())
            err("can only index an array or pointer", e,
                "too many [] levels for this object", "sema.not-indexable");
        long long stride = elem_bytes;
        for (size_t i = 1; i < dims.size(); i++) stride *= dims[i];
        dims.erase(dims.begin());
        e->stride = (int)stride;
        e->dims = dims;
        e->type = elem;
        e->etype = dims.empty() ? elem : "array";
    }

    // Resolve `base.member` / `base->member` and record the field offset.
    const StructField* member_field(const NodeP& e) {
        std::string base_type;
        if (e->kind == "->") {
            base_type = expr(e->a);
            if (!is_ptr_type(base_type))
                err("'->' needs a pointer to a struct/union", e,
                    "use '.' for a struct value, or take the address with '&'",
                    "sema.not-pointer");
            base_type = ptr_base(base_type);
        } else {
            base_type = lvalue(e->a, false);
        }
        if (!is_aggregate_type(base_type))
            err(fmt("member access needs a struct/union, got '%s'", base_type.c_str()), e,
                "declare the object as a struct/union first", "sema.not-aggregate");
        const StructField* f = find_field(base_type, e->sval);
        if (!f) {
            std::vector<std::string> names;
            if (const TypeDef* d = type_def(base_type))
                for (const auto& x : d->fields) names.push_back(x.name);
            err(fmt("'%s' has no member '%s'", base_type.c_str(), e->sval.c_str()), e,
                did_you_mean(e->sval, names), "sema.unknown-member");
        }
        if (f->bits)
            err("bitfield members are syntax-only on this target", e,
                "read/write the whole storage member instead", "sema.bitfield-access");
        if (f->flexible)
            err("a flexible array member is not directly accessible", e,
                "take the address of the struct and use a pointer past its fixed part",
                "sema.flexible-access");
        e->value = f->offset;
        return f;
    }

    std::string lvalue(const NodeP& e, bool writing) {
        if (e->k == NK::Ident) {
            Sym* sym = resolve(e->sval);
            if (!sym) {
                if (an.funcs.count(e->sval))
                    err(fmt("'%s' is a function, not a variable", e->sval.c_str()), e,
                        "call it with '" + e->sval + "()'", "sema.function-as-value");
                err(fmt("'%s' is not defined", e->sval.c_str()), e,
                    did_you_mean(e->sval, name_candidates()),
                    "sema.undefined-identifier");
            }
            if (sym->kind == "const") {
                if (writing)
                    err(fmt("cannot assign to constant '%s'", e->sval.c_str()), e,
                        "constants are read-only; use a variable if the value changes",
                        "sema.const-assign");
                err(fmt("cannot take the address of constant '%s'", e->sval.c_str()), e,
                    "copy the value into a variable first", "sema.const-address");
            }
            if (sym->kind == "array" || sym->kind == "rom" || sym->kind == "rom8") {
                if (writing)
                    err(fmt("cannot assign to '%s' (it is an array/table)", e->sval.c_str()), e,
                        "assign to an element instead, for example '" + e->sval + "[0] = value;'",
                        "sema.array-assign");
                if (sym->kind != "array")
                    err(fmt("cannot take the address of ROM table '%s' (ROM is read with peek/_rpeek)",
                            e->sval.c_str()),
                        e, "read a ROM table with index syntax or peek()/rpeek()",
                        "sema.rom-address");
                e->sym = sym;
                return e->etype = ptr_to(elem_type(sym->elem_bytes));
            }
            e->sym = sym;
            return e->etype = (sym->vtype.empty() ? "int" : sym->vtype);
        }
        if (e->k == NK::Index) {
            analyze_index(e);
            if (!e->via_ptr && e->sym && e->sym->kind != "array") {
                if (writing)
                    err(fmt("cannot write to read-only table '%s'", e->a->sval.c_str()), e,
                        "ROM tables are read-only; copy the value into a RAM array or variable",
                        "sema.rom-write");
                err(fmt("cannot take the address of ROM table '%s' (ROM is read with peek/_rpeek)",
                        e->a->sval.c_str()),
                    e, "read a ROM table with index syntax or peek()/rpeek()",
                    "sema.rom-address");
            }
            expr(e->b);
            if (!e->dims.empty()) {
                if (writing)
                    err("cannot assign to a whole array row", e,
                        "index one more level, for example a[i][j]", "sema.array-assign");
                return e->etype = ptr_to(e->type);
            }
            if (writing && is_aggregate_type(e->type))
                err("assigning whole struct values is not supported", e,
                    "assign each member, or copy through a pointer", "sema.aggregate-assign");
            return e->etype = e->type;
        }
        if (e->k == NK::CompoundLit) return expr(e);
        if (e->k == NK::Member) {
            const StructField* f = member_field(e);
            if (writing && is_aggregate_type(f->type))
                err("assigning whole struct members is not supported", e,
                    "assign each member of the nested struct", "sema.aggregate-assign");
            return e->etype = f->type;
        }
        if (e->k == NK::Unary && e->sval == "*") {
            std::string t = expr(e->a);
            if (!is_ptr_type(t))
                err("cannot dereference a non-pointer value", e,
                    "only int*/char* values can be dereferenced; use '&x' to take an address",
                    "sema.not-pointer");
            return e->etype = ptr_base(t);
        }
        err("cannot assign to this expression", e,
            "the left side must be a variable, array element or *pointer", "sema.not-assignable");
        return "";
    }

    std::string call(const NodeP& c) {
        const std::string& name = c->sval;
        if (name == "print" || name == "print_cw") {
            if ((int)c->list.size() < PRINT_MIN)
                err(name + "(x, y, value, ...) needs at least 3 arguments", c);
            for (auto& a : c->list) expr(a);
            c->kind = name;
            return "void";
        }
        if (name == "__va_start") {
            if (!fn || !fn->variadic || !fn->va_array)
                err("__va_start is only valid inside a variadic function", c,
                    "use va_start(ap, last) in a function declared with '...'",
                    "sema.va-start");
            c->kind = "va_start";
            return "int";
        }
        auto bit = CW_BUILTINS.find(name);
        if (bit != CW_BUILTINS.end()) {
            const BuiltinInfo& bi = bit->second;
            int lo = bi.lo, hi = bi.hi;
            int n = (int)c->list.size();
            if (!(lo <= n && n <= hi)) {
                std::string expected = (lo == hi) ? fmt("%d", lo) : fmt("%d-%d", lo, hi);
                if (lo == hi)
                    err(fmt("%s() takes %d argument%s, got %d (expected %s)", name.c_str(), lo,
                            lo == 1 ? "" : "s", n, bi.sig),
                        c, fmt("pass exactly %s argument(s): %s", expected.c_str(), bi.sig),
                        "sema.argument-count");
                err(fmt("%s() takes %d-%d arguments, got %d (expected %s)", name.c_str(), lo, hi,
                        n, bi.sig),
                    c, fmt("pass %s argument(s): %s", expected.c_str(), bi.sig),
                    "sema.argument-count");
            }
            if (name == "exit" && target.rom_seg == 0)
                err("exit() exists only for the cwapp target", c,
                    "use it only in a #cwapp program", "sema.builtin-target");
            for (size_t i = 0; i < c->list.size(); i++) {
                NodeP a = c->list[i];
                std::string t = expr(a);
                if (t == "void")
                    err("a void function cannot be used as a call argument", a,
                        "remove the call, or call a function that returns a value",
                        "sema.void-expression");
                bool addr_ok = (bi.addr_args >> i) & 1u;
                if (!addr_ok && a->k == NK::Str)
                    err(fmt("%s() argument %d expects int, got a string", name.c_str(),
                            (int)i + 1),
                        a, fmt("%s: pass a 16-bit integer here", bi.sig),
                        "sema.argument-type");
            }
            c->kind = "builtin";
            c->builtin = name;
            return "int";
        }
        auto fit = an.funcs.find(name);
        if (fit != an.funcs.end()) {
            Func* f = fit->second;
            int required = 0;
            for (auto& p : f->params)
                if (!p->a) required++;
            int total = (int)f->params.size();
            int n = (int)c->list.size();
            if (!(f->variadic ? (n >= required) : (required <= n && n <= total))) {
                if (required == total)
                    err(fmt("%s() takes %d argument%s, got %d", name.c_str(), total,
                            total == 1 ? "" : "s", n),
                        c, fmt("pass exactly %d argument(s)", total), "sema.argument-count");
                err(fmt("%s() takes %d-%d arguments, got %d", name.c_str(), required, total, n),
                    c, fmt("pass %d-%d argument(s)", required, total),
                    "sema.argument-count");
            }
            for (auto& a : c->list) {
                if (expr(a) == "void")
                    err("a void function cannot be used as a call argument", a,
                        "remove the call, or call a function that returns a value",
                        "sema.void-expression");
            }
            c->kind = "func";
            c->func = f;
            return f->ret.empty() ? "int" : f->ret;
        }
        if (Sym* s = resolve(name)) {
            if (s->vtype != "fnptr")
                err(fmt("'%s' is not a function", name.c_str()), c,
                    "call a function or a function pointer", "sema.unknown-function");
            for (auto& a : c->list)
                if (expr(a) == "void")
                    err("a void function cannot be used as a call argument", a,
                        "remove the call, or call a value-returning function",
                        "sema.void-expression");
            expr(c->a);
            c->kind = "indirect";
            return "int";
        }
        err(fmt("'%s' is not a function", name.c_str()), c,
            did_you_mean(name, name_candidates()), "sema.unknown-function");
        return "";
    }

    void mark_recursion() {
        // A function is recursive when it can reach itself through any chain
        // of calls, not only through a direct self-call.
        std::function<bool(const std::string&, const std::string&, std::set<std::string>&)> reaches =
            [&](const std::string& at, const std::string& target, std::set<std::string>& seen) {
                if (at == target) return true;
                if (!seen.insert(at).second) return false;
                auto it = call_graph.find(at);
                if (it == call_graph.end()) return false;
                for (const std::string& nx : it->second)
                    if (reaches(nx, target, seen)) return true;
                return false;
            };
        for (auto& kv : an.funcs) {
            auto it = call_graph.find(kv.first);
            if (it == call_graph.end()) continue;
            std::set<std::string> seen;
            for (const std::string& callee : it->second)
                if (reaches(callee, kv.first, seen)) {
                    kv.second->recursive = true;
                    break;
                }
        }
    }
};

// =================================================================== codegen
static const std::map<std::string, std::string> CW_INV_JUMP = {
    {"==", "JNE"}, {"!=", "JEQ"}, {"<", "JGES"}, {">", "JLES"}, {"<=", "JGTS"}, {">=", "JLTS"}};
static const std::map<std::string, std::string> CW_TRUE_JUMP = {
    {"==", "JEQ"}, {"!=", "JNE"}, {"<", "JLTS"}, {">", "JGTS"}, {"<=", "JLES"}, {">=", "JGES"}};
static const std::map<std::string, std::string> CW_BUILTIN_LABEL = {
    {"_rpeek", "__rpeek"}, {"render", "__render"}, {"renderStatus", "__renderStatus"}};

class Codegen {
public:
    explicit Codegen(Analyzed& an) : an(an), target(*an.target) { allocate(); }

    std::string generate() {
        label("__wiz_entry");
        // The firmware target has no linked CWiz runtime. Its startup code is
        // recovered/defined by the firmware source itself; do not emit a call
        // to the runtime's __rt_init entry point.
        if (target.name != "fwnx") emit("BL __rt_init");
        emit("BL __init");
        if (an.has_main) emit("BL f_main");
        label("__wiz_halt");
        emit("B __wiz_halt");

        label("__init");
        emit("PUSH LR");
        for (auto& gi : global_inits) {
            ex(gi.second);
            if (type_bytes(gi.first->vtype) == 1)
                emit(fmt("ST R0, %d", gi.first->addr));
            else
                emit(fmt("ST ER0, %d", gi.first->addr));
        }
        for (Sym* sym : an.globals)
            if (sym->init_kind == 4) emit_aggregate_init(sym, sym->init_list);
        for (Sym* sym : an.arrays) emit_array_init(sym);
        emit("MOV ER0, #0");
        emit("POP PC");

        for (auto& name : an.func_order) {
            lines.push_back("");
            emit_func(an.funcs[name]);
        }
        emit_data();
        std::string out;
        for (size_t i = 0; i < lines.size(); i++) {
            if (i) out += "\n";
            out += lines[i];
        }
        return out + "\n";
    }

private:
    Analyzed& an;
    const Target& target;
    std::vector<std::string> lines;
    int nlabel = 0;
    std::vector<std::pair<std::string, std::vector<uint8_t>>> str_order;
    int next_addr = 0;
    Func* fn = nullptr;
    std::map<std::string, int> tmp;
    std::vector<std::pair<Sym*, NodeP>> global_inits;
    std::vector<std::pair<std::string, std::string>> loops;  // (continue, break)
    std::vector<std::unique_ptr<Sym>> tmp_syms;
    int switch_tmp = 0;

    void allocate() {
        next_addr = target.var_base;
        for (Sym* sym : an.globals)
            alloc(sym, type_bytes(sym->vtype), "global '" + sym->name + "'");
        for (Sym* sym : an.arrays) alloc(sym, sym->elem_bytes * sym->size, "array '" + sym->name + "'");
        for (auto& name : an.func_order)
            for (Sym* sym : an.funcs[name]->locals) {
                const int bytes = sym->kind == "array" ? sym->elem_bytes * sym->size
                                                       : type_bytes(sym->vtype);
                alloc(sym, bytes, "local '" + sym->qname + "'");
            }
        for (Sym* sym : an.globals)
            if (sym->init_kind == 1) global_inits.push_back({sym, sym->init_node});
    }

    int alloc(Sym* sym, int size_bytes, const std::string& what) {
        if (size_bytes < 0) throw CwizError("invalid RAM allocation size for " + what);
        int a = align_up(next_addr, sym ? std::max(1, sym->align) : 1);
        bool moved;
        do {
            moved = false;
            for (const auto& [reserved_start, reserved_end] : target.reserved_ram) {
                const int allocation_end = a + size_bytes - 1;
                if (size_bytes > 0 && a <= reserved_end && allocation_end >= reserved_start) {
                    a = reserved_end + 1;
                    moved = true;
                    break;
                }
            }
        } while (moved);
        next_addr = a + size_bytes;
        if (next_addr > target.var_limit)
            throw CwizError("out of RAM for " + what, 0, 0,
                            "use smaller arrays, fewer globals, or move constant data into rom/rom8",
                            "memory.out-of-ram");
        if (sym) sym->addr = a;
        return a;
    }

    int temp_addr(const std::string& key) {
        auto it = tmp.find(key);
        if (it != tmp.end()) return it->second;
        int a = alloc(nullptr, 2, "variables");
        tmp[key] = a;
        return a;
    }

    void emit(const std::string& s) { lines.push_back("    " + s); }
    void label(const std::string& n) { lines.push_back(n + ":"); }
    std::string new_label() { return "L" + std::to_string(++nlabel); }

    void const_(int reg, long long v) {
        v = wrap16(v);
        if (v >= -64 && v <= 63) {
            emit(fmt("MOV ER%d, #%lld", reg, v));
        } else {
            emit(fmt("MOV R%d, #%lld", reg, v & 0xFF));
            emit(fmt("MOV R%d, #%lld", reg + 1, (v >> 8) & 0xFF));
        }
    }
    void load_label(int reg, const std::string& lbl) {
        emit(fmt("MOV R%d, #(%s & 255)", reg, lbl.c_str()));
        emit(fmt("MOV R%d, #((%s >> 8) & 255)", reg + 1, lbl.c_str()));
    }

    // -- const eval for table data ------------------------------------
    long long ev(const NodeP& node) {
        if (node->k == NK::Num) return wrap16(node->value);
        if (node->k == NK::Unary) {
            long long v = ev(node->a);
            if (node->sval == "-") return wrap16(-v);
            if (node->sval == "+") return v;
            if (node->sval == "~") return wrap16(~v);
            if (node->sval == "!") return v ? 0 : 1;
        }
        if (node->k == NK::Bin) return fold2(node->sval, ev(node->a), ev(node->b));
        if (node->k == NK::Ident) {
            auto it = an.consts.find(node->sval);
            if (it != an.consts.end()) return wrap16(it->second);
        }
        throw CwizError("initializer is not a constant expression", node->line, node->col,
                        "use literals and const values; runtime expressions cannot initialise ROM/arrays",
                        "sema.constant");
    }
    long long fold2(const std::string& op, long long a, long long b) {
        if (op == "+") return wrap16(a + b);
        if (op == "-") return wrap16(a - b);
        if (op == "*") return wrap16(a * b);
        if (op == "&") return wrap16(a & b);
        if (op == "|") return wrap16(a | b);
        if (op == "^") return wrap16(a ^ b);
        if (op == "/" || op == "%") {
            if (b == 0) return 0;
            long long q = std::llabs(a) / std::llabs(b);
            if ((a < 0) != (b < 0)) q = -q;
            return wrap16(op == "/" ? q : a - q * b);
        }
        if (op == "<<") return (b >= 0 && b <= 15) ? wrap16(a << (b & 15)) : 0;
        if (op == ">>") return (b >= 0 && b <= 15) ? wrap16(a >> (b & 15)) : 0;
        return 0;
    }

    // -- leaves -------------------------------------------------------
    static bool is_leaf(const NodeP& e) {
        return e->k == NK::Num || e->k == NK::Str || e->k == NK::Ident;
    }
    void load(int reg, const NodeP& e) {
        if (e->k == NK::Num) const_(reg, e->value);
        else if (e->k == NK::Str) load_label(reg, intern(e->cps, e));
        else if (e->k == NK::Ident) {
            Sym* sym = e->sym;
            if (!sym) {
                auto it = an.funcs.find(e->sval);
                if (it != an.funcs.end()) { load_label(reg, it->second->label); return; }
                throw CwizError("internal: unresolved identifier '" + e->sval + "'", e->line);
            }
            if (sym->kind == "const") const_(reg, sym->value);
            else if (sym->kind == "var" || sym->kind == "param") {
                if (type_bytes(sym->vtype) == 1) {
                    emit(fmt("L R%d, %d", reg, sym->addr));
                    emit(fmt("MOV R%d, #0", reg + 1));
                } else {
                    emit(fmt("L ER%d, %d", reg, sym->addr));
                }
            }
            else if (sym->kind == "array") const_(reg, sym->addr);
            else load_label(reg, "__d_" + sym->name);
        } else {
            throw CwizError("internal: load() got a non-leaf", e->line);
        }
    }
    void operands(const NodeP& l, const NodeP& r) {
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

    // -- expressions --------------------------------------------------
    void ex(const NodeP& e) {
        switch (e->k) {
            case NK::Num:
            case NK::Str:
            case NK::Ident:
                load(0, e);
                return;
            case NK::Index:
                ex_index(e);
                return;
            case NK::Member:
                ex_member(e);
                return;
            case NK::Cast:
                ex(e->a);
                if (e->type == "char") {
                    emit("AND R0, #255");
                    emit("MOV R1, #0");
                }
                return;
            case NK::CompoundLit:
                emit_aggregate_init(e->sym, e->list);
                const_(0, e->sym->addr);
                return;
            case NK::Unary:
                if (e->sval == "*") {
                    ex_deref(e);
                    return;
                }
                if (e->sval == "&") {
                    ex_addr(e->a);
                    return;
                }
                ex_unary(e);
                return;
            case NK::IncDec:
                ex_incdec(e);
                return;
            case NK::Bin:
                ex_bin(e);
                return;
            case NK::Cond: {
                std::string other = new_label(), end = new_label();
                branch(e->a, other, false);
                ex(e->b);
                emit("B " + end);
                label(other);
                ex(e->c);
                label(end);
                return;
            }
            case NK::Assign:
                ex_assign(e);
                return;
            case NK::Call:
                ex_call(e);
                return;
            default:
                throw CwizError("internal: cannot generate expression", e->line);
        }
    }

    void ex_unary(const NodeP& e) {
        if (e->sval == "!") {
            materialize(e);
            return;
        }
        ex(e->a);
        if (e->sval == "-") {
            emit("MOV ER2, ER0");
            emit("MOV ER0, #0");
            emit("SUB R0, R2");
            emit("SUBC R1, R3");
        } else if (e->sval == "~") {
            emit("XOR R0, #255");
            emit("XOR R1, #255");
        }
    }

    void ex_incdec(const NodeP& e) {
        int step = is_ptr_type(e->a->etype) ? ptr_step(e->a->etype) : 1;
        ex(e->a);
        if (!e->prefix) emit("PUSH ER0");
        const_(2, step);
        if (e->sval == "++") emit("ADD ER0, ER2");
        else {
            emit("SUB R0, R2");
            emit("SUBC R1, R3");
        }
        store(e->a);
        if (!e->prefix) emit("POP ER0");
    }

    void ex_bin(const NodeP& e) {
        const std::string& op = e->sval;
        if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=" ||
            op == "&&" || op == "||") {
            materialize(e);
            return;
        }
        if (op == "+" || op == "-") {
            bool pa = is_ptr_type(e->a->etype), pb = is_ptr_type(e->b->etype);
            if (pa && pb) {  // pointer difference: scale back down
                operands(e->a, e->b);
                apply("-");
                if (ptr_step(e->a->etype) == 2) {
                    const_(2, 2);
                    emit("BL __div16");
                }
                return;
            }
            if (pa) {  // pointer + / - integer
                ex(e->a);
                emit("PUSH ER0");
                ex(e->b);
                if (ptr_step(e->a->etype) == 2) emit("ADD ER0, ER0");
                emit("MOV ER2, ER0");
                emit("POP ER0");
                apply(op);
                return;
            }
            if (pb && op == "+") {  // integer + pointer
                ex(e->b);
                emit("PUSH ER0");
                ex(e->a);
                if (ptr_step(e->b->etype) == 2) emit("ADD ER0, ER0");
                emit("MOV ER2, ER0");
                emit("POP ER0");
                emit("ADD ER0, ER2");
                return;
            }
        }
        operands(e->a, e->b);
        apply(op);
    }

    void apply(const std::string& op) {
        if (op == "+") emit("ADD ER0, ER2");
        else if (op == "-") { emit("SUB R0, R2"); emit("SUBC R1, R3"); }
        else if (op == "&") { emit("AND R0, R2"); emit("AND R1, R3"); }
        else if (op == "|") { emit("OR R0, R2"); emit("OR R1, R3"); }
        else if (op == "^") { emit("XOR R0, R2"); emit("XOR R1, R3"); }
        else if (op == "*") emit("BL __mul16");
        else if (op == "/") emit("BL __div16");
        else if (op == "%") { emit("BL __div16"); emit("MOV ER0, ER2"); }
        else if (op == "<<") emit("BL __shl16");
        else if (op == ">>") emit("BL __sra16");
        else throw CwizError("internal: unknown operator " + op);
    }

    void materialize(const NodeP& e) {
        std::string lfalse = new_label(), end = new_label();
        branch(e, lfalse, false);
        const_(0, 1);
        emit("B " + end);
        label(lfalse);
        const_(0, 0);
        label(end);
    }

    void branch(const NodeP& e, const std::string& lbl, bool jump_if) {
        if (e->k == NK::Bin &&
            (e->sval == "==" || e->sval == "!=" || e->sval == "<" || e->sval == ">" ||
             e->sval == "<=" || e->sval == ">=")) {
            operands(e->a, e->b);
            emit("CMP R0, R2");
            emit("CMPC R1, R3");
            const auto& m = jump_if ? CW_TRUE_JUMP : CW_INV_JUMP;
            emit(m.at(e->sval) + " " + lbl);
            return;
        }
        if (e->k == NK::Bin && e->sval == "&&") {
            if (jump_if) {
                std::string skip = new_label();
                branch(e->a, skip, false);
                branch(e->b, lbl, true);
                label(skip);
            } else {
                branch(e->a, lbl, false);
                branch(e->b, lbl, false);
            }
            return;
        }
        if (e->k == NK::Bin && e->sval == "||") {
            if (jump_if) {
                branch(e->a, lbl, true);
                branch(e->b, lbl, true);
            } else {
                std::string skip = new_label();
                branch(e->a, skip, true);
                branch(e->b, lbl, false);
                label(skip);
            }
            return;
        }
        if (e->k == NK::Unary && e->sval == "!") {
            branch(e->a, lbl, !jump_if);
            return;
        }
        if (e->k == NK::Num) {
            if ((e->value != 0) == jump_if) emit("B " + lbl);
            return;
        }
        ex(e);
        emit("OR R0, R1");
        emit(std::string(jump_if ? "JNE " : "JEQ ") + lbl);
    }

    void ex_assign(const NodeP& e) {
        if (e->sval == "=") {
            ex(e->b);
            store(e->a);
            return;
        }
        ex(e->a);
        emit("PUSH ER0");
        ex(e->b);
        if (is_ptr_type(e->a->etype) && ptr_step(e->a->etype) == 2 &&
            (e->sval == "+=" || e->sval == "-="))
            emit("ADD ER0, ER0");  // p += n moves by n elements
        emit("MOV ER2, ER0");
        emit("POP ER0");
        apply(e->sval.substr(0, e->sval.size() - 1));
        store(e->a);
    }

    void store(const NodeP& target) {
        if (target->k == NK::Ident) {
            const std::string ty = target->etype.empty() ? target->sym->vtype : target->etype;
            if (type_bytes(ty) == 1)
                emit(fmt("ST R0, %d", target->sym->addr));
            else
                emit(fmt("ST ER0, %d", target->sym->addr));
            return;
        }
        if (target->k == NK::Unary && target->sval == "*") {
            // ER0 holds the value; the pointer comes from the operand.
            emit("PUSH ER0");
            ex(target->a);
            emit("MOV ER2, ER0");
            emit("POP ER0");
            emit(indirect_store(target->etype));
            return;
        }
        emit("PUSH ER0");
        if (target->k == NK::Member) member_addr(target);
        else index_addr(target);
        emit("POP ER0");
        emit(indirect_store(target->etype));
    }

    static std::string indirect_store(const std::string& t) {
        return type_bytes(t) == 1 ? "ST R0, [ER2]" : "ST ER0, [ER2]";
    }
    static std::string indirect_load(const std::string& t) {
        return type_bytes(t) == 1 ? "L R0, [ER2]" : "L ER0, [ER2]";
    }

    // ER0 = value; the address of `e->a` goes to ER2. Works for RAM arrays and for
    // pointer-valued expressions (`p`, `p + n`, `arr`).
    void ex_addr(const NodeP& t) {
        if (t->k == NK::Ident) {
            const_(0, t->sym->addr);
            return;
        }
        if (t->k == NK::CompoundLit) {
            emit_aggregate_init(t->sym, t->list);
            const_(0, t->sym->addr);
            return;
        }
        if (t->k == NK::Unary && t->sval == "*") {  // &*p == p
            ex(t->a);
            return;
        }
        if (t->k == NK::Member) {
            member_addr(t);  // leaves the address in ER2
            emit("MOV ER0, ER2");
            return;
        }
        index_addr(t);  // leaves the address in ER2
        emit("MOV ER0, ER2");
    }

    // ER2 = address of a member.  The base is an lvalue (dot) or a pointer
    // expression (arrow).
    void member_addr(const NodeP& e) {
        if (e->kind == "->") {
            ex(e->a);
        } else {
            ex_addr(e->a);
        }
        if (e->value != 0) {
            const_(2, (int)e->value);
            emit("ADD ER0, ER2");
        }
        emit("MOV ER2, ER0");
    }

    void ex_member(const NodeP& e) {
        member_addr(e);
        if (type_bytes(e->etype) == 1) {
            emit("L R0, [ER2]");
            emit("MOV R1, #0");
        } else {
            emit("L ER0, [ER2]");
        }
    }

    void ex_deref(const NodeP& e) {
        ex(e->a);
        emit("MOV ER2, ER0");
        emit(indirect_load(e->etype));
        if (type_bytes(e->etype) == 1) emit("MOV R1, #0");
    }

    void scale_index(const NodeP& e) {
        const int step = e->stride > 0 ? e->stride : 1;
        if (step == 2) {
            emit("ADD ER0, ER0");
        } else if (step > 2) {
            const_(2, step);
            emit("BL __mul16");
        }
    }

    void index_addr(const NodeP& e) {
        if (e->via_ptr) {
            // address = pointer_value + stride * index
            ex(e->b);
            scale_index(e);
            emit("PUSH ER0");
            ex(e->a);
            emit("MOV ER2, ER0");
            emit("POP ER0");
            emit("ADD ER0, ER2");
            emit("MOV ER2, ER0");
            return;
        }
        if (e->a->k == NK::Index) {
            index_addr(e->a);  // ER2 = base row/plane address
            emit("PUSH ER2");
            ex(e->b);
            scale_index(e);
            emit("POP ER2");
            emit("ADD ER0, ER2");
            emit("MOV ER2, ER0");
            return;
        }
        Sym* sym = e->sym;
        ex(e->b);
        scale_index(e);
        if (sym->kind == "array") const_(2, sym->addr);
        else load_label(2, "__d_" + sym->name);
        emit("ADD ER0, ER2");
        emit("MOV ER2, ER0");
    }

    void ex_index(const NodeP& e) {
        Sym* sym = e->sym;
        index_addr(e);
        if (!e->dims.empty()) {  // an array row decays to its address
            emit("MOV ER0, ER2");
            return;
        }
        if (!e->via_ptr && sym && (sym->kind == "rom" || sym->kind == "rom8") && target.rom_seg)
            emit("DSRI #3");
        if (type_bytes(e->type) == 1) {
            emit("L R0, [ER2]");
            emit("MOV R1, #0");
        } else {
            emit("L ER0, [ER2]");
        }
    }

    // -- calls --------------------------------------------------------
    void ex_call(const NodeP& c) {
        if (c->kind == "print") { ex_print(c, "text", "num"); return; }
        if (c->kind == "print_cw") { ex_print(c, "text_cw", "num_cw"); return; }
        if (c->kind == "va_start") {
            const_(0, fn && fn->va_array ? fn->va_array->addr : 0);
            return;
        }
        if (c->kind == "builtin") { ex_builtin(c); return; }
        if (c->kind == "indirect") {
            for (auto& a : c->list) {
                ex(a);
                emit("PUSH ER0");
            }
            ex(c->a);
            emit("MOV ER2, ER0");
            emit("BL ER2");
            return;
        }
        emit_call(c->func, c->list);
    }

    std::vector<NodeP> effective_args(Func* f, const std::vector<NodeP>& args) {
        std::vector<NodeP> out = args;
        for (size_t i = args.size(); i < f->params.size(); i++) {
            NodeP p = f->params[i];
            NodeP n = mk(NK::Num, p->line, p->col);
            n->value = p->default_val;
            out.push_back(n);
        }
        return out;
    }

    void emit_call(Func* f, const std::vector<NodeP>& args) {
        bool recursive_here = f->recursive && fn == f;
        if (recursive_here) save_locals(f);
        std::vector<NodeP> effective = effective_args(f, args);
        for (auto& a : effective) {
            ex(a);
            emit("PUSH ER0");
        }
        if (f->variadic) {
            const_(0, (long long)effective.size());
            emit("PUSH ER0");
        }
        // The callee pops its parameters in emit_func(); this uniform stack
        // convention also makes indirect calls through function pointers work.
        emit("BL " + f->label);
        if (recursive_here) restore_locals(f);
    }

    int param_addr(Func* f, const NodeP& p) {
        for (Sym* sym : f->locals)
            if (sym->kind == "param" && sym->name == p->sval) return sym->addr;
        throw CwizError("internal: parameter " + p->sval + " not allocated");
    }

    void save_locals(Func* f) {
        std::vector<Sym*> ls = f->locals;
        std::sort(ls.begin(), ls.end(), [](Sym* a, Sym* b) { return a->addr < b->addr; });
        for (Sym* sym : ls) {
            emit(fmt("L ER2, %d", sym->addr));
            emit("PUSH ER2");
        }
    }
    void restore_locals(Func* f) {
        std::vector<Sym*> ls = f->locals;
        std::sort(ls.begin(), ls.end(), [](Sym* a, Sym* b) { return a->addr > b->addr; });
        for (Sym* sym : ls) {
            emit("POP ER2");
            emit(fmt("ST ER2, %d", sym->addr));
        }
    }

    void ex_builtin(const NodeP& c) {
        const std::string& name = c->builtin;
        for (auto& a : c->list) {
            ex(a);
            emit("PUSH ER0");
        }
        for (int i = (int)c->list.size() - 1; i >= 0; i--) emit(fmt("POP ER%d", i * 2));
        if (name == "pixel" && c->list.size() == 2) const_(4, 1);
        if (name == "_rpeek" && !target.rom_seg) emit("BL __peek");
        else {
            auto it = CW_BUILTIN_LABEL.find(name);
            std::string bare = name;
            while (!bare.empty() && bare[0] == '_') bare.erase(bare.begin());
            emit("BL " + (it != CW_BUILTIN_LABEL.end() ? it->second : "__" + bare));
        }
    }

    void ex_print(const NodeP& c, const char* text_fn, const char* num_fn) {
        NodeP x = c->list[0], y = c->list[1];
        int px = temp_addr("print.x"), py = temp_addr("print.y");
        ex(x);
        emit(fmt("ST ER0, %d", px));
        ex(y);
        emit(fmt("ST ER0, %d", py));
        for (size_t i = 2; i < c->list.size(); i++) {
            NodeP v = c->list[i];
            Func* f = an.funcs[is_text(v) ? text_fn : num_fn];
            std::vector<NodeP> args = {tmp_ident(px, v), tmp_ident(py, v), v};
            emit_call(f, args);
            emit(fmt("ST ER0, %d", px));
        }
    }

    NodeP tmp_ident(int addr, const NodeP& like) {
        tmp_syms.push_back(std::make_unique<Sym>());
        Sym* s = tmp_syms.back().get();
        s->kind = "var";
        s->name = "__tmp";
        s->qname = "__tmp";
        s->addr = addr;
        NodeP n = mk(NK::Ident, like->line, like->col);
        n->sval = "__tmp";
        n->sym = s;
        return n;
    }

    static bool is_text(const NodeP& v) {
        if (v->k == NK::Str) return true;
        if (v->k == NK::Ident && v->sym) return v->sym->kind == "rom8";
        // normalizeText() returns a char pointer, so print_cw() must lay it
        // out with text_cw rather than num_cw.
        if (v->k == NK::Call && v->sval == "normalizeText") return true;
        return false;
    }

    // -- strings / data -----------------------------------------------
    std::string intern(const std::vector<uint32_t>& cps, const NodeP& node) {
        std::vector<uint8_t> bs;
        for (uint32_t cp : cps) {
            if (cp > 0xFF)
                throw CwizError(fmt("string contains a non-byte character (0x%X)", cp),
                               node ? node->line : 0, node ? node->col : 0);
            bs.push_back((uint8_t)cp);
        }
        for (auto& kv : str_order)
            if (kv.second == bs) return kv.first;
        std::string lbl = "__s" + std::to_string(str_order.size());
        str_order.push_back({lbl, bs});
        return lbl;
    }

    void emit_data() {
        if (!str_order.empty()) {
            lines.push_back("");
            emit("; --- strings ---");
            for (auto& kv : str_order) {
                const std::string& lbl = kv.first;
                const std::vector<uint8_t>& bs = kv.second;
                std::string vals;
                if (bs.empty()) vals = "0";
                else {
                    for (size_t i = 0; i < bs.size(); i++) {
                        if (i) vals += ", ";
                        vals += std::to_string(bs[i]);
                    }
                    vals += ", 0";
                }
                label(lbl);
                emit("DB " + vals);
                if ((bs.size() + 1) % 2) emit("DB 0");
            }
        }
        for (Sym* sym : an.tables) {
            lines.push_back("");
            emit("; --- table " + sym->name + " ---");
            emit_table(sym);
        }
    }

    void emit_table(Sym* sym) {
        label("__d_" + sym->name);
        std::vector<long long> vals;
        if (sym->init_kind == 3) {
            std::vector<long long> v;
            for (uint32_t ch : sym->init_cps) v.push_back((long long)(ch & 0xFF));
            v.resize(std::max<size_t>(v.size(), sym->size), 0);
            v.resize(sym->size);
            vals = v;
        } else if (sym->init_kind == 2) {
            if (sym->elem_bytes == 1)
                for (auto& n : sym->init_list) vals.push_back(ev(n) & 0xFF);
            else
                for (auto& n : sym->init_list) vals.push_back(ev(n));
        } else {
            vals.assign(sym->size, 0);
        }
        if (sym->elem_bytes == 1) {
            const size_t rows = 12;
            for (size_t i = 0; i < vals.size(); i += rows) {
                std::string s = "DB ";
                for (size_t j = i; j < vals.size() && j < i + rows; j++) {
                    if (j > i) s += ", ";
                    s += std::to_string(vals[j]);
                }
                emit(s);
            }
            if (vals.size() % 2) emit("DB 0");
        } else {
            const size_t rows = 8;
            for (auto& v : vals) v = wrap16(v);
            for (size_t i = 0; i < vals.size(); i += rows) {
                std::string s = "DW ";
                for (size_t j = i; j < vals.size() && j < i + rows; j++) {
                    if (j > i) s += ", ";
                    s += std::to_string(vals[j]);
                }
                emit(s);
            }
        }
    }

    // -- functions ----------------------------------------------------
    void emit_func(Func* f) {
        fn = f;
        label(f->label);
        if (f->variadic && f->va_count && f->va_array) {
            emit("POP ER0");
            emit(fmt("ST ER0, %d", f->va_count->addr));
            // ER2 = number of extra arguments.
            emit(fmt("L ER2, %d", f->va_count->addr));
            const_(0, (long long)f->params.size());
            emit("SUB R2, R0");
            emit("SUBC R3, R1");
            // ER4 = array + extra*2 - 2, filled from the top of the stack.
            emit("MOV ER4, ER2");
            emit("ADD ER4, ER4");
            const_(0, f->va_array->addr);
            emit("ADD ER4, ER0");
            emit("ADD ER4, #-2");
            std::string loop = new_label(), done = new_label();
            label(loop);
            emit("MOV R0, R2");
            emit("OR R0, R3");
            emit("BEQ " + done);
            emit("POP ER0");
            emit("ST ER0, [ER4]");
            emit("ADD ER4, #-2");
            emit("ADD ER2, #-1");
            emit("B " + loop);
            label(done);
            const_(0, f->va_array->addr);
            emit("ST ER0, 0E806H");
        }
        for (auto it = f->params.rbegin(); it != f->params.rend(); ++it) {
            emit("POP ER0");
            if (type_bytes((*it)->type) == 1)
                emit(fmt("ST R0, %d", param_addr(f, *it)));
            else
                emit(fmt("ST ER0, %d", param_addr(f, *it)));
        }
        emit("PUSH LR");
        stmt(f->body);
        emit("MOV ER0, #0");
        label(f->label + "__ret");
        emit("POP PC");
        fn = nullptr;
    }

    void stmt(const NodeP& s) {
        switch (s->k) {
            case NK::Block:
                for (auto& st : s->list) stmt(st);
                return;
            case NK::DeclStmt:
                for (const NodeP& vd : s->list) {
                    if (vd->k == NK::ArrayDecl) {
                        if (vd->sym) emit_local_array_init(vd, vd->sym);
                    } else if (vd->k == NK::ConstDecl) {
                        // compile-time only
                    } else if (!vd->list.empty()) {
                        emit_aggregate_init(vd->sym, vd->list);
                    } else if (vd->a) {
                        ex(vd->a);
                        const std::string ty = vd->sym ? vd->sym->vtype : vd->type;
                        if (type_bytes(ty) == 1)
                            emit(fmt("ST R0, %d", addr_of_local(vd->sval)));
                        else
                            emit(fmt("ST ER0, %d", addr_of_local(vd->sval)));
                    }
                }
                if (s->a && s->a->a) {
                    ex(s->a->a);
                    emit(fmt("ST ER0, %d", addr_of_local(s->a->sval)));
                }
                return;
            case NK::ExprStmt:
                ex(s->a);
                return;
            case NK::If: {
                std::string other = new_label(), end = new_label();
                branch(s->a, other, false);
                stmt(s->b);
                emit("B " + end);
                label(other);
                if (s->c) stmt(s->c);
                label(end);
                return;
            }
            case NK::While: {
                std::string top = new_label(), end = new_label();
                loops.push_back({top, end});
                label(top);
                branch(s->a, end, false);
                stmt(s->b);
                emit("B " + top);
                label(end);
                loops.pop_back();
                return;
            }
            case NK::DoWhile: {
                std::string top = new_label(), cont = new_label(), end = new_label();
                loops.push_back({cont, end});
                label(top);
                stmt(s->a);
                label(cont);
                branch(s->b, top, true);
                label(end);
                loops.pop_back();
                return;
            }
            case NK::For: {
                if (s->a) stmt(s->a);
                std::string top = new_label(), cont = new_label(), end = new_label();
                loops.push_back({cont, end});
                label(top);
                if (s->b) branch(s->b, end, false);
                stmt(s->d);
                label(cont);
                if (s->c) ex(s->c);
                emit("B " + top);
                label(end);
                loops.pop_back();
                return;
            }
            case NK::Switch: {
                // Evaluate the selector once, then dispatch to the matching
                // case label.  Bodies follow in source order, so C fallthrough
                // works without extra jumps; break targets the shared end.
                const int tmp = temp_addr("switch." + std::to_string(++switch_tmp));
                ex(s->a);
                emit(fmt("ST ER0, %d", tmp));
                const std::string end = new_label();
                std::vector<std::string> labels;
                std::string default_label = end;
                for (const NodeP& c : s->list) {
                    labels.push_back(new_label());
                    if (c->k == NK::Case && c->sval == "default")
                        default_label = labels.back();
                }
                for (size_t i = 0; i < s->list.size(); i++) {
                    const NodeP& c = s->list[i];
                    if (c->sval == "default") continue;
                    emit(fmt("L ER0, %d", tmp));
                    const_(2, c->value);
                    emit("CMP R0, R2");
                    emit("CMPC R1, R3");
                    emit("JEQ " + labels[i]);
                }
                emit("B " + default_label);
                const std::string cont = loops.empty() ? std::string() : loops.back().first;
                loops.push_back({cont, end});
                for (size_t i = 0; i < s->list.size(); i++) {
                    label(labels[i]);
                    for (const NodeP& st : s->list[i]->list) stmt(st);
                }
                loops.pop_back();
                label(end);
                return;
            }
            case NK::Return:
                if (s->a) ex(s->a);
                else const_(0, 0);
                emit("B " + fn->label + "__ret");
                return;
            case NK::Break:
                emit("B " + loops.back().second);
                return;
            case NK::Continue:
                emit("B " + loops.back().first);
                return;
            case NK::Label:
                label(fn->label + "__lbl_" + s->sval);
                return;
            case NK::Goto:
                emit("B " + fn->label + "__lbl_" + s->sval);
                return;
            case NK::StaticAssert:
                return;  // compile-time only
            default:
                throw CwizError("internal: cannot generate statement", s->line);
        }
    }

    int addr_of_local(const std::string& name) {
        for (Sym* sym : fn->locals)
            if (sym->name == name) return sym->addr;
        throw CwizError("internal: local '" + name + "' not allocated");
    }

    void emit_aggregate_init(Sym* sym, const std::vector<NodeP>& vals) {
        const TypeDef* td = type_def(sym->vtype);
        if (!td) return;
        // C zero-fills the members that the initializer does not name.
        for (const auto& f : td->fields) {
            const_(0, 0);
            if (type_bytes(f.type) == 1) emit(fmt("ST R0, %d", sym->addr + f.offset));
            else emit(fmt("ST ER0, %d", sym->addr + f.offset));
        }
        int positional = 0;
        for (size_t i = 0; i < vals.size(); i++) {
            NodeP item = vals[i];
            const StructField* fp = nullptr;
            if (item->k == NK::Designator) {
                if (!item->sval.empty()) {
                    for (const auto& f : td->fields)
                        if (f.name == item->sval) fp = &f;
                } else {
                    positional = (int)item->b->value;  // resolved by the analyser
                }
                item = item->a;
            } else {
                if (positional < (int)td->fields.size()) fp = &td->fields[(size_t)positional];
                positional++;
            }
            if (!fp) continue;
            ex(item);
            if (type_bytes(fp->type) == 1) emit(fmt("ST R0, %d", sym->addr + fp->offset));
            else emit(fmt("ST ER0, %d", sym->addr + fp->offset));
        }
    }

    void emit_local_array_init(const NodeP& node, Sym* sym) {
        std::vector<long long> vals;
        if (node->init_is_str) {
            for (uint32_t ch : node->cps) vals.push_back((long long)(ch & 0xFF));
        } else {
            size_t pos = 0;
            for (const NodeP& n : node->list) {
                if (n->k == NK::Designator) {
                    pos = (size_t)n->value;
                    if (vals.size() <= pos) vals.resize(pos + 1, 0);
                    vals[pos++] = ev(n->a);
                } else {
                    vals.push_back(ev(n));
                }
            }
        }
        if (vals.empty()) return;  // C leaves an uninitialized local array alone
        vals.resize(std::max<size_t>(vals.size(), sym->size), 0);
        vals.resize(sym->size);
        const int el = sym->elem_bytes;
        for (size_t i = 0; i < vals.size(); ++i) {
            const_(0, vals[i]);
            if (el == 1) emit(fmt("ST R0, %d", sym->addr + (int)i));
            else emit(fmt("ST ER0, %d", sym->addr + el * (int)i));
        }
    }

    void emit_array_init(Sym* sym) {
        std::vector<long long> vals;
        if (sym->init_kind == 2) {
            size_t pos = 0;
            for (auto& n : sym->init_list) {
                if (n->k == NK::Designator) {
                    pos = (size_t)n->value;
                    if (vals.size() <= pos) vals.resize(pos + 1, 0);
                    vals[pos++] = ev(n->a);
                } else {
                    vals.push_back(ev(n));
                }
            }
        } else if (sym->init_kind == 3) {
            for (uint32_t ch : sym->init_cps) vals.push_back((long long)(ch & 0xFF));
        }
        if (vals.empty() && sym->init_kind != 2) vals.assign(sym->size, 0);
        vals.resize(std::max<size_t>(vals.size(), sym->size), 0);
        vals.resize(sym->size);
        if (vals.empty()) return;
        int el = sym->elem_bytes;
        for (size_t i = 0; i < vals.size(); i++) {
            const_(0, vals[i]);
            if (el == 1) emit(fmt("ST R0, %d", sym->addr + (int)i));
            else emit(fmt("ST ER0, %d", sym->addr + el * (int)i));
        }
    }
};

}  // namespace

// =================================================================== entry
CwizResult compile_cwiz(const std::string& src, const Target& target) {
    TypeTable types;
    g_types = &types;
    NodeP program = CwizParser(lex(src), types).parse();

    static NodeP prelude_program;
    static TypeTable prelude_types;
    if (!prelude_program) {
        TypeTable* save = g_types;
        g_types = &prelude_types;
        prelude_program = CwizParser(lex(prelude_source()), prelude_types).parse();
        g_types = save;
    }

    std::vector<NodeP> selected = select_prelude(program, prelude_program);
    NodeP merged = mk(NK::Program, program->line, program->col);
    for (auto& d : selected) merged->list.push_back(d);
    for (auto& d : program->list) merged->list.push_back(d);

    std::map<std::string, long long> predef;
    for (auto& kv : key_consts(target.name)) predef[kv.first] = kv.second;
    predef["_ROWS"] = target.rows;
    predef["_ROW_OFF"] = target.row_off;

    Analyzed an = Analyzer(merged, target, predef).run();
    Codegen cg(an);

    CwizResult r;
    r.asm_body = cg.generate();
    for (Sym* sym : an.globals)
        r.variables[sym->name] = CwizVarInfo{sym->addr, sym->kind, sym->size};
    for (Sym* sym : an.arrays)
        r.variables[sym->name] = CwizVarInfo{sym->addr, sym->kind, sym->size};
    for (Sym* sym : an.tables)
        r.variables[sym->name] = CwizVarInfo{sym->addr, sym->kind, sym->size};
    g_types = nullptr;
    return r;
}

}  // namespace wiz
