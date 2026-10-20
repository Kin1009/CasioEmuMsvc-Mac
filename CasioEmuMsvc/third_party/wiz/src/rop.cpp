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
//   #launcher 8154       the simple stub placed at an explicit address
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
// The emitter is a C++ port of CasioC's casio-asm (v0): a call is the
// hdcompiler chain word 0x30300000 + gadget address, `print("s", x, y)`
// builds an xr0 argument block, and strings are appended to a pool whose
// addresses are patched in.

#include "rop.hpp"
#include "rop_data.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace wiz {
namespace {

// ---------------------------------------------------------------- charset
struct CharMap {
    std::unordered_map<unsigned, std::vector<uint8_t>> map;
    CharMap() {
        static const char* chars =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
            "ÁáÀàẢảÃãẠạĂăẮắẰằẲẳẴẵẶặÂâẤấẦầẨẩẪẫẬậÉéÈèẺẻẼẽẸẹÊêẾếỀềỂểỄễỆệ"
            "ÍíÌìỈỉĨĩỊịÓóÒòỎỏÕõỌọÔôỐốỒồỔổỖỗỘộƠơỚớỜờỞởỠỡỢợÚúÙùỦủŨũỤụ"
            "ƯưỨứỪừỬửỮữỰựÝýỲỳỶỷỸỹỴỵĐđ~@_&-+()/*':!?|√÷×^°{}[]%.,";
        static const char* hexes[] = {
            "30","31","32","33","34","35","36","37","38","39",
            "41","42","43","44","45","46","47","48","49","4A",
            "4B","4C","4D","4E","4F","50","51","52","53","54",
            "55","56","57","58","59","5A","61","62","63","64",
            "65","66","67","68","69","6A","6B","6C","6D","6E",
            "6F","70","71","72","73","74","75","76","77","78",
            "79","7A","F451","F471","F450","F470","F454","F474",
            "F453","F473","F410","F465","F455","F475","F411","F431",
            "F412","F432","F490","F456","F491","F457","F413","F433",
            "F452","F472","F414","F434","F415","F435","F416","F436",
            "F492","F477","F417","F437","F459","F479","F458","F478",
            "F45B","F47B","F418","F438","F419","F439","F45A","F47A",
            "F41A","F43A","F41B","F43B","F41C","F43C","F41D","F43D",
            "F41E","F43E","F45D","F47D","F45C","F47C","F42B","F47F",
            "F45E","F47E","F428","F448","F463","F483","F462","F482",
            "F429","F486","F430","F485","F42A","F487","F464","F484",
            "F41F","F43F","F420","F440","F421","F441","F422","F442",
            "F423","F445","F444","F44D","F425","F44E","F426","F446",
            "F427","F447","F443","F46E","F424","F48E","F46A","F48A",
            "F469","F489","F42C","F48C","F42D","F48B","F42E","F488",
            "F44F","F46F","F44A","F461","F44B","F467","F44C","F468",
            "F48F","F476","F449","F481","F46D","F48D","F42F","F45F",
            "F493","F466","F494","F46B","F495","F46C","F460","F480",
            "20","40","5F","1A","2D","2B","28","29","2F",
            "2A","27","3A","21","3F","7C","98","26",
            "24","5E","85","7B","7D","5B","5D","25",
            "2E","2C",
        };
        // decode UTF-8 so multi-byte Vietnamese letters work
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
    size_t i = 0, line = 1;
    auto push = [&](Tk k, std::string s, uint32_t n = 0) { Tok t; t.k = k; t.s = std::move(s); t.n = n; out.push_back(t); };
    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
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
    size_t line_ = 1;
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
            else if (v.k == Tk::Ident) { g.init = 0; g.has_init = true; }
            else if (v.k == Tk::Str) { g.init = 0; g.has_init = true; }
            else fail("bad initialiser");
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
            if (name.size() > 3 && std::tolower((unsigned char)name[0]) == 'b' &&
                std::tolower((unsigned char)name[1]) == 'l' && std::isspace((unsigned char)name[2]))
                name.erase(0, name.find_first_not_of(" \t", 2));
            name = key(name);
            uint32_t addr = (uint32_t)std::strtoul(a.c_str(), nullptr, 16);
            if (name.size() > 2 && name[0] == 'd' && name[1] == '_') data[name.substr(2)] = addr;
            else labels[name] = addr;
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
    if (k == "screen_del" || k == "screen_clear" || k == "cls") push({"buffer_clear","buffer_clear.ca54","buffer_clear.d654","memzero"});
    else if (k == "screen_fill") push({"fill_screen"});
    else if (k == "draw_line" || k == "line") push({"line_draw"});
    else if (k == "draw_pixel" || k == "pixel") push({"pixel_draw"});
    else if (k == "draw_rect" || k == "rect") push({"line_draw"});
    else if (k == "draw_byte") push({"draw_byte"});
    else if (k == "print" || k == "print_line") push({"smallprint","line_print","printline","basen_base_print"});
    else if (k == "print_at" || k == "draw_text") push({"line_print"});
    else if (k == "print_small") push({"smallprint"});
    else if (k == "print_num" || k == "print_dec" || k == "show_score") push({"hex_to_dec"});
    else if (k == "print_hex") push({"hex_byte"});
    else if (k == "render" || k == "flush") push({"render","render.ddd4","render.ca54","render_bitmap"});
    else if (k == "delay" || k == "sleep") push({"delay"});
    else if (k == "get_key" || k == "key") push({"getkey","getkeycode","getscancode","cvt_key"});
    else if (k == "is_key_pressed" || k == "check_key") push({"check_any_key_pressed__"});
    else if (k == "wait_key" || k == "pause") push({"diagnostic_wait_key"});
    else if (k == "mem_copy" || k == "memcpy") push({"memcpy","smart_strcpy","memmove"});
    else if (k == "mem_set" || k == "memset") push({"memset","memzero"});
    else if (k == "mem_zero" || k == "memzero") push({"memzero"});
    else if (k == "mem_move") push({"memmove"});
    else if (k == "double_buffer_flip" || k == "flip") push({"buf1_to_buf2"});
    else if (k == "str_copy") push({"strcpy","smart_strcpy"});
    else if (k == "str_cat") push({"strcat","smart_strcat"});
    else if (k == "str_len") push({"strlen","smart_strlen_n","byte_strlen_n"});
    else if (k == "fill_screen") push({"fill_screen"});
    else if (k == "buffer_clear") push({"buffer_clear","buffer_clear.ca54"});
    v.push_back(k);
    return v;
}

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
    uint32_t home = 0xE9E0;

    explicit Emitter(const Db& d) : db(d) {}

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
        uint32_t addr = 0; std::string found;
        if (!find_addr(name, addr, found)) {
            log += "  ; error [E100]: gadget/label " + name + " not found (aliases tried)\n";
            return;
        }
        emit_le(addr + 0x30300000u, 4);
        if (lower(found) != lower(name)) log += "  ; call " + name + " -> " + found + "\n";
        else log += "  ; call " + name + "\n";
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
                return alloc.count(e->s) ? alloc[e->s] : 0;
            }
            case Expr::Str: return (uint32_t)e->s.size();
            case Expr::Unary: {
                uint32_t v = eval(e->a, err);
                return e->s == "!" ? (v == 0 ? 1u : 0u) : v;
            }
            case Expr::Call: return e->args.empty() ? 0 : eval(e->args[0], err);
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
    void stmt(const StmtP& s) {
        switch (s->k) {
            case Stmt::Assign: {
                if (is_register(s->lhs)) {
                    call("pop " + lower(s->lhs));
                    emit_le(eval(s->rhs, log), reg_size(s->lhs));
                } else {
                    uint32_t v = eval(s->rhs, log);
                    uint32_t addr = alloc_var(s->lhs);
                    call("pop er0");
                    emit_le(v, 2);
                    log += "  ; " + s->lhs + " @ " + hex4(addr) + " = " + std::to_string(v) + "\n";
                    emit_le(addr, 2);
                }
                return;
            }
            case Stmt::Call: {
                std::string key = lower(s->name);
                // print(...) accepts both orders: CWiz's print(x, y, "text")
                // and CasioC's print("text", x, y).
                size_t text_arg = 3;
                if (key == "print" && s->args.size() == 3) {
                    if (s->args[0]->k == Expr::Str) text_arg = 0;
                    else if (s->args[2]->k == Expr::Str) text_arg = 2;
                }
                if (text_arg != 3) {
                    const ExprP& text = s->args[text_arg];
                    const ExprP& ax = s->args[text_arg == 0 ? 1 : 0];
                    const ExprP& ay = s->args[text_arg == 0 ? 2 : 1];
                    std::vector<uint8_t> enc; std::string err;
                    if (!charmap().encode(text->s, enc, err)) { log += "  ; error: " + err + "\n"; return; }
                    uint32_t x = eval(ax, log), y = eval(ay, log);
                    call("pop xr0");
                    out.push_back((uint8_t)(x & 0xFF));
                    out.push_back((uint8_t)(y & 0xFF));
                    size_t sp = out.size();
                    out.push_back(0); out.push_back(0);
                    str_pool.push_back({sp, enc});
                    log += "  ; print \"" + text->s + "\" x=" + std::to_string(x) + " y=" + std::to_string(y) + "\n";
                    call("line_print");
                    return;
                }
                uint32_t a = 0; std::string f;
                if (find_addr(key, a, f)) {
                    call(key);
                    for (auto& arg : s->args) {
                        if (arg->k == Expr::Str) {
                            std::vector<uint8_t> enc; std::string err;
                            if (!charmap().encode(arg->s, enc, err)) { log += "  ; error: " + err + "\n"; continue; }
                            size_t pos = out.size();
                            out.push_back(0); out.push_back(0);
                            str_pool.push_back({pos, enc});
                        } else emit_le(eval(arg, log), 2);
                    }
                } else {
                    log += "  ; warning [W001]: function " + s->name + " not in gadgets/labels -> skipped\n";
                    for (auto& arg : s->args) emit_le(eval(arg, log), 2);
                }
                return;
            }
            case Stmt::If: {
                int c = eval_cond(s->cond);
                if (c == 0) { for (auto& x : s->else_b) stmt(x); return; }
                if (c == 1) { for (auto& x : s->then_b) stmt(x); return; }
                std::string el = "else_" + std::to_string(out.size());
                std::string en = "end_" + std::to_string(out.size());
                for (auto& x : s->then_b) stmt(x);
                jump_label(en, "goto " + en);
                labels[el] = out.size();
                log += el + ":\n";
                for (auto& x : s->else_b) stmt(x);
                labels[en] = out.size();
                log += en + ":\n";
                return;
            }
            case Stmt::While: {
                std::string lp = "loop_" + std::to_string(out.size());
                std::string en = "end_" + std::to_string(out.size());
                labels[lp] = out.size();
                log += lp + ":\n";
                int c = eval_cond(s->cond);
                if (c == 0) { labels[en] = out.size(); log += "  ; while false\n"; return; }
                for (auto& x : s->body) stmt(x);
                jump_label(lp, "goto " + lp);
                labels[en] = out.size();
                log += en + ":\n";
                return;
            }
            case Stmt::For: {
                if (s->init) stmt(s->init);
                std::string lp = "for_" + std::to_string(out.size());
                std::string en = "for_end_" + std::to_string(out.size());
                labels[lp] = out.size();
                int c = s->cond ? eval_cond(s->cond) : 1;
                if (c != 0) {
                    for (auto& x : s->body) stmt(x);
                    if (s->step) stmt(s->step);
                    jump_label(lp, "goto " + lp);
                }
                labels[en] = out.size();
                return;
            }
            case Stmt::Goto: jump_label(s->name, "goto " + s->name); return;
            case Stmt::Label: labels[s->name] = out.size(); log += s->name + ":\n"; return;
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
    RopResult run(const Program& prog) {
        RopResult r;
        home = prog.has_org ? prog.org : 0xE9E0;
        for (auto& g : prog.globals) {
            uint32_t a = g.has_addr ? g.addr : alloc_var(g.name);
            alloc[g.name] = a;
            if (g.has_init) log += "  ; global " + g.name + " @ " + hex4(a) + " = " + std::to_string(g.init) + "\n";
        }
        for (auto& f : prog.funcs) {
            log += "func " + f.name + "\n";
            for (auto& s : f.body) stmt(s);
            if (f.body.empty() || (f.body.back()->k != Stmt::Return && f.body.back()->k != Stmt::Goto)) {
                std::string end = "rop_end_" + f.name;
                labels[end] = out.size();
                jump_label(end, "goto " + end + " (halt)");
            }
        }
        // append string pool
        for (auto& sp : str_pool) {
            size_t off = out.size();
            out.insert(out.end(), sp.second.begin(), sp.second.end());
            uint32_t target = home + (uint32_t)off;
            out[sp.first] = (uint8_t)(target & 0xFF);
            out[sp.first + 1] = (uint8_t)((target >> 8) & 0xFF);
            log += "  ; str pool -> [" + hex4(target) + "] " + std::to_string(sp.second.size()) + " bytes\n";
        }
        // patch gotos
        for (auto& p : patches) {
            auto it = labels.find(p.second);
            if (it == labels.end()) { log += "  ; warning: label " + p.second + " not found\n"; continue; }
            uint32_t target = home + (uint32_t)it->second;
            out[p.first] = (uint8_t)(target & 0xFF);
            out[p.first + 1] = (uint8_t)((target >> 8) & 0xFF);
            log += "  ; patch " + p.second + " -> [" + hex4(target) + "]\n";
        }
        if (prog.funcs.empty()) log += "warning: no function found\n";
        r.ok = true;
        r.bytes = std::move(out);
        r.base = home;
        r.log = std::move(log);
        return r;
    }
};

enum class LauncherKind { Common, Simple };

struct LauncherOpts {
    LauncherKind kind = LauncherKind::Common;
    uint32_t base = 0x8154;     // where the launcher bytes are placed
    uint32_t length = 3;        // 580 program-size code: 1 short, 2 medium, 3 long
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
                             LauncherOpts& lopts) {
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
                    }
                }
                if (lopts.kind == LauncherKind::Common) {
                    std::string v;
                    if (ls >> v) { lopts.length = parse_length(v); lopts.has_length = true; }
                    if (ls >> v) { lopts.backup = parse_number(v); lopts.has_backup = true; }
                    if (ls >> v) { lopts.main = parse_number(v); lopts.has_main = true; }
                } else {
                    std::string v;
                    if (ls >> v) lopts.base = parse_number(v);
                }
            }
            out += "\n";
            continue;
        }
        out += line + "\n";
    }
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

RopResult build_rop(const std::string& source, const std::string& model) {
    RopResult r;
    r.model = (model == "880" || lower(model) == "880btg") ? "880" : "580";
    try {
        std::string m = r.model;
        uint32_t org = 0xE9E0;
        bool has_org = false;
        LauncherOpts lopts;
        std::string body = strip_directives(source, m, org, has_org, lopts);
        r.model = m;
        Program prog = Parser(lex(body)).parse();
        if (prog.model.empty()) prog.model = m;
        if (!prog.model.empty()) {
            std::string pm = lower(prog.model);
            if (pm.find("880") != std::string::npos) r.model = "880";
            else if (pm.find("580") != std::string::npos) r.model = "580";
        }
        if (has_org) { prog.org = org; prog.has_org = true; }
        Db db;
        db.load(r.model == "880" ? ROP_DB_880 : ROP_DB_580);
        Emitter em(db);
        r = em.run(prog);
        r.model = r.model.empty() ? m : r.model;
        r.base = prog.has_org ? prog.org : 0xE9E0;

        auto emit16 = [](std::vector<uint8_t>& out, uint32_t v) {
            out.push_back((uint8_t)(v & 0xFF));
            out.push_back((uint8_t)((v >> 8) & 0xFF));
        };
        // Chain words are: 16-bit gadget address followed by a 16-bit class
        // tag (0x3030 call, 0x3031 pop, 0x3032 stack/flow).
        auto emit_gadget = [](std::vector<uint8_t>& out, uint32_t addr, uint32_t tag) {
            out.push_back((uint8_t)(addr & 0xFF));
            out.push_back((uint8_t)((addr >> 8) & 0xFF));
            out.push_back((uint8_t)(tag & 0xFF));
            out.push_back((uint8_t)((tag >> 8) & 0xFF));
        };

        r.launcher_base = lopts.base;
        const uint32_t backup = lopts.has_backup ? lopts.backup : r.base;
        const uint32_t main = lopts.has_main ? lopts.main : 0xD730;
        const uint32_t length = lopts.has_length ? lopts.length : 3;

        if (lopts.kind == LauncherKind::Common) {
            if (r.model == "880") {
                // 880: fd 26 <payload-2> <sp gadget>
                r.launcher.push_back(0xFD);
                r.launcher.push_back(0x26);
                emit16(r.launcher, (backup - 2) & 0xFFFF);
                emit_gadget(r.launcher, 0x75C0, 0x3032);
            } else {
                // 580 "launcher chung": copy the payload from the injection
                // address to <main>, then continue the chain there.
                r.launcher.push_back(0xFD);
                r.launcher.push_back(0x24);
                emit16(r.launcher, 0x3030);
                emit_gadget(r.launcher, 0x7BDA, 0x3031);              // pop qr0
                r.launcher.push_back(0xFE);                           // size marker
                r.launcher.push_back((uint8_t)(length & 0xFF));        // 01/02/03
                emit16(r.launcher, backup);                           // injection
                emit16(r.launcher, main);                             // execution
                emit16(r.launcher, (main - 2) & 0xFFFF);
                emit_gadget(r.launcher, 0x8932, 0x3031);              // BL memcpy
                emit16(r.launcher, 0x3030);
                emit_gadget(r.launcher, 0x1F74, 0x3032);              // sp = er6, pop er8
            }
            r.log += "  ; launcher (" + std::string(r.model == "880" ? "880 common" : "580 common") +
                     ") " + std::to_string(r.launcher.size()) + " bytes\n";
        } else {
            // Simple 10-byte stub: pop er14 / payload-2 / sp = er14, pop er14.
            const uint32_t pop_er14 = 0x15C78;
            const uint32_t set_sp = 0x20D60;
            emit_gadget(r.launcher, pop_er14 & 0xFFFF, 0x3031);
            emit16(r.launcher, (r.base >= 2 ? r.base - 2 : r.base) & 0xFFFF);
            emit_gadget(r.launcher, set_sp & 0xFFFF, 0x3032);
            r.log += "  ; launcher (simple) " + std::to_string(r.launcher.size()) + " bytes\n";
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
