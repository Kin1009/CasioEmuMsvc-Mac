// sim.hpp - minimal nX-U8/100 simulator. Decodes raw machine code (it does NOT share
// tables with the assembler, so assembler + compiler + runtime get cross-checked).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "targets.hpp"

namespace wiz {

struct Halt {
    std::string msg;
};

class U8 {
public:
    U8(const std::vector<uint8_t>& rom, std::vector<std::pair<int, int>> keys = {},
       const std::string& target = "", int start_csr = -1, int start_pc = -1,
       std::vector<std::pair<int, int>> stop = {});

    // machine state
    const Target& tg;
    std::string core;
    std::vector<uint8_t> mem;
    int r[16] = {0};
    int csr = 0, lcsr = 0;
    int dsr_next = -1, dsr_cur = -1;
    std::vector<std::pair<int, int>> stop;
    int pc = 0;
    int sp = 0;
    int lr = 0;
    int ea = 0;
    int psw = 0;
    int C = 0, Z = 0, S = 0, OV = 0;
    std::vector<uint8_t> planes[2];
    long steps = 0;
    std::vector<std::pair<int, int>> keys;
    int key_i = 0;
    int ko = 0;

    std::string run(long max_steps = 5000000);
    int pixel(int x, int y) const;
    std::string ascii() const;

private:
    void step();
    uint16_t fetch16(int a) const;
    int rd(int a);
    void wr(int a, int v);
    int key_in();
    int rd16(int a);
    void wr16(int a, int v);
    int er(int n) const { return r[n] | (r[n + 1] << 8); }
    void set_er(int n, int v) {
        r[n] = v & 0xFF;
        r[n + 1] = (v >> 8) & 0xFF;
    }
    int f8(int res, int a, int b, bool sub, bool chain = false);
    int f16(int res, int a, int b, bool sub);
    void zs(int v);
    bool cond(int c) const;
    void push_bytes(const std::vector<int>& bs);
    std::vector<int> pop_bytes(int n);
    void shift(int kind, int n, int amt);
    bool loadstore(int w, int nxt);
    void access(int n, int op, int addr);
    bool ctrl_a(int w, int nxt);
    bool f_group(int w, int nxt);
};

}  // namespace wiz
