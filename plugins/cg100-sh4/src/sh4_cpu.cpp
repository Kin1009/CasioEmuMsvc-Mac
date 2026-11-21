// sh4_cpu.cpp - SH-4 register model and starting interpreter.
//
// Opcode encodings follow the Go reference core (emu_go/cpu.go execute()).
// SR, delay slots and PC advancement mirror it exactly. Only the documented
// subset is decoded; everything else increments the illegal-op counter.

#include "sh4_cpu.h"
#include "mmio.h"

#include <cstdint>

namespace cg100 {
namespace {

inline int32_t signExtend(uint32_t value, int bits) {
	const uint32_t sign = 1u << (bits - 1);
	return static_cast<int32_t>((value ^ sign) - sign);
}

inline bool bank1(uint32_t sr) {
	return (sr & (SR_MD | SR_RB)) == (SR_MD | SR_RB);
}

// True for instructions forbidden in a delay slot (a subset of the SH4 rule).
inline bool slotIllegal(uint16_t op) {
	const uint32_t o = op;
	if ((o & 0xF000u) == 0xA000u || (o & 0xF000u) == 0xB000u) return true; // bra/bsr
	if ((o & 0xF000u) == 0x8000u) {
		const uint32_t n = (o >> 8) & 0xFu;
		if (n == 0x9u || n == 0xBu || n == 0xDu || n == 0xFu) return true; // bt/bf (+/s)
	}
	if (o == 0x000Bu || o == 0x002Bu || o == 0x001Bu) return true; // rts/rte/sleep
	if ((o & 0xF000u) == 0x4000u) {
		const uint32_t sub = o & 0x00FFu;
		if (sub == 0x0Bu || sub == 0x2Bu) return true; // jsr/jmp
	}
	if ((o & 0xF000u) == 0xC000u && ((o >> 8) & 0xFu) >= 0xCu) return true; // ldc variants
	return false;
}

} // namespace

void Sh4Cpu::setSr(uint32_t value) {
	if (bank1(state_.sr) != bank1(value)) {
		for (int i = 0; i < 8; ++i) {
			const uint32_t tmp = state_.r[i];
			state_.r[i] = state_.rbank1[i];
			state_.rbank1[i] = tmp;
		}
	}
	state_.sr = value;
}

void Sh4Cpu::setT(bool value) {
	if (value) state_.sr |= SR_T;
	else state_.sr &= ~SR_T;
}

void Sh4Cpu::reset() {
	for (auto& r : state_.r) r = 0;
	for (auto& r : state_.rbank1) r = 0;
	state_.pc = 0x80000000u; // SH7305 reset vector (flash mirror)
	state_.pr = 0;
	state_.gbr = 0;
	state_.vbr = 0;
	state_.ssr = 0;
	state_.spc = 0;
	state_.sgr = 0;
	state_.dbr = 0;
	state_.mach = 0;
	state_.macl = 0;
	state_.fpul = 0;
	state_.fpscr = 0;
	state_.sr = 0;
	setSr(SR_MD | SR_RB | SR_BL | SR_IMASK); // privileged, masked, no-FPU
	state_.sleeping = false;
	state_.inSlot = false;
	state_.slotPC = 0;
	pending_.clear();
	illegalCount_ = 0;
	lastIllegalOpcode_ = 0;
	lastIllegalPc_ = 0;
}

uint16_t Sh4Cpu::fetch16() {
	return bus_.fetch16(state_.pc);
}

void Sh4Cpu::branchDelayed(uint32_t target) {
	const uint16_t slot = fetch16();
	if (slotIllegal(slot)) {
		++illegalCount_;
		lastIllegalOpcode_ = slot;
		lastIllegalPc_ = state_.pc;
		state_.pc = target; // exception entry is not modelled; continue at target
		return;
	}
	state_.pc += 2;
	state_.inSlot = true;
	state_.slotPC = state_.pc - 4;
	execute(slot);
	state_.inSlot = false;
	state_.pc = target;
}

void Sh4Cpu::step() {
	if (state_.sleeping) {
		++state_.cycles;
		// A pending request wakes the CPU even while blocked; delivery waits for
		// SR.BL / IMASK to be cleared (not modelled yet).
		if (!pending_.empty()) state_.sleeping = false;
		return;
	}

	// Interrupt delivery is stubbed: accept the highest pending request when
	// SR.BL is clear and its level beats IMASK.
	if (!pending_.empty() && !(state_.sr & SR_BL)) {
		const uint32_t imask = (state_.sr & SR_IMASK) >> 4;
		size_t best = pending_.size();
		for (size_t i = 0; i < pending_.size(); ++i) {
			if (pending_[i].level > imask &&
			    (best == pending_.size() || pending_[i].level > pending_[best].level)) {
				best = i;
			}
		}
		if (best != pending_.size()) {
			const uint32_t intevt = pending_[best].intevt;
			const uint32_t level = pending_[best].level;
			pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(best));
			state_.ssr = state_.sr;
			state_.spc = state_.pc;
			state_.sgr = state_.r[15];
			bus_.write(0xFF000028u, 4, intevt); // INTEVT
			setSr((state_.sr | SR_MD | SR_RB | SR_BL) |
			      ((level << 4) & SR_IMASK));
			state_.pc = state_.vbr + 0x600u;
		}
	}

	const uint16_t op = fetch16();
	state_.pc += 2;
	execute(op);
	++state_.cycles;
	bus_.tickDevices(state_.cycles, ips_);
}

void Sh4Cpu::run(uint64_t cycles) {
	for (uint64_t i = 0; i < cycles && !state_.sleeping; ++i)
		step();
}

void Sh4Cpu::raiseIrq(uint32_t intevt, uint32_t level) {
	for (const auto& p : pending_)
		if (p.intevt == intevt) return; // already pending
	pending_.push_back({intevt, level});
}

void Sh4Cpu::execute(uint16_t op) {
	const uint32_t o = op;
	const uint32_t n = (o >> 8) & 0xFu;
	const uint32_t m = (o >> 4) & 0xFu;
	const uint32_t d4 = o & 0xFu;
	const uint32_t d8 = o & 0xFFu;
	const uint32_t d12 = o & 0xFFFu;
	uint32_t* const r = state_.r;

	// ---- No-operand / control instructions ---------------------------------
	switch (o) {
	case 0x0009u: return;                       // nop
	case 0x0008u: setT(false); return;          // clrt
	case 0x0018u: setT(true); return;           // sett
	case 0x0019u:                               // div0u
		state_.sr &= ~(SR_Q | SR_M | SR_T);
		return;
	case 0x000Bu: branchDelayed(state_.pr); return;              // rts
	case 0x002Bu: setSr(state_.ssr); branchDelayed(state_.spc); return; // rte
	case 0x001Bu: state_.sleeping = true; return;                // sleep
	case 0x0028u: state_.mach = 0; state_.macl = 0; return;      // clrmac
	default: break;
	}

	switch (o >> 12) {
	// ---- 0x1: mov.l Rm,@(disp,Rn) -----------------------------------------
	case 0x1u:
		bus_.write(r[n] + d4 * 4u, 4, r[m]);
		return;

	// ---- 0x2: stores, logic, comparisons ----------------------------------
	case 0x2u:
		switch (d4) {
		case 0x0u: bus_.write(r[n],      1, r[m] & 0xFFu);    return; // mov.b
		case 0x1u: bus_.write(r[n],      2, r[m] & 0xFFFFu);  return; // mov.w
		case 0x2u: bus_.write(r[n],      4, r[m]);            return; // mov.l
		case 0x4u: r[n] -= 1; bus_.write(r[n], 1, r[m] & 0xFFu);   return;
		case 0x5u: r[n] -= 2; bus_.write(r[n], 2, r[m] & 0xFFFFu); return;
		case 0x6u: r[n] -= 4; bus_.write(r[n], 4, r[m]);           return;
		case 0x9u: r[n] &= r[m]; return; // and
		case 0xAu: r[n] ^= r[m]; return; // xor
		case 0xBu: r[n] |= r[m]; return; // or
		case 0xCu: {                    // cmp/str
			bool equal = false;
			for (uint32_t i = 0; i < 4; ++i)
				if (((r[n] >> (8 * i)) & 0xFFu) == ((r[m] >> (8 * i)) & 0xFFu)) equal = true;
			setT(equal);
			return;
		}
		default: break;
		}
		break;

	// ---- 0x3: add/sub/cmp (register form) ---------------------------------
	case 0x3u:
		switch (d4) {
		case 0x0u: setT(r[n] == r[m]); return;                    // cmp/eq
		case 0x2u: setT(r[n] >= r[m]); return;                    // cmp/hs
		case 0x3u: setT(static_cast<int32_t>(r[n]) >= static_cast<int32_t>(r[m])); return;
		case 0x6u: setT(r[n] > r[m]); return;                     // cmp/hi
		case 0x7u: setT(static_cast<int32_t>(r[n]) > static_cast<int32_t>(r[m])); return;
		case 0x8u: r[n] -= r[m]; return;                          // sub
		case 0xAu: {                                             // subc
			const int64_t result = static_cast<int64_t>(r[n]) - r[m] -
			                       (state_.sr & SR_T ? 1 : 0);
			setT(result < 0);
			r[n] = static_cast<uint32_t>(result);
			return;
		}
		case 0xCu: r[n] += r[m]; return;                          // add
		case 0xEu: {                                             // addc
			const uint64_t result = static_cast<uint64_t>(r[n]) + r[m] +
			                        (state_.sr & SR_T ? 1u : 0u);
			setT(result > 0xFFFFFFFFull);
			r[n] = static_cast<uint32_t>(result);
			return;
		}
		default: break;
		}
		break;

	// ---- 0x4: jmp/jsr/ldc --------------------------------------------------
	case 0x4u:
		switch (o & 0x00FFu) {
		case 0x0Bu: state_.pr = state_.pc + 2; branchDelayed(r[n]); return; // jsr
		case 0x2Bu: branchDelayed(r[n]); return;                            // jmp
		case 0x0Eu: setSr(r[n]); return;                                    // ldc Rn,SR
		default: break;
		}
		break;

	// ---- 0x5: mov.l @(disp,Rm),Rn -----------------------------------------
	case 0x5u:
		r[n] = bus_.read(r[m] + d4 * 4u, 4);
		return;

	// ---- 0x6: register loads / moves --------------------------------------
	case 0x6u:
		switch (d4) {
		case 0x0u: r[n] = static_cast<uint32_t>(signExtend(bus_.read(r[m], 1), 8));  return;
		case 0x1u: r[n] = static_cast<uint32_t>(signExtend(bus_.read(r[m], 2), 16)); return;
		case 0x2u: r[n] = bus_.read(r[m], 4); return;
		case 0x3u: r[n] = r[m]; return;                                           // mov
		case 0x4u: r[n] = static_cast<uint32_t>(signExtend(bus_.read(r[m], 1), 8));  r[m] += 1; return;
		case 0x5u: r[n] = static_cast<uint32_t>(signExtend(bus_.read(r[m], 2), 16)); r[m] += 2; return;
		case 0x6u: r[n] = bus_.read(r[m], 4); r[m] += 4; return;
		case 0x7u: r[n] = ~r[m]; return;                                           // not
		case 0xCu: r[n] = r[m] & 0xFFu; return;                                    // extu.b
		case 0xDu: r[n] = r[m] & 0xFFFFu; return;                                  // extu.w
		case 0xEu: r[n] = static_cast<uint32_t>(signExtend(r[m], 8)); return;       // exts.b
		case 0xFu: r[n] = static_cast<uint32_t>(signExtend(r[m], 16)); return;      // exts.w
		default: break;
		}
		break;

	// ---- 0x7: add #imm,Rn --------------------------------------------------
	case 0x7u:
		r[n] += static_cast<uint32_t>(signExtend(d8, 8));
		return;

	// ---- 0x8: @R0 stores/loads, bt/bf --------------------------------------
	case 0x8u:
		switch (n) {
		case 0x0u: bus_.write(r[m] + d4,     1, r[0] & 0xFFu);   return;
		case 0x1u: bus_.write(r[m] + d4 * 2, 2, r[0] & 0xFFFFu); return;
		case 0x4u: r[0] = static_cast<uint32_t>(signExtend(bus_.read(r[m] + d4, 1), 8));  return;
		case 0x5u: r[0] = static_cast<uint32_t>(signExtend(bus_.read(r[m] + d4 * 2, 2), 16)); return;
		case 0x6u: r[0] = bus_.read(r[m] + d4 * 4, 4); return;
		case 0x8u: setT(r[0] == static_cast<uint32_t>(signExtend(d8, 8))); return; // cmp/eq #imm
		case 0x9u: if (state_.sr & SR_T) state_.pc += 2 + static_cast<uint32_t>(signExtend(d8, 8)) * 2; return; // bt
		case 0xBu: if (!(state_.sr & SR_T)) state_.pc += 2 + static_cast<uint32_t>(signExtend(d8, 8)) * 2; return; // bf
		case 0xDu: if (state_.sr & SR_T) branchDelayed(state_.pc + 2 + static_cast<uint32_t>(signExtend(d8, 8)) * 2); return; // bt/s
		case 0xFu: if (!(state_.sr & SR_T)) branchDelayed(state_.pc + 2 + static_cast<uint32_t>(signExtend(d8, 8)) * 2); return; // bf/s
		default: break;
		}
		break;

	// ---- 0x9: mov.w @(disp,PC),Rn -----------------------------------------
	case 0x9u:
		r[n] = static_cast<uint32_t>(signExtend(bus_.read(state_.pc + 2 + d8 * 2u, 2), 16));
		return;

	// ---- 0xA: bra ----------------------------------------------------------
	case 0xAu:
		branchDelayed(state_.pc + 2 + static_cast<uint32_t>(signExtend(d12, 12)) * 2u);
		return;

	// ---- 0xB: bsr ----------------------------------------------------------
	case 0xBu:
		state_.pr = state_.pc + 2;
		branchDelayed(state_.pc + 2 + static_cast<uint32_t>(signExtend(d12, 12)) * 2u);
		return;

	// ---- 0xC: GBR-relative accesses ---------------------------------------
	case 0xCu:
		switch (n) {
		case 0x0u: bus_.write(state_.gbr + d8, 1, r[0] & 0xFFu);   return;
		case 0x1u: bus_.write(state_.gbr + d8, 2, r[0] & 0xFFFFu); return;
		case 0x2u: bus_.write(state_.gbr + d8, 4, r[0]);           return;
		case 0x3u: bus_.write(state_.gbr + r[m], 1, r[0] & 0xFFu); return;
		case 0x4u: bus_.write(state_.gbr + r[m], 2, r[0] & 0xFFFFu); return;
		case 0x5u: bus_.write(state_.gbr + r[m], 4, r[0]);         return;
		case 0x6u: r[0] = bus_.read(state_.gbr + d8, 1); return;
		case 0x7u: r[0] = bus_.read(state_.gbr + d8, 2); return;
		case 0x8u: r[0] = bus_.read(state_.gbr + d8, 4); return;
		case 0x9u: r[0] = bus_.read(state_.gbr + r[m], 1); return;
		case 0xAu: r[0] = bus_.read(state_.gbr + r[m], 2); return;
		case 0xBu: r[0] = bus_.read(state_.gbr + r[m], 4); return;
		default: break;
		}
		break;

	// ---- 0xD: mov.l @(disp,PC),Rn -----------------------------------------
	case 0xDu:
		r[n] = bus_.read(((state_.pc + 2u) & ~3u) + d8 * 4u, 4);
		return;

	// ---- 0xE: mov #imm,Rn --------------------------------------------------
	case 0xEu:
		r[n] = static_cast<uint32_t>(signExtend(d8, 8));
		return;

	// ---- 0xF: DSP space (no FPU on SH7305) -- not modelled ----------------
	case 0xFu:
		break;

	default: break;
	}

	++illegalCount_;
	lastIllegalOpcode_ = o;
	lastIllegalPc_ = state_.pc - 2;
}

} // namespace cg100
