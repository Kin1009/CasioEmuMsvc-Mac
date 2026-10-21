// asm.cpp - implementation of the nX-U8/100 / nX-U16/100 assembler.
#include "asm.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace wiz {

// ------------------------------------------------------------------ cores
std::string resolve_core(const std::string& name) {
    std::string k = to_lower(strip(name));
    if (k == "nx-u8/100" || k == "nx-u8" || k == "nxu8" || k == "u8") return "nX-U8/100";
    if (k == "nx-u16/100" || k == "nx-u16" || k == "nxu16" || k == "u16") return "nX-U16/100";
    throw CompileError(fmt("unknown core '%s' (use nX-U8/100, nX-U16/100)", name.c_str()));
}

// ------------------------------------------------------------------ helpers
static bool is_ident_start(char c) {
    return std::isalpha((unsigned char)c) || c == '_' || c == '.' || c == '$';
}
static bool is_ident_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '$';
}
static bool hexval(char c) { return std::isxdigit((unsigned char)c) != 0; }

// Rn  -> register index, or -1
static int opt_reg(const std::string& tok) {
    if (tok.size() < 2) return -1;
    if (tok[0] != 'r' && tok[0] != 'R') return -1;
    std::string d = tok.substr(1);
    for (char c : d) if (!std::isdigit((unsigned char)c)) return -1;
    int n = std::atoi(d.c_str());
    return n < 16 ? n : -1;
}

// ERn -> even register index, or -1
static int opt_ereg(const std::string& tok) {
    if (tok.size() < 3) return -1;
    if ((tok[0] != 'e' && tok[0] != 'E') || (tok[1] != 'r' && tok[1] != 'R')) return -1;
    std::string d = tok.substr(2);
    for (char c : d) if (!std::isdigit((unsigned char)c)) return -1;
    int n = std::atoi(d.c_str());
    return (n < 16 && n % 2 == 0) ? n : -1;
}

static std::string lstrip_hash(const std::string& s) { return lstrip(s, "#"); }

// ------------------------------------------------------------------ expression
class Ev {
public:
    Ev(const std::string& src, bool final, const std::map<std::string, int>& syms)
        : s(src), final_(final), syms_(syms) {
        v = parse_or();
        skip();
        if (i != s.size()) throw AsmError("bad expression: '" + s + "'");
    }
    long long value() const { return v; }

private:
    const std::string& s;
    size_t i = 0;
    bool final_;
    const std::map<std::string, int>& syms_;
    long long v = 0;

    void skip() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; }
    bool eat(const char* op) {
        skip();
        size_t n = std::strlen(op);
        if (s.compare(i, n, op) == 0) { i += n; return true; }
        return false;
    }
    [[noreturn]] void bad() { throw AsmError("bad expression: '" + s + "'"); }

    long long parse_or() {
        long long a = parse_xor();
        while (true) {
            if (eat("|")) a |= parse_xor();
            else return a;
        }
    }
    long long parse_xor() {
        long long a = parse_and();
        while (true) {
            if (eat("^")) a ^= parse_and();
            else return a;
        }
    }
    long long parse_and() {
        long long a = parse_shift();
        while (true) {
            if (eat("&")) a &= parse_shift();
            else return a;
        }
    }
    long long parse_shift() {
        long long a = parse_add();
        while (true) {
            if (eat("<<")) a <<= parse_add();
            else if (eat(">>")) a >>= parse_add();
            else return a;
        }
    }
    long long parse_add() {
        long long a = parse_mul();
        while (true) {
            if (eat("+")) a += parse_mul();
            else if (eat("-")) a -= parse_mul();
            else return a;
        }
    }
    long long parse_mul() {
        long long a = parse_unary();
        while (true) {
            if (eat("*")) a *= parse_unary();
            else if (eat("/")) {
                long long b = parse_unary();
                a = b ? a / b : 0;
            } else if (eat("%")) {
                long long b = parse_unary();
                if (b) { long long r = a % b; if (r && ((r < 0) != (b < 0))) r += b; a = r; }
            } else return a;
        }
    }
    long long parse_unary() {
        skip();
        if (i < s.size() && s[i] == '-') { i++; return -parse_unary(); }
        if (i < s.size() && s[i] == '+') { i++; return parse_unary(); }
        if (i < s.size() && s[i] == '~') { i++; return ~parse_unary(); }
        return parse_primary();
    }
    long long parse_primary() {
        skip();
        if (i >= s.size()) bad();
        if (s[i] == '(') {
            i++;
            long long a = parse_or();
            skip();
            if (i >= s.size() || s[i] != ')') bad();
            i++;
            return a;
        }
        if (std::isdigit((unsigned char)s[i])) return parse_number();
        if (is_ident_start(s[i])) {
            size_t st = i++;
            while (i < s.size() && is_ident_char(s[i])) i++;
            std::string name = s.substr(st, i - st);
            auto it = syms_.find(name);
            if (it != syms_.end()) return it->second;
            if (final_) throw AsmError("undefined symbol " + name);
            return 0;
        }
        bad();
    }
    long long parse_number() {
        if (s[i] == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            i += 2;
            size_t st = i;
            while (i < s.size() && hexval(s[i])) i++;
            if (i == st) bad();
            return (long long)std::strtoull(s.substr(st, i - st).c_str(), nullptr, 16);
        }
        size_t st = i;
        while (i < s.size() && hexval(s[i])) i++;
        std::string run = s.substr(st, i - st);
        if (i < s.size() && (s[i] == 'h' || s[i] == 'H') && !run.empty() &&
            std::isdigit((unsigned char)run[0])) {
            i++;
            return (long long)std::strtoull(run.c_str(), nullptr, 16);
        }
        for (char c : run)
            if (!std::isdigit((unsigned char)c)) bad();
        if (run.empty()) bad();
        return (long long)std::strtoll(run.c_str(), nullptr, 10);
    }
};

// ------------------------------------------------------------------ assembler
Assembler::Assembler(const std::string& core_) : core(resolve_core(core_)) {}

int Assembler::ev(const std::string& str, bool final) const {
    return (int)Ev(str, final, syms).value();
}

static int cond_code(const std::string& suf, bool allow_al) {
    struct E { const char* n; int c; };
    static const E t[] = {
        {"GES", 0x4}, {"LTS", 0x5}, {"GTS", 0x6}, {"LES", 0x7}, {"GE", 0x0}, {"LT", 0x1},
        {"GT", 0x2}, {"LE", 0x3}, {"NE", 0x8}, {"EQ", 0x9}, {"NV", 0xA}, {"OV", 0xB},
        {"PS", 0xC}, {"NS", 0xD}, {"AL", 0xE},
    };
    for (const auto& e : t) {
        if (suf == e.n) {
            if (!allow_al && suf == "AL") return -1;
            return e.c;
        }
    }
    return -1;
}

// Match "[ERn]" -> n, else -1
static int bracket_ereg(const std::string& tok) {
    if (tok.size() < 4 || tok.front() != '[' || tok.back() != ']') return -1;
    std::string inner = tok.substr(1, tok.size() - 2);
    if (inner.size() < 3) return -1;
    if ((inner[0] != 'e' && inner[0] != 'E') || (inner[1] != 'r' && inner[1] != 'R')) return -1;
    std::string d = inner.substr(2);
    for (char c : d) if (!std::isdigit((unsigned char)c)) return -1;
    return std::atoi(d.c_str());
}

// Match "<expr>[ERn]" -> (expr, n); returns false if no match
static bool expr_bracket_ereg(const std::string& tok, std::string& expr, int& n) {
    size_t close = tok.rfind(']');
    if (close == std::string::npos || close + 1 != tok.size()) return false;
    size_t open = tok.rfind('[', close);
    if (open == std::string::npos || open == 0) return false;
    std::string inner = tok.substr(open + 1, close - open - 1);
    if (inner.size() < 3) return false;
    if ((inner[0] != 'e' && inner[0] != 'E') || (inner[1] != 'r' && inner[1] != 'R')) return false;
    std::string d = inner.substr(2);
    for (char c : d) if (!std::isdigit((unsigned char)c)) return false;
    expr = tok.substr(0, open);
    n = std::atoi(d.c_str());
    return true;
}

// Match "R<n>.<bit>" -> (n, bit); returns false otherwise
static bool reg_bit(const std::string& tok, int& n, int& bit) {
    size_t dot = tok.rfind('.');
    if (dot == std::string::npos || dot + 1 >= tok.size()) return false;
    if (dot + 2 != tok.size()) return false;
    char b = tok[dot + 1];
    if (b < '0' || b > '7') return false;
    std::string head = tok.substr(0, dot);
    int r = opt_reg(head);
    if (r < 0) return false;
    n = r;
    bit = b - '0';
    return true;
}

// Match "<expr>.<bit>" -> (expr, bit); returns false otherwise
static bool expr_bit(const std::string& tok, std::string& expr, int& bit) {
    size_t dot = tok.rfind('.');
    if (dot == std::string::npos || dot + 1 >= tok.size()) return false;
    if (dot + 2 != tok.size()) return false;
    char b = tok[dot + 1];
    if (b < '0' || b > '7') return false;
    expr = tok.substr(0, dot);
    bit = b - '0';
    return true;
}

std::vector<uint16_t> Assembler::encode(const std::string& mn_in, const std::vector<std::string>& ops,
                                        int pc, bool final) const {
    std::string mn = mn_in;
    for (char& c : mn) c = (char)std::toupper((unsigned char)c);
    size_t n = ops.size();
    auto imm = [&](const std::string& tok) { return ev(lstrip_hash(tok), final); };

    // --- no operand ---
    static const std::map<std::string, int> simple = {
        {"NOP", 0xFE8F}, {"RT", 0xFE1F}, {"RTI", 0xFE0F}, {"BRK", 0xFFFF}, {"EI", 0xED08},
        {"DI", 0xEBF7}, {"SC", 0xED80}, {"RC", 0xEB7F}, {"CPLC", 0xFECF}, {"INC", 0xFE2F},
        {"DEC", 0xFE3F}};
    auto sit = simple.find(mn);
    if (sit != simple.end() && n == 0) return {(uint16_t)sit->second};
    if ((mn == "INC" || mn == "DEC") && n == 1 && ops[0] == "[EA]")
        return {(uint16_t)simple.at(mn)};

    // --- PUSH / POP ---
    if ((mn == "PUSH" || mn == "POP") && n >= 1) {
        std::vector<std::string> names;
        for (auto& o : ops) {
            std::string u = o;
            for (char& c : u) c = (char)std::toupper((unsigned char)c);
            names.push_back(u);
        }
        if (n == 1) {
            int e = opt_ereg(names[0]);
            if (e >= 0) return {(uint16_t)(0xF000 | (e << 8) | (mn == "PUSH" ? 0x5E : 0x1E))};
            int r = opt_reg(names[0]);
            if (r >= 0) return {(uint16_t)(0xF000 | (r << 8) | (mn == "PUSH" ? 0x4E : 0x0E))};
        }
        int bits = 0;
        for (auto& nm : names) {
            if (nm == "EA") bits |= 1;
            else if (nm == "LR") bits |= 8;
            else if (nm == "PSW" || nm == "EPSW") bits |= 4;
            else if (nm == "PC" || nm == "ELR") bits |= 2;
            else throw AsmError("bad register list item " + nm);
        }
        return {(uint16_t)(0xF000 | (bits << 8) | (mn == "PUSH" ? 0xCE : 0x8E))};
    }

    // --- DSRI prefix ---
    if (mn == "DSRI" && n == 1) return {(uint16_t)(0xE300 | (imm(ops[0]) & 0xFF))};

    // --- branches ---
    if ((mn == "B" || mn == "BL") && n == 1) {
        int e = opt_ereg(ops[0]);
        if (e >= 0) return {(uint16_t)(0xF002 | (e << 8) | (mn == "B" ? 0 : 1))};
        int tgt = ev(ops[0], final);
        return {(uint16_t)(0xF000 | ((((tgt >> 16) & 0xF)) << 8) | (mn == "B" ? 0 : 1)),
                (uint16_t)(tgt & 0xFFFF)};
    }
    if (mn == "JMP" && n == 1) return encode("B", ops, pc, final);

    if (mn.size() > 1 && mn[0] == 'B') {
        int c = cond_code(mn.substr(1), true);
        if (c >= 0 && n == 1) {
            int tgt = ev(ops[0], final);
            int r = final ? ((tgt - (pc + 2)) >> 1) : 0;
            if (final && (((tgt - (pc + 2)) & 1) || r < -128 || r > 127))
                throw AsmError(fmt("branch out of range (%d words) at %04X", r, pc));
            return {(uint16_t)(0xC000 | (c << 8) | (r & 0xFF))};
        }
    }
    if (mn.size() > 1 && mn[0] == 'J') {
        int c = cond_code(mn.substr(1), false);
        if (c >= 0 && n == 1) {
            int inv = c ^ 1;
            int tgt = ev(ops[0], final);
            return {(uint16_t)(0xC000 | (inv << 8) | 2),
                    (uint16_t)(0xF000 | ((((tgt >> 16) & 0xF)) << 8)), (uint16_t)(tgt & 0xFFFF)};
        }
    }

    // --- LEA ---
    if (mn == "LEA" && n == 1) {
        int k = bracket_ereg(ops[0]);
        if (k >= 0) return {(uint16_t)(0xF00A | (k << 4))};
        std::string expr;
        if (expr_bracket_ereg(ops[0], expr, k))
            return {(uint16_t)(0xF00B | (k << 4)), (uint16_t)(ev(expr, final) & 0xFFFF)};
        return {(uint16_t)0xF00C, (uint16_t)(ev(ops[0], final) & 0xFFFF)};
    }

    // --- MUL / DIV ---
    if ((mn == "MUL" || mn == "DIV") && n == 2) {
        int e = opt_ereg(ops[0]), r = opt_reg(ops[1]);
        if (e < 0 || r < 0) throw AsmError(mn + " needs ERn,Rm");
        return {(uint16_t)(0xF000 | (e << 8) | (r << 4) | (mn == "MUL" ? 0x4 : 0x9))};
    }

    // --- extend / ALU one-operand ---
    if (mn == "EXTBW" && n == 1) {
        int e = opt_ereg(ops[0]);
        return {(uint16_t)(0x800F | ((e | 1) << 8) | (e << 4))};
    }
    if ((mn == "NEG" || mn == "DAA" || mn == "DAS") && n == 1) {
        int r = opt_reg(ops[0]);
        int code = mn == "DAA" ? 0x1F : (mn == "DAS" ? 0x3F : 0x5F);
        return {(uint16_t)(0x8000 | (r << 8) | code)};
    }

    // --- SB / RB / TB ---
    if ((mn == "SB" || mn == "RB" || mn == "TB") && n == 1) {
        int low = mn == "SB" ? 0x0 : (mn == "RB" ? 0x2 : 0x1);
        int r, bit;
        if (reg_bit(ops[0], r, bit))
            return {(uint16_t)(0xA000 | (r << 8) | (bit << 4) | low)};
        std::string expr;
        if (expr_bit(ops[0], expr, bit))
            return {(uint16_t)(0xA080 | (bit << 4) | low),
                    (uint16_t)(ev(expr, final) & 0xFFFF)};
        throw AsmError("bad bit operand " + ops[0]);
    }

    // --- shifts ---
    static const std::map<std::string, int> SHIFT = {
        {"SLL", 0xA}, {"SLLC", 0xB}, {"SRL", 0xC}, {"SRLC", 0xD}, {"SRA", 0xE}};
    auto sh = SHIFT.find(mn);
    if (sh != SHIFT.end() && n == 2) {
        int r = opt_reg(ops[0]);
        if (!ops[1].empty() && ops[1][0] == '#')
            return {(uint16_t)(0x9000 | (r << 8) | ((imm(ops[1]) & 7) << 4) | sh->second)};
        return {(uint16_t)(0x8000 | (r << 8) | (opt_reg(ops[1]) << 4) | sh->second)};
    }

    // --- L / ST ---
    if ((mn == "L" || mn == "ST") && n == 2) {
        bool st = (mn == "ST");
        const std::string& dst = ops[0];
        const std::string& src = ops[1];
        int r = opt_reg(dst), e = opt_ereg(dst);
        if (r < 0 && e < 0) throw AsmError(mn + " first operand must be Rn/ERn");
        bool is_e = (e >= 0);
        int reg = is_e ? e : r;
        int base = st ? 1 : 0;

        int k = bracket_ereg(src);
        if (k >= 0) {
            if (!is_e) return {(uint16_t)(0x9000 | (reg << 8) | (k << 4) | base)};
            return {(uint16_t)(0x9000 | (reg << 8) | (k << 4) | (0x2 + base))};
        }
        if (src == "[EA]") {
            if (!is_e) return {(uint16_t)(0x9000 | (reg << 8) | 0x30 | base)};
            return {(uint16_t)(0x9000 | (reg << 8) | 0x30 | (0x2 + base))};
        }
        if (src == "[EA+]") {
            if (!is_e) return {(uint16_t)(0x9000 | (reg << 8) | 0x50 | base)};
            return {(uint16_t)(0x9000 | (reg << 8) | 0x50 | (0x2 + base))};
        }
        std::string expr;
        if (expr_bracket_ereg(src, expr, k)) {
            int d = ev(expr, final) & 0xFFFF;
            if (!is_e)
                return {(uint16_t)(0x9000 | (reg << 8) | (k << 4) | (0x8 + base)), (uint16_t)d};
            return {(uint16_t)(0xA000 | (reg << 8) | (k << 4) | (0x8 + base)), (uint16_t)d};
        }
        int d = ev(src, final) & 0xFFFF;
        if (!is_e) return {(uint16_t)(0x9010 | (reg << 8) | base), (uint16_t)d};
        return {(uint16_t)(0x9010 | (reg << 8) | (0x2 + base)), (uint16_t)d};
    }

    // --- MOV special registers ---
    if (mn == "MOV" && n == 2) {
        std::string a = ops[0], b = ops[1];
        for (char& c : a) c = (char)std::toupper((unsigned char)c);
        for (char& c : b) c = (char)std::toupper((unsigned char)c);
        if (a == "PSW" && !b.empty() && b[0] == '#')
            return {(uint16_t)(0xE900 | (imm(ops[1]) & 0xFF))};
        int rb = opt_reg(b);
        if (a == "PSW" && rb >= 0) return {(uint16_t)(0xA00B | (rb << 4))};
        int ra = opt_reg(a);
        if (ra >= 0 && b == "PSW") return {(uint16_t)(0xA003 | (ra << 8))};
        int ea = opt_ereg(a);
        if (ea >= 0 && b == "SP") return {(uint16_t)(0xA01A | (ea << 8))};
        int eb = opt_ereg(b);
        if (a == "SP" && eb >= 0) return {(uint16_t)(0xA10A | (eb << 4))};
    }
    if (mn == "ADD" && n == 2) {
        std::string a = ops[0];
        for (char& c : a) c = (char)std::toupper((unsigned char)c);
        if (a == "SP") return {(uint16_t)(0xE100 | (imm(ops[1]) & 0xFF))};
    }

    // --- ALU ---
    static const std::map<std::string, int> ALU_RR = {
        {"ADD", 0x1}, {"ADDC", 0x6}, {"AND", 0x2}, {"CMP", 0x7}, {"CMPC", 0x5},
        {"MOV", 0x0}, {"OR", 0x3},  {"XOR", 0x4}, {"SUB", 0x8}, {"SUBC", 0x9}};
    static const std::map<std::string, int> ALU_IMM = {
        {"ADD", 0x1}, {"ADDC", 0x6}, {"AND", 0x2}, {"CMP", 0x7},
        {"CMPC", 0x5}, {"MOV", 0x0}, {"OR", 0x3}, {"XOR", 0x4}};
    auto ar = ALU_RR.find(mn);
    if (ar != ALU_RR.end() && n == 2) {
        int r = opt_reg(ops[0]), e = opt_ereg(ops[0]);
        if (r >= 0) {
            int r2 = opt_reg(ops[1]);
            if (r2 >= 0) return {(uint16_t)(0x8000 | (r << 8) | (r2 << 4) | ar->second)};
            auto ai = ALU_IMM.find(mn);
            if (!ops[1].empty() && ops[1][0] == '#' && ai != ALU_IMM.end()) {
                int v = imm(ops[1]);
                if (v < -128 || v > 255) throw AsmError(fmt("imm8 out of range: %d", v));
                return {(uint16_t)((ai->second << 12) | (r << 8) | (v & 0xFF))};
            }
        }
        if (e >= 0) {
            int e2 = opt_ereg(ops[1]);
            if (e2 >= 0 && (mn == "ADD" || mn == "MOV" || mn == "CMP")) {
                int low = mn == "ADD" ? 0x6 : (mn == "MOV" ? 0x5 : 0x7);
                return {(uint16_t)(0xF000 | (e << 8) | (e2 << 4) | low)};
            }
            if (!ops[1].empty() && ops[1][0] == '#' && (mn == "ADD" || mn == "MOV")) {
                int v = imm(ops[1]);
                if (v < -64 || v > 63)
                    throw AsmError(fmt("imm7 out of range: %d (use two MOV Rn,#imm8)", v));
                int top = mn == "ADD" ? 0x80 : 0x00;
                return {(uint16_t)(0xE000 | (e << 8) | top | (v & 0x7F))};
            }
        }
    }
    throw AsmError(fmt("cannot encode: %s %s", mn.c_str(), join(ops, ", ").c_str()));
}

// split operands on commas at bracket depth 0
static std::vector<std::string> split_ops(const std::string& rest) {
    std::vector<std::string> out;
    if (rest.empty()) return out;
    int depth = 0;
    std::string cur;
    for (char c : rest) {
        if (c == '[') depth++;
        else if (c == ']') depth--;
        if (c == ',' && depth == 0) {
            out.push_back(strip(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(strip(cur));
    return out;
}

std::map<uint32_t, uint8_t> Assembler::assemble(const std::string& text, int base) {
    std::vector<std::string> lines;
    for (const std::string& raw : split(replace_all(text, "\r", ""), '\n')) {
        std::string line = strip(split(raw, ';')[0]);
        if (!line.empty()) lines.push_back(line);
    }
    std::map<uint32_t, uint8_t> image;
    for (int pass = 0; pass < 2; pass++) {
        bool final = (pass == 1);
        int pc = base;
        image.clear();
        for (std::string line : lines) {
            // labels
            while (true) {
                size_t colon = std::string::npos;
                size_t k = 0;
                if (k < line.size() && is_ident_start(line[k])) {
                    k++;
                    while (k < line.size() && is_ident_char(line[k])) k++;
                    if (k < line.size() && line[k] == ':') colon = k;
                }
                if (colon == std::string::npos) break;
                std::string name = line.substr(0, colon);
                if (!final) syms[name] = pc;
                line = strip(line.substr(colon + 1));
            }
            if (line.empty()) continue;

            // EQU
            {
                size_t sp = line.find_first_of(" \t");
                if (sp != std::string::npos) {
                    std::string head = line.substr(0, sp);
                    std::string rest = strip(line.substr(sp + 1));
                    size_t rp = rest.find_first_of(" \t");
                    std::string kw = rp == std::string::npos ? rest : rest.substr(0, rp);
                    std::string after = rp == std::string::npos ? "" : strip(rest.substr(rp + 1));
                    if (to_lower(kw) == "equ" && !after.empty() && is_ident_start(head[0])) {
                        syms[head] = ev(after, final);
                        continue;
                    }
                }
            }

            size_t sp = line.find_first_of(" \t");
            std::string head = sp == std::string::npos ? line : line.substr(0, sp);
            std::string rest = sp == std::string::npos ? "" : strip(line.substr(sp + 1));
            std::string head_u = head;
            for (char& c : head_u) c = (char)std::toupper((unsigned char)c);

            if (head_u == "ORG") { pc = ev(rest, final); continue; }
            if (head_u == "DW") {
                for (auto& t : split(rest, ',')) {
                    int v = ev(t, final) & 0xFFFF;
                    image[pc] = (uint8_t)(v & 0xFF);
                    image[pc + 1] = (uint8_t)(v >> 8);
                    pc += 2;
                }
                continue;
            }
            if (head_u == "DB") {
                for (auto& t : split(rest, ',')) {
                    image[pc] = (uint8_t)(ev(t, final) & 0xFF);
                    pc += 1;
                }
                continue;
            }
            std::vector<std::string> ops = split_ops(rest);
            if (pc & 1) throw AsmError(fmt("instruction at odd address %04X: %s", pc, line.c_str()));
            try {
                std::vector<uint16_t> words = encode(head_u, ops, pc, final);
                for (uint16_t w : words) {
                    image[pc] = (uint8_t)(w & 0xFF);
                    image[pc + 1] = (uint8_t)((w >> 8) & 0xFF);
                    pc += 2;
                }
            } catch (const AsmError& ex) {
                throw AsmError(fmt("%s  [%s]", ex.what(), line.c_str()));
            }
        }
    }
    return image;
}

std::vector<uint8_t> to_bin(const std::map<uint32_t, uint8_t>& image, int size) {
    uint32_t top = 0;
    for (const auto& kv : image) top = std::max(top, kv.first + 1);
    if (size > 0) top = std::max(top, (uint32_t)size);
    std::vector<uint8_t> out(top, 0xFF);
    for (const auto& kv : image)
        if (kv.first < out.size()) out[kv.first] = kv.second;
    return out;
}

}  // namespace wiz
