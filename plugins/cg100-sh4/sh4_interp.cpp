// sh4_interp.cpp -- big-endian SH-4 / SH-4AL-DSP interpreter + physical memory.
// Faithful behavioural port of emu_go/cpu.go and emu_go/memory.go.

#include "sh4_interp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cg100 {

// ===========================================================================
// Memory
// ===========================================================================

Memory::Memory(MmioIf* mmio)
    : flash(FLASH_MUT_TOP, 0xFF),
      dram(DRAM_SIZE, 0),
      ilram(ILRAM_SIZE, 0),
      ocram(OCRAM_SIZE, 0),
      xyram(XYRAM_SIZE, 0),
      mmio(mmio) {}

void Memory::loadFlash(const uint8_t* img, size_t n) {
    std::fill(flash.begin(), flash.end(), 0xFF);
    if (img && n) {
        const size_t c = std::min(n, flash.size());
        std::memcpy(flash.data(), img, c);
    }
    fcmd = F_IDLE;
    bufRem = 0;
    bufW.clear();
    eraseBusy = 0;
}

uint32_t Memory::beRead(const std::vector<uint8_t>& b, uint32_t off, uint32_t size) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < size; ++i) {
        const uint64_t o = static_cast<uint64_t>(off) + i;
        const uint8_t byte = o < b.size() ? b[static_cast<size_t>(o)] : 0xFFu;
        v = (v << 8) | byte;
    }
    return v;
}

void Memory::beWrite(std::vector<uint8_t>& b, uint32_t off, uint32_t size, uint32_t val) {
    for (uint32_t i = 0; i < size; ++i) {
        const uint64_t o = static_cast<uint64_t>(off) + size - 1 - i;
        if (o < b.size()) b[static_cast<size_t>(o)] = static_cast<uint8_t>(val >> (8 * i));
    }
}

uint32_t Memory::flashStatus(uint32_t size) {
    if (eraseBusy > 0) --eraseBusy;
    eraseTog ^= 0x44;
    const uint32_t st = 0x08u | eraseTog;
    return st * (0x01010101u >> ((4 - size) * 8));
}

void Memory::flashProgram(uint32_t phys, uint32_t size, uint32_t val) {
    if (static_cast<uint64_t>(phys) + size > FLASH_MUT_TOP) return;
    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t b = static_cast<uint8_t>(val >> (8 * (size - 1 - i)));
        flash[phys + i] &= b;   // NOR: bits only go 1 -> 0
    }
}

void Memory::flashCmd(uint32_t phys, uint32_t size, uint32_t val) {
    const uint32_t a12 = phys & 0xFFF;
    const uint32_t c = val & 0xFF;
    switch (fcmd) {
    case F_IDLE:
        if (a12 == 0xAAA && c == 0xAA) fcmd = F_UNLOCK1;
        break;
    case F_UNLOCK1:
        fcmd = (a12 == 0x554 && c == 0x55) ? F_UNLOCK2 : F_IDLE;
        break;
    case F_UNLOCK2:
        if (a12 == 0xAAA && c == 0xA0) fcmd = F_PROGRAM;
        else if (a12 == 0xAAA && c == 0x80) fcmd = F_ERASE1;
        else if (c == 0x25) { bufW.clear(); bufRem = -1; fcmd = F_BUFCOUNT; }
        else fcmd = F_IDLE; // 0x90 autoselect, 0x98 CFI, 0xF0 reset: no array change
        break;
    case F_PROGRAM:
        flashProgram(phys, size, val);
        fcmd = F_IDLE;
        break;
    case F_ERASE1:
        fcmd = (a12 == 0xAAA && c == 0xAA) ? F_ERASE2 : F_IDLE;
        break;
    case F_ERASE2:
        fcmd = (a12 == 0x554 && c == 0x55) ? F_ERASE3 : F_IDLE;
        break;
    case F_ERASE3:
        if (c == 0x10) { // chip erase
            std::fill(flash.begin(), flash.end(), 0xFF);
        } else if (c == 0x30) { // 128 KB sector erase
            const uint32_t s = phys & ~(FLASH_SECTOR - 1);
            for (uint32_t i = s; i < s + FLASH_SECTOR && i < FLASH_MUT_TOP; ++i) flash[i] = 0xFF;
            eraseBusy = ERASE_STATUS_READS;
            eraseSect = s;
        }
        fcmd = F_IDLE;
        break;
    case F_BUFCOUNT:
        bufRem = static_cast<int>(val & 0xFFFF) + 1;
        fcmd = F_BUFDATA;
        break;
    case F_BUFDATA:
        if (bufRem > 0) { bufW.push_back({phys, size, val}); --bufRem; }
        else { for (const auto& w : bufW) flashProgram(w.addr, w.size, w.val); fcmd = F_IDLE; }
        break;
    }
}

uint32_t Memory::read(uint32_t va, uint32_t size) {
    // CG100 second flash window at physical 0x0B000000.
    if (va >= FLASH_ALIAS && va < FLASH_ALIAS + FLASH_MUT_TOP) {
        return beRead(flash, va - FLASH_ALIAS, size);
    }
    if (va >= 0xE0000000u) {
        if (va >= ILRAM_BASE && va < ILRAM_BASE + ILRAM_SIZE)
            return beRead(ilram, (va - ILRAM_BASE) & ILRAM_MASK, size);
        if (va - XYRAM_BASE < XYRAM_SIZE)
            return beRead(xyram, va - XYRAM_BASE, size);
        if (va >= OCRAM_BASE && va < OCRAM_BASE + OCRAM_SIZE)
            return beRead(ocram, va - OCRAM_BASE, size);
        if (mmio) return mmio->read(va, size);
        unmappedFault = true; faultAddr = va;
        return 0;
    }
    // DRAM via ANY mirror, including the uncached VRAM mirror 0xAC000000.
    const uint32_t mphys = va & 0x1FFFFFFFu;
    if (mphys >= DRAM_BASE && mphys < DRAM_BASE + DRAM_SIZE)
        return beRead(dram, mphys - DRAM_BASE, size);
    if (va >= 0xA4000000u && va < 0xC0000000u) {
        if (mmio) return mmio->read(va, size);
        unmappedFault = true; faultAddr = va;
        return 0;
    }
    if (mphys < FLASH_SIZE) {
        if (mphys < FLASH_MUT_TOP) {
            if (eraseBusy > 0 && (mphys & ~(FLASH_SECTOR - 1)) == eraseSect)
                return flashStatus(size);
            return beRead(flash, mphys, size);
        }
        return (uint32_t(1) << (size * 8)) - 1; // erased 0xFF
    }
    unmappedFault = true; faultAddr = va;
    return 0;
}

void Memory::write(uint32_t va, uint32_t size, uint32_t val) {
    const uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((uint32_t(1) << (size * 8)) - 1);
    val &= mask;
    if (va >= 0xE0000000u) {
        if (va >= ILRAM_BASE && va < ILRAM_BASE + ILRAM_SIZE) {
            beWrite(ilram, (va - ILRAM_BASE) & ILRAM_MASK, size, val);
            return;
        }
        if (va - XYRAM_BASE < XYRAM_SIZE) { beWrite(xyram, va - XYRAM_BASE, size, val); return; }
        if (va >= OCRAM_BASE && va < OCRAM_BASE + OCRAM_SIZE) {
            beWrite(ocram, va - OCRAM_BASE, size, val);
            return;
        }
        if (mmio) { mmio->write(va, size, val); return; }
        unmappedFault = true; faultAddr = va;
        return;
    }
    const uint32_t mphys = va & 0x1FFFFFFFu;
    if (mphys >= DRAM_BASE && mphys < DRAM_BASE + DRAM_SIZE) {
        beWrite(dram, mphys - DRAM_BASE, size, val);
        return;
    }
    if (va >= 0xA4000000u && va < 0xC0000000u) {
        if (mmio) { mmio->write(va, size, val); return; }
        unmappedFault = true; faultAddr = va;
        return;
    }
    if (mphys < FLASH_SIZE) { flashCmd(mphys, size, val); return; }
    unmappedFault = true; faultAddr = va;
}

uint16_t Memory::fetch16(uint32_t pc) {
    // Hot path: untranslated code in flash or DRAM.  Reads straight from the
    // backing slice; everything else takes the general (MMIO) path.
    if (pc < 0xA4000000u) {
        const uint32_t p = pc & 0x1FFFFFFFu;
        if (p < FLASH_MUT_TOP - 1) {
            return static_cast<uint16_t>((flash[p] << 8) | flash[p + 1]);
        }
        if (p >= DRAM_BASE && p < DRAM_BASE + DRAM_SIZE - 1) {
            const uint32_t o = p - DRAM_BASE;
            return static_cast<uint16_t>((dram[o] << 8) | dram[o + 1]);
        }
    }
    return static_cast<uint16_t>(read(pc, 2));
}

std::vector<uint8_t> Memory::span(uint32_t va, uint32_t n) {
    const uint32_t p = va & 0x1FFFFFFFu;
    if (p >= DRAM_BASE && p + n <= DRAM_BASE + DRAM_SIZE) {
        return std::vector<uint8_t>(dram.begin() + (p - DRAM_BASE),
                                    dram.begin() + (p - DRAM_BASE) + n);
    }
    if (va - XYRAM_BASE < XYRAM_SIZE && va - XYRAM_BASE + n <= XYRAM_SIZE) {
        return std::vector<uint8_t>(xyram.begin() + (va - XYRAM_BASE),
                                    xyram.begin() + (va - XYRAM_BASE) + n);
    }
    std::vector<uint8_t> b(n);
    for (uint32_t i = 0; i < n; ++i)
        b[i] = static_cast<uint8_t>(read(0xA0000000u | ((va + i) & 0x1FFFFFFFu), 1));
    return b;
}

// ===========================================================================
// CPU
// ===========================================================================

void Sh4CPU::reset() { hardwareReset(); }

void Sh4CPU::hardwareReset() {
    std::memset(r, 0, sizeof(r));
    std::memset(rbank1, 0, sizeof(rbank1));
    pc = 0x80000000u;
    pr = gbr = vbr = 0;
    ssr = spc = sgr = dbr = 0;
    mach = macl = fpul = 0;
    dspMod = dspRs = dspRe = dspDsr = 0;
    dspA0 = dspX0 = dspX1 = dspY0 = dspY1 = dspRtc = 0;
    sr = 0;
    setSR(SR_MD | SR_RB | SR_BL | 0xF0u);
    pending.clear();
    sleeping = inSlot = excTaken = ldst = false;
    slotPC = 0;
    fault_.clear();
    halted_ = false;
    mem.unmappedFault = false;
}

void Sh4CPU::setSR(uint32_t val) {
    if (bank1() != ((val & (SR_MD | SR_RB)) == (SR_MD | SR_RB))) {
        for (uint32_t i = 0; i < 8; ++i) std::swap(r[i], rbank1[i]);
    }
    sr = val;
}

void Sh4CPU::raiseIRQ(uint32_t intevt, uint32_t level) {
    for (const auto& p : pending) if (p.intevt == intevt) return;
    pending.push_back({level, intevt});
}

bool Sh4CPU::gatePending() {
    int best = -1;
    std::vector<Pending> k;
    k.reserve(pending.size());
    for (const auto& p : pending) {
        uint32_t prio = 0;
        bool masked = false;
        const bool known = mem.mmio && mem.mmio->irqGate(p.intevt, prio, masked);
        if (known && prio == 0) continue;      // disabled source: drop request
        k.push_back(p);
        if (known && masked) continue;         // masked source: wait
        if (best < 0 || p.level > k[best].level ||
            (p.level == k[best].level && p.intevt > k[best].intevt))
            best = static_cast<int>(k.size()) - 1;
    }
    pending.swap(k);
    return best >= 0;
}

bool Sh4CPU::acceptInterrupt() {
    if (pending.empty() || (sr & SR_BL) != 0) return false;
    const uint32_t imask = (sr & SR_IMASK) >> 4;
    if (!gatePending()) return false;
    // Find the winning pending entry again (highest level, then highest INTEVT).
    size_t bi = 0;
    for (size_t i = 1; i < pending.size(); ++i) {
        if (pending[i].level > pending[bi].level ||
            (pending[i].level == pending[bi].level && pending[i].intevt > pending[bi].intevt))
            bi = i;
    }
    bool masked = false; uint32_t prio = 0;
    if (mem.mmio && mem.mmio->irqGate(pending[bi].intevt, prio, masked) && masked) return false;
    const Pending top = pending[bi];
    if (top.level <= imask) return false; // masked by current priority level
    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(bi));
    ldst = false;
    ssr = sr;
    spc = pc;
    sgr = r[15];
    mem.write(0xFF000028u, 4, top.intevt);
    uint32_t nsr = sr | SR_MD | SR_RB | SR_BL;
    // CPUOPM.INTMU (bit 3 at 0xFF2F0000): IMASK takes the accepted level.
    if (mem.read(0xFF2F0000u, 1) & 0x8u)
        nsr = (nsr & ~SR_IMASK) | (top.level << 4);
    setSR(nsr);
    pc = vbr + 0x600u;
    ++irqCnt;
    return true;
}

void Sh4CPU::generalException(uint32_t expevt, uint32_t spc_) {
    if (sr & SR_BL) {
        noteFault("exception with SR.BL set (real CPU would reset)", pc);
        return;
    }
    mem.write(0xFF000024u, 4, expevt);
    ssr = sr;
    spc = spc_;
    sgr = r[15];
    ldst = false;
    setSR(sr | SR_MD | SR_RB | SR_BL);
    pc = vbr + 0x100u;
    excTaken = inSlot;
}

void Sh4CPU::illegal(uint32_t op) {
    if (illegalHook) illegalHook(op, pc - 2, inSlot);
    if (inSlot) generalException(0x1A0u, slotPC);
    else generalException(0x180u, pc - 2);
}

void Sh4CPU::trapa(uint32_t imm) {
    mem.write(0xFF000020u, 4, imm << 2);
    generalException(0x160u, pc);
}

bool Sh4CPU::isDelayedBranch(uint32_t op) {
    if (op == 0x000Bu || op == 0x002Bu) return true;
    if ((op & 0xF0FFu) == 0x0003u || (op & 0xF0FFu) == 0x0023u ||
        (op & 0xF0FFu) == 0x400Bu || (op & 0xF0FFu) == 0x402Bu) return true;
    if ((op & 0xE000u) == 0xA000u || (op & 0xFD00u) == 0x8D00u) return true;
    return false;
}

bool Sh4CPU::slotIllegal(uint32_t op) {
    return isDelayedBranch(op) ||
           (op & 0xFD00u) == 0x8900u || (op & 0xFF00u) == 0xC300u ||
           (op & 0xF0FFu) == 0x400Eu || (op & 0xF0FFu) == 0x4007u;
}

void Sh4CPU::branchDelayed(uint32_t target) {
    const uint32_t slot = mem.fetch16(pc);
    if (slotIllegal(slot)) {
        illegal(slot);
        return;
    }
    pc += 2;
    inSlot = true;
    slotPC = pc - 4;
    execute(slot);
    inSlot = false;
    if (excTaken) { excTaken = false; return; }
    pc = target;
}

void Sh4CPU::noteFault(const char* what, uint32_t a) {
    if (fault_.empty()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s at pc=%08x addr=%08x", what, pc, a);
        fault_ = buf;
    }
    halted_ = true;
}

void Sh4CPU::step() {
    if (halted_) return;
    if (mem.mmio) mem.mmio->tick(cycles, idle);

    if (sleeping) {
        ++cycles; ++idle;
        if (!pending.empty() && gatePending()) sleeping = false;
        return;
    }
    acceptInterrupt();

    // Compatibility HLE hooks (coreabi.go resetBootstrap).
    if (compatHooks) {
        if (pc == 0x80005194u) {
            if (r[9] == 0) r[9] = 0xA0020080u;
            // The Go hook returns false: the real instruction still executes.
        } else if (pc == 0xA0020134u) {
            r[0] = 1;
            pc = pr;
            return; // Go hook returns true: instruction skipped, no cycle charged.
        }
    }

    const uint32_t op = mem.fetch16(pc);
    pc += 2;
    execute(op);
    ++cycles;
    if (mem.unmappedFault) {
        noteFault("unmapped memory access", mem.faultAddr);
        mem.unmappedFault = false;
    }
}

void Sh4CPU::run(uint64_t n) {
    const uint64_t end = cycles + n;
    while (cycles < end && !halted_) step();
}

// ---------------------------------------------------------------------------
// div1: one radix-2 division step (SR.Q/M/T), from cpu.go.
// ---------------------------------------------------------------------------
void Sh4CPU::div1(uint32_t n, uint32_t m) {
    const uint32_t Rm = r[m];
    uint32_t Rn = r[n];
    const uint32_t oldQ = (sr & SR_Q) ? 1u : 0u;
    const uint32_t M = (sr & SR_M) ? 1u : 0u;
    const uint32_t T = (sr & SR_T) ? 1u : 0u;
    uint32_t Q = (Rn >> 31) & 1u;
    Rn = (Rn << 1) | T;
    uint32_t tmp0, tmp1;
    if (oldQ == 0) {
        if (M == 0) {
            tmp0 = Rn; Rn -= Rm; tmp1 = (Rn > tmp0) ? 1u : 0u;
            Q = (Q == 0) ? tmp1 : (tmp1 ^ 1u);
        } else {
            tmp0 = Rn; Rn += Rm; tmp1 = (Rn < tmp0) ? 1u : 0u;
            Q = (Q == 0) ? (tmp1 ^ 1u) : tmp1;
        }
    } else {
        if (M == 0) {
            tmp0 = Rn; Rn += Rm; tmp1 = (Rn < tmp0) ? 1u : 0u;
            Q = (Q == 0) ? tmp1 : (tmp1 ^ 1u);
        } else {
            tmp0 = Rn; Rn -= Rm; tmp1 = (Rn > tmp0) ? 1u : 0u;
            Q = (Q == 0) ? (tmp1 ^ 1u) : tmp1;
        }
    }
    r[n] = Rn;
    if (Q) sr |= SR_Q; else sr &= ~SR_Q;
    setT(Q == M);
}

void Sh4CPU::macw(uint32_t n, uint32_t m) {
    const int64_t tn = s16(mem.read(r[n], 2)); r[n] += 2;
    const int64_t tm = s16(mem.read(r[m], 2)); r[m] += 2;
    const int64_t p = tn * tm;
    if ((sr & SR_S) == 0) {
        const int64_t mac = (static_cast<int64_t>(static_cast<uint64_t>(mach) << 32) |
                             static_cast<int32_t>(macl)) + p;
        mach = static_cast<uint32_t>(static_cast<uint64_t>(mac) >> 32);
        macl = static_cast<uint32_t>(mac);
        return;
    }
    const int64_t v = static_cast<int32_t>(macl) + p;
    if (v > 0x7FFFFFFF) { macl = 0x7FFFFFFF; mach |= 1; }
    else if (v < static_cast<int64_t>(0x80000000)) { macl = 0x80000000; mach |= 1; }
    else macl = static_cast<uint32_t>(v);
}

// ---------------------------------------------------------------------------
// execute: the full opcode table (cpu.go execute + exec4).
// ---------------------------------------------------------------------------
void Sh4CPU::execute(uint32_t op) {
    const uint32_t n  = (op >> 8) & 0xF;
    const uint32_t m  = (op >> 4) & 0xF;
    const uint32_t d4 = op & 0xF;
    const uint32_t d8 = op & 0xFF;
    const uint32_t d12 = op & 0xFFF;
    const uint32_t hi = op >> 12;
    uint32_t* const R = r;

    switch (op) {
    case 0x0009: return;                       // nop
    case 0x000B: branchDelayed(pr); return;    // rts
    case 0x002B: {                             // rte
        const uint32_t tgt = spc;
        setSR(ssr);
        branchDelayed(tgt);
        return;
    }
    case 0x0008: setT(false); return;          // clrt
    case 0x0018: setT(true); return;           // sett
    case 0x0019: sr &= ~(SR_Q | SR_M | SR_T); return; // div0u
    case 0x0028: mach = macl = 0; return;      // clrmac
    case 0x0048: sr &= ~SR_S; return;          // clrs
    case 0x0058: sr |= SR_S; return;           // sets
    case 0x001B: sleeping = true; return;      // sleep
    case 0x0038: return;                       // ldtlb (no MMU)
    case 0x00AB: case 0x0093: return;          // synco / reserved -> nop
    }
    if ((op & 0xF0FFu) == 0x00C3u) {           // movca.l R0,@Rn (cache alloc = plain store)
        mem.write(R[n], 4, R[0]);
        return;
    }

    switch (hi) {
    case 0x0:
        if (op == 0x0B00u) return;             // undefined in OS 2.00 boot stub: nop
        switch (d8) {
        case 0x02: R[n] = sr; return;          // stc SR,Rn
        case 0x12: R[n] = gbr; return;         // stc GBR,Rn
        case 0x22: R[n] = vbr; return;         // stc VBR,Rn
        case 0x32: R[n] = ssr; return;         // stc SSR,Rn
        case 0x42: R[n] = spc; return;         // stc SPC,Rn
        case 0x0A: R[n] = mach; return;        // sts MACH,Rn
        case 0x1A: R[n] = macl; return;        // sts MACL,Rn
        case 0x2A: R[n] = pr; return;          // sts PR,Rn
        case 0x3A: R[n] = sgr; return;         // stc SGR,Rn
        case 0x52: R[n] = dspMod; return;      // stc MOD,Rn
        case 0x62: R[n] = dspRs; return;       // stc RS,Rn
        case 0x72: R[n] = dspRe; return;       // stc RE,Rn
        case 0x6A: R[n] = dspDsr; return;      // sts DSR,Rn
        case 0x7A: R[n] = dspA0; return;       // sts A0,Rn
        case 0x8A: R[n] = dspX0; return;
        case 0x9A: R[n] = dspX1; return;
        case 0xAA: R[n] = dspY0; return;
        case 0xBA: R[n] = dspY1; return;
        case 0xFA: R[n] = dbr; return;         // stc DBR,Rn
        case 0x29: R[n] = sr & SR_T; return;   // stc T,Rn
        case 0x03: pr = pc + 2; branchDelayed(pc + 2 + R[n]); return; // bsrf
        case 0x23: branchDelayed(pc + 2 + R[n]); return;              // braf
        case 0x83: case 0x93: case 0xA3: case 0xB3: case 0xD3: case 0xE3:
            return; // pref / ocbi / ocbp / ocbwb / prefi / icbi: nop (no cache model)
        case 0x63: R[0] = mem.read(R[n], 4); ldst = true; return; // movli.l @Rm,R0
        case 0x73: // movco.l R0,@Rn
            if (ldst) mem.write(R[n], 4, R[0]);
            setT(ldst);
            ldst = false;
            return;
        }
        if ((op & 0x8Fu) == 0x82u) { R[n] = rbank1[(op >> 4) & 7]; return; } // stc Rm_bank,Rn
        switch (d4) {
        case 0x4: mem.write(R[0] + R[n], 1, R[m] & 0xFF); return;
        case 0x5: mem.write(R[0] + R[n], 2, R[m] & 0xFFFF); return;
        case 0x6: mem.write(R[0] + R[n], 4, R[m]); return;
        case 0x7: macl = static_cast<uint32_t>(s32(R[n]) * s32(R[m])); return; // mul.l
        case 0xF: { // mac.l @Rm+,@Rn+
            const int64_t tn = s32(mem.read(R[n], 4)); R[n] += 4;
            const int64_t tm = s32(mem.read(R[m], 4)); R[m] += 4;
            int64_t acc = (static_cast<int64_t>(static_cast<uint64_t>(mach) << 32) |
                           static_cast<int32_t>(macl)) + tn * tm;
            if (sr & SR_S) {
                const int64_t hi = (int64_t(1) << 47) - 1, lo = -(int64_t(1) << 47);
                if (acc > hi) acc = hi; else if (acc < lo) acc = lo;
            }
            mach = static_cast<uint32_t>(static_cast<uint64_t>(acc) >> 32);
            macl = static_cast<uint32_t>(acc);
            return;
        }
        case 0xC: R[n] = static_cast<uint32_t>(s8(mem.read(R[0] + R[m], 1))); return;
        case 0xD: R[n] = static_cast<uint32_t>(s16(mem.read(R[0] + R[m], 2))); return;
        case 0xE: R[n] = mem.read(R[0] + R[m], 4); return;
        }
        illegal(op);
        return;

    case 0x1: mem.write(R[n] + d4 * 4, 4, R[m]); return; // mov.l Rm,@(disp,Rn)

    case 0x2:
        switch (d4) {
        case 0x0: mem.write(R[n], 1, R[m] & 0xFF); return;
        case 0x1: mem.write(R[n], 2, R[m] & 0xFFFF); return;
        case 0x2: mem.write(R[n], 4, R[m]); return;
        case 0x4: --R[n]; mem.write(R[n], 1, R[m] & 0xFF); return;
        case 0x5: R[n] -= 2; mem.write(R[n], 2, R[m] & 0xFFFF); return;
        case 0x6: R[n] -= 4; mem.write(R[n], 4, R[m]); return;
        case 0x7: // div0s
            if (R[n] & 0x80000000u) sr |= SR_Q; else sr &= ~SR_Q;
            if (R[m] & 0x80000000u) sr |= SR_M; else sr &= ~SR_M;
            setT(((R[n] >> 31) ^ (R[m] >> 31)) != 0);
            return;
        case 0x8: setT((R[n] & R[m]) == 0); return;      // tst Rn,Rm
        case 0x9: R[n] &= R[m]; return;                  // and
        case 0xA: R[n] ^= R[m]; return;                  // xor
        case 0xB: R[n] |= R[m]; return;                  // or
        case 0xC: { // cmp/str
            bool t = false;
            for (uint32_t i = 0; i < 4; ++i)
                if (((R[n] >> (8 * i)) & 0xFF) == ((R[m] >> (8 * i)) & 0xFF)) t = true;
            setT(t);
            return;
        }
        case 0xD: R[n] = ((R[m] & 0xFFFF) << 16) | (R[n] >> 16); return; // xtrct
        case 0xE: macl = (R[n] & 0xFFFF) * (R[m] & 0xFFFF); return;      // mulu.w
        case 0xF: macl = static_cast<uint32_t>(s16(R[n]) * s16(R[m])); return; // muls.w
        }
        illegal(op);
        return;

    case 0x3:
        switch (d4) {
        case 0x0: setT(R[n] == R[m]); return;            // cmp/eq
        case 0x2: setT(R[n] >= R[m]); return;            // cmp/hs (unsigned)
        case 0x3: setT(s32(R[n]) >= s32(R[m])); return;  // cmp/ge (signed)
        case 0x6: setT(R[n] > R[m]); return;             // cmp/hi (unsigned)
        case 0x7: setT(s32(R[n]) > s32(R[m])); return;   // cmp/gt (signed)
        case 0x8: R[n] -= R[m]; return;                  // sub
        case 0xC: R[n] += R[m]; return;                  // add
        case 0xA: { // subc
            const int64_t res = static_cast<int64_t>(R[n]) - R[m] - (sr & SR_T);
            setT(res < 0);
            R[n] = static_cast<uint32_t>(res);
            return;
        }
        case 0xE: { // addc
            const uint64_t res = static_cast<uint64_t>(R[n]) + R[m] + (sr & SR_T);
            setT(res > 0xFFFFFFFFull);
            R[n] = static_cast<uint32_t>(res);
            return;
        }
        case 0xB: { // subv
            const int64_t res = static_cast<int64_t>(s32(R[n])) - s32(R[m]);
            setT(res < -0x80000000ll || res > 0x7FFFFFFFll);
            R[n] = static_cast<uint32_t>(res);
            return;
        }
        case 0xF: { // addv
            const int64_t res = static_cast<int64_t>(s32(R[n])) + s32(R[m]);
            setT(res < -0x80000000ll || res > 0x7FFFFFFFll);
            R[n] = static_cast<uint32_t>(res);
            return;
        }
        case 0x4: div1(n, m); return;
        case 0x5: { // dmulu.l
            const uint64_t res = static_cast<uint64_t>(R[n]) * R[m];
            mach = static_cast<uint32_t>(res >> 32);
            macl = static_cast<uint32_t>(res);
            return;
        }
        case 0xD: { // dmuls.l
            const int64_t res = static_cast<int64_t>(s32(R[n])) * s32(R[m]);
            mach = static_cast<uint32_t>(static_cast<uint64_t>(res) >> 32);
            macl = static_cast<uint32_t>(res);
            return;
        }
        }
        illegal(op);
        return;

    case 0x4: exec4(op, n, m, d8); return;

    case 0x5: R[n] = mem.read(R[m] + d4 * 4, 4); return; // mov.l @(disp,Rm),Rn

    case 0x6:
        switch (d4) {
        case 0x0: R[n] = static_cast<uint32_t>(s8(mem.read(R[m], 1))); return;
        case 0x1: R[n] = static_cast<uint32_t>(s16(mem.read(R[m], 2))); return;
        case 0x2: R[n] = mem.read(R[m], 4); return;
        case 0x3: R[n] = R[m]; return;
        case 0x4: R[n] = static_cast<uint32_t>(s8(mem.read(R[m], 1))); R[m] += 1; return;
        case 0x5: R[n] = static_cast<uint32_t>(s16(mem.read(R[m], 2))); R[m] += 2; return;
        case 0x6: R[n] = mem.read(R[m], 4); R[m] += 4; return;
        case 0x7: R[n] = ~R[m]; return;
        case 0x8: R[n] = ((R[m] & 0xFF) << 8) | ((R[m] >> 8) & 0xFF) | (R[m] & 0xFFFF0000); return;
        case 0x9: R[n] = ((R[m] & 0xFFFF) << 16) | ((R[m] >> 16) & 0xFFFF); return;
        case 0xA: { // negc
            const int64_t res = -static_cast<int64_t>(R[m]) - (sr & SR_T);
            setT(res < 0);
            R[n] = static_cast<uint32_t>(res);
            return;
        }
        case 0xB: R[n] = static_cast<uint32_t>(-s32(R[m])); return;
        case 0xC: R[n] = R[m] & 0xFF; return;
        case 0xD: R[n] = R[m] & 0xFFFF; return;
        case 0xE: R[n] = static_cast<uint32_t>(s8(R[m])); return;
        case 0xF: R[n] = static_cast<uint32_t>(s16(R[m])); return;
        }
        return;

    case 0x7: R[n] += static_cast<uint32_t>(s8(d8)); return; // add #imm,Rn

    case 0x8:
        switch (n) {
        case 0x0: mem.write(R[m] + d4, 1, R[0] & 0xFF); return;
        case 0x1: mem.write(R[m] + d4 * 2, 2, R[0] & 0xFFFF); return;
        case 0x4: R[0] = static_cast<uint32_t>(s8(mem.read(R[m] + d4, 1))); return;
        case 0x5: R[0] = static_cast<uint32_t>(s16(mem.read(R[m] + d4 * 2, 2))); return;
        case 0x6: R[0] = mem.read(R[m] + d4 * 4, 4); return;
        case 0x8: setT(R[0] == static_cast<uint32_t>(s8(d8))); return; // cmp/eq #imm,R0
        case 0x9: if (sr & SR_T) pc += 2 + static_cast<uint32_t>(s8(d8)) * 2; return; // bt
        case 0xB: if (!(sr & SR_T)) pc += 2 + static_cast<uint32_t>(s8(d8)) * 2; return; // bf
        case 0xD: if (sr & SR_T) branchDelayed(pc + 2 + static_cast<uint32_t>(s8(d8)) * 2); return; // bt/s
        case 0xF: if (!(sr & SR_T)) branchDelayed(pc + 2 + static_cast<uint32_t>(s8(d8)) * 2); return; // bf/s
        case 0x2: dspRtc = d8; return; // setrc #imm
        case 0xA: dspRtc = d8; return; // ldrc #imm
        case 0xC: dspRs = pc + 2 + static_cast<uint32_t>(s8(d8)) * 2; return; // ldrs
        case 0xE: dspRe = pc + 2 + static_cast<uint32_t>(s8(d8)) * 2; return; // ldre
        }
        illegal(op);
        return;

    case 0x9: {                                // mov.w @(disp,PC),Rn
        const uint32_t ea = pc + 2 + d8 * 2;
        R[n] = static_cast<uint32_t>(s16(mem.read(ea, 2)));
        return;
    }
    case 0xA: {                                // bra
        const uint32_t disp = d12 & 0x800u ? d12 - 0x1000u : d12;
        branchDelayed(pc + 2 + disp * 2);
        return;
    }
    case 0xB: {                                // bsr
        const uint32_t disp = d12 & 0x800u ? d12 - 0x1000u : d12;
        pr = pc + 2;
        branchDelayed(pc + 2 + disp * 2);
        return;
    }
    case 0xC:
        switch (n) {
        case 0x0: mem.write(gbr + d8, 1, R[0] & 0xFF); return;
        case 0x1: mem.write(gbr + d8 * 2, 2, R[0] & 0xFFFF); return;
        case 0x2: mem.write(gbr + d8 * 4, 4, R[0]); return;
        case 0x3: trapa(d8); return;
        case 0x4: R[0] = static_cast<uint32_t>(s8(mem.read(gbr + d8, 1))); return;
        case 0x5: R[0] = static_cast<uint32_t>(s16(mem.read(gbr + d8 * 2, 2))); return;
        case 0x6: R[0] = mem.read(gbr + d8 * 4, 4); return;
        case 0x7: R[0] = ((pc + 2) & ~3u) + d8 * 4; return; // mova
        case 0x8: setT((R[0] & d8) == 0); return;          // tst #imm,R0
        case 0x9: R[0] &= d8; return;
        case 0xA: R[0] ^= d8; return;
        case 0xB: R[0] |= d8; return;
        case 0xC: setT((mem.read(gbr + R[0], 1) & d8) == 0); return; // tst.b #imm,@(R0,GBR)
        case 0xD: mem.write(gbr + R[0], 1, mem.read(gbr + R[0], 1) & d8); return;
        case 0xE: mem.write(gbr + R[0], 1, mem.read(gbr + R[0], 1) ^ d8); return;
        case 0xF: mem.write(gbr + R[0], 1, mem.read(gbr + R[0], 1) | d8); return;
        }
        return;

    case 0xD: {                                // mov.l @(disp,PC),Rn
        const uint32_t ea = ((pc + 2) & ~3u) + d8 * 4;
        R[n] = mem.read(ea, 4);
        return;
    }
    case 0xE: R[n] = static_cast<uint32_t>(s8(d8)); return; // mov #imm,Rn

    case 0xF: return; // SH4AL-DSP (no FPU on this part): treated as nop

    default: break;
    }
    illegal(op);
}

// ---------------------------------------------------------------------------
// exec4: hi==0x0 nibble group (shifts, jsr/jmp, ldc/stc, ...).
// ---------------------------------------------------------------------------
void Sh4CPU::exec4(uint32_t op, uint32_t n, uint32_t m, uint32_t d8) {
    uint32_t* const R = r;
    switch (d8) {
    case 0x00: setT((R[n] >> 31) != 0); R[n] <<= 1; return;            // shll
    case 0x01: setT((R[n] & 1) != 0); R[n] >>= 1; return;              // shlr
    case 0x04: { const uint32_t t = R[n] >> 31; R[n] = (R[n] << 1) | t; setT(t != 0); return; } // rotl
    case 0x05: { const uint32_t t = R[n] & 1; R[n] = (R[n] >> 1) | (t << 31); setT(t != 0); return; } // rotr
    case 0x08: R[n] <<= 2; return;
    case 0x09: R[n] >>= 2; return;
    case 0x18: R[n] <<= 8; return;
    case 0x19: R[n] >>= 8; return;
    case 0x28: R[n] <<= 16; return;
    case 0x29: R[n] >>= 16; return;
    case 0x20: setT((R[n] >> 31) != 0); R[n] <<= 1; return;            // shal
    case 0x21: setT((R[n] & 1) != 0); R[n] = static_cast<uint32_t>(s32(R[n]) >> 1); return; // shar
    case 0x24: { const uint32_t t = R[n] >> 31; R[n] = (R[n] << 1) | (sr & SR_T); setT(t != 0); return; } // rotcl
    case 0x25: { const uint32_t t = R[n] & 1; R[n] = (R[n] >> 1) | ((sr & SR_T) << 31); setT(t != 0); return; } // rotcr
    case 0x10: --R[n]; setT(R[n] == 0); return;                        // dt
    case 0x11: setT(s32(R[n]) >= 0); return;                           // cmp/pz
    case 0x15: setT(s32(R[n]) > 0); return;                            // cmp/pl
    case 0x0B: pr = pc + 2; branchDelayed(R[n]); return;               // jsr
    case 0x2B: branchDelayed(R[n]); return;                            // jmp
    case 0x0E: setSR(R[n]); return;                                    // ldc Rn,SR
    case 0x1E: gbr = R[n]; return;
    case 0x2E: vbr = R[n]; return;
    case 0x3E: ssr = R[n]; return;
    case 0x4E: spc = R[n]; return;
    case 0x3A: sgr = R[n]; return; // ldc Rn,SGR
    case 0xFA: dbr = R[n]; return; // ldc Rn,DBR
    case 0x5E: dspMod = R[n]; return;
    case 0x6E: dspRs = R[n]; return;
    case 0x7E: dspRe = R[n]; return;
    case 0x14: case 0x34: dspRtc = R[n]; return; // setrc / ldrc Rn
    case 0x0A: mach = R[n]; return;
    case 0x1A: macl = R[n]; return;
    case 0x2A: pr = R[n]; return;
    case 0x5A: fpul = R[n]; return;
    case 0x6A: dspDsr = R[n]; return;
    case 0x7A: dspA0 = R[n]; return;
    case 0x8A: dspX0 = R[n]; return;
    case 0x9A: dspX1 = R[n]; return;
    case 0xAA: dspY0 = R[n]; return;
    case 0xBA: dspY1 = R[n]; return;
    case 0x07: setSR(mem.read(R[n], 4)); R[n] += 4; return;           // ldc.l @Rn+,SR
    case 0x17: gbr = mem.read(R[n], 4); R[n] += 4; return;
    case 0x27: vbr = mem.read(R[n], 4); R[n] += 4; return;
    case 0x37: ssr = mem.read(R[n], 4); R[n] += 4; return;
    case 0x47: spc = mem.read(R[n], 4); R[n] += 4; return;
    case 0x36: sgr = mem.read(R[n], 4); R[n] += 4; return;
    case 0xF6: dbr = mem.read(R[n], 4); R[n] += 4; return;
    case 0x57: dspMod = mem.read(R[n], 4); R[n] += 4; return;
    case 0x67: dspRs = mem.read(R[n], 4); R[n] += 4; return;
    case 0x77: dspRe = mem.read(R[n], 4); R[n] += 4; return;
    case 0x06: mach = mem.read(R[n], 4); R[n] += 4; return;
    case 0x16: macl = mem.read(R[n], 4); R[n] += 4; return;
    case 0x26: pr = mem.read(R[n], 4); R[n] += 4; return;
    case 0x56: fpul = mem.read(R[n], 4); R[n] += 4; return;
    case 0x66: dspDsr = mem.read(R[n], 4); R[n] += 4; return;
    case 0x76: dspA0 = mem.read(R[n], 4); R[n] += 4; return;
    case 0x86: dspX0 = mem.read(R[n], 4); R[n] += 4; return;
    case 0x96: dspX1 = mem.read(R[n], 4); R[n] += 4; return;
    case 0xA6: dspY0 = mem.read(R[n], 4); R[n] += 4; return;
    case 0xB6: dspY1 = mem.read(R[n], 4); R[n] += 4; return;
    case 0x03: R[n] -= 4; mem.write(R[n], 4, sr); return;             // stc.l SR,@-Rn
    case 0x13: R[n] -= 4; mem.write(R[n], 4, gbr); return;
    case 0x23: R[n] -= 4; mem.write(R[n], 4, vbr); return;
    case 0x33: R[n] -= 4; mem.write(R[n], 4, ssr); return;
    case 0x43: R[n] -= 4; mem.write(R[n], 4, spc); return;
    case 0x32: R[n] -= 4; mem.write(R[n], 4, sgr); return;
    case 0xF2: R[n] -= 4; mem.write(R[n], 4, dbr); return;
    case 0x53: R[n] -= 4; mem.write(R[n], 4, dspMod); return;
    case 0x63: R[n] -= 4; mem.write(R[n], 4, dspRs); return;
    case 0x73: R[n] -= 4; mem.write(R[n], 4, dspRe); return;
    case 0x02: R[n] -= 4; mem.write(R[n], 4, mach); return;
    case 0x12: R[n] -= 4; mem.write(R[n], 4, macl); return;
    case 0x22: R[n] -= 4; mem.write(R[n], 4, pr); return;
    case 0x52: R[n] -= 4; mem.write(R[n], 4, fpul); return;
    case 0x62: R[n] -= 4; mem.write(R[n], 4, dspDsr); return;
    case 0x72: R[n] -= 4; mem.write(R[n], 4, dspA0); return;
    case 0x82: R[n] -= 4; mem.write(R[n], 4, dspX0); return;
    case 0x92: R[n] -= 4; mem.write(R[n], 4, dspX1); return;
    case 0xA2: R[n] -= 4; mem.write(R[n], 4, dspY0); return;
    case 0xB2: R[n] -= 4; mem.write(R[n], 4, dspY1); return;
    case 0x1B: { // tas.b @Rn
        const uint32_t v = mem.read(R[n], 1);
        setT(v == 0);
        mem.write(R[n], 1, v | 0x80);
        return;
    }
    case 0xA9: case 0xE9: { // movua.l @Rm,R0 (unaligned load)
        const uint32_t a = R[n];
        R[0] = mem.read(a, 1) << 24 | mem.read(a + 1, 1) << 16 |
               mem.read(a + 2, 1) << 8 | mem.read(a + 3, 1);
        if (d8 == 0xE9 && n != 0) R[n] += 4;
        return;
    }
    }
    if ((op & 0x8Fu) == 0x8Eu) { rbank1[(op >> 4) & 7] = R[n]; return; } // ldc Rn,Rm_bank
    if ((op & 0x8Fu) == 0x87u) { rbank1[(op >> 4) & 7] = mem.read(R[n], 4); R[n] += 4; return; }
    if ((op & 0x8Fu) == 0x83u) { R[n] -= 4; mem.write(R[n], 4, rbank1[(op >> 4) & 7]); return; }
    const uint32_t d4 = op & 0xF;
    if (d4 == 0xF) { macw(n, m); return; } // mac.w @Rm+,@Rn+
    if (d4 == 0xC) {                       // shad
        const int32_t sh = s32(R[m]);
        if (sh >= 0) R[n] <<= (static_cast<uint32_t>(sh) & 0x1F);
        else {
            uint32_t sa = static_cast<uint32_t>(-sh) & 0x1F;
            if (sh == -32) sa = 31;
            R[n] = static_cast<uint32_t>(s32(R[n]) >> sa);
        }
        return;
    }
    if (d4 == 0xD) {                       // shld
        const int32_t sh = s32(R[m]);
        if (sh >= 0) R[n] <<= (static_cast<uint32_t>(sh) & 0x1F);
        else if (sh == -32) R[n] = 0;
        else R[n] >>= (static_cast<uint32_t>(-sh) & 0x1F);
        return;
    }
    illegal(op);
}

} // namespace cg100
