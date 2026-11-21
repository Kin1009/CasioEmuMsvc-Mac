#pragma once
// cg100_mmio.h -- SH7305 MMIO bus for CG100 (KEYSC, PFC, INTC, timers, RTC,
// DMAC, LCD R61524, CCN, CPG, and the assorted stubs needed at boot).
//
// Behavioural port of emu_go/mmio.go, keysc.go, rtc.go and lcd.go.  Implements
// cg100::MmioIf from sh4_interp.h.  See CPU_MMIO_STATUS.md for coverage.

#include "sh4_interp.h"

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace cg100 {

class CG100MMIO : public MmioIf {
public:
    // Frame geometry: the whole LCD panel (the OS renders into the middle
    // 384x216 of it).  Matches the Go panelW/panelH = 396/224.
    static constexpr uint32_t PANEL_W = 396;
    static constexpr uint32_t PANEL_H = 224;
    static constexpr uint32_t OS_W = 384;
    static constexpr uint32_t OS_H = 216;

    // Interrupt codes / levels (mmio.go).
    static constexpr uint32_t INTEVT_TIMER = 0x560;
    static constexpr uint32_t INTEVT_RTC = 0xAA0;
    static constexpr uint32_t INTEVT_KEYSC = 0xBE0;
    static constexpr uint32_t LEVEL_TIMER = 8;
    static constexpr uint32_t LEVEL_RTC = 9;
    static constexpr uint32_t LEVEL_KEYSC = 13;

    explicit CG100MMIO(Sh4CPU* cpu = nullptr);
    void attachCPU(Sh4CPU* cpu) { cpu_ = cpu; }

    // --- MmioIf -------------------------------------------------------------
    uint32_t read(uint32_t va, uint32_t size) override;
    void write(uint32_t va, uint32_t size, uint32_t val) override;
    bool irqGate(uint32_t intevt, uint32_t& prio, bool& masked) override;
    void tick(uint64_t cycles, uint64_t idle) override;

    // --- host configuration -------------------------------------------------
    void setInstrPerSecond(uint64_t ips);
    void setKeyScanPeriod(uint64_t instr);
    uint64_t scanPeriod() const { return scanPeriod_; }
    uint64_t instrPerSecond() const { return instrPerSec_; }

    // Snapshot support for the adapter in native_core.h.
    std::vector<uint16_t>& gramRaw() { return gram_; }
    const std::vector<uint16_t>& gramRaw() const { return gram_; }
    void restoreFromSnapshot(uint32_t expevt, uint32_t rcr2,
                             uint32_t keyCtrl, uint32_t keyMode,
                             uint32_t keyIE, uint32_t keyFlags);
    uint32_t expevt() const { return expevt_; }
    uint32_t rcr2() const { return rcr2_; }
    uint32_t keyCtrl() const { return keyCtrl_; }
    uint32_t keyMode() const { return keyMode_; }
    uint32_t keyIE() const { return keyIE_; }

    // --- keyboard (hardware matrix) ----------------------------------------
    void keyPress(uint32_t row, uint32_t col);
    void keyRelease(uint32_t row, uint32_t col);
    void keyReleaseAll();
    void keyMatrixLevels(const bool levels[8][12]); // set the whole matrix from row/col levels

    // --- framebuffer --------------------------------------------------------
    // Big-endian RGB565 of the whole 396x224 panel (matches Go renderPanel).
    void renderPanelBE565(uint16_t* dst) const;
    // 384x216 big-endian RGB565 of the OS drawing area (matches renderOS).
    void renderOSBE565(uint16_t* dst) const;
    // Convenience: unpack the panel into 8-bit RGBA (w*h*4 bytes).
    void renderPanelRGBA(uint8_t* dst) const;
    bool screenOn() const;

    // Diagnostics exposed for tests / status text.
    uint64_t pushes() const { return pushes_; }
    uint32_t lastIntevt() const { return lastIntevt_; }
    uint32_t keyFlags() const { return keyFlags_; }

private:
    Sh4CPU* cpu_ = nullptr;

    // Generic registers keyed by absolute MMIO address (base regions).
    std::unordered_map<uint32_t, uint32_t> regs_;
    uint32_t regRead(uint32_t va, uint32_t def = 0) const;

    // CCN (0xFF000000): EXPEVT strap, PVR/PRR, MMUCR.
    uint32_t expevt_ = 0;
    static constexpr uint32_t PVR = 0x10300B00u;
    static constexpr uint32_t PRR = 0x00002C00u;

    // INTC (0xA4080000).
    std::array<uint16_t, 12> ipr_{};
    std::array<uint8_t, 13> imr_{};

    // KEYSC (0xA44B0000).
    std::array<uint16_t, 6> keyHeld_{};
    uint32_t keyCtrl_ = 0, keyMode_ = 0, keyIE_ = 0, keyFlags_ = 0;
    uint64_t scanPeriod_ = 750000;   // 33 Hz at ~25M instr/s
    uint64_t scanNext_ = 0;
    int scanLeft_ = 0;
    bool keyWasHeld_ = false;

    // RTC (0xA413FEC0).
    uint32_t rcr1_ = 0, rcr2_ = 0;
    uint64_t epochSec_ = 0;          // seconds at cycle 0 (2010-01-01 epoch)
    uint64_t nextPeriodic_ = 0;

    // Free counter (0xA4130000) and battery ADC (0xA4610000).
    uint32_t freeCount_ = 0;
    static constexpr uint32_t ADC_BATTERY_RAW = 0x7140u;

    // Periodic tick (legacy timer, INTEVT 0x560) used during boot.
    uint64_t timerPeriod_ = 30000;
    uint64_t timerNext_ = 0;

    // ETMU2 (0xA44D0000) 32 kHz counter and ETMU (0xA44A0000) stubs.
    uint64_t etmuCycles_ = 0;
    uint32_t etmuDiv_ = 1;

    // DMAC (0xFE008000): channel registers for the LCD push.
    struct DmaChan { uint32_t sar = 0, dar = 0, tcr = 0, chcr = 0; };
    std::array<DmaChan, 6> dma_{};

    // LCD R61524 (0xB4000000 / P0 alias 0x14000000).
    uint32_t lcdIdx_ = 0;
    std::array<uint16_t, 0x800> lcdReg_{};
    uint32_t lcdH_ = 0, lcdV_ = 0;
    std::vector<uint16_t> gram_;
    uint64_t frameGen_ = 0;

    uint64_t instrPerSec_ = 70000000;
    uint64_t pushes_ = 0;
    uint32_t lastIntevt_ = 0;

    // Helpers.
    void raise(uint32_t intevt, uint32_t level);
    bool anyHeld() const;
    bool anyEnabled() const;
    uint32_t keyData(uint32_t w) const;
    void scan();
    void initRegions();

    uint64_t rtcPeriodInstr() const;
    void rtcAlignPeriodic();
    void rtcTick();
    uint32_t rtcRead(uint32_t off, uint32_t size);
    void etmuTick();
    uint32_t etmuRead(uint32_t off);

    bool lcdRS() const;
    void lcdData(uint16_t v);
    void lcdPixel(uint16_t c);
    void lcdSetAddress();
    void lcdWindow(uint32_t& hsa, uint32_t& hea, uint32_t& vsa, uint32_t& vea) const;
    void lcdDma(const std::vector<uint8_t>& src);
    bool lcdLit() const;

    static uint32_t dmaUnit(uint32_t chcr);
    void dmaWrite(uint32_t off, uint32_t val);
    void doDmaTransfer(uint32_t ch);

    // INTC source map: INTEVT -> {ipr index, shift, imr index, bit, level}.
    struct Src { int ipr, shift, imr; uint8_t bit; uint32_t level; };
    static bool source(uint32_t intevt, Src& out);
};

} // namespace cg100
