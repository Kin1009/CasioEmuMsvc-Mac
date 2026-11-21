#pragma once
// native_core.h -- adapter that lets the skeleton IEmulatorCore glue in
// src/plugin.cpp drive the REAL interpreter in sh4_interp.cpp / cg100_mmio.cpp.
//
// The skeleton plugin.cpp includes "sh4_cpu.h" and "mmio.h" and uses the
// cg100::Bus / cg100::Sh4Cpu names.  Including THIS header instead gives those
// same names a full implementation backed by cg100::Sh4CPU, cg100::Memory and
// cg100::CG100MMIO.  No host / Go code is involved.

#include "sh4_interp.h"
#include "cg100_mmio.h"

#include <cstring>
#include <vector>

namespace cg100 {

// POD register snapshot, layout-compatible with the skeleton's Sh4State so the
// existing save-state code keeps working.
struct Sh4State {
    uint32_t r[16]{};
    uint32_t rbank1[8]{};
    uint32_t pc{0}, pr{0}, gbr{0}, vbr{0};
    uint32_t ssr{0}, spc{0}, sgr{0}, dbr{0};
    uint32_t mach{0}, macl{0}, fpul{0}, fpscr{0};
    uint32_t sr{0};
    uint64_t cycles{0};
    bool sleeping{false}, inSlot{false};
    uint32_t slotPC{0};
};

namespace detail {
class KeyScProxy {
public:
    explicit KeyScProxy(CG100MMIO& m) : m_(m) {}
    void press(uint32_t row, uint32_t col) { m_.keyPress(row, col); }
    void release(uint32_t row, uint32_t col) { m_.keyRelease(row, col); }
    void releaseAll() { m_.keyReleaseAll(); }
    void setScanPeriod(uint64_t v) { m_.setKeyScanPeriod(v); }
    uint64_t scanPeriod() const { return m_.scanPeriod(); }
private:
    CG100MMIO& m_;
};
} // namespace detail

// Full memory + peripheral bus.  Construction order: mmio first, then mem
// (which receives &mmio), matching the member declaration order below.
class Bus {
public:
    Bus() : mmio_(), mem_(&mmio_), fb_(CG100MMIO::PANEL_W * CG100MMIO::PANEL_H, 0) {}

    bool loadFlash(const uint8_t* data, size_t len) {
        if (!data || len > Memory::FLASH_MUT_TOP) return false;
        mem_.loadFlash(data, len);
        return true;
    }
    size_t flashSize() const { return mem_.flash.size(); }

    uint32_t read(uint32_t va, int size) { return mem_.read(va, static_cast<uint32_t>(size)); }
    void write(uint32_t va, int size, uint32_t value) { mem_.write(va, static_cast<uint32_t>(size), value); }
    uint16_t fetch16(uint32_t pc) { return mem_.fetch16(pc); }

    // Big-endian RGB565 of the whole 396x224 panel.  The returned pointer stays
    // valid until the next framebuffer() call.
    const uint16_t* framebuffer() {
        mmio_.renderPanelBE565(fb_.data());
        return fb_.data();
    }

    detail::KeyScProxy keysc() { return detail::KeyScProxy(mmio_); }

    void tickDevices(uint64_t cpuCycles, uint64_t ips) {
        if (ips != mmio_.instrPerSecond()) mmio_.setInstrPerSecond(ips);
        mmio_.tick(cpuCycles, 0);
    }
    uint32_t mmioWriteCount() const { return mmioWrites_; }
    uint32_t unmappedCount() const { return mem_.unmappedFault ? 1u : 0u; }

    // Minimal but complete memory snapshot (flash + on-chip RAMs + key MMIO).
    void serialize(std::vector<uint8_t>& out) const {
        putVec(out, mem_.flash);
        putVec(out, mem_.dram);
        putVec(out, mem_.ilram);
        putVec(out, mem_.ocram);
        putVec(out, mem_.xyram);
        putPod(out, mmio_.expevt());
        putPod(out, mmio_.rcr2());
        putPod(out, mmio_.keyCtrl());
        putPod(out, mmio_.keyMode());
        putPod(out, mmio_.keyIE());
        putPod(out, mmio_.keyFlags());
        for (uint16_t g : mmio_.gramRaw()) putPod(out, g);
    }
    bool deserialize(const uint8_t* data, size_t len) {
        size_t off = 0;
        if (!getVec(data, len, off, mem_.flash)) return false;
        if (!getVec(data, len, off, mem_.dram)) return false;
        if (!getVec(data, len, off, mem_.ilram)) return false;
        if (!getVec(data, len, off, mem_.ocram)) return false;
        if (!getVec(data, len, off, mem_.xyram)) return false;
        uint32_t expevt = 0, rcr2 = 0, keyCtrl = 0, keyMode = 0, keyIE = 0, keyFlags = 0;
        if (!getPod(data, len, off, expevt)) return false;
        if (!getPod(data, len, off, rcr2)) return false;
        if (!getPod(data, len, off, keyCtrl)) return false;
        if (!getPod(data, len, off, keyMode)) return false;
        if (!getPod(data, len, off, keyIE)) return false;
        if (!getPod(data, len, off, keyFlags)) return false;
        auto& g = mmio_.gramRaw();
        for (size_t i = 0; i < g.size(); ++i) if (!getPod(data, len, off, g[i])) return false;
        mmio_.restoreFromSnapshot(expevt, rcr2, keyCtrl, keyMode, keyIE, keyFlags);
        return true;
    }

    Memory& memory() { return mem_; }
    CG100MMIO& mmio() { return mmio_; }

private:
    static void putPod(std::vector<uint8_t>& o, const uint8_t& v) { o.push_back(v); }
    template <class T> static void putPod(std::vector<uint8_t>& o, const T& v) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        o.insert(o.end(), p, p + sizeof(T));
    }
    static void putVec(std::vector<uint8_t>& o, const std::vector<uint8_t>& v) {
        const uint32_t n = static_cast<uint32_t>(v.size());
        putPod(o, n);
        o.insert(o.end(), v.begin(), v.end());
    }
    template <class T> static bool getPod(const uint8_t* d, size_t len, size_t& off, T& v) {
        if (off > len || len - off < sizeof(T)) return false;
        std::memcpy(&v, d + off, sizeof(T));
        off += sizeof(T);
        return true;
    }
    static bool getVec(const uint8_t* d, size_t len, size_t& off, std::vector<uint8_t>& v) {
        uint32_t n = 0;
        if (!getPod(d, len, off, n)) return false;
        if (off > len || len - off < n) return false;
        v.assign(d + off, d + off + n);
        off += n;
        return true;
    }

    CG100MMIO mmio_;
    Memory mem_;
    mutable std::vector<uint16_t> fb_;
    uint32_t mmioWrites_{0};
};

class Sh4Cpu {
public:
    explicit Sh4Cpu(Bus& bus) : bus_(bus), cpu_(bus.memory()) {
        bus_.mmio().attachCPU(&cpu_);
    }

    void reset() {
        cpu_.hardwareReset();
        bus_.mmio().setInstrPerSecond(ips_);
    }
    void step() { cpu_.step(); }
    void run(uint64_t cycles) { cpu_.run(cycles); }
    void raiseIrq(uint32_t intevt, uint32_t level) { cpu_.raiseIRQ(intevt, level); }
    void setIps(uint64_t ips) { ips_ = ips; bus_.mmio().setInstrPerSecond(ips); }
    uint64_t ips() const { return ips_; }

    Sh4State state() const {
        Sh4State s{};
        std::memcpy(s.r, cpu_.r, sizeof(s.r));
        std::memcpy(s.rbank1, cpu_.rbank1, sizeof(s.rbank1));
        s.pc = cpu_.pc; s.pr = cpu_.pr; s.gbr = cpu_.gbr; s.vbr = cpu_.vbr;
        s.ssr = cpu_.ssr; s.spc = cpu_.spc; s.sgr = cpu_.sgr; s.dbr = cpu_.dbr;
        s.mach = cpu_.mach; s.macl = cpu_.macl; s.fpul = cpu_.fpul;
        s.sr = cpu_.sr; s.cycles = cpu_.cycles;
        s.sleeping = cpu_.sleeping; s.inSlot = cpu_.inSlot; s.slotPC = cpu_.slotPC;
        return s;
    }
    void setState(const Sh4State& s) {
        std::memcpy(cpu_.r, s.r, sizeof(cpu_.r));
        std::memcpy(cpu_.rbank1, s.rbank1, sizeof(cpu_.rbank1));
        cpu_.pc = s.pc; cpu_.pr = s.pr; cpu_.gbr = s.gbr; cpu_.vbr = s.vbr;
        cpu_.ssr = s.ssr; cpu_.spc = s.spc; cpu_.sgr = s.sgr; cpu_.dbr = s.dbr;
        cpu_.mach = s.mach; cpu_.macl = s.macl; cpu_.fpul = s.fpul;
        cpu_.sr = s.sr; cpu_.cycles = s.cycles;
        cpu_.sleeping = s.sleeping; cpu_.inSlot = s.inSlot; cpu_.slotPC = s.slotPC;
    }

    Sh4CPU& real() { return cpu_; }
    const Sh4CPU& real() const { return cpu_; }

private:
    Bus& bus_;
    Sh4CPU cpu_;
    uint64_t ips_{200000000ull};
};

} // namespace cg100
