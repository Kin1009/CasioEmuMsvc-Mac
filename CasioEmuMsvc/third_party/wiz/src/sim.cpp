// sim.cpp - nX-U8/100 / nX-U16/100 simulator.
#include "sim.hpp"

#include <algorithm>

namespace wiz {

static int sx8(int v) { return (v & 0x80) ? v - 256 : v; }

U8::U8(const std::vector<uint8_t>& rom, std::vector<std::pair<int, int>> keys_,
       const std::string& target, int start_csr, int start_pc,
       std::vector<std::pair<int, int>> stop_)
    : tg(get_target(target.empty() ? DEFAULT_TARGET : target)),
      core(get_target(target.empty() ? DEFAULT_TARGET : target).core),
      stop(std::move(stop_)),
      keys(std::move(keys_)) {
    mem.assign(std::max<size_t>(65536, rom.size()), 0);
    std::copy(rom.begin(), rom.end(), mem.begin());
    planes[0].assign(0x800, 0);
    planes[1].assign(0x800, 0);
    pc = mem[2] | (mem[3] << 8);
    sp = mem[0] | (mem[1] << 8);
    if (start_csr >= 0) {
        csr = start_csr;
        pc = start_pc;
        sp = 0xF000;
    }
}

uint16_t U8::fetch16(int a) const {
    uint32_t b = ((uint32_t)csr << 16) | (uint32_t)(a & 0xFFFF);
    uint8_t lo = b < mem.size() ? mem[b] : 0;
    uint8_t hi = (b + 1) < mem.size() ? mem[b + 1] : 0;
    return (uint16_t)(lo | (hi << 8));
}

int U8::rd(int a) {
    a &= 0xFFFF;
    if (dsr_cur > 0) {
        uint32_t b = ((uint32_t)dsr_cur << 16) | (uint32_t)a;
        return b < mem.size() ? mem[b] : 0xFF;
    }
    if (a >= 0xF800) {
        int plane = (tg.planes == 2 && (mem[0xF037] & 4)) ? 1 : 0;
        return planes[plane][a - 0xF800];
    }
    if (a == 0xF040) return key_in();
    return mem[a];
}

void U8::wr(int a, int v) {
    a &= 0xFFFF;
    v &= 0xFF;
    if (a >= 0xF800) {
        int plane = (tg.planes == 2 && (mem[0xF037] & 4)) ? 1 : 0;
        planes[plane][a - 0xF800] = (uint8_t)v;
        return;
    }
    if (a < tg.ram_lo)
        throw Halt{fmt("write to ROM at %04X (pc=%04X)", a, pc)};
    if (a == 0xF037 && tg.planes == 1)
        throw Halt{fmt("write to 0F037H (plane select) on a 1-plane target (pc=%04X)", pc)};
    mem[a] = (uint8_t)v;
    if (a == 0xF046) ko = v;
}

int U8::key_in() {
    int val = 0xFF;
    for (auto& k : keys)
        if (ko & k.first) val &= ~k.second & 0xFF;
    return val;
}

int U8::rd16(int a) { return rd(a) | (rd(a + 1) << 8); }
void U8::wr16(int a, int v) {
    wr(a, v);
    wr(a + 1, v >> 8);
}

int U8::f8(int res, int a, int b, bool sub, bool chain) {
    int rr = res & 0xFF;
    C = (res > 0xFF || res < 0) ? 1 : 0;
    int z = rr == 0 ? 1 : 0;
    Z = chain ? (Z & z) : z;
    S = rr >> 7;
    if (sub) OV = ((a ^ b) & (a ^ rr) & 0x80) ? 1 : 0;
    else OV = ((~(a ^ b)) & (a ^ rr) & 0x80) ? 1 : 0;
    return rr;
}

int U8::f16(int res, int a, int b, bool sub) {
    int rr = res & 0xFFFF;
    C = (res > 0xFFFF || res < 0) ? 1 : 0;
    Z = rr == 0 ? 1 : 0;
    S = rr >> 15;
    if (sub) OV = ((a ^ b) & (a ^ rr) & 0x8000) ? 1 : 0;
    else OV = ((~(a ^ b)) & (a ^ rr) & 0x8000) ? 1 : 0;
    return rr;
}

void U8::zs(int v) {
    Z = (v & 0xFF) == 0 ? 1 : 0;
    S = (v >> 7) & 1;
}

bool U8::cond(int c) const {
    bool bC = C != 0, bZ = Z != 0, bS = S != 0, bOV = OV != 0;
    switch (c) {
        case 0x0: return !bC;
        case 0x1: return bC;
        case 0x2: return !bC && !bZ;
        case 0x3: return bZ || bC;
        case 0x4: return !(bOV ^ bS);
        case 0x5: return bOV ^ bS;
        case 0x6: return !((bOV ^ bS) || bZ);
        case 0x7: return (bOV ^ bS) || bZ;
        case 0x8: return !bZ;
        case 0x9: return bZ;
        case 0xA: return !bOV;
        case 0xB: return bOV;
        case 0xC: return !bS;
        case 0xD: return bS;
        default: return true;
    }
}

void U8::push_bytes(const std::vector<int>& bs) {
    sp -= (int)bs.size();
    for (size_t i = 0; i < bs.size(); i++) wr(sp + (int)i, bs[i]);
}

std::vector<int> U8::pop_bytes(int n) {
    std::vector<int> out;
    for (int i = 0; i < n; i++) out.push_back(rd(sp + i));
    sp += n;
    return out;
}

void U8::step() {
    int pc0 = pc;
    for (auto& s : stop)
        if (s.first == csr && s.second == pc0) throw Halt{fmt("stop at %X:%04X", csr, pc0)};
    dsr_cur = dsr_next;
    dsr_next = -1;
    int w = fetch16(pc0);
    steps++;
    int hi = w >> 12;
    int n = (w >> 8) & 0xF;
    int m = (w >> 4) & 0xF;
    int lo = w & 0xF;
    int i8 = w & 0xFF;
    int nxt = pc0 + 2;

    if ((w & 0xFF00) == 0xE300) {
        dsr_next = i8;
        pc = nxt;
        return;
    }

    // imm8 ALU
    if (hi <= 7) {
        int a = r[n];
        if (hi == 0) {
            r[n] = i8;
            zs(i8);
        } else if (hi == 1) {
            r[n] = f8(a + i8, a, i8, false);
        } else if (hi == 2) {
            r[n] = a & i8;
            zs(r[n]);
        } else if (hi == 3) {
            r[n] = a | i8;
            zs(r[n]);
        } else if (hi == 4) {
            r[n] = a ^ i8;
            zs(r[n]);
        } else if (hi == 5) {
            f8(a - i8 - C, a, i8, true, true);
        } else if (hi == 6) {
            r[n] = f8(a + i8 + C, a, i8, false, true);
        } else if (hi == 7) {
            f8(a - i8, a, i8, true);
        }
        pc = nxt;
        return;
    }
    if (hi == 8) {
        if (lo <= 9) {
            int a = r[n], b = r[m];
            if (lo == 0) { r[n] = b; zs(b); }
            else if (lo == 1) r[n] = f8(a + b, a, b, false);
            else if (lo == 2) { r[n] = a & b; zs(r[n]); }
            else if (lo == 3) { r[n] = a | b; zs(r[n]); }
            else if (lo == 4) { r[n] = a ^ b; zs(r[n]); }
            else if (lo == 5) f8(a - b - C, a, b, true, true);
            else if (lo == 6) r[n] = f8(a + b + C, a, b, false, true);
            else if (lo == 7) f8(a - b, a, b, true);
            else if (lo == 8) r[n] = f8(a - b, a, b, true);
            else if (lo == 9) r[n] = f8(a - b - C, a, b, true, true);
            pc = nxt;
            return;
        }
        if (lo == 0xA || lo == 0xB || lo == 0xC || lo == 0xD || lo == 0xE) {
            shift(lo, n, r[m]);
            pc = nxt;
            return;
        }
        if (lo == 0xF) {
            if (m == 5) {
                int a = r[n];
                r[n] = f8(0 - a, 0, a, true);
                pc = nxt;
                return;
            }
            if ((n & 1) && !(m & 1)) {
                int e = m;
                int v = r[e];
                r[e + 1] = (v & 0x80) ? 0xFF : 0;
                Z = er(e) == 0 ? 1 : 0;
                S = r[e + 1] >> 7;
                pc = nxt;
                return;
            }
        }
    }
    if (hi == 9) {
        if ((lo == 0xA || lo == 0xB || lo == 0xC || lo == 0xD || lo == 0xE) && !(m & 8)) {
            shift(lo, n, m & 7);
            pc = nxt;
            return;
        }
        if (loadstore(w, nxt)) return;
    }
    if (hi == 0xA) {
        if (ctrl_a(w, nxt)) return;
        if (loadstore(w, nxt)) return;
    }
    if (hi == 0xC) {
        int c = n;
        int rr = sx8(i8);
        if (c <= 0xE) {
            pc = cond(c) ? ((nxt + 2 * rr) & 0xFFFF) : nxt;
            return;
        }
    }
    if (hi == 0xE) {
        if ((n & 1) == 0 && !(w & 0x0100)) {
            int e = n;
            int v = w & 0x7F;
            v = (v & 0x40) ? v - 128 : v;
            if (w & 0x80) {
                int a = er(e);
                set_er(e, f16(a + v, a, v & 0xFFFF, false));
            } else {
                set_er(e, v & 0xFFFF);
                Z = (v & 0xFFFF) == 0 ? 1 : 0;
                S = v < 0 ? 1 : 0;
            }
            pc = nxt;
            return;
        }
        if ((w >> 8) == 0xE9) { psw = i8; pc = nxt; return; }
        if ((w >> 8) == 0xE1) { sp += sx8(i8); pc = nxt; return; }
        if (w == 0xED08 || w == 0xEBF7) { pc = nxt; return; }
        if (w == 0xED80) { C = 1; pc = nxt; return; }
        if (w == 0xEB7F) { C = 0; pc = nxt; return; }
    }
    if (hi == 0xF) {
        if (f_group(w, nxt)) return;
    }
    throw Halt{fmt("unknown opcode %04X at %04X", w, pc0)};
}

void U8::shift(int kind, int n, int amt) {
    amt &= 7;
    if (amt == 0) return;
    int a = r[n];
    if (kind == 0xA) {
        C = (a >> (8 - amt)) & 1;
        r[n] = (a << amt) & 0xFF;
    } else if (kind == 0xC) {
        C = (a >> (amt - 1)) & 1;
        r[n] = a >> amt;
    } else if (kind == 0xE) {
        int sa = (a & 0x80) ? a - 256 : a;
        C = (sa >> (amt - 1)) & 1;
        r[n] = (sa >> amt) & 0xFF;
    } else if (kind == 0xB) {
        int v = (a << 8) | r[(n - 1) & 15];
        C = (v >> (16 - amt)) & 1;
        r[n] = ((v << amt) >> 8) & 0xFF;
    } else if (kind == 0xD) {
        int v = (r[(n + 1) & 15] << 8) | a;
        C = (v >> (amt - 1)) & 1;
        r[n] = (v >> amt) & 0xFF;
    }
}

bool U8::loadstore(int w, int nxt) {
    int hi = w >> 12;
    int n = (w >> 8) & 0xF;
    int m = (w >> 4) & 0xF;
    int lo = w & 0xF;
    int pc0 = pc;
    if (hi == 9) {
        if (m == 0x1 && lo <= 3) {
            int addr = fetch16(pc0 + 2);
            access(n, lo, addr);
            pc = pc0 + 4;
            return true;
        }
        if (m == 0x3 && lo <= 3) {
            access(n, lo, ea);
            pc = nxt;
            return true;
        }
        if (m == 0x5 && lo <= 3) {
            access(n, lo, ea);
            ea += lo < 2 ? 1 : 2;
            pc = nxt;
            return true;
        }
        if (lo <= 3 && (m & 1) == 0) {
            int base = er(m);
            access(n, lo, base);
            pc = nxt;
            return true;
        }
        if ((lo == 8 || lo == 9) && (m & 1) == 0) {
            int d = fetch16(pc0 + 2);
            access(n, lo - 8, (er(m) + d) & 0xFFFF);
            pc = pc0 + 4;
            return true;
        }
    }
    if (hi == 0xA && (lo == 8 || lo == 9) && (m & 1) == 0 && (n & 1) == 0) {
        int d = fetch16(pc0 + 2);
        access(n, 2 + (lo - 8), (er(m) + d) & 0xFFFF);
        pc = pc0 + 4;
        return true;
    }
    return false;
}

void U8::access(int n, int op, int addr) {
    if (op == 0) {
        r[n] = rd(addr);
        zs(r[n]);
    } else if (op == 1) {
        wr(addr, r[n]);
    } else if (op == 2) {
        int v = rd16(addr);
        set_er(n, v);
        Z = v == 0 ? 1 : 0;
        S = v >> 15;
    } else {
        wr16(addr, er(n));
    }
}

bool U8::ctrl_a(int w, int nxt) {
    int n = (w >> 8) & 0xF;
    int m = (w >> 4) & 0xF;
    int lo = w & 0xF;
    if (lo <= 2 && (m & 8) == 0) {
        int b = m & 7;
        if (lo == 0) r[n] |= (1 << b);
        else if (lo == 2) r[n] &= ~(1 << b) & 0xFF;
        else Z = ((r[n] >> b) & 1) ? 0 : 1;
        pc = nxt;
        return true;
    }
    if (lo <= 2 && (m & 8) && n == 0) {
        int b = m & 7;
        int addr = fetch16(pc + 2);
        int v = rd(addr);
        if (lo == 0) wr(addr, v | (1 << b));
        else if (lo == 2) wr(addr, v & ~(1 << b));
        else Z = ((v >> b) & 1) ? 0 : 1;
        pc = pc + 4;
        return true;
    }
    if ((w & 0xFF0F) == 0xA00B) { psw = r[m]; pc = nxt; return true; }
    if (lo == 3 && m == 0) { r[n] = psw; pc = nxt; return true; }
    if ((w & 0xF0FF) == 0xA01A) { set_er(n, sp); pc = nxt; return true; }
    if ((w & 0xFF0F) == 0xA10A) { sp = er(m); pc = nxt; return true; }
    return false;
}

bool U8::f_group(int w, int nxt) {
    int n = (w >> 8) & 0xF;
    int m = (w >> 4) & 0xF;
    int lo = w & 0xF;
    int pc0 = pc;
    if (w == 0xFFFF) throw Halt{fmt("BRK at %04X", pc0)};
    if (w == 0xFE8F) { pc = nxt; return true; }
    if (w == 0xFE1F) { pc = lr; csr = lcsr; return true; }
    if (w == 0xFE0F) { pc = lr; csr = lcsr; return true; }
    if (w == 0xFECF) { C ^= 1; pc = nxt; return true; }
    if (w == 0xFE2F || w == 0xFE3F) {
        int v = rd(ea);
        int d = (w == 0xFE2F) ? 1 : -1;
        wr(ea, f8(v + d, v, 1, d < 0));
        pc = nxt;
        return true;
    }
    if ((w & 0xFF0F) == 0xF00A) { ea = er(m); pc = nxt; return true; }
    if ((w & 0xFF0F) == 0xF00B) {
        ea = (er(m) + fetch16(pc0 + 2)) & 0xFFFF;
        pc = pc0 + 4;
        return true;
    }
    if (w == 0xF00C) { ea = fetch16(pc0 + 2); pc = pc0 + 4; return true; }
    if ((w & 0x00FF) == 0x00 || (w & 0x00FF) == 0x01) {
        int tgt = fetch16(pc0 + 2);
        if (w & 1) { lr = pc0 + 4; lcsr = csr; }
        if (tgt == pc0 && n == csr && (w & 1) == 0) throw Halt{fmt("halt: B to self at %04X", pc0)};
        pc = tgt;
        csr = n;
        return true;
    }
    if ((w & 0xFF0F) == 0xF002) { pc = er(m); return true; }
    if ((w & 0xFF0F) == 0xF003) { lr = nxt; lcsr = csr; pc = er(m); return true; }
    if (lo == 4 && (n & 1) == 0) {
        set_er(n, (r[n] * r[m]) & 0xFFFF);
        Z = er(n) == 0 ? 1 : 0;
        pc = nxt;
        return true;
    }
    if (lo == 9 && (n & 1) == 0) {
        int d = r[m];
        int a = er(n);
        if (d == 0) { set_er(n, 0xFFFF); C = 1; }
        else { set_er(n, a / d); r[m] = a % d; C = 0; }
        Z = er(n) == 0 ? 1 : 0;
        pc = nxt;
        return true;
    }
    if (lo == 5 && (n & 1) == 0 && (m & 1) == 0) {
        int v = er(m);
        set_er(n, v);
        Z = v == 0 ? 1 : 0;
        S = v >> 15;
        pc = nxt;
        return true;
    }
    if (lo == 6 && (n & 1) == 0 && (m & 1) == 0) {
        int a = er(n), b = er(m);
        set_er(n, f16(a + b, a, b, false));
        pc = nxt;
        return true;
    }
    if (lo == 7 && (n & 1) == 0 && (m & 1) == 0) {
        int a = er(n), b = er(m);
        f16(a - b, a, b, true);
        pc = nxt;
        return true;
    }
    if ((w & 0x00FF) == 0x00CE) {
        int bits = n;
        if (bits & 1) push_bytes({ea & 0xFF, ea >> 8});
        if (bits & 8) push_bytes({lr & 0xFF, lr >> 8, lcsr, 0});
        pc = nxt;
        return true;
    }
    if ((w & 0x00FF) == 0x008E) {
        int bits = n;
        if (bits & 1) {
            auto b = pop_bytes(2);
            ea = b[0] | (b[1] << 8);
        }
        if (bits & 8) {
            auto b = pop_bytes(4);
            lr = b[0] | (b[1] << 8);
            lcsr = b[2];
        }
        if (bits & 2) {
            int p = (bits & 8) ? lr : -1;
            if (p < 0) {
                auto b = pop_bytes(4);
                pc = b[0] | (b[1] << 8);
                csr = b[2];
            } else {
                pc = p;
                csr = lcsr;
            }
            return true;
        }
        pc = nxt;
        return true;
    }
    if (lo == 0xE && m == 5) {
        int v = er(n);
        push_bytes({v & 0xFF, v >> 8});
        pc = nxt;
        return true;
    }
    if (lo == 0xE && m == 1) {
        auto b = pop_bytes(2);
        set_er(n, b[0] | (b[1] << 8));
        pc = nxt;
        return true;
    }
    if (lo == 0xE && m == 4) {
        push_bytes({r[n]});
        pc = nxt;
        return true;
    }
    if (lo == 0xE && m == 0) {
        r[n] = pop_bytes(1)[0];
        pc = nxt;
        return true;
    }
    return false;
}

std::string U8::run(long max_steps) {
    try {
        while (steps < max_steps) step();
    } catch (const Halt& h) {
        return h.msg;
    }
    return "max steps";
}

int U8::pixel(int x, int y) const {
    y += tg.row_off;
    if (y >= 64) return 0;
    int a = y * 32 + (x >> 3);
    int bit = 7 - (x & 7);
    int lop = (planes[0][a] >> bit) & 1;
    int hip = (planes[1][a] >> bit) & 1;
    if (tg.planes == 1) return lop ? 3 : 0;
    return (hip << 1) | lop;
}

std::string U8::ascii() const {
    static const char* palette = " .+#";
    std::string out;
    for (int y = 0; y < tg.rows; y++) {
        for (int x = 0; x < 192; x++) out += palette[pixel(x, y)];
        if (y + 1 < tg.rows) out += '\n';
    }
    return out;
}

}  // namespace wiz
