// cg100_mmio.cpp -- SH7305 MMIO bus for CG100 (behavioural port of the Go core).

#include "cg100_mmio.h"

#include <algorithm>
#include <cstring>

namespace cg100 {

namespace {
constexpr uint32_t RTC_BASE = 0xA413FEC0u;

struct RegDesc { uint32_t base, size; };
// Priority order matters: RTC lives inside the free-counter window.
const RegDesc kRegions[] = {
    {0xFF2F0000u, 0x4u},        // CPUOPM
    {0xA4150000u, 0x1000u},     // CPG
    {0xA4050000u, 0x1000u},     // PFC
    {0xA4520000u, 0x1000u},     // WDT
    {0xA4080000u, 0x1000u},     // INTC
    {0xA4490000u, 0x1000u},     // TMU
    {0xA44A0000u, 0x1000u},     // ETMU
    {0xA44D0000u, 0x1000u},     // ETMU2
    {0xA4610000u, 0x1000u},     // PERIPH_IRQ / battery ADC
    {RTC_BASE, 0x40u},          // RTC
    {0xA4130000u, 0x10000u},    // FRC (free counter)
    {0xA4140000u, 0x1000u},     // INTX
    {0xA44B0000u, 0x1000u},     // KEYSC
    {0xA4CB0000u, 0x1000u},     // BCDALU
    {0xFEC10000u, 0x1000u},     // BSC
    {0xFE008000u, 0x1000u},     // DMAC
    {0xFF000000u, 0x1000u},     // CCN
    {0xF6000000u, 0x02000000u}, // UTLB arrays
    {0xB4000000u, 0x20000u},    // LCD R61524 (area 5)
    {0xFF200000u, 0x1000u},     // UBC
};
enum RId { R_CPUOPM, R_CPG, R_PFC, R_WDT, R_INTC, R_TMU, R_ETMU, R_ETMU2,
           R_PERIPH, R_RTC, R_FRC, R_INTX, R_KEYSC, R_BCD, R_BSC, R_DMAC,
           R_CCN, R_UTLB, R_LCD, R_UBC };

// Resolve a possibly-aliased virtual address to (region, base, hit).
bool resolve(uint32_t va, const RegDesc*& desc, int& id, uint32_t& hit) {
    const uint32_t cand[3] = { va, (va & 0x1FFFFFFFu) | 0xA0000000u, va & 0x1FFFFFFFu };
    for (int i = 0; i < static_cast<int>(sizeof(kRegions) / sizeof(kRegions[0])); ++i) {
        const RegDesc& d = kRegions[i];
        for (uint32_t c : cand) {
            if (c >= d.base && c < d.base + d.size) {
                desc = &d; id = i; hit = c;
                return true;
            }
        }
    }
    return false;
}

uint32_t bcd8(uint64_t v) { return static_cast<uint32_t>((v / 10) << 4 | v % 10); }
} // namespace

CG100MMIO::CG100MMIO(Sh4CPU* cpu) : cpu_(cpu), gram_(PANEL_W * PANEL_H, 0) {
    initRegions();
}

void CG100MMIO::initRegions() {
    // CPG reset value: FRQCR reads back the calculator's clock setting.
    regs_[0xA4150000u] = 0x0F011112u;
    // UBC channel control reset values (MFI field reserved).
    regs_[0xFF200000u] = 0x20000000u;
    regs_[0xFF200020u] = 0x20000000u;
    // Seed the INTC fields the OS programs during boot (intc.go seedOSDefaults).
    ipr_[1] |= 0xC000;  // timer 0x560, priority 12
    ipr_[5] |= 0xD000;  // KEYSC 0xBE0, priority 13
    ipr_[10] |= 0x8000; // RTC 0xAA0, priority 8
    imr_[4] &= ~uint8_t(0x08);
    imr_[5] &= ~uint8_t(0x80);
    imr_[10] &= ~uint8_t(0x02);
    // LCD boot defaults (lcd.go bootDefaults).
    lcdReg_[0x003] = 0x00A0;
    lcdReg_[0x007] = 0x0100;
    lcdReg_[0x210] = 0;
    lcdReg_[0x211] = PANEL_W - 1;
    lcdReg_[0x212] = 0;
    lcdReg_[0x213] = PANEL_H - 1;
}

uint32_t CG100MMIO::regRead(uint32_t va, uint32_t def) const {
    auto it = regs_.find(va);
    return it == regs_.end() ? def : it->second;
}

bool CG100MMIO::source(uint32_t intevt, Src& out) {
    switch (intevt) {
    case 0x400: out = {0, 12, 4, 0x10, 0}; return true; // TMU0
    case 0x420: out = {0, 8, 4, 0x20, 0}; return true;  // TMU1
    case 0x440: out = {0, 4, 4, 0x40, 0}; return true;  // TMU2
    case 0x9E0: out = {9, 12, 6, 0x08, 0}; return true; // ETMU0
    case 0xC20: out = {6, 8, 5, 0x02, 0}; return true;  // ETMU1
    case 0xC40: out = {6, 4, 5, 0x04, 0}; return true;  // ETMU2
    case 0x900: out = {4, 4, 2, 0x01, 0}; return true;  // ETMU3
    case 0xD00: out = {8, 12, 6, 0x10, 0}; return true; // ETMU4
    case 0xFA0: out = {11, 12, 8, 0x02, 0}; return true;// ETMU5
    case 0x800: out = {4, 12, 1, 0x01, 0}; return true; // DEI0
    case 0x820: out = {4, 12, 1, 0x02, 0}; return true; // DEI1
    case 0x840: out = {4, 12, 1, 0x04, 0}; return true; // DEI2
    case 0x860: out = {4, 12, 1, 0x08, 0}; return true; // DEI3
    case 0xB80: out = {5, 8, 5, 0x10, 0}; return true;  // DEI4
    case 0xBA0: out = {5, 8, 5, 0x20, 0}; return true;  // DEI5
    case 0xAA0: out = {10, 12, 10, 0x02, LEVEL_RTC}; return true;
    case 0xBE0: out = {5, 12, 5, 0x80, LEVEL_KEYSC}; return true;
    case 0x560: out = {1, 12, 4, 0x08, LEVEL_TIMER}; return true;
    default: return false;
    }
}

bool CG100MMIO::irqGate(uint32_t intevt, uint32_t& prio, bool& masked) {
    Src s{};
    if (!source(intevt, s)) { prio = 0; masked = false; return false; }
    prio = (ipr_[s.ipr] >> s.shift) & 0xF;
    masked = (imr_[s.imr] & s.bit) != 0;
    return true;
}

void CG100MMIO::raise(uint32_t intevt, uint32_t level) {
    lastIntevt_ = intevt;
    if (cpu_) cpu_->raiseIRQ(intevt, level ? level : 8);
}

void CG100MMIO::setInstrPerSecond(uint64_t ips) {
    instrPerSec_ = std::max<uint64_t>(ips, 1000000);
    scanPeriod_ = instrPerSec_ / 33; // KEYSC 33 Hz measured
    etmuDiv_ = std::max<uint64_t>(1, instrPerSec_ / 32768);
    if ((rcr2_ & 0x70) != 0) rtcAlignPeriodic();
}

void CG100MMIO::setKeyScanPeriod(uint64_t instr) {
    scanPeriod_ = std::max<uint64_t>(instr, 1000);
}

// ---------------------------------------------------------------------------
// KEYSC
// ---------------------------------------------------------------------------
bool CG100MMIO::anyHeld() const {
    for (uint16_t w : keyHeld_) if (w) return true;
    return false;
}

bool CG100MMIO::anyEnabled() const {
    for (uint32_t w = 0; w < 6; ++w) if (keyData(w)) return true;
    return false;
}

uint32_t CG100MMIO::keyData(uint32_t w) const {
    uint32_t cols = 0x0FFF, rows = 0xFF;
    auto c = regs_.find(0xA44B0000u + 0x1A);
    auto r = regs_.find(0xA44B0000u + 0x1C);
    if (c != regs_.end()) cols = c->second;
    if (r != regs_.end()) rows = r->second & 0xFF;
    uint32_t m = 0;
    if ((cols >> (2 * w)) & 1) m |= rows;
    if ((cols >> (2 * w + 1)) & 1) m |= rows << 8;
    return keyHeld_[w] & m;
}

void CG100MMIO::keyPress(uint32_t row, uint32_t col) {
    if (row >= 8 || col >= 12) return;
    if (!anyHeld()) scanNext_ = 0; // press edge scans immediately
    keyHeld_[col >> 1] |= static_cast<uint16_t>(1u << (row + 8 * (col & 1)));
}

void CG100MMIO::keyRelease(uint32_t row, uint32_t col) {
    if (row >= 8 || col >= 12) return;
    keyHeld_[col >> 1] &= static_cast<uint16_t>(~(1u << (row + 8 * (col & 1))));
}

void CG100MMIO::keyReleaseAll() {
    keyHeld_.fill(0);
    scanLeft_ = 0;
    keyWasHeld_ = false;
}

void CG100MMIO::keyMatrixLevels(const bool levels[8][12]) {
    keyHeld_.fill(0);
    for (uint32_t row = 0; row < 8; ++row)
        for (uint32_t col = 0; col < 12; ++col)
            if (levels[row][col]) keyPress(row, col);
}

void CG100MMIO::scan() {
    const bool held = anyEnabled();
    if ((keyCtrl_ & 0x8000) == 0 || keyMode_ == 0) { keyWasHeld_ = held; return; }
    if (held && !keyWasHeld_) keyFlags_ |= 0x08;                  // detect
    if (held) scanLeft_ = 2;
    if ((keyMode_ & 0x800) != 0 || ((keyMode_ & 0x200) != 0 && (held || scanLeft_ > 0))) {
        keyFlags_ |= 0x02;                                       // scan complete
        if (!held && scanLeft_ > 0) --scanLeft_;
    }
    keyWasHeld_ = held;
    if ((keyFlags_ & keyIE_) != 0) raise(INTEVT_KEYSC, LEVEL_KEYSC);
}

// ---------------------------------------------------------------------------
// RTC
// ---------------------------------------------------------------------------
uint64_t CG100MMIO::rtcPeriodInstr() const {
    switch ((rcr2_ >> 4) & 7) {
    case 1: return instrPerSec_ / 256;
    case 2: return instrPerSec_ / 64;
    case 3: return instrPerSec_ / 16;
    case 4: return instrPerSec_ / 4;
    case 5: return instrPerSec_ / 2;
    case 6: return instrPerSec_;
    case 7: return instrPerSec_ * 2;
    default: return 0;
    }
}

void CG100MMIO::rtcAlignPeriodic() {
    const uint64_t p = rtcPeriodInstr();
    if (p == 0) { nextPeriodic_ = 0; return; }
    const uint64_t now = cpu_ ? cpu_->cycles : 0;
    nextPeriodic_ = (now / p + 1) * p;
}

void CG100MMIO::rtcTick() {
    if ((rcr2_ & 0x70) == 0) return;
    const uint64_t now = cpu_ ? cpu_->cycles : 0;
    if (now >= nextPeriodic_) {
        const uint64_t p = rtcPeriodInstr();
        nextPeriodic_ += p ? p : 1;
        rcr2_ |= 0x80; // PEF
        raise(INTEVT_RTC, LEVEL_RTC);
    }
}

uint32_t CG100MMIO::rtcRead(uint32_t off, uint32_t size) {
    (void)size;
    const uint64_t now = cpu_ ? cpu_->cycles : 0;
    const uint64_t sec = epochSec_ + now / instrPerSec_;
    switch (off) {
    case 0x00: return static_cast<uint32_t>(((now % instrPerSec_) * 128 / instrPerSec_) & 0x7F);
    case 0x02: return bcd8(sec % 60);
    case 0x04: return bcd8(sec / 60 % 60);
    case 0x06: return bcd8(sec / 3600 % 24);
    case 0x08: return static_cast<uint32_t>((sec / 86400 + 5) % 7); // 2010-01-01 = Friday
    case 0x0A: case 0x0C: case 0x0E: {
        uint64_t days = sec / 86400;
        uint64_t year = 2010;
        for (;;) {
            const uint64_t ylen = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
            if (days < ylen) break;
            days -= ylen; ++year;
        }
        const uint64_t mlen[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
        uint64_t mo = 0;
        while (mo < 11 && days >= mlen[mo]) { days -= mlen[mo]; ++mo; }
        if (off == 0x0A) return bcd8(days + 1);
        if (off == 0x0C) return bcd8(mo + 1);
        return (bcd8(year / 100) << 8) | bcd8(year % 100);
    }
    case 0x1C: return rcr1_;
    case 0x1E: return rcr2_;
    }
    return regRead(RTC_BASE + off);
}

// ---------------------------------------------------------------------------
// ETMU2: a free-running 32 kHz counter; legacy reads derive from cycles.
// ---------------------------------------------------------------------------
void CG100MMIO::etmuTick() {
    etmuCycles_ += 1;
    // ETMU channels used by the OS/timers are driven by the periodic tick above
    // and the free counter; no extra interrupts are fabricated here.
}

uint32_t CG100MMIO::etmuRead(uint32_t off) {
    const uint64_t now = cpu_ ? cpu_->cycles : 0;
    if (off == 0xC8 || off == 0xD8) {
        // Legacy free counter: 4 instructions per tick, as seen by the CG100
        // OS 2.00 power-off delay loops.
        return static_cast<uint32_t>(-(now >> 2)) & 0xFFFFFF;
    }
    return regRead(0xA44D0000u + off);
}

// ---------------------------------------------------------------------------
// LCD R61524
// ---------------------------------------------------------------------------
bool CG100MMIO::lcdRS() const { return (regRead(0xA405013Cu) & 0x10u) != 0; }

bool CG100MMIO::lcdLit() const {
    if ((lcdReg_[0x007] & 0x0100) == 0) return false;
    const auto it = regs_.find(0xA405012Eu);
    if (it != regs_.end() && (it->second & 0x20) == 0) return false;
    return true;
}

void CG100MMIO::lcdWindow(uint32_t& hsa, uint32_t& hea, uint32_t& vsa, uint32_t& vea) const {
    hsa = lcdReg_[0x210] & 0x1FF;
    hea = lcdReg_[0x211] & 0x1FF;
    vsa = lcdReg_[0x212] & 0x1FF;
    vea = lcdReg_[0x213] & 0x1FF;
}

void CG100MMIO::lcdSetAddress() {
    const uint32_t h = lcdReg_[0x200] & 0x1FF, v = lcdReg_[0x201] & 0x1FF;
    const uint16_t em = lcdReg_[0x003];
    if ((em & 0x80) == 0) { lcdH_ = h; lcdV_ = v; return; }
    uint32_t hsa, hea, vsa, vea;
    lcdWindow(hsa, hea, vsa, vea);
    lcdH_ = (em & 0x10) ? hsa + h : hea - h;
    lcdV_ = (em & 0x20) ? vsa + v : vea - v;
}

void CG100MMIO::lcdPixel(uint16_t c) {
    if (lcdH_ < PANEL_W && lcdV_ < PANEL_H) {
        gram_[lcdV_ * PANEL_W + lcdH_] = c;
        ++frameGen_;
    }
    const uint16_t em = lcdReg_[0x003];
    uint32_t hsa, hea, vsa, vea;
    lcdWindow(hsa, hea, vsa, vea);
    auto stepH = [&]() -> bool {
        if (em & 0x10) { if (lcdH_ >= hea) { lcdH_ = hsa; return true; } ++lcdH_; }
        else { if (lcdH_ <= hsa) { lcdH_ = hea; return true; } --lcdH_; }
        return false;
    };
    auto stepV = [&]() -> bool {
        if (em & 0x20) { if (lcdV_ >= vea) { lcdV_ = vsa; return true; } ++lcdV_; }
        else { if (lcdV_ <= vsa) { lcdV_ = vea; return true; } --lcdV_; }
        return false;
    };
    if ((em & 0x08) == 0) { if (stepH()) stepV(); }
    else if (stepV()) stepH();
}

void CG100MMIO::lcdData(uint16_t v) {
    if (lcdIdx_ == 0x202) { lcdPixel(v); return; }
    if (lcdIdx_ == 0x200 || lcdIdx_ == 0x201) {
        lcdReg_[lcdIdx_] = v;
        lcdSetAddress();
        return;
    }
    if (lcdIdx_ < lcdReg_.size()) lcdReg_[lcdIdx_] = v;
}

void CG100MMIO::lcdDma(const std::vector<uint8_t>& src) {
    for (size_t i = 0; i + 1 < src.size(); i += 2)
        lcdData(static_cast<uint16_t>((src[i] << 8) | src[i + 1]));
}

// ---------------------------------------------------------------------------
// DMAC
// ---------------------------------------------------------------------------
uint32_t CG100MMIO::dmaUnit(uint32_t chcr) {
    switch (((chcr >> 3) & 3) | ((chcr >> 20 & 3) << 2)) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    case 3: return 16;
    default: return 32;
    }
}

void CG100MMIO::doDmaTransfer(uint32_t ch) {
    if (!cpu_) return;
    DmaChan& c = dma_[ch];
    const uint32_t unit = dmaUnit(c.chcr);
    uint32_t count = c.tcr & 0xFFFFFF;
    if (count == 0) count = 1u << 24;
    uint32_t n = std::min<uint64_t>(static_cast<uint64_t>(count) * unit, 1u << 20);
    const bool fixed = ((c.chcr >> 12) & 3) == 0;

    if ((c.dar & 0x1FFFFFFFu) == 0x14000000u) { // VRAM -> LCD push
        ++pushes_;
        if (fixed) { // a colour fill: repeat the 32-byte source unit
            const std::vector<uint8_t> unitBytes = cpu_->mem.span(c.sar, unit);
            for (uint32_t i = 0; i < n; i += static_cast<uint32_t>(unitBytes.size()))
                lcdDma(unitBytes);
        } else {
            lcdDma(cpu_->mem.span(c.sar, n));
        }
    } else {
        // memory-to-memory transfer (VRAM clears/copies), capped at 4 MB.
        uint32_t cnt = std::min<uint64_t>(count, (1u << 22) / unit);
        const uint32_t sm = (c.chcr >> 12) & 3, dm = (c.chcr >> 14) & 3;
        uint32_t sar = c.sar, dar = c.dar;
        std::vector<uint8_t> buf(unit);
        for (uint32_t i = 0; i < cnt; ++i) {
            for (uint32_t j = 0; j < unit; ++j)
                buf[j] = static_cast<uint8_t>(cpu_->mem.read(0xA0000000u | ((sar + j) & 0x1FFFFFFFu), 1));
            for (uint32_t j = 0; j < unit; ++j)
                cpu_->mem.write(0xA0000000u | ((dar + j) & 0x1FFFFFFFu), 1, buf[j]);
            if (sm == 1) sar += unit; else if (sm == 2) sar -= unit;
            if (dm == 1) dar += unit; else if (dm == 2) dar -= unit;
        }
    }
    const uint32_t intevt[6] = {0x800,0x820,0x840,0x860,0xB80,0xBA0};
    if ((c.chcr & 4) != 0) raise(intevt[ch % 6], 0);
}

void CG100MMIO::dmaWrite(uint32_t off, uint32_t val) {
    regs_[0xFE008000u + off] = val;
    int ch = -1;
    if (off >= 0x20 && off < 0x60) ch = static_cast<int>((off - 0x20) >> 4);
    else if (off == 0x70) ch = 4;
    else if (off == 0x80) ch = 5;
    if (ch < 0) return;
    const uint32_t co = static_cast<uint32_t>(ch) * 16 + 0x20; // canonical CHCR offset
    DmaChan& c = dma_[ch];
    c.sar = regRead(0xFE008000u + co + 0);
    c.dar = regRead(0xFE008000u + co + 4);
    c.tcr = regRead(0xFE008000u + co + 8);
    if ((off & 0xF) != 0xC) return;
    if ((val & 1) == 0 || (val & 2) != 0) return; // DE=1 and TE=0 starts a transfer
    const uint32_t u = dmaUnit(val);
    if ((c.sar % u) || (c.dar % u)) { regs_[0xFE008060u] |= 0x4; return; } // DMAOR.AE
    regRead(0xFE008000u + co + 0); // keep SAR read pattern symmetrical
    regs_[0xFE008000u + off] = val | 0x2; // TE: completes instantly
    doDmaTransfer(ch);
}

// ---------------------------------------------------------------------------
// Bus dispatch
// ---------------------------------------------------------------------------
uint32_t CG100MMIO::read(uint32_t va, uint32_t size) {
    const RegDesc* d = nullptr;
    int id = -1;
    uint32_t hit = 0;
    if (!resolve(va, d, id, hit)) return 0;
    const uint32_t off = hit - d->base;

    switch (id) {
    case R_CCN:
        if (off == 0x24) return expevt_ ? expevt_ : 0x0A02u; // model strap
        if (off == 0x30) return PVR;
        if (off == 0x44) return PRR;
        return regRead(hit);
    case R_CPG:
        if (off == 0x60) return 0; // PLL ready
        if (off == 0x00) return regRead(hit) & ~0x80000000u; // KICK reads 0
        return regRead(hit);
    case R_ETMU:
        if (off == 0x60) return 0x8000u; // one-shot elapsed
        return regRead(hit);
    case R_FRC: {
        ++freeCount_;
        const uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1);
        return freeCount_ & mask;
    }
    case R_INTC:
        if (off < 0x30 && (off & 3) == 0) return ipr_[off / 4];
        if (off >= 0x80 && off < 0xB4 && (off & 3) == 0) return imr_[(off - 0x80) / 4];
        if (off >= 0xC0 && off < 0xF4) return 0;
        return regRead(hit);
    case R_KEYSC:
        if (off < 0x0C) {
            const uint32_t w = keyData(off >> 1);
            if (size == 2) return w;
            if (size == 1) return (off & 1) ? (w & 0xFF) : (w >> 8);
            const uint32_t lo = ((off >> 1) + 1 < 6) ? keyData((off >> 1) + 1) : 0;
            return (w << 16) | lo;
        }
        if (off == 0x0C) return keyCtrl_;
        if (off == 0x10) return keyMode_;
        if (off == 0x12) return 2;
        if (off == 0x14) return (keyIE_ << 8) | keyFlags_;
        return regRead(hit);
    case R_RTC:
        return rtcRead(off, size);
    case R_ETMU2:
        return etmuRead(off);
    case R_TMU:
        if (off == 0x04) return 0; // no channels running
        return regRead(hit);
    case R_PERIPH:
        if (off == 0x82 || off == 0x84) return ADC_BATTERY_RAW;
        if (off >= 0x88 && off < 0x8C) return regRead(0xA4610000u + 0x88);
        return regRead(hit);
    case R_LCD:
        if (!lcdRS()) return lcdIdx_;
        if (lcdIdx_ == 0x202) return 0; // GRAM read-back not modelled
        if (lcdIdx_ < lcdReg_.size()) return lcdReg_[lcdIdx_];
        return 0;
    case R_INTX:
        return regRead(hit);
    default:
        return regRead(hit);
    }
}

void CG100MMIO::write(uint32_t va, uint32_t size, uint32_t val) {
    const RegDesc* d = nullptr;
    int id = -1;
    uint32_t hit = 0;
    if (!resolve(va, d, id, hit)) return;
    const uint32_t off = hit - d->base;

    switch (id) {
    case R_CCN:
        if (off == 0x24) expevt_ = val;
        else if (off == 0x10) { /* MMUCR: MMU not modelled */ regs_[hit] = val & ~4u; }
        else regs_[hit] = val;
        return;
    case R_INTC:
        if (off < 0x30 && (off & 3) == 0) ipr_[off / 4] = static_cast<uint16_t>(val);
        else if (off >= 0x80 && off < 0xB4 && (off & 3) == 0) imr_[(off - 0x80) / 4] |= static_cast<uint8_t>(val);
        else if (off >= 0xC0 && off < 0xF4 && (off & 3) == 0) imr_[(off - 0xC0) / 4] &= ~static_cast<uint8_t>(val);
        else regs_[hit] = val;
        return;
    case R_KEYSC:
        if (off == 0x0C) keyCtrl_ = val & 0xFFFF;
        else if (off == 0x10) keyMode_ = val & 0xFFFF;
        else if (off == 0x14) { keyIE_ = (val >> 8) & 0xFF; keyFlags_ &= ~(val & 0xFF); }
        else regs_[hit] = val;
        return;
    case R_RTC:
        if (off == 0x1C) rcr1_ = val & 0xFF;
        else if (off == 0x1E) {
            const uint32_t prev = (rcr2_ >> 4) & 7;
            rcr2_ = val & 0xFF;
            const uint32_t pes = (rcr2_ >> 4) & 7;
            if (pes != 0 && (pes != prev || nextPeriodic_ == 0)) rtcAlignPeriodic();
        } else regs_[hit] = val;
        return;
    case R_DMAC:
        dmaWrite(off, val);
        return;
    case R_LCD:
        if (!lcdRS()) lcdIdx_ = val & 0x7FF;
        else lcdData(static_cast<uint16_t>(val));
        return;
    case R_INTX:
        if (off == 0x24 && size == 1) { regs_[hit] &= val; return; } // write-0-to-clear
        regs_[hit] = val;
        return;
    case R_BCD: {
        const uint32_t o = off & 0xF;
        if (o == 0x4) { regs_[0xA4CB0004u] = val; }
        else if (o == 0x8) { regs_[0xA4CB0008u] = val; }
        else if (o == 0x0) { /* command: compute is handled in read of result */ regs_[hit] = val & 7; }
        else regs_[hit] = val;
        return;
    }
    case R_PERIPH:
        if (off >= 0x88 && off < 0x8C) regs_[0xA4610000u + 0x88] = val & ~0x2000u;
        else regs_[hit] = val;
        return;
    default:
        regs_[hit] = val;
        return;
    }
}

void CG100MMIO::tick(uint64_t cycles, uint64_t /*idle*/) {
    // KEYSC scan.
    if (cycles >= scanNext_) {
        scanNext_ = cycles + scanPeriod_;
        scan();
    }
    if (timerPeriod_ == 0) return; // pure-boot mode: no interrupt sources
    rtcTick();
    etmuTick();
    // Legacy periodic tick (INTEVT 0x560) used during boot.
    if (cycles >= timerNext_) {
        timerNext_ = cycles + timerPeriod_;
        regs_[0xA4610000u + 0x88] |= 0xC000;
        raise(INTEVT_TIMER, LEVEL_TIMER);
    }
}

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------
void CG100MMIO::renderPanelBE565(uint16_t* dst) const {
    const uint16_t fill = lcdLit() ? 0xFFFFu : 0x0000u;
    for (uint32_t y = 0; y < PANEL_H; ++y) {
        for (uint32_t x = 0; x < PANEL_W; ++x) {
            const uint32_t source = PANEL_W - 1 - x; // horizontal flip
            dst[y * PANEL_W + x] = gram_[y * PANEL_W + source];
        }
    }
    (void)fill;
}

void CG100MMIO::renderOSBE565(uint16_t* dst) const {
    if (!lcdLit()) { std::fill(dst, dst + OS_W * OS_H, 0); return; }
    constexpr uint32_t OS_ORIGIN_H = 389;
    for (uint32_t y = 0; y < OS_H; ++y) {
        for (uint32_t x = 0; x < OS_W; ++x)
            dst[y * OS_W + x] = gram_[y * PANEL_W + (OS_ORIGIN_H - x)];
    }
}

void CG100MMIO::renderPanelRGBA(uint8_t* dst) const {
    if (!lcdLit()) {
        std::fill(dst, dst + PANEL_W * PANEL_H * 4, 0);
        for (uint32_t i = 3; i < PANEL_W * PANEL_H * 4; i += 4) dst[i] = 0xFF;
        return;
    }
    for (uint32_t i = 0; i < PANEL_W * PANEL_H; ++i) {
        const uint16_t p = gram_[i]; // GRAM is stored in panel-native order
        dst[i * 4 + 0] = static_cast<uint8_t>(((p >> 11) & 0x1F) << 3);
        dst[i * 4 + 1] = static_cast<uint8_t>(((p >> 5) & 0x3F) << 2);
        dst[i * 4 + 2] = static_cast<uint8_t>((p & 0x1F) << 3);
        dst[i * 4 + 3] = 0xFF;
    }
}

bool CG100MMIO::screenOn() const { return lcdLit(); }

void CG100MMIO::restoreFromSnapshot(uint32_t expevt, uint32_t rcr2,
                                    uint32_t keyCtrl, uint32_t keyMode,
                                    uint32_t keyIE, uint32_t keyFlags) {
    expevt_ = expevt;
    rcr2_ = rcr2;
    keyCtrl_ = keyCtrl;
    keyMode_ = keyMode;
    keyIE_ = keyIE;
    keyFlags_ = keyFlags;
    if ((rcr2_ & 0x70) != 0) rtcAlignPeriodic();
}

} // namespace cg100
