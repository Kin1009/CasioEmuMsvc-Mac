#pragma once
// sh4_cpu.h - SH-4/SH-4A integer register model and a small starting interpreter.
//
// The fx-CG100 uses a Renesas SH7305 (SH-4A DSP core, no FPU). This file
// implements the architectural register file, big-endian instruction fetch and
// a deliberately SMALL set of instructions (see STATUS.md). It is a foundation,
// not a full SH4 interpreter.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace cg100 {

class Bus; // defined in mmio.h

// SH4 Status Register bits (subset relevant to the foundation).
enum : uint32_t {
	SR_T     = 0x00000001u, // boolean result flag
	SR_S     = 0x00000002u, // saturation
	SR_IMASK = 0x000000F0u, // interrupt mask level
	SR_Q     = 0x00000100u, // divide step Q bit
	SR_M     = 0x00000200u, // divide step M bit
	SR_FD    = 0x00008000u, // FPU disable (set on this no-FPU part)
	SR_BL    = 0x10000000u, // block interrupts / exception mask
	SR_RB    = 0x20000000u, // register bank select
	SR_MD    = 0x40000000u, // privileged mode
};

// Architectural register file. Layout is explicit so save-state is trivial.
struct Sh4State {
	uint32_t r[16]{};        // general registers R0..R15 (currently selected bank for R0-R7)
	uint32_t rbank1[8]{};    // alternate bank for R0-R7 (swapped on SR.RB change in privileged mode)
	uint32_t pc{0};          // program counter
	uint32_t pr{0};          // procedure register (return address / link)
	uint32_t gbr{0};         // global base register
	uint32_t vbr{0};         // vector base register
	uint32_t ssr{0};         // saved SR on exception
	uint32_t spc{0};         // saved PC on exception
	uint32_t sgr{0};         // saved R15 (global) on exception
	uint32_t dbr{0};         // debug base register
	uint32_t mach{0};        // multiply-accumulate high
	uint32_t macl{0};        // multiply-accumulate low
	uint32_t fpul{0};        // floating-point communication (DSP on this part)
	uint32_t fpscr{0};       // floating-point status/control (DSP on this part)
	uint32_t sr{0};          // status register
	uint64_t cycles{0};      // instruction cycles executed
	bool sleeping{false};    // `sleep` executed: halted until an interrupt is accepted
	bool inSlot{false};      // currently executing a delayed branch slot
	uint32_t slotPC{0};      // PC of the branch that owns the current delay slot
};

class Sh4Cpu {
public:
	explicit Sh4Cpu(Bus& bus) : bus_(bus) {}

	// Restore the SH-4 power-on register state (PC = reset vector 0x80000000,
	// SR = MD|RB|BL|IMASK, all GPRs cleared). Cycle counters are preserved.
	void reset();

	// Execute exactly one instruction. Handles sleep, delayed branches and the
	// small decoded subset; unknown opcodes increment a counter (no exception
	// entry yet -- see STATUS.md).
	void step();

	// Execute up to `cycles` instructions (stops early if asleep).
	void run(uint64_t cycles);

	// Raise a maskable interrupt request (recorded; delivery is limited -- see STATUS.md).
	void raiseIrq(uint32_t intevt, uint32_t level);

	// Instruction-per-second rate used to pace the 64 Hz RTC / KEYSC scan.
	void setIps(uint64_t ips) { ips_ = ips; }
	uint64_t ips() const { return ips_; }

	Sh4State& state() { return state_; }
	const Sh4State& state() const { return state_; }

	uint32_t illegalCount() const { return illegalCount_; }
	uint32_t lastIllegalOpcode() const { return lastIllegalOpcode_; }
	uint32_t lastIllegalPc() const { return lastIllegalPc_; }

private:
	void execute(uint16_t op);
	void branchDelayed(uint32_t target);
	void setSr(uint32_t value);
	void setT(bool value);
	uint16_t fetch16();

	struct PendingIrq { uint32_t intevt; uint32_t level; };

	Bus& bus_;
	Sh4State state_;
	uint64_t ips_{200000000ull};
	uint32_t illegalCount_{0};
	uint32_t lastIllegalOpcode_{0};
	uint32_t lastIllegalPc_{0};
	std::vector<PendingIrq> pending_; // recorded only; delivery is not modelled yet
};

} // namespace cg100
