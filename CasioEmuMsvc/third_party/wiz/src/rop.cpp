// rop.cpp - CWiz-syntax front-end for the ROP (gadget chain) backend.
//
// The language is CWiz's C-like syntax with several extra directives:
//
//   #rop                 selects the ROP backend
//   #for 580 | 880       picks the gadget database
//   #org E9E0            payload base address (the `opn` of hdcompiler)
//   #launcher common     the "launcher chung": copies the payload to its
//                        execution address and jumps into it
//   #launcher simple     the 10-byte pop er14 / sp = er14 stub
//   #launcher <address>  the simple stub placed at an explicit address
//
// `#launcher common` accepts three optional values:
//
//   #launcher common <length> <backup> <main>
//
// length is short | medium | long (FE01/FE02/FE03), backup is where the
// payload is injected (defaults to #org) and main is the address it is copied
// to before it runs (580 defaults to D730).
//
// and fixed-address globals:
//
//   int counter at [EB40] = 0;
//
// The emitter is a C++ port of CasioC's casio-asm (v0): a call is a chain word
// built from the gadget address (segment in byte 3, call variant in byte 4),
// `print("s", x, y)` builds an xr0 argument block, and strings are appended to
// a pool whose addresses are patched in.
//
// RAC-compatible ROP startup is inserted when the source does not provide it:
// setlr; setsfr.  This is the exact hdcompiler preamble required before
// BL-based display routines on the 580.

#include "rop.hpp"
#include "rop_data.hpp"
#include "vn_f4.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "targets.hpp"

namespace wiz {
namespace {

// ---------------------------------------------------------------- charset
struct CharMap {
    std::unordered_map<unsigned, std::vector<uint8_t>> map;
    CharMap() {
        // ASCII and the calculator's extra symbols.  Vietnamese letters are
        // merged from VN_F4_TABLE (vn_f4.hpp) below, the same source used by
        // the CWiz/Wiz front ends, so this table is not duplicated here.
        static const char* chars =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz~@_&-+()/*"
            "':!?|√÷×^°{}[]%.,";
        static const char* hexes[] = {
            "30", "31", "32", "33", "34", "35", "36", "37", "38", "39",
            "41", "42", "43", "44", "45", "46", "47", "48", "49", "4A",
            "4B", "4C", "4D", "4E", "4F", "50", "51", "52", "53", "54",
            "55", "56", "57", "58", "59", "5A", "61", "62", "63", "64",
            "65", "66", "67", "68", "69", "6A", "6B", "6C", "6D", "6E",
            "6F", "70", "71", "72", "73", "74", "75", "76", "77", "78",
            "79", "7A", "20", "40", "5F", "1A", "2D", "2B", "28", "29",
            "2F", "2A", "27", "3A", "21", "3F", "7C", "98", "26", "24",
            "5E", "85", "7B", "7D", "5B", "5D", "25", "2E", "2C",
        };
        // decode UTF-8 so the symbols above work as codepoints
        std::vector<unsigned> cps;
        for (const unsigned char* p = (const unsigned char*)chars; *p;) {
            unsigned c = *p++;
            if (c < 0x80) { cps.push_back(c); continue; }
            int n = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : 3;
            c &= (0x3F >> n);
            while (n--) c = (c << 6) | (*p++ & 0x3F);
            cps.push_back(c);
        }
        for (size_t i = 0; i < cps.size(); i++) {
            unsigned cp = cps[i];
            const char* h = hexes[i];
            std::vector<uint8_t> b;
            for (size_t k = 0; h[k]; k += 2)
                b.push_back((uint8_t)std::strtoul(std::string(h + k, 2).c_str(), nullptr, 16));
            map[cp] = std::move(b);
        }
        // Vietnamese: the firmware F4 key encoding is 0xF4 followed by a
        // column byte.  rop.cpp, cwiz.cpp and wiz.cpp all use this table.
        for (const auto& e : VN_F4_TABLE) map[e.cp] = {0xF4, e.code};
        map[' '] = {0x20};
        map['~'] = {0x20};
    }
    bool encode(const std::string& s, std::vector<uint8_t>& out, std::string& err) const {
        for (size_t i = 0; i < s.size();) {
            unsigned char c = (unsigned char)s[i];
            unsigned cp = c;
            if (c >= 0x80) {
                int n = (c >> 5) == 0x6 ? 1 : (c >> 4) == 0xE ? 2 : 3;
                cp = c & (0x3F >> n);
                for (int k = 0; k < n && i + 1 < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[++i] & 0x3F);
            }
            auto it = map.find(cp);
            if (it == map.end()) {
                err = "character not in the calculator charset";
                return false;
            }
            out.insert(out.end(), it->second.begin(), it->second.end());
            i++;
        }
        return true;
    }
};
const CharMap& charmap() { static CharMap m; return m; }

// ---------------------------------------------------------------- token
enum class Tk { End, Ident, Number, Addr, Str, Sym };
struct Tok {
    Tk k = Tk::End;
    std::string s;   // ident text or symbol
    uint32_t n = 0;  // number / address
};
[[noreturn]] void fail(const std::string& m) { throw std::runtime_error(m); }

bool is_sym_char(char c) {
    static const std::string s = "+-*/%=<>!&|^~(){}[];,:";
    return s.find(c) != std::string::npos;
}

std::vector<Tok> lex(const std::string& src) {
    std::vector<Tok> out;
    size_t i = 0;
    auto push = [&](Tk k, std::string s, uint32_t n = 0) { Tok t; t.k = k; t.s = std::move(s); t.n = n; out.push_back(t); };
    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') { i++; continue; }
        if (std::isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < src.size() && src[i + 1] == '/') { while (i < src.size() && src[i] != '\n') i++; continue; }
        if (c == '#') { while (i < src.size() && src[i] != '\n') i++; continue; }
        if (c == '[') {  // [ADDR]
            size_t j = i + 1;
            std::string h;
            while (j < src.size() && src[j] != ']') h += src[j++];
            if (j >= src.size()) fail("unterminated address literal");
            h.erase(0, h.find_first_not_of(" \t"));
            std::string hex;
            for (char ch : h)
                if (std::isxdigit((unsigned char)ch)) hex += ch;
            if (hex.empty()) fail("empty address literal");
            push(Tk::Addr, hex, (uint32_t)std::strtoul(hex.c_str(), nullptr, 16));
            i = j + 1;
            continue;
        }
        if (c == '"') {
            std::string s;
            i++;
            while (i < src.size() && src[i] != '"') {
                if (src[i] == '\\' && i + 1 < src.size()) {
                    char e = src[++i];
                    s += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
                } else s += src[i];
                i++;
            }
            if (i >= src.size()) fail("unterminated string");
            i++;
            push(Tk::Str, s);
            continue;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t j = i;
            std::string num;
            if (c == '0' && i + 1 < src.size() && (src[i + 1] == 'x' || src[i + 1] == 'X')) {
                j = i + 2; std::string h;
                while (j < src.size() && std::isxdigit((unsigned char)src[j])) h += src[j++];
                push(Tk::Number, h, (uint32_t)std::strtoul(h.c_str(), nullptr, 16));
                i = j;
                continue;
            }
            while (j < src.size() && std::isdigit((unsigned char)src[j])) num += src[j++];
            push(Tk::Number, num, (uint32_t)std::strtoul(num.c_str(), nullptr, 10));
            i = j;
            continue;
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < src.size() && (std::isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            push(Tk::Ident, src.substr(i, j - i));
            i = j;
            continue;
        }
        if (is_sym_char(c)) {
            std::string two = src.substr(i, 2);
            if (two == "==" || two == "!=" || two == "<=" || two == ">=" || two == "&&" || two == "||" ||
                two == "+=" || two == "-=" || two == "++" || two == "--" || two == "<<" || two == ">>") {
                push(Tk::Sym, two);
                i += 2;
                continue;
            }
            push(Tk::Sym, std::string(1, c));
            i++;
            continue;
        }
        fail(std::string("unexpected character '") + c + "'");
    }
    push(Tk::End, "");
    return out;
}

// ---------------------------------------------------------------- AST
struct Expr;
using ExprP = std::shared_ptr<Expr>;
struct Expr {
    enum Kind { Num, Addr, Var, Str, Bin, Call, Unary } k = Num;
    uint32_t n = 0;
    std::string s;
    ExprP a, b;
    std::vector<ExprP> args;
};
struct Stmt;
using StmtP = std::shared_ptr<Stmt>;
struct Stmt {
    enum Kind { Assign, Call, If, While, For, Goto, Label, Asm, Return } k = Assign;
    std::string lhs;
    ExprP rhs;
    std::vector<StmtP> then_b, else_b, body;
    StmtP init, step;
    ExprP cond;
    std::string name;
    std::vector<ExprP> args;
    std::vector<std::string> asm_lines;
};
struct Global {
    std::string ty, name;
    bool has_addr = false;
    uint32_t addr = 0;
    bool has_init = false;
    uint32_t init = 0;
};
struct Func {
    std::string name;
    std::vector<StmtP> body;
};
struct Program {
    std::string model;
    uint32_t org = 0xE9E0;
    bool has_org = false;
    std::vector<Global> globals;
    std::vector<Func> funcs;
};

class Parser {
public:
    explicit Parser(std::vector<Tok> toks) : t_(std::move(toks)) {}
    Program parse() {
        Program p;
        while (peek().k != Tk::End) {
            if (is_ident("model")) {
                next();
                p.model = next_ident_or_num();
                eat_sym(";");
                continue;
            }
            if (is_ident("opn")) {
                next();
                if (peek().k != Tk::Addr) fail("opn needs [ADDR]");
                p.org = next().n;
                p.has_org = true;
                eat_sym(";");
                continue;
            }
            if (is_ident("csc")) { next(); p.funcs.push_back(func("")); continue; }
            if (peek().k == Tk::Ident && at(1).k == Tk::Ident && at(2).s == "(") {
                std::string ty = next().s;
                p.funcs.push_back(func(ty));
                continue;
            }
            if (peek().k == Tk::Ident && at(1).s == "(") {
                p.funcs.push_back(func(""));
                continue;
            }
            p.globals.push_back(global());
        }
        return p;
    }
private:
    std::vector<Tok> t_;
    size_t p_ = 0;
    const Tok& peek() const { return t_[p_]; }
    const Tok& at(size_t i) const { return p_ + i < t_.size() ? t_[p_ + i] : t_.back(); }
    Tok next() { Tok x = t_[p_]; if (p_ + 1 < t_.size()) p_++; return x; }
    bool is_ident(const char* s) const { return peek().k == Tk::Ident && peek().s == s; }
    bool eat_sym(const char* s) { if (peek().k == Tk::Sym && peek().s == s) { next(); return true; } return false; }
    void expect_sym(const char* s) { if (!eat_sym(s)) fail(std::string("expected '") + s + "'"); }
    std::string next_ident_or_num() {
        Tok x = next();
        if (x.k == Tk::Ident) return x.s;
        if (x.k == Tk::Number) return std::to_string(x.n);
        fail("expected identifier");
    }
    Global global() {
        Global g;
        if (is_ident("let") || is_ident("var")) next();
        if (peek().k == Tk::Ident && at(1).k == Tk::Ident) g.ty = next().s;
        if (peek().k != Tk::Ident) fail("expected variable name");
        g.name = next().s;
        if (is_ident("at")) {
            next();
            if (peek().k != Tk::Addr) fail("at needs [ADDR]");
            g.addr = next().n;
            g.has_addr = true;
        }
        if (eat_sym("=")) {
            Tok v = next();
            if (v.k == Tk::Number || v.k == Tk::Addr) { g.init = v.n; g.has_init = true; }
            else fail("ROP global initializers must be numeric constants");
        }
        eat_sym(";");
        return g;
    }
    Func func(std::string ty) {
        (void)ty;
        Func f;
        if (peek().k != Tk::Ident) fail("expected function name");
        f.name = next().s;
        expect_sym("(");
        while (peek().k != Tk::Sym || peek().s != ")") {
            if (peek().k == Tk::Ident && at(1).k == Tk::Ident) next();
            if (peek().k == Tk::Ident) next(); else fail("bad parameter");
            if (!eat_sym(",")) break;
        }
        expect_sym(")");
        expect_sym("{");
        f.body = block();
        expect_sym("}");
        return f;
    }
    std::vector<StmtP> block() {
        std::vector<StmtP> v;
        while (!(peek().k == Tk::Sym && peek().s == "}") && peek().k != Tk::End)
            v.push_back(stmt());
        return v;
    }
    StmtP stmt() {
        if (peek().k == Tk::Ident && at(1).s == ":") {
            auto s = std::make_shared<Stmt>(); s->k = Stmt::Label; s->name = next().s; next(); return s;
        }
        if (is_ident("if")) {
            next(); expect_sym("(");
            auto s = std::make_shared<Stmt>(); s->k = Stmt::If; s->cond = expr();
            expect_sym(")"); expect_sym("{"); s->then_b = block(); expect_sym("}");
            if (is_ident("else")) {
                next();
                if (is_ident("if")) { s->else_b.push_back(stmt()); }
                else { expect_sym("{"); s->else_b = block(); expect_sym("}"); }
            }
            return s;
        }
        if (is_ident("while")) {
            next(); expect_sym("(");
            auto s = std::make_shared<Stmt>(); s->k = Stmt::While; s->cond = expr();
            expect_sym(")"); expect_sym("{"); s->body = block(); expect_sym("}");
            return s;
        }
        if (is_ident("for")) {
            next(); expect_sym("(");
            auto s = std::make_shared<Stmt>(); s->k = Stmt::For;
            if (!(peek().k == Tk::Sym && peek().s == ";")) s->init = stmt_no_semi();
            expect_sym(";");
            if (!(peek().k == Tk::Sym && peek().s == ";")) s->cond = expr();
            expect_sym(";");
            if (!(peek().k == Tk::Sym && peek().s == ")")) s->step = stmt_no_semi();
            expect_sym(")"); expect_sym("{"); s->body = block(); expect_sym("}");
            return s;
        }
        if (is_ident("asm")) {
            next(); expect_sym("{");
            auto s = std::make_shared<Stmt>(); s->k = Stmt::Asm;
            while (!(peek().k == Tk::Sym && peek().s == "}") && peek().k != Tk::End) {
                if (peek().k == Tk::Str) { s->asm_lines.push_back(next().s); continue; }
                std::string cur;
                while (!(peek().k == Tk::Sym && peek().s == ";") && !(peek().k == Tk::Sym && peek().s == "}") && peek().k != Tk::End) {
                    Tok x = next();
                    if (x.k == Tk::Addr) cur += "[0x" + to_hex(x.n) + "]";
                    else cur += (cur.empty() ? "" : " ") + (x.k == Tk::Number ? std::to_string(x.n) : x.s);
                }
                if (!cur.empty()) s->asm_lines.push_back(cur);
                eat_sym(";");
            }
            expect_sym("}");
            return s;
        }
        if (is_ident("goto")) {
            next();
            auto s = std::make_shared<Stmt>(); s->k = Stmt::Goto; s->name = next_ident_or_num(); eat_sym(";"); return s;
        }
        if (is_ident("return")) {
            next();
            auto s = std::make_shared<Stmt>(); s->k = Stmt::Return;
            if (!(peek().k == Tk::Sym && peek().s == ";")) s->rhs = expr();
            eat_sym(";"); return s;
        }
        auto s = stmt_no_semi();
        eat_sym(";");
        return s;
    }
    StmtP stmt_no_semi() {
        auto s = std::make_shared<Stmt>();
        if (peek().k == Tk::Ident && (at(1).s == "=" || at(1).s == "+=" || at(1).s == "-=")) {
            s->k = Stmt::Assign;
            s->lhs = next().s;
            std::string op = next().s;
            s->rhs = expr();
            if (op == "+=" || op == "-=") {
                auto v = std::make_shared<Expr>(); v->k = Expr::Var; v->s = s->lhs;
                auto b = std::make_shared<Expr>(); b->k = Expr::Bin; b->s = op == "+=" ? "+" : "-";
                b->a = v; b->b = s->rhs; s->rhs = b;
            }
            return s;
        }
        if (peek().k == Tk::Ident && (at(1).s == "++" || at(1).s == "--")) {
            s->k = Stmt::Assign; s->lhs = next().s; std::string op = next().s;
            auto v = std::make_shared<Expr>(); v->k = Expr::Var; v->s = s->lhs;
            auto one = std::make_shared<Expr>(); one->k = Expr::Num; one->n = 1;
            auto b = std::make_shared<Expr>(); b->k = Expr::Bin; b->s = op == "++" ? "+" : "-";
            b->a = v; b->b = one; s->rhs = b;
            return s;
        }
        if (peek().k == Tk::Ident && at(1).s == "(") {
            s->k = Stmt::Call; s->name = next().s; next();
            while (!(peek().k == Tk::Sym && peek().s == ")")) {
                s->args.push_back(expr());
                if (!eat_sym(",")) break;
            }
            expect_sym(")");
            return s;
        }
        auto e = expr();
        if (e->k == Expr::Call) { s->k = Stmt::Call; s->name = e->s; s->args = e->args; return s; }
        fail("cannot parse statement");
    }
    static std::string to_hex(uint32_t n) { char b[16]; std::snprintf(b, sizeof b, "%04X", n); return b; }
    ExprP expr() { return or_(); }
    ExprP bin(ExprP a, const std::string& op, ExprP b) {
        auto e = std::make_shared<Expr>(); e->k = Expr::Bin; e->s = op; e->a = a; e->b = b; return e;
    }
    ExprP or_() { auto l = and_(); while (eat_sym("||")) l = bin(l, "||", and_()); return l; }
    ExprP and_() { auto l = cmp(); while (eat_sym("&&")) l = bin(l, "&&", cmp()); return l; }
    ExprP cmp() {
        auto l = add();
        for (;;) {
            std::string op;
            if (eat_sym("==")) op = "=="; else if (eat_sym("!=")) op = "!=";
            else if (eat_sym("<=")) op = "<="; else if (eat_sym(">=")) op = ">=";
            else if (eat_sym("<")) op = "<"; else if (eat_sym(">")) op = ">";
            else break;
            l = bin(l, op, add());
        }
        return l;
    }
    ExprP add() {
        auto l = mul();
        for (;;) {
            std::string op;
            if (eat_sym("+")) op = "+"; else if (eat_sym("-")) op = "-";
            else if (eat_sym("|")) op = "|"; else if (eat_sym("^")) op = "^";
            else break;
            l = bin(l, op, mul());
        }
        return l;
    }
    ExprP mul() {
        auto l = unary();
        for (;;) {
            std::string op;
            if (eat_sym("*")) op = "*"; else if (eat_sym("/")) op = "/";
            else if (eat_sym("%")) op = "%"; else if (eat_sym("&")) op = "&";
            else if (eat_sym("<<")) op = "<<"; else if (eat_sym(">>")) op = ">>";
            else break;
            l = bin(l, op, unary());
        }
        return l;
    }
    ExprP unary() {
        if (eat_sym("!")) { auto e = std::make_shared<Expr>(); e->k = Expr::Unary; e->s = "!"; e->a = unary(); return e; }
        if (eat_sym("-")) {
            auto z = std::make_shared<Expr>(); z->k = Expr::Num; z->n = 0;
            return bin(z, "-", unary());
        }
        return primary();
    }
    ExprP primary() {
        Tok x = next();
        if (x.k == Tk::Number) { auto e = std::make_shared<Expr>(); e->k = Expr::Num; e->n = x.n; return e; }
        if (x.k == Tk::Addr) { auto e = std::make_shared<Expr>(); e->k = Expr::Addr; e->n = x.n; return e; }
        if (x.k == Tk::Str) { auto e = std::make_shared<Expr>(); e->k = Expr::Str; e->s = x.s; return e; }
        if (x.k == Tk::Ident) {
            auto e = std::make_shared<Expr>(); e->k = Expr::Var; e->s = x.s;
            if (eat_sym("(")) {
                e->k = Expr::Call;
                while (!(peek().k == Tk::Sym && peek().s == ")")) {
                    e->args.push_back(expr());
                    if (!eat_sym(",")) break;
                }
                expect_sym(")");
            }
            return e;
        }
        if (x.k == Tk::Sym && x.s == "(") {
            auto e = expr();
            expect_sym(")");
            return e;
        }
        fail("unexpected token in expression");
    }
};

// ---------------------------------------------------------------- DB
struct Db {
    std::unordered_map<std::string, uint32_t> gadgets;
    std::unordered_map<std::string, uint32_t> labels;
    std::unordered_map<std::string, uint32_t> data;
    std::string model;
    void load(const char* text) {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            size_t sp = line.find_first_of(" \t");
            if (sp == std::string::npos) continue;
            std::string a = line.substr(0, sp);
            std::string name = line.substr(sp);
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            if (name.empty()) continue;
            const bool is_bl = name.size() > 3 && std::tolower((unsigned char)name[0]) == 'b' &&
                               std::tolower((unsigned char)name[1]) == 'l' && std::isspace((unsigned char)name[2]);
            if (is_bl)
                name.erase(0, name.find_first_not_of(" \t", 2));
            name = key(name);
            uint32_t addr = (uint32_t)std::strtoul(a.c_str(), nullptr, 16);
            if (name.size() > 2 && name[0] == 'd' && name[1] == '_') data[name.substr(2)] = addr;
            else {
                labels[name] = addr;
                if (is_bl) labels["bl_" + name] = addr;
            }
        }
    }
    static std::string key(const std::string& s) {
        std::string r;
        for (char c : s)
            if (!std::isspace((unsigned char)c)) r += (char)std::tolower((unsigned char)c);
        return r;
    }
    uint32_t find_gadget(const std::string& n) const {
        auto it = labels.find(key(n));
        return it == labels.end() ? 0xFFFFFFFFu : it->second;
    }
};

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool is_register(const std::string& r) {
    static const std::set<std::string> regs = {
        "r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r12",
        "er0","er2","er4","er6","er8","er10","er12","er14",
        "xr0","xr4","xr8","xr12","qr0","qr8","ea","sp"};
    return regs.count(lower(r)) != 0;
}
int reg_size(const std::string& r) {
    char c = (char)std::tolower((unsigned char)r[0]);
    return c == 'q' ? 8 : c == 'x' ? 4 : c == 'e' ? 2 : 1;
}

std::vector<std::string> stdlib_candidates(const std::string& name) {
    std::vector<std::string> v;
    auto push = [&](std::initializer_list<const char*> xs) { for (auto* x : xs) v.push_back(x); };
    std::string k = lower(name);
    if (k == "screen_del" || k == "screen_clear" || k == "cls") push({"buffer_clear","buffer_clear.ca54","buffer_clear.d654"});
    else if (k == "reset_sfr" || k == "setsfr") push({"setsfr"});
    else if (k == "ea_switchcase" || k == "switch_case") push({"cmp_ea"});
    else if (k == "screen_fill") push({"fill_screen"});
    else if (k == "draw_line" || k == "line") push({"line_draw"});
    else if (k == "draw_pixel" || k == "pixel") push({"pixel_draw"});
    else if (k == "draw_byte") push({"draw_byte"});
    else if (k == "print" || k == "print_line") push({"smallprint","line_print","printline","basen_base_print"});
    else if (k == "print_at" || k == "draw_text") push({"line_print"});
    else if (k == "print_small") push({"smallprint"});
    else if (k == "render" || k == "flush" || k == "flip") push({"render","render.ddd4","render.ca54"});
    else if (k == "delay" || k == "sleep") push({"delay"});
    else if (k == "wait_shift" || k == "waitshift" || k == "wait") push({"waitshift"});
    else if (k == "wait_shift_striped" || k == "waitshift_striped") push({"waitshift_striped"});
    else if (k == "render_bitmap") push({"render_bitmap"});
    else if (k == "get_key" || k == "key") push({"getkey","getkeycode","getscancode","cvt_key"});
    else if (k == "is_key_pressed" || k == "check_key") push({"check_any_key_pressed__"});
    else if (k == "wait_key" || k == "pause") push({"diagnostic_wait_key"});
    else if (k == "mem_copy" || k == "memcpy") push({"memcpy","smart_strcpy","memmove"});
    else if (k == "mem_set" || k == "memset") push({"memset","memzero"});
    else if (k == "mem_zero" || k == "memzero") push({"memzero"});
    else if (k == "mem_move") push({"memmove"});
    else if (k == "double_buffer_flip") push({"buf1_to_buf2"});
    else if (k == "str_copy") push({"strcpy","smart_strcpy"});
    else if (k == "str_cat") push({"strcat","smart_strcat"});
    else if (k == "str_len") push({"strlen","smart_strlen_n","byte_strlen_n"});
    else if (k == "fill_screen") push({"fill_screen"});
    else if (k == "buffer_clear") push({"buffer_clear","buffer_clear.ca54"});
    v.push_back(k);
    return v;
}

// ---------------------------------------------------------------- chain words
// Every gadget reference in a ROP chain is a four byte word:
//
//     [addr_lo, addr_hi, 0x30 | segment, variant]
//
// The nX-U16 code space is 20 bit: a four bit code segment (CSR) plus a 16 bit
// program counter.  The community guide (section 2.3 and 2.4 of
// docs/rop-notes/rop-580-guide.txt) spells the encoding out:
//
//     address = Z:YYXXH  =>  code = XX YY ?Z ??
//
// so the segment nibble Z goes into the third byte (0x3Z) and the fourth byte
// selects the call variant; 0x30 is the usual one.  Examples:
//
//     pop xr0    @ 0x17B34 -> 34 7B 31 30
//     pop qr0    @ 0x17BDA -> DA 7B 31 30
//     printline  @ 0x23DC8 -> C8 3D 32 30
//     smallprint @ 0x23DCC -> CC 3D 32 30
//     brk        @ 0x33030 -> 30 30 33 30
//
// The variant byte is a property of the call site, not of the gadget: the same
// "sp = er6,pop er8" gadget is called with 0x30 inside a payload and with 0x48
// by the community launcher, which hands control over to the payload without
// linking (0x30 = "call and link", 0x48 = "branch").
constexpr uint8_t kChainVariantCall = 0x30;
constexpr uint8_t kChainVariantBranch = 0x48;

uint8_t chain_segment(uint32_t addr) {
    return (uint8_t)(0x30 | ((addr >> 16) & 0x0F));
}

void emit_gadget(std::vector<uint8_t>& out, uint32_t addr, uint8_t variant = kChainVariantCall) {
    out.push_back((uint8_t)(addr & 0xFF));
    out.push_back((uint8_t)((addr >> 8) & 0xFF));
    out.push_back(chain_segment(addr));
    out.push_back(variant);
}

// What a gadget does.  This only drives the low level listing; the bytes of
// the chain word come from the address (see emit_gadget).
enum class GKind : uint8_t {
    Call = 0,      // plain function call
    Pop,           // register pop (pop er0, pop xr0, ...)
    Flow,          // stack / flow control (sp = ..., pop pc)
    SetlrPc,       // setlr + pc redirect
};

// Normalised gadget name: lower case, no whitespace.
std::string gadget_key(const std::string& name) {
    std::string k;
    for (char c : name)
        if (!std::isspace((unsigned char)c)) k += (char)std::tolower((unsigned char)c);
    return k;
}

// Which kind of gadget is this?
GKind gadget_kind(const std::string& name) {
    const std::string k = gadget_key(name);
    if (k.rfind("setlr_pc", 0) == 0) return GKind::SetlrPc;
    if (k.rfind("pop", 0) == 0) {
        // "pop pc" redirects the program counter, every other pop loads a
        // register.
        if (k.compare(3, 2, "pc") == 0) return GKind::Flow;
        return GKind::Pop;
    }
    if (k.rfind("sp", 0) == 0) return GKind::Flow;
    return GKind::Call;
}

// The register a "pop <reg>" gadget loads ("pop xr0" -> "xr0").
std::string pop_register(const std::string& name) {
    std::string k = gadget_key(name);
    if (k.rfind("pop", 0) != 0) return name;
    std::string rest = k.substr(3);
    const size_t comma = rest.find(',');
    if (comma != std::string::npos) rest = rest.substr(0, comma);
    return rest;
}

// "ff8181" and "ff 81 81" both describe the same bytes.
std::vector<uint8_t> parse_hex_bytes(const std::string& s) {
    std::string h;
    for (char c : s)
        if (std::isxdigit((unsigned char)c)) h += c;
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < h.size(); i += 2)
        bytes.push_back((uint8_t)std::strtoul(h.substr(i, 2).c_str(), nullptr, 16));
    return bytes;
}

// ---------------------------------------------------------------- listing
// One row of the low level view of the chain.
struct ChainRow {
    size_t offset = 0;   // offset of the word inside the payload
    uint32_t addr = 0;   // gadget address
    GKind kind = GKind::Call;
    std::string name;    // gadget name (or the register for a pop)
};

const char* kind_name(GKind c) {
    switch (c) {
        case GKind::Pop: return "pop";
        case GKind::Flow: return "flow";
        case GKind::SetlrPc: return "setlr";
        case GKind::Call: break;
    }
    return "call";
}

std::string hex_bytes_compact(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (i) s += ' ';
        s += digits[p[i] >> 4];
        s += digits[p[i] & 0x0F];
    }
    return s;
}

// Build the low level listing of a payload:
//
//   E9E0  09 FA 30 30   call     0x09FA8  setlr
//   E9E8  34 7B 31 30   pop      0x17B34  xr0
//   E9EC  01 30 0C EA             data    01 30 0C EA
std::string build_listing(const std::vector<uint8_t>& bytes, const std::vector<ChainRow>& rows,
                           uint32_t base) {
    std::string s;
    char addr[16];
    size_t i = 0, r = 0;
    while (i < bytes.size()) {
        if (r < rows.size() && rows[r].offset == i) {
            const ChainRow& g = rows[r];
            std::snprintf(addr, sizeof addr, "%04X", base + (uint32_t)i);
            std::string kind = kind_name(g.kind);
            while (kind.size() < 9) kind += ' ';
            char ga[16];
            std::snprintf(ga, sizeof ga, "0x%05X", g.addr);
            s += std::string(addr) + "  " + hex_bytes_compact(&bytes[i], 4) + "   " + kind + ga +
                 "  " + g.name + "\n";
            i += 4;
            r++;
            continue;
        }
        // Plain data (arguments, string pool, ...): up to four bytes per row.
        size_t n = 4;
        if (r < rows.size() && rows[r].offset > i) n = std::min(n, rows[r].offset - i);
        n = std::min(n, bytes.size() - i);
        std::snprintf(addr, sizeof addr, "%04X", base + (uint32_t)i);
        std::string col = hex_bytes_compact(&bytes[i], n);
        while (col.size() < 11) col += ' ';
        s += std::string(addr) + "  " + col + "             data    " +
             hex_bytes_compact(&bytes[i], n) + "\n";
        i += n;
    }
    return s;
}

// ---------------------------------------------------------------- key tables
// A cmp_ea table is a list of (16 bit key, 16 bit target) entries.  The
// target is stored as "label - 2" because the branch gadget is
// "sp = er6, pop er8" (see jump_label).
struct KeyTableEntry {
    uint16_t key = 0;
    std::string label;
};
struct KeyTable {
    size_t addr_pos = 0;   // where the dispatch sequence stores the table address
    std::vector<KeyTableEntry> entries;
};

// ---------------------------------------------------------------- emitter
struct Emitter {
    const Db& db;
    std::vector<uint8_t> out;
    std::string log;
    std::map<std::string, uint32_t> alloc;
    uint32_t next_alloc = 0xE800;
    std::map<std::string, size_t> labels;
    std::vector<std::pair<size_t, std::string>> patches;
    std::vector<std::pair<size_t, std::vector<uint8_t>>> str_pool;
    std::vector<ChainRow> rows;      // the chain, for the low level listing
    std::vector<KeyTable> key_tables;
    uint32_t home = 0xE9E0;
    uint32_t data_home = 0xE9E0;
    int pad_size = 256;
    bool has_key_slot = false;
    uint32_t key_slot = 0;
    bool failed = false;
    std::string first_error;

    explicit Emitter(const Db& d) : db(d) {}

    void report_error(const std::string& message, const std::string& code = "E100") {
        failed = true;
        if (first_error.empty()) first_error = message;
        log += "  ; error [" + code + "]: " + message + "\n";
    }

    uint32_t alloc_var(const std::string& n) {
        auto it = alloc.find(n);
        if (it != alloc.end()) return it->second;
        uint32_t a = next_alloc;
        alloc[n] = a;
        next_alloc += 2;
        return a;
    }
    void emit_le(uint32_t v, int sz) {
        for (int i = 0; i < sz; i++) { out.push_back((uint8_t)(v & 0xFF)); v >>= 8; }
    }
    void emit_word_store(uint32_t address, uint32_t value) {
        call("setlr");
        call("pop xr0");
        emit_le(address, 2);
        emit_le(value, 2);
        call("[er0]=er2,rt");
    }
    bool find_addr(const std::string& name, uint32_t& addr, std::string& found) {
        for (auto& c : stdlib_candidates(name)) {
            const std::string k = Db::key(c);
            auto l = db.labels.find(k);
            if (l != db.labels.end()) { addr = l->second; found = c; return true; }
            auto d = db.data.find(k);
            if (d != db.data.end()) { addr = d->second; found = c; return true; }
        }
        return false;
    }
    void call(const std::string& name) {
        // Keep old Wiz examples source-compatible, but compile the historical
        // setlr_pc spelling as RAC's working setlr gadget.
        const std::string lookup_name = lower(name) == "setlr_pc" ? "setlr" : name;
        uint32_t addr = 0; std::string found;
        if (!find_addr(lookup_name, addr, found)) {
            report_error("gadget/label " + name + " not found (aliases tried)");
            return;
        }
        // The chain word is built from the gadget address: the low two bytes
        // are the program counter, the third byte carries the code segment
        // (0x30 | segment) and the fourth byte the call variant (0x30).
        const GKind kind = gadget_kind(found);
        emit_gadget(out, addr);
        // Remember the word for the low level listing.
        ChainRow row;
        row.offset = out.size() - 4;
        row.addr = addr;
        row.kind = kind;
        row.name = (kind == GKind::Pop) ? pop_register(found) : found;
        rows.push_back(row);
    }
    uint32_t eval(const ExprP& e, std::string& err) {
        switch (e->k) {
            case Expr::Num: case Expr::Addr: return e->n;
            case Expr::Var: {
                const std::string k = Db::key(e->s);
                auto d = db.data.find(k);
                if (d != db.data.end()) return d->second;
                auto g = db.labels.find(k);
                if (g != db.labels.end()) return g->second;
                auto v = alloc.find(e->s);
                if (v != alloc.end()) return v->second;
                report_error("unknown ROP value '" + e->s + "'", "E103");
                return 0;
            }
            case Expr::Str: return (uint32_t)e->s.size();
            case Expr::Unary: {
                uint32_t v = eval(e->a, err);
                return e->s == "!" ? (v == 0 ? 1u : 0u) : v;
            }
            case Expr::Call:
                report_error("function calls cannot be used inside ROP expressions", "E103");
                return 0;
            case Expr::Bin: {
                uint32_t a = eval(e->a, err), b = eval(e->b, err);
                const std::string& o = e->s;
                if (o == "+") return a + b; if (o == "-") return a - b;
                if (o == "*") return a * b; if (o == "/") return b ? a / b : 0;
                if (o == "%") return b ? a % b : 0;
                if (o == "&") return a & b; if (o == "|") return a | b; if (o == "^") return a ^ b;
                if (o == "<<") return a << (b & 31); if (o == ">>") return a >> (b & 31);
                if (o == "==") return a == b; if (o == "!=") return a != b;
                if (o == "<") return a < b; if (o == ">") return a > b;
                if (o == "<=") return a <= b; if (o == ">=") return a >= b;
                if (o == "&&") return a && b; if (o == "||") return a || b;
                return 0;
            }
        }
        return 0;
    }
    // 0 = always false, 1 = always true, 2 = dynamic
    int eval_cond(const ExprP& e) {
        if (e->k == Expr::Num || e->k == Expr::Addr) return eval(e, log) ? 1 : 0;
        if (e->k == Expr::Bin) {
            const std::string& o = e->s;
            if (o == "==" || o == "!=" || o == "<" || o == ">" || o == "<=" || o == ">=") {
                bool lc = e->a->k == Expr::Num || e->a->k == Expr::Addr;
                bool rc = e->b->k == Expr::Num || e->b->k == Expr::Addr;
                return (lc && rc) ? (eval(e, log) ? 1 : 0) : 2;
            }
        }
        return 2;
    }
    void jump_label(const std::string& lbl, const std::string& why) {
        if (!why.empty()) log += "  ; " + why + "\n";
        call("pop er6");
        size_t pos = out.size();
        out.push_back(0); out.push_back(0);
        patches.push_back({pos, lbl});
        call("sp=er6,pop er8");
    }
    // Index of the string argument of a call, or -1.
    static int text_arg_index(const std::vector<ExprP>& args) {
        for (size_t i = 0; i < args.size(); i++)
            if (args[i]->k == Expr::Str) return (int)i;
        return -1;
    }
    bool encode_text(const std::string& s, std::vector<uint8_t>& enc) {
        std::string err;
        if (!charmap().encode(s, enc, err)) { log += "  ; error: " + err + "\n"; return false; }
        enc.push_back(0);   // strings are NUL terminated
        return true;
    }
    // print(pad, linepos, "text") / print("text", pad, linepos)
    //   -> pop xr0 with [linepos, pad, addr_lo, addr_hi], then printline.
    // print_small(font, linepos, "text") / print_small("text", font, linepos)
    //   -> pop xr0 with [font, linepos, addr_lo, addr_hi], then smallprint.
    void emit_print(const StmtP& s, bool small) {
        const std::vector<ExprP>& args = s->args;
        const int ti = text_arg_index(args);
        if (ti < 0 || args.size() != 3) {
            report_error(std::string(small ? "print_small" : "print") +
                         " expects exactly two numeric arguments and one string literal", "E101");
            return;
        }
        std::vector<const ExprP*> rest;
        for (size_t i = 0; i < args.size(); i++)
            if ((int)i != ti) rest.push_back(&args[i]);
        const uint32_t a = eval(*rest[0], log);   // pad (print) / font (print_small)
        const uint32_t b = eval(*rest[1], log);   // line position
        std::vector<uint8_t> enc;
        if (!encode_text(args[ti]->s, enc)) return;
        uint32_t addr = 0; std::string found;
        if (!find_addr(small ? "smallprint" : "printline", addr, found) &&
            !find_addr("line_print", addr, found)) {
            log += "  ; error [E100]: gadget " + std::string(small ? "smallprint" : "printline") +
                   " not found (aliases tried)\n";
            return;
        }
        // printline wants ER0 = pad << 8 | linepos, smallprint and line_print
        // want ER0 = linepos << 8 | font.
        const bool swap = !small && found == "printline";
        call("pop xr0");
        if (swap) {
            out.push_back((uint8_t)(b & 0xFF));   // linepos
            out.push_back((uint8_t)(a & 0xFF));   // pad
        } else {
            out.push_back((uint8_t)(a & 0xFF));   // font
            out.push_back((uint8_t)(b & 0xFF));   // linepos
        }
        size_t sp = out.size();
        out.push_back(0); out.push_back(0);
        str_pool.push_back({sp, enc});
        log += "  ; " + std::string(small ? "print_small" : "print") + " \"" + args[ti]->s + "\" " +
               (small ? "font=0x" : "pad=0x") + hex2(a) + " linepos=0x" + hex2(b) + " -> " + found + "\n";
        call(found);
    }
    // line(x1, y1, x2, y2) -> pop xr0 with [x1, y1, x2, y2], then line_draw.
    void emit_line_coords(uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2) {
        call("pop xr0");
        const uint32_t c[4] = {x1, y1, x2, y2};
        for (int i = 0; i < 4; i++) {
            if (c[i] > 0xFF)
                log += "  ; warning [W003]: line coordinate " + std::to_string(c[i]) +
                       " does not fit in one byte\n";
            out.push_back((uint8_t)(c[i] & 0xFF));
        }
        call("line_draw");
    }
    void emit_line(const StmtP& s) {
        if (s->args.size() != 4) { report_error("line expects four coordinates", "E101"); return; }
        emit_line_coords(eval(s->args[0], log), eval(s->args[1], log),
                         eval(s->args[2], log), eval(s->args[3], log));
    }
    // rect(x, y, w, h) -> four line_draw calls (top, bottom, left, right).
    // line_draw only knows two points, so a rectangle cannot be a single
    // call with x, y, w, h in the coordinate slots.
    void emit_rect(const StmtP& s) {
        if (s->args.size() != 4) { report_error("rect expects x, y, width, and height", "E101"); return; }
        const uint32_t x = eval(s->args[0], log), y = eval(s->args[1], log);
        const uint32_t w = eval(s->args[2], log), h = eval(s->args[3], log);
        const uint32_t x2 = x + w, y2 = y + h;
        emit_line_coords(x, y, x2, y);
        emit_line_coords(x, y2, x2, y2);
        emit_line_coords(x, y, x, y2);
        emit_line_coords(x2, y, x2, y2);
        log += "  ; rect " + std::to_string(x) + "," + std::to_string(y) + " " +
               std::to_string(w) + "x" + std::to_string(h) + " -> 4 line_draw calls\n";
    }
    // bitmap(x, y, w, h, "ff8181") -> pop xr0 with [x, y, w, h], render_bitmap,
    // pop er0 with the bitmap address, then render.ddd4.
    void emit_bitmap(const StmtP& s) {
        if (s->args.size() != 5 || s->args[4]->k != Expr::Str) {
            report_error("bitmap expects x, y, width, height, and a quoted hexadecimal byte string", "E101");
            return;
        }
        const std::vector<uint8_t> data = parse_hex_bytes(s->args[4]->s);
        if (data.empty()) { report_error("bitmap data is empty", "E101"); return; }
        call("pop xr0");
        for (size_t i = 0; i < 4; i++) {
            const uint32_t v = eval(s->args[i], log);
            if (v > 0xFF)
                log += "  ; warning [W003]: bitmap size " + std::to_string(v) + " does not fit in one byte\n";
            out.push_back((uint8_t)(v & 0xFF));
        }
        call("render_bitmap");
        call("pop er0");
        size_t sp = out.size();
        out.push_back(0); out.push_back(0);
        str_pool.push_back({sp, data});
        log += "  ; bitmap " + std::to_string(data.size()) + " bytes\n";
        call("render");
    }
    // The generic form: the call, then one 16 bit word per argument.
    void emit_generic_call(const StmtP& s) {
        const std::string key = lower(s->name);
        uint32_t a = 0; std::string f;
        if (!s->args.empty()) {
            report_error("no registered argument ABI for " + s->name +
                         "; use the dedicated API or load registers before calling the gadget", "E107");
            return;
        }
        if (find_addr(key, a, f)) {
            call(key);
        } else {
            report_error("function " + s->name + " is not a supported ROP API or gadget", "E104");
        }
    }
    // getkey() follows the community sequence.  The inline slot is cleared
    // before every read because getkey() leaves [ER0] unchanged when no key is
    // down: the community loops re-copy the payload from backup, which has the
    // same effect, but a compiler `goto` does not re-copy.
    //
    //     setlr           ; render/BL before this point may clobber LR
    //     pop xr0
    //     <slot>, 0000    ; ER0 = slot, ER2 = 0
    //     [er0]=er2,rt    ; clear the 2-byte KI/KO slot
    //     pop er0
    //     <slot>          ; ER0 = address of the 2-byte KI/KO slot
    //     getkey          ; writes the scancode into [ER0]
    //     setlr           ; establish LR before the final pop
    //     pop er0         ; load the word getkey just wrote
    //     slot: 00 00     ; inline data consumed by the pop above
    //
    // The final pop reads the value, not the address.  ER0 is left holding the
    // KI/KO word for keytable().
    void emit_getkey(const StmtP& s) {
        if (!s->args.empty()) {
            report_error(s->name + " takes no arguments; the compiler owns its inline KI/KO slot", "E101");
            return;
        }
        // The write gadget ends in `rt`; LR must be valid.  A preceding
        // print/render BL may have clobbered the LR set by the preamble.
        call("setlr");
        // Keep the OS key/display interrupt out of the chain stack while the
        // key is polled.  The ISR otherwise returns through the payload and
        // crashes on the first key press (community `di,rt` rule).
        call("di,rt");
        call("pop xr0");
        const size_t zero_pos = out.size();
        out.push_back(0);
        out.push_back(0);
        out.push_back(0);   // ER2 = 0
        out.push_back(0);
        call("[er0]=er2,rt");
        call("pop er0");
        const size_t addr_pos = out.size();
        out.push_back(0);
        out.push_back(0);
        const std::string name = lower(s->name);
        // getkey and getkeycode share the community inline-slot ABI; the
        // database maps the getkeycode spelling to the getkey entry.
        const std::string gc = name == "getscancode" ? "getscancode" : "getkey";
        call(gc);
        call("setlr");
        // setlr re-enables interrupts (its BL target is EI,RT); turn them back
        // off so the keytable jump and the handler do not run an ISR on the
        // chain stack.
        call("di,rt");
        call("pop er0");
        const size_t slot_pos = out.size();
        out.push_back(0);
        out.push_back(0);
        key_slot = home + (uint32_t)slot_pos;
        has_key_slot = true;
        out[zero_pos] = (uint8_t)(key_slot & 0xFF);
        out[zero_pos + 1] = (uint8_t)((key_slot >> 8) & 0xFF);
        out[addr_pos] = (uint8_t)(key_slot & 0xFF);
        out[addr_pos + 1] = (uint8_t)((key_slot >> 8) & 0xFF);
        log += "  ; " + std::string(s->name) + " -> inline KI/KO slot [" +
               hex4(key_slot) + "] (cleared before read)\n";
    }
    // keytable(0x0480, up, 0x0840, down, ..., 0x0000, loop)
    //
    // Reads the KI/KO word written by getkey(), scans the table with cmp_ea
    // and jumps to the matching label.  The last pair is usually
    // (0x0000, loop): cmp_ea treats 00 00 as the default entry.
    void emit_keytable(const StmtP& s) {
        const std::vector<ExprP>& args = s->args;
        if (args.size() < 2 || args.size() % 2 != 0) {
            report_error("keytable needs (key, label) pairs", "E101");
            return;
        }
        for (const char* gadget : {"pop ea", "cmp_ea", "er6 = [ea]", "sp=er6,pop er8"}) {
            uint32_t address = 0;
            std::string found;
            if (!find_addr(gadget, address, found)) {
                report_error("keytable is not supported by the " + std::string(db.model) +
                             " gadget table (missing " + gadget + ")", "E102");
                return;
            }
        }
        KeyTable table;
        bool has_default = false;
        for (size_t i = 0; i < args.size(); i += 2) {
            KeyTableEntry e;
            e.key = (uint16_t)eval(args[i], log);
            if (args[i + 1]->k != Expr::Var && args[i + 1]->k != Expr::Str) {
                report_error("keytable target must be a label", "E101");
                return;
            }
            e.label = args[i + 1]->s;
            if (e.key == 0) has_default = true;
            table.entries.push_back(e);
        }
        if (!has_key_slot)
            log += "  ; warning [W004]: keytable without a getkey(); ER0 is not a scancode\n";
        if (!has_default)
            log += "  ; warning [W005]: keytable has no 0x0000 default entry\n";
        // ER0 was loaded by getkey(); only the table address still has to be
        // put into EA.  Do not emit another pop er0 here: that would load the
        // slot address instead of the KI/KO value.
        call("pop ea");
        table.addr_pos = out.size();
        out.push_back(0);
        out.push_back(0);
        call("cmp_ea");
        call("er6 = [ea]");
        call("sp=er6,pop er8");
        log += "  ; keytable: " + std::to_string(table.entries.size()) + " entries\n";
        for (const auto& e : table.entries)
            log += "  ;   " + hex4(e.key) + " -> " + e.label + "\n";
        key_tables.push_back(std::move(table));
    }
    // calc_func(ptr, result): XR0 = address of the token pointer, result area.
    void emit_calc(const StmtP& s) {
        if (s->args.size() != 2) { report_error("calc_func expects token pointer and NUM result area", "E101"); return; }
        call("pop xr0");
        emit_le(eval(s->args[0], log), 2);
        emit_le(eval(s->args[1], log), 2);
        call("calc_func");
    }
    // num_to_str(num, text): XR0 = NUM area, destination text buffer.
    void emit_num_to_str(const StmtP& s) {
        if (s->args.size() != 2) { report_error("num_to_str expects NUM area and destination text", "E101"); return; }
        call("pop xr0");
        emit_le(eval(s->args[0], log), 2);
        emit_le(eval(s->args[1], log), 2);
        call("num_to_str");
    }
    // delay(ticks): ER0 = ticks, 0x1F40 (8000) is about one second.
    void emit_delay(const StmtP& s) {
        if (s->args.size() != 1) { report_error("delay expects one tick count", "E101"); return; }
        call("pop er0");
        emit_le(eval(s->args[0], log), 2);
        call("delay");
    }
    // pixel(x, y, color): XR0 = x, y, color, pad.  color 0 = black, 1 = white.
    void emit_pixel(const StmtP& s) {
        if (s->args.size() != 3) { report_error("pixel expects x, y, and color", "E101"); return; }
        call("pop xr0");
        for (int i = 0; i < 3; i++) {
            const uint32_t v = eval(s->args[i], log);
            if (v > 0xFF)
                log += "  ; warning [W003]: pixel value " + std::to_string(v) +
                       " does not fit in one byte\n";
            out.push_back((uint8_t)(v & 0xFF));
        }
        out.push_back(0);   // r3 is unused padding
        call("pixel_draw");
    }
    // memcpy(dst, src, length): QR8 = dst, src, length, pad.
    void emit_memcpy(const StmtP& s) {
        if (s->args.size() != 3) { report_error("memcpy expects destination, source, and byte count", "E101"); return; }
        uint32_t address = 0;
        std::string found;
        if (!find_addr("memcpy_length_rn/8", address, found)) {
            report_error("memcpy is not supported by the " + std::string(db.model) +
                         " gadget table", "E102");
            return;
        }
        call("pop qr8");
        for (int i = 0; i < 3; i++) emit_le(eval(s->args[i], log), 2);
        emit_le(0, 2);   // pad
        call("memcpy_length_rn/8");
    }
    // strcpy(dst, src): XR0 = dst, src.
    void emit_strcpy(const StmtP& s) {
        if (s->args.size() != 2) { report_error("strcpy expects destination and source", "E101"); return; }
        call("pop xr0");
        emit_le(eval(s->args[0], log), 2);
        emit_le(eval(s->args[1], log), 2);
        // The 580 table has both `BL strcpy` (203CC) and `strcpy` (0E5C8);
        // the community loops use the BL variant.  The 880 only has strcpy.
        call(db.find_gadget("bl_strcpy") != 0xFFFFFFFFu ? "bl_strcpy" : "strcpy");
    }
    void emit_strcat(const StmtP& s) {
        if (s->args.size() != 2) { report_error("str_cat expects destination and source", "E101"); return; }
        call("pop xr0");
        emit_le(eval(s->args[0], log), 2);
        emit_le(eval(s->args[1], log), 2);
        call(db.find_gadget("bl_strcat") != 0xFFFFFFFFu ? "bl_strcat" : "strcat");
    }
    void emit_strlen(const StmtP& s) {
        if (s->args.size() != 1) { report_error("str_len expects a string address", "E101"); return; }
        call("setlr");
        call("pop er0");
        emit_le(eval(s->args[0], log), 2);
        call("strlen");
    }
    // set_byte(addr, value): XR0 = addr, value, 0; [er0] = r2.
    void emit_set_byte(const StmtP& s) {
        if (s->args.size() != 2) { report_error("set_byte expects address and value", "E101"); return; }
        const uint32_t addr = eval(s->args[0], log);
        const uint32_t value = eval(s->args[1], log);
        if (value > 0xFF)
            log += "  ; warning [W003]: byte value " + std::to_string(value) + " is truncated\n";
        call("pop xr0");
        emit_le(addr, 2);
        out.push_back((uint8_t)(value & 0xFF));
        out.push_back(0);
        call("[er0]=r2");
    }
    // set_word(addr, value): XR0 = addr, value; [er0] = er2.
    void emit_set_word(const StmtP& s) {
        if (s->args.size() != 2) { report_error("set_word expects address and value", "E101"); return; }
        // [er0]=er2,rt returns through LR, which a previous BL may have lost.
        call("setlr");
        call("pop xr0");
        emit_le(eval(s->args[0], log), 2);
        emit_le(eval(s->args[1], log), 2);
        call("[er0]=er2,rt");
    }
    // add_word(addr, delta): ER2 = delta, ER8 = addr, [er8] += er2.
    void emit_add_word(const StmtP& s) {
        if (s->args.size() != 2) { report_error("add_word expects address and delta", "E101"); return; }
        uint32_t address = 0;
        std::string found;
        if (!find_addr("[er8]+=er2,pop xr8", address, found)) {
            report_error("add_word is not supported by the " + std::string(db.model) +
                         " gadget table", "E102");
            return;
        }
        call("pop er2");
        emit_le(eval(s->args[1], log), 2);
        call("pop er8");
        emit_le(eval(s->args[0], log), 2);
        call("[er8]+=er2,pop xr8");
        out.push_back(0x30);   // the gadget pops XR8 from the chain
        out.push_back(0x30);
        out.push_back(0x30);
        out.push_back(0x30);
    }
    void stmt(const StmtP& s) {
        switch (s->k) {
            case Stmt::Assign: {
                if (is_register(s->lhs)) {
                    call("pop " + lower(s->lhs));
                    emit_le(eval(s->rhs, log), reg_size(s->lhs));
                } else {
                    uint32_t v = eval(s->rhs, log);
                    uint32_t addr = alloc_var(s->lhs);
                    log += "  ; " + s->lhs + " @ " + hex4(addr) + " = " + std::to_string(v) + "\n";
                    emit_word_store(addr, v);
                }
                return;
            }
            case Stmt::Call: {
                std::string key = lower(s->name);
                // The text/graphics builtins have their own argument ABI and
                // are emitted by hand; everything else goes through the
                // generic "call then arguments" path.
                if (key == "print" || key == "print_line") { emit_print(s, false); return; }
                if (key == "print_small") { emit_print(s, true); return; }
                if (key == "line" || key == "draw_line") {
                    emit_line(s);
                    return;
                }
                if (key == "rect" || key == "draw_rect") { emit_rect(s); return; }
                if (key == "bitmap" || key == "draw_bitmap") { emit_bitmap(s); return; }
                if (key == "getkey" || key == "get_key" || key == "key" ||
                    key == "getkeycode" || key == "get_keycode" ||
                    key == "getscancode_nodelay" || key == "getscancode") { emit_getkey(s); return; }
                if (key == "keytable" || key == "table") { emit_keytable(s); return; }
                if (key == "delay" || key == "sleep") { emit_delay(s); return; }
                if (key == "calc_func" || key == "calc") { emit_calc(s); return; }
                if (key == "num_to_str") { emit_num_to_str(s); return; }
                if (key == "pixel" || key == "draw_pixel") { emit_pixel(s); return; }
                if (key == "memcpy" || key == "mem_copy") { emit_memcpy(s); return; }
                if (key == "strcpy" || key == "str_copy") { emit_strcpy(s); return; }
                if (key == "strcat" || key == "str_cat") { emit_strcat(s); return; }
                if (key == "strlen" || key == "str_len") { emit_strlen(s); return; }
                if (key == "set_byte" || key == "poke") { emit_set_byte(s); return; }
                if (key == "set_word" || key == "poke_word") { emit_set_word(s); return; }
                if (key == "add_word" || key == "add16") { emit_add_word(s); return; }
                emit_generic_call(s);
                return;
            }
            case Stmt::If: {
                int c = eval_cond(s->cond);
                if (c == 0) { for (auto& x : s->else_b) stmt(x); return; }
                if (c == 1) { for (auto& x : s->then_b) stmt(x); return; }
                report_error("dynamic if conditions are not supported by the ROP chain backend; use keytable() or explicit gadget flow", "E105");
                return;
            }
            case Stmt::While: {
                std::string lp = "loop_" + std::to_string(out.size());
                std::string en = "end_" + std::to_string(out.size());
                labels[lp] = out.size();
                log += "; " + lp + "\n";
                int c = eval_cond(s->cond);
                if (c == 0) { labels[en] = out.size(); log += "  ; while false\n"; return; }
                if (c == 2) {
                    report_error("dynamic while conditions are not supported by the ROP chain backend; use labels and gadget flow", "E105");
                    return;
                }
                for (auto& x : s->body) stmt(x);
                jump_label(lp, "goto " + lp);
                labels[en] = out.size();
                log += "; " + en + "\n";
                return;
            }
            case Stmt::For: {
                if (s->init) stmt(s->init);
                std::string lp = "for_" + std::to_string(out.size());
                std::string en = "for_end_" + std::to_string(out.size());
                labels[lp] = out.size();
                int c = s->cond ? eval_cond(s->cond) : 1;
                if (c == 2) {
                    report_error("dynamic for-loop conditions are not supported by the ROP chain backend", "E105");
                    return;
                }
                if (c != 0) {
                    for (auto& x : s->body) stmt(x);
                    if (s->step) stmt(s->step);
                    jump_label(lp, "goto " + lp);
                }
                labels[en] = out.size();
                return;
            }
            case Stmt::Goto: jump_label(s->name, "goto " + s->name); return;
            case Stmt::Label: labels[s->name] = out.size(); log += "; " + s->name + "\n"; return;
            case Stmt::Asm:
                for (auto& l : s->asm_lines) {
                    log += "  ; asm " + l + "\n";
                    std::string k = lower(l);
                    uint32_t a = 0; std::string f;
                    if (find_addr(k, a, f)) { call(k); continue; }
                    if (k.rfind("0x", 0) == 0 || k.rfind("hex ", 0) == 0) {
                        std::string h = k.rfind("hex ", 0) == 0 ? k.substr(4) : k.substr(2);
                        h.erase(std::remove_if(h.begin(), h.end(), [](unsigned char c) { return !std::isxdigit(c); }), h.end());
                        for (size_t i = 0; i + 1 < h.size(); i += 2)
                            out.push_back((uint8_t)std::strtoul(h.substr(i, 2).c_str(), nullptr, 16));
                    }
                }
                return;
            case Stmt::Return: log += "  ; return\n"; call("rt"); return;
        }
    }
    static std::string hex4(uint32_t v) { char b[8]; std::snprintf(b, sizeof b, "%04X", v); return b; }
    static std::string hex2(uint32_t v) { char b[8]; std::snprintf(b, sizeof b, "%02X", v & 0xFF); return b; }
    RopResult run(const Program& prog) {
        RopResult r;
        home = prog.has_org ? prog.org : 0xE9E0;
        bool has_lr_setup = false;
        bool has_sfr_setup = false;
        for (const auto& f : prog.funcs) {
            for (const auto& s : f.body) {
                if (s->k != Stmt::Call) continue;
                const std::string n = lower(s->name);
                has_lr_setup = has_lr_setup || n == "setlr_pc" || n == "setlr";
                has_sfr_setup = has_sfr_setup || n == "setsfr";
            }
        }
        // This is the exact hdcompiler/RAC preamble.  setlr_pc is a different
        // gadget and does not reproduce the working RAC chain's startup.
        if (!has_lr_setup) call("setlr");
        if (!has_sfr_setup) call("setsfr");
        for (auto& g : prog.globals) {
            uint32_t a = g.has_addr ? g.addr : alloc_var(g.name);
            alloc[g.name] = a;
            const uint32_t initial = g.has_init ? g.init : 0;
            log += "  ; global " + g.name + " @ " + hex4(a) + " = " + std::to_string(initial) + "\n";
            emit_word_store(a, initial);
        }
        for (auto& f : prog.funcs) {
            log += "; func " + f.name + "\n";
            for (auto& s : f.body) stmt(s);
        }
        call("brk"); // halt after main: without it the chain runs into the
                     // string pool and the calculator resets.
        // Append cmp_ea tables.  They are part of the payload and the launcher
        // copies them together with the code, so their addresses use the
        // execution address (home), just like the community `adr_of table`.
        for (auto& t : key_tables) {
            const uint32_t base = home + (uint32_t)out.size();
            out[t.addr_pos] = (uint8_t)(base & 0xFF);
            out[t.addr_pos + 1] = (uint8_t)((base >> 8) & 0xFF);
            log += "  ; keytable @ [" + hex4(base) + "]\n";
            for (const auto& e : t.entries) {
                emit_le(e.key, 2);
                size_t pos = out.size();
                out.push_back(0);
                out.push_back(0);
                patches.push_back({pos, e.label});
            }
        }
        // append string pool
        for (auto& sp : str_pool) {
            size_t off = out.size();
            out.insert(out.end(), sp.second.begin(), sp.second.end());
            uint32_t target = data_home + (uint32_t)off;
            out[sp.first] = (uint8_t)(target & 0xFF);
            out[sp.first + 1] = (uint8_t)((target >> 8) & 0xFF);
            log += "  ; str pool -> [" + hex4(target) + "] " + std::to_string(sp.second.size()) + " bytes\n";
        }
        // patch gotos
        for (auto& p : patches) {
            auto it = labels.find(p.second);
            if (it == labels.end()) { log += "  ; warning: label " + p.second + " not found\n"; continue; }
            uint32_t target = home + (uint32_t)it->second;
            // "sp = er6, pop er8" (and the other branch gadgets) load PC from
            // the stored word after pre-incrementing the stack pointer, so a
            // chain stores "label - 2".  The community tables spell this as
            // `adr_of [-2] label`.
            target -= 2;
            out[p.first] = (uint8_t)(target & 0xFF);
            out[p.first + 1] = (uint8_t)((target >> 8) & 0xFF);
            log += "  ; patch " + p.second + " -> [" + hex4(target) + "]\n";
        }
        if (prog.funcs.empty()) report_error("no function found in ROP source", "E106");
        // A ROP payload always ends with a null buffer.  The community
        // programs rely on it: the copy performed by the launcher moves a
        // fixed number of bytes, and the trailing zeros keep the chain from
        // running into whatever the calculator has in RAM after the program.
        // 256 bytes is the default; `#pad N` sets another size and `#nopad`
        // turns the buffer off.
        if (pad_size > 0) {
            out.insert(out.end(), (size_t)pad_size, (uint8_t)0);
            log += "  ; null buffer: " + std::to_string(pad_size) + " bytes\n";
        }
        r.ok = !failed;
        if (failed) r.error = first_error.empty() ? "ROP compilation failed" : first_error;
        r.bytes = std::move(out);
        r.base = home;
        // The low level view of the chain comes first, the notes follow.
        std::string listing = build_listing(r.bytes, rows, data_home);
        if (home != data_home)
            listing = "; payload injected at " + hex4(data_home) + ", executed at " + hex4(home) + "\n" + listing;
        r.log = listing;
        if (!log.empty()) r.log += log;
        return r;
    }
};

enum class LauncherKind { Common, Simple };

struct LauncherOpts {
    LauncherKind kind = LauncherKind::Common;
    uint32_t base = 0x8154;     // where the launcher bytes are placed
    bool has_base = false;
    uint32_t length = 2;        // RAC 580 launcher size code: 1 short, 2 medium, 3 long
    uint32_t backup = 0;        // 0 = use the payload base (#org)
    uint32_t main = 0xD730;     // 580 address the payload is copied to
    bool has_length = false;
    bool has_backup = false;
    bool has_main = false;
};

uint32_t parse_number(std::string a) {
    if (!a.empty() && a.front() == '[') a.erase(a.begin());
    if (!a.empty() && a.back() == ']') a.pop_back();
    if (a.rfind("0x", 0) == 0 || a.rfind("0X", 0) == 0) a = a.substr(2);
    return (uint32_t)std::strtoul(a.c_str(), nullptr, 16);
}

uint32_t parse_length(std::string a) {
    const std::string l = lower(a);
    if (l == "short" || l == "small") return 1;
    if (l == "medium" || l == "mid") return 2;
    if (l == "long" || l == "large") return 3;
    return parse_number(a) & 0xFF;
}

std::string strip_directives(std::string src, std::string& model, uint32_t& org, bool& has_org,
                             LauncherOpts& lopts, int& pad) {
    std::string out;
    std::istringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i != std::string::npos && line[i] == '#') {
            std::istringstream ls(line.substr(i + 1));
            std::string d; ls >> d;
            std::string dlow = lower(d);
            if (dlow == "for") {
                std::string m; ls >> m;
                std::string ml = lower(m);
                if (ml == "580" || ml == "580vnx" || ml == "fx-580vn" ) model = "580";
                else if (ml == "880" || ml == "880btg") model = "880";
            } else if (dlow == "org") {
                std::string a; ls >> a;
                org = parse_number(a);
                has_org = true;
            } else if (dlow == "pad" || dlow == "nopad") {
                if (dlow == "nopad") {
                    pad = 0;
                } else {
                    std::string a; ls >> a;
                    if (!a.empty()) pad = (int)parse_number(a);
                }
            } else if (dlow == "launcher") {
                std::string a;
                if (!(ls >> a)) {
                    lopts.kind = LauncherKind::Common;
                } else {
                    const std::string al = lower(a);
                    if (al == "common" || al == "shared" || al == "chung") {
                        lopts.kind = LauncherKind::Common;
                    } else if (al == "simple" || al == "stub") {
                        lopts.kind = LauncherKind::Simple;
                    } else {
                        // Backwards compatible: `#launcher 8154` places the
                        // simple stub at that address.
                        lopts.kind = LauncherKind::Simple;
                        lopts.base = parse_number(a);
                        lopts.has_base = true;
                    }
                }
                if (lopts.kind == LauncherKind::Common) {
                    std::string v;
                    if (ls >> v) { lopts.length = parse_length(v); lopts.has_length = true; }
                    if (ls >> v) { lopts.backup = parse_number(v); lopts.has_backup = true; }
                    if (ls >> v) { lopts.main = parse_number(v); lopts.has_main = true; }
                } else {
                    std::string v;
                    if (ls >> v) { lopts.base = parse_number(v); lopts.has_base = true; }
                }
            }
            out += "\n";
            continue;
        }
        out += line + "\n";
    }
    return out;
}

// The ROP backend shares the CWiz gadget emitter, but the editor/CLI also lets
// users choose Wiz's indentation-based syntax. Translate that small statement
// subset into the parser's C-like syntax instead of silently treating `.wz` as
// CWiz. The generated form is intentionally simple; unsupported dynamic flow
// is rejected later by the emitter rather than producing a misleading chain.
std::string wiz_rop_to_cwiz(const std::string& source) {
    std::string out;
    std::vector<int> blocks;
    auto trim = [](std::string s) {
        const size_t first = s.find_first_not_of(" \t\r");
        if (first == std::string::npos) return std::string();
        const size_t last = s.find_last_not_of(" \t\r");
        return s.substr(first, last - first + 1);
    };
    auto spaces = [](const std::string& s) {
        int n = 0;
        for (char c : s) {
            if (c == ' ') n++;
            else if (c == '\t') n += 4;
            else break;
        }
        return n;
    };
    auto convert_expr = [](std::string s) {
        s = replace_all(s, " and ", " && ");
        s = replace_all(s, " or ", " || ");
        s = replace_all(s, "not ", "!");
        s = replace_all(s, "True", "1");
        s = replace_all(s, "False", "0");
        return s;
    };
    auto close_to = [&](int indent, bool include_equal) {
        while (!blocks.empty() && (include_equal ? blocks.back() >= indent : blocks.back() > indent)) {
            out += "}\n";
            blocks.pop_back();
        }
    };
    auto split_args = [&trim](const std::string& s) {
        std::vector<std::string> args;
        int depth = 0;
        size_t start = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '(') depth++;
            else if (s[i] == ')') depth--;
            else if (s[i] == ',' && depth == 0) {
                args.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        args.push_back(s.substr(start));
        for (auto& a : args) a = trim(a);
        return args;
    };

    for (const std::string& raw : split(replace_all(source, "\r", ""), '\n')) {
        const int indent = spaces(raw);
        std::string line = trim(raw);
        if (line.empty()) continue;

        // Preserve ROP/backend directives, but drop ordinary Wiz comments.
        if (line[0] == '#') {
            const std::string d = lower(line);
            if (d == "#rop" || d.rfind("#for ", 0) == 0 || d.rfind("#org ", 0) == 0 ||
                d.rfind("#launcher", 0) == 0 || d.rfind("#pad", 0) == 0 || d == "#nopad")
                out += line + "\n";
            continue;
        }
        const size_t comment = line.find('#');
        if (comment != std::string::npos) line = trim(line.substr(0, comment));
        if (line.empty()) continue;

        if (line == "else:" || line.rfind("elif ", 0) == 0) {
            if (blocks.empty() || blocks.back() != indent)
                fail("Wiz ROP indentation: else/elif without matching if");
            out += "}\n";
            blocks.pop_back();
            if (line == "else:") out += "else {\n";
            else out += "else if (" + convert_expr(trim(line.substr(5, line.size() - 6))) + ") {\n";
            blocks.push_back(indent);
            continue;
        }
        close_to(indent, true);

        if (line.rfind("def ", 0) == 0 && line.back() == ':') {
            std::string sig = trim(line.substr(4, line.size() - 5));
            const size_t lp = sig.find('(');
            const size_t rp = sig.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos || rp < lp)
                fail("Wiz ROP syntax: expected def name(...):");
            out += "void " + trim(sig.substr(0, lp)) + "(" + sig.substr(lp + 1, rp - lp - 1) + ") {\n";
            blocks.push_back(indent);
            continue;
        }
        if ((line.rfind("if ", 0) == 0 || line.rfind("while ", 0) == 0) && line.back() == ':') {
            const bool is_if = line.rfind("if ", 0) == 0;
            out += std::string(is_if ? "if (" : "while (") +
                   convert_expr(trim(line.substr(is_if ? 3 : 6, line.size() - (is_if ? 4 : 7)))) + ") {\n";
            blocks.push_back(indent);
            continue;
        }
        if (line.rfind("for ", 0) == 0 && line.back() == ':') {
            std::string spec = trim(line.substr(4, line.size() - 5));
            const size_t in = spec.find(" in range(");
            const size_t rp = spec.rfind(')');
            if (in == std::string::npos || rp == std::string::npos || rp < in)
                fail("Wiz ROP syntax: for-loops must use range(start, stop[, step])");
            const std::string var = trim(spec.substr(0, in));
            const auto args = split_args(spec.substr(in + 10, rp - (in + 10)));
            if (args.size() < 1 || args.size() > 3)
                fail("Wiz ROP syntax: range() expects one to three arguments");
            const std::string start = args.size() == 1 ? "0" : convert_expr(args[0]);
            const std::string stop = convert_expr(args.size() == 1 ? args[0] : args[1]);
            const std::string step = args.size() == 3 ? convert_expr(args[2]) : "1";
            const bool down = !step.empty() && step[0] == '-';
            out += "for (" + var + " = " + start + "; " + var + (down ? " > " : " < ") + stop + "; " +
                   var + (down ? " -= " : " += ") + (down ? step.substr(1) : step) + ") {\n";
            blocks.push_back(indent);
            continue;
        }
        if (line == "pass" || line.rfind("global ", 0) == 0) continue;
        if (line.back() == ':' && line.find_first_of(" \t") == std::string::npos) {
            out += line + "\n";  // ROP labels (e.g. `loop:`)
            continue;
        }
        out += convert_expr(line);
        if (out.back() != ';' && out.back() != '}' && out.back() != ':') out += ';';
        out += '\n';
    }
    close_to(-1, true);
    return out;
}

std::string hex_bytes(const std::vector<uint8_t>& bytes) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) out.push_back(' ');
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return out;
}

} // namespace

bool source_is_rop(const std::string& source) {
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i != std::string::npos && line.compare(i, 4, "#rop") == 0) return true;
    }
    return false;
}

RopResult build_rop(const std::string& source, const std::string& model, const std::string& language) {
    RopResult r;
    r.model = (model == "880" || lower(model) == "880btg") ? "880" : "580";
    try {
        std::string m = r.model;
        uint32_t org = 0xE9E0;
        bool has_org = false;
        LauncherOpts lopts;
        int pad = 256;  // null buffer appended to the payload
        const std::string selected = select_sections(source, get_target(m));
        const std::string normalized = lower(language) == "wiz" ? wiz_rop_to_cwiz(selected) : selected;
        std::string body = strip_directives(normalized, m, org, has_org, lopts, pad);
        r.model = m;
        Program prog = Parser(lex(body)).parse();
        if (prog.model.empty()) prog.model = m;
        if (!prog.model.empty()) {
            std::string pm = lower(prog.model);
            if (pm.find("880") != std::string::npos) r.model = "880";
            else if (pm.find("580") != std::string::npos) r.model = "580";
        }
        if (!has_org) org = (m == "880") ? 0xE330 : 0xE9E0;

        // The 580 common launcher copies the injected payload before it lets
        // the chain run.  Absolute addresses emitted into the chain therefore
        // have to be based on the copy destination, not on the temporary
        // injection address.  880 common and the simple stub execute in place.
        const uint32_t backup = lopts.has_backup ? lopts.backup : org;
        const uint32_t main = lopts.has_main ? lopts.main : 0xD730;
        const bool common = lopts.kind == LauncherKind::Common;
        const bool common_580 = common && m == "580";
        const uint32_t execution_org = common_580 ? main : (common ? backup : org);
        prog.org = execution_org;
        prog.has_org = true;
        Db db;
        db.model = r.model;
        db.load(r.model == "880" ? ROP_DB_880 : ROP_DB_580);
        Emitter em(db);
        em.data_home = common ? backup : org;
        em.pad_size = pad;
        r = em.run(prog);
        r.model = r.model.empty() ? m : r.model;
        // `base` is where the debugger writes the payload.  It is not
        // necessarily the address used for absolute pointers inside it.
        r.base = common ? backup : org;

        auto emit16 = [](std::vector<uint8_t>& out, uint32_t v) {
            out.push_back((uint8_t)(v & 0xFF));
            out.push_back((uint8_t)((v >> 8) & 0xFF));
        };

        // The common launcher is entered through the calculator's program
        // input area; the simple stub defaults to the historical 8154 entry.
        if (lopts.has_base)
            r.launcher_base = lopts.base;
        else
            r.launcher_base = (r.model == "880" ? 0x9268u : 0xD180u);
        const uint32_t length = lopts.length;
        // RAC length codes: 01 -> 0x01FE (510) bytes, 02 -> 0x02FE (766),
        // 03 -> 0x03FE (1022) bytes.
        const uint32_t code = length == 1 ? 1u : (length == 2 ? 2u : 3u);
        const uint32_t copy_len = (code << 8) | 0xFEu;

        // The launcher gadgets are taken from the gadget database as well, so
        // that no ROM address is hard coded in the compiler.  The simple stub
        // keeps its historical bytes: for the 880 the database does not know
        // these two gadgets, so the old addresses are used as a fallback.
        std::string missing_gadget;
        auto launcher_gadget = [&](const char* name, uint32_t fallback) -> uint32_t {
            const uint32_t a = db.find_gadget(name);
            if (a == 0xFFFFFFFFu) {
                if (fallback == 0) {
                    if (missing_gadget.empty()) missing_gadget = name;
                    return 0;
                }
                return fallback;
            }
            return a;
        };

        if (lopts.kind == LauncherKind::Common) {
            if (r.model == "880") {
                // 880 common launcher: FD 26 | 2E E3 | C0 75 32 30
                // (C0 75 32 30 = segment-2 call of the 880 sp gadget).
                r.launcher.push_back(0xFD);
                r.launcher.push_back(0x26);
                emit16(r.launcher, (backup - 2) & 0xFFFF);   // 2E E3
                r.launcher.push_back(0xC0);
                r.launcher.push_back(0x75);
                r.launcher.push_back(0x32);
                r.launcher.push_back(0x30);
            } else {
                // The 580 "launcher chung", byte for byte the one from the
                // community guide (section 3.1):
                //
                //   FD 24 30 30 | DA 7B 31 30 | FE <code> | <src> | <dst> |
                //   <dst-2> | 32 89 31 30 | 30 30 | 74 1F 32 48
                //
                // "pop qr0" loads the copy length, the backup address, the
                // execution address and the jump target; the copy gadget then
                // moves the payload from the backup area to the execution area
                // and the last word branches into it.
                emit_gadget(r.launcher, 0x024FD);                  // trigger
                emit_gadget(r.launcher, launcher_gadget("pop qr0", 0));
                r.launcher.push_back(0xFE);                       // length marker
                r.launcher.push_back((uint8_t)(code & 0xFF));     // 01/02/03
                emit16(r.launcher, backup);                       // source
                emit16(r.launcher, main);                         // destination
                emit16(r.launcher, (main - 2) & 0xFFFF);          // jump target
                emit_gadget(r.launcher, launcher_gadget("launcher_copy", 0));
                r.launcher.push_back(0x30);                       // filler
                r.launcher.push_back(0x30);
                emit_gadget(r.launcher, launcher_gadget("sp = er6,pop er8", 0),
                            kChainVariantBranch);
                if (r.bytes.size() > copy_len)
                    r.log += "  ; warning [W002]: payload too large for launcher length code (" +
                             std::to_string(copy_len) + " bytes, payload is " +
                             std::to_string(r.bytes.size()) + " bytes)\n";
            }
            r.log += "  ; launcher (" + std::string(r.model == "880" ? "880 common" : "580 common") +
                     ") " + std::to_string(r.launcher.size()) + " bytes\n";
        } else {
            // Simple 10-byte stub: pop er14 / payload-2 / sp = er14, pop er14.
            //
            // This is the launcher from the community notes: "78 5C 31 30
            // [payload - 2] 60 0D 32 30".  The gadgets come from the gadget
            // database of the model, so the 580 uses 0x15C78 / 0x20D60 and the
            // 880 uses its own 0x1086C / 0x29852.
            const uint32_t pop_er14 = r.model == "880"
                                          ? launcher_gadget("pop er14", 0x1086Cu)
                                          : launcher_gadget("pop er14", 0x15C78u);
            const uint32_t set_sp = r.model == "880"
                                        ? launcher_gadget("sp = er14,pop er14", 0x29852u)
                                        : launcher_gadget("sp = er14,pop er14", 0x20D60u);
            emit_gadget(r.launcher, pop_er14);
            emit16(r.launcher, (r.base >= 2 ? r.base - 2 : r.base) & 0xFFFF);
            emit_gadget(r.launcher, set_sp);
            r.log += "  ; launcher (simple) " + std::to_string(r.launcher.size()) + " bytes\n";
        }
        if (!missing_gadget.empty()) {
            r.ok = false;
            r.error = "launcher gadget '" + missing_gadget + "' not found in the gadget database";
            return r;
        }
        r.launcher_hex = hex_bytes(r.launcher);
        char hb[16];
        std::snprintf(hb, sizeof hb, "%04X", r.launcher_base);
        char pb[16];
        std::snprintf(pb, sizeof pb, "%04X", r.base);
        r.log += std::string("  ; launcher @ ") + hb + " -> [" + pb + "]\n";
        return r;
    } catch (const std::exception& e) {
        RopResult bad;
        bad.ok = false;
        bad.error = e.what();
        bad.model = r.model;
        return bad;
    }
}

} // namespace wiz
