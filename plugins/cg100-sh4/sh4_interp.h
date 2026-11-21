#pragma once
// sh4_interp.h -- SH-4 / SH-4AL-DSP big-endian CPU interpreter + physical memory.
//
// Behavioural port of the authoritative Go reference core
// (/Volumes/Code/cg100-os/emu_go/cpu.go and memory.go).  This is the NEW C++
// plugin codebase: it deliberately shares no code with the old Go plugin or
// the host tree.  See CPU_MMIO_STATUS.md for coverage and integration notes.
//
// Namespace `cg100` is used throughout so these classes can live alongside
// the skeleton sh4_cpu.* / mmio.* files without symbol collisions.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cg100 {

// ---------------------------------------------------------------------------
// SR bit masks (SH-4)
// ---------------------------------------------------------------------------
enum : uint32_t {
    SR_T  = 0x00000001u,
    SR_S  = 0x00000002u,
    SR_Q  = 0x00000100u,
    SR_M  = 0x00000200u,
    SR_FD = 0x00008000u,
    SR_BL = 0x10000000u,
    SR_RB = 0x20000000u,
    SR_MD = 0x40000000u,
    SR_IMASK = 0x000000F0u,
};

inline int32_t s8(uint32_t x)  { return static_cast<int32_t>(static_cast<int8_t>(x)); }
inline int32_t s16(uint32_t x) { return static_cast<int32_t>(static_cast<int16_t>(x)); }
inline int32_t s32(uint32_t x) { return static_cast<int32_t>(x); }

// ---------------------------------------------------------------------------
// Abstract MMIO bus.  CG100MMIO implements this; the CPU/memory only need this
// interface, which keeps the two translation units independent.
// ---------------------------------------------------------------------------
struct MmioIf {
    virtual ~MmioIf() = default;
    virtual uint32_t read(uint32_t va, uint32_t size) = 0;
    virtual void write(uint32_t va, uint32_t size, uint32_t val) = 0;
    // INTC gate for interrupt acceptance.  Returns true if `intevt` is a
    // source the INTC knows; sets prio / masked accordingly (see intc.go).
    virtual bool irqGate(uint32_t /*intevt*/, uint32_t& prio, bool& masked) {
        prio = 0; masked = false; return false;
    }
    // Called once per instruction by the CPU (device clocks are cycle based).
    virtual void tick(uint64_t /*cycles*/, uint64_t /*idle*/) {}
};

// ---------------------------------------------------------------------------
// Physical memory: NOR flash, DRAM, ILRAM, OCRAM and X/Y RAM, all big-endian.
// Mirrors memory.go.  Address translation is the SH-4 P0/P1/P2 alias
// (phys = vaddr & 0x1FFFFFFF); MMIO is delegated to MmioIf.
// ---------------------------------------------------------------------------
class Memory {
public:
    static constexpr uint32_t FLASH_SIZE    = 0x08000000u; // NOR window; beyond image reads 0xFF
    static constexpr uint32_t FLASH_MUT_TOP = 0x02000000u; // 32 MB mutable array
    static constexpr uint32_t DRAM_BASE     = 0x0C000000u;
    static constexpr uint32_t DRAM_SIZE     = 0x00800000u; // 8 MB
    static constexpr uint32_t ILRAM_BASE    = 0xFD800000u;
    static constexpr uint32_t ILRAM_SIZE    = 0x00010000u;
    static constexpr uint32_t ILRAM_MASK    = 0x00003FFFu; // 16 KB mirrored over 64 KB
    static constexpr uint32_t OCRAM_BASE    = 0xFE200000u;
    static constexpr uint32_t OCRAM_SIZE    = 0x00200000u;
    static constexpr uint32_t XYRAM_BASE    = 0xE5000000u;
    static constexpr uint32_t XYRAM_SIZE    = 0x00210000u;
    static constexpr uint32_t FLASH_ALIAS   = 0x0B000000u; // CG100 second flash window

    explicit Memory(MmioIf* mmio = nullptr);

    // Fill flash from an OS image prefix; the rest of the mutable array is 0xFF.
    void loadFlash(const uint8_t* img, size_t n);

    uint32_t read(uint32_t va, uint32_t size);
    void write(uint32_t va, uint32_t size, uint32_t val);
    uint16_t fetch16(uint32_t pc);   // hot-path big-endian instruction fetch

    uint8_t  r8(uint32_t va)  { return static_cast<uint8_t>(read(va, 1)); }
    uint16_t r16(uint32_t va) { return static_cast<uint16_t>(read(va, 2)); }
    uint32_t r32(uint32_t va) { return read(va, 4); }
    void w8 (uint32_t va, uint32_t v) { write(va, 1, v); }
    void w16(uint32_t va, uint32_t v) { write(va, 2, v); }
    void w32(uint32_t va, uint32_t v) { write(va, 4, v); }

    // A direct view of a DRAM range (used by the LCD DMA model), capped to n.
    // Returns a COPY so callers cannot mutate emulator memory through it.
    std::vector<uint8_t> span(uint32_t va, uint32_t n);

    // Helpers for the boot bootstrap writes to ILRAM.
    void setIL32(uint32_t va, uint32_t v) { write(va, 4, v); }

    std::vector<uint8_t> flash;
    std::vector<uint8_t> dram;
    std::vector<uint8_t> ilram;
    std::vector<uint8_t> ocram;
    std::vector<uint8_t> xyram;
    MmioIf* mmio = nullptr;

    // Unmapped accesses are recorded here instead of throwing (the Go core
    // panics; the C++ host must never crash).  The CPU turns this into a fault.
    bool unmappedFault = false;
    uint32_t faultAddr = 0;

    // NOR command state machine (program / erase).  Only these mutate flash;
    // unlock/ID/CFI/reset commands only change state so code fetch stays valid.
    enum FlashCmd { F_IDLE, F_UNLOCK1, F_UNLOCK2, F_PROGRAM, F_ERASE1,
                    F_ERASE2, F_ERASE3, F_BUFCOUNT, F_BUFDATA };
    int fcmd = F_IDLE;
    int bufRem = 0;
    struct FWord { uint32_t addr, size, val; };
    std::vector<FWord> bufW;
    int eraseBusy = 0;
    uint32_t eraseSect = 0, eraseTog = 0;
    static constexpr uint32_t FLASH_SECTOR = 0x20000u;
    static constexpr int ERASE_STATUS_READS = 4;

private:
    void flashCmd(uint32_t phys, uint32_t size, uint32_t val);
    void flashProgram(uint32_t phys, uint32_t size, uint32_t val);
    uint32_t flashStatus(uint32_t size);
    static uint32_t beRead(const std::vector<uint8_t>& b, uint32_t off, uint32_t size);
    static void beWrite(std::vector<uint8_t>& b, uint32_t off, uint32_t size, uint32_t val);
};

// ---------------------------------------------------------------------------
// SH-4 CPU: registers, delay slots, exceptions, interrupts, HLE hooks.
// ---------------------------------------------------------------------------
class Sh4CPU {
public:
    explicit Sh4CPU(Memory& mem) : mem(mem) {
        // Reset state from NewCPU(): privileged, bank 1, interrupts blocked.
        sr = SR_MD | SR_RB | SR_BL | 0xF0u;
        pc = 0x80000000u;
    }

    Memory& mem;
    uint32_t r[16]{};
    uint32_t rbank1[8]{};
    uint32_t pc = 0, pr = 0, gbr = 0, vbr = 0;
    uint32_t ssr = 0, spc = 0, sgr = 0, dbr = 0;
    uint32_t mach = 0, macl = 0, fpul = 0;
    uint32_t sr = 0;
    uint64_t cycles = 0, idle = 0;
    uint64_t irqCnt = 0;

    // SH4AL-DSP system registers (the F... opcode space is DSP, not FPU).
    uint32_t dspMod = 0, dspRs = 0, dspRe = 0, dspDsr = 0;
    uint32_t dspA0 = 0, dspX0 = 0, dspX1 = 0, dspY0 = 0, dspY1 = 0, dspRtc = 0;

    bool sleeping = false;
    bool inSlot = false;
    uint32_t slotPC = 0;
    bool excTaken = false;
    bool ldst = false;         // SR.LDST for movli.l / movco.l

    struct Pending { uint32_t level, intevt; };
    std::vector<Pending> pending;

    // Compatibility HLE hooks (see resetBootstrap in coreabi.go):
    //   0x80005194 : if r9 == 0 then r9 = 0xA0020080, then EXECUTE the instruction
    //   0xA0020134 : r0 = 1; pc = pr; SKIP the instruction (acts like an rts)
    bool compatHooks = true;

    // Optional diagnostics hook called for every illegal opcode before the
    // exception is taken: (opcode, pc, inDelaySlot).  Handy for unit tests.
    std::function<void(uint32_t, uint32_t, bool)> illegalHook;

    void reset();              // hardware reset: regs cleared, SR privileged, PC=0x80000000
    void hardwareReset();      // alias kept for clarity
    void step();               // one instruction (devices tick + IRQ accept + fetch/execute)
    void run(uint64_t n);      // n instructions, stops early if faulted
    void execute(uint32_t op);

    void raiseIRQ(uint32_t intevt, uint32_t level);
    bool acceptInterrupt();
    bool gatePending();

    void setSR(uint32_t val);
    void setT(bool cond) { if (cond) sr |= SR_T; else sr &= ~SR_T; }
    bool bank1() const { return (sr & (SR_MD | SR_RB)) == (SR_MD | SR_RB); }

    // Fault capture: instead of panicking on an unmapped access, the CPU
    // records a message and stops.  The host reads it via faulted().
    bool faulted() const { return !fault_.empty(); }
    const std::string& fault() const { return fault_; }
    void clearFault() { fault_.clear(); }

    // Halted on a fatal fault (illegal with BL=1, unmapped memory).  step/run
    // become no-ops while set.
    bool halted() const { return halted_; }

private:
    std::string fault_;
    bool halted_ = false;

    void branchDelayed(uint32_t target);
    void generalException(uint32_t expevt, uint32_t spc);
    void illegal(uint32_t op);
    void trapa(uint32_t imm);
    void div1(uint32_t n, uint32_t m);
    void macw(uint32_t n, uint32_t m);
    void exec4(uint32_t op, uint32_t n, uint32_t m, uint32_t d8);
    static bool isDelayedBranch(uint32_t op);
    static bool slotIllegal(uint32_t op);
    void noteFault(const char* what, uint32_t a);
};

} // namespace cg100
