// mmio.cpp - CG100 address space + placeholder peripheral regions.
// See mmio.h and DESIGN.md for the address map and what is still stubbed.

#include "mmio.h"

#include <algorithm>
#include <cstring>

namespace cg100 {
namespace {

template <class T>
void appendPod(std::vector<uint8_t>& out, const T& value) {
	const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
	out.insert(out.end(), p, p + sizeof(T));
}

template <class T>
bool takePod(const uint8_t* data, size_t len, size_t& off, T& value) {
	if (off > len || len - off < sizeof(T)) return false;
	std::memcpy(&value, data + off, sizeof(T));
	off += sizeof(T);
	return true;
}

void appendVec(std::vector<uint8_t>& out, const std::vector<uint8_t>& v) {
	const uint32_t n = static_cast<uint32_t>(v.size());
	appendPod(out, n);
	if (n) out.insert(out.end(), v.begin(), v.end());
}

bool takeVec(const uint8_t* data, size_t len, size_t& off, std::vector<uint8_t>& v) {
	uint32_t n = 0;
	if (!takePod(data, len, off, n)) return false;
	if (off > len || len - off < n) return false;
	if (n) v.assign(data + off, data + off + n);
	else v.clear();
	off += n;
	return true;
}

} // namespace

// ---- big-endian accessors --------------------------------------------------
uint32_t beRead(const std::vector<uint8_t>& mem, uint32_t off, int size) {
	uint32_t result = 0;
	for (int i = 0; i < size; ++i) {
		const uint32_t index = off + static_cast<uint32_t>(i);
		const uint8_t byte = index < mem.size() ? mem[index] : 0xFFu;
		result = (result << 8) | byte;
	}
	return result;
}

void beWrite(std::vector<uint8_t>& mem, uint32_t off, int size, uint32_t value) {
	for (int i = size - 1; i >= 0; --i) {
		const uint32_t index = off + static_cast<uint32_t>(i);
		if (index < mem.size())
			mem[index] = static_cast<uint8_t>(value >> (8 * i));
	}
}

// ---- SimpleRegion ---------------------------------------------------------
uint32_t SimpleRegion::read(uint32_t off, int size) const {
	const uint32_t index = (off >> 2) & 0x3FF;
	const uint32_t word = regs_[index];
	switch (size) {
	case 1: return (word >> (8 * (3 - (off & 3)))) & 0xFFu;
	case 2: return (word >> (8 * (2 - (off & 2)))) & 0xFFFFu;
	default: return word;
	}
}

void SimpleRegion::write(uint32_t off, int size, uint32_t value) {
	const uint32_t index = (off >> 2) & 0x3FF;
	if (size >= 4) {
		regs_[index] = value;
		return;
	}
	const unsigned shift = 8u * (3u - (off & 3u));
	const uint32_t mask = size == 1 ? 0xFFu : 0xFFFFu;
	regs_[index] = (regs_[index] & ~(mask << shift)) | ((value & mask) << shift);
}

// ---- KEYSC ---------------------------------------------------------------
uint32_t KeyScRegion::dataWord(uint32_t w) const {
	if (w >= 6) return 0;
	uint32_t cols = 0x0FFFu;
	uint32_t rows = 0xFFu;
	const uint32_t cv = reg(0x1A);
	const uint32_t rv = reg(0x1C);
	if (cv) cols = cv;                 // unwritten == all columns enabled
	if (rv) rows = rv & 0xFFu;         // unwritten == all rows enabled
	uint32_t result = 0;
	if ((cols >> (2 * w)) & 1u) result |= rows;
	if ((cols >> (2 * w + 1)) & 1u) result |= rows << 8;
	return held_[w] & result;
}

bool KeyScRegion::anyEnabled() const {
	for (uint32_t w = 0; w < 6; ++w)
		if (dataWord(w)) return true;
	return false;
}

uint32_t KeyScRegion::read(uint32_t off, int size) const {
	if (off < 0x0Cu) {
		const uint32_t word = dataWord(off >> 1);
		if (size == 1) return (off & 1u) ? ((word >> 8) & 0xFFu) : (word & 0xFFu);
		return word & 0xFFFFu;
	}
	switch (off) {
	case 0x0C: return ctrl_ & 0xFFFFu;
	case 0x10: return mode_ & 0xFFFFu;
	case 0x12: return 2u; // scan never busy (matches the reference model)
	case 0x14: return ((ie_ & 0xFFu) << 8) | (flags_ & 0xFFu);
	default: return SimpleRegion::read(off, size);
	}
}

void KeyScRegion::write(uint32_t off, int size, uint32_t value) {
	switch (off) {
	case 0x0C:
		ctrl_ = value & 0xFFFFu;
		return;
	case 0x10:
		mode_ = value & 0xFFFFu;
		return;
	case 0x14:
		ie_ = (value >> 8) & 0xFFu;
		flags_ &= ~(value & 0xFFu); // status flags are write-1-to-clear
		return;
	default:
		SimpleRegion::write(off, size, value);
		return;
	}
}

void KeyScRegion::press(uint32_t row, uint32_t col) {
	if (row >= 8 || col >= 12) return;
	const bool wasEmpty = !anyEnabled();
	held_[col >> 1] |= static_cast<uint16_t>(1u << (row + 8u * (col & 1u)));
	if (wasEmpty) scanNext_ = 0; // press edge wakes the scanner immediately
}

void KeyScRegion::release(uint32_t row, uint32_t col) {
	if (row >= 8 || col >= 12) return;
	held_[col >> 1] &= static_cast<uint16_t>(~(1u << (row + 8u * (col & 1u))));
}

void KeyScRegion::releaseAll() {
	for (auto& w : held_) w = 0;
}

void KeyScRegion::tick(uint64_t cpuCycles) {
	if (!(ctrl_ & 0x8000u) || mode_ == 0) return; // disabled / off
	if (cpuCycles < scanNext_) return;
	scanNext_ = cpuCycles + scanPeriod_;

	const bool held = anyEnabled();
	if (held && !wasHeld_) flags_ |= 0x08u; // key-detect (empty -> non-empty)
	if (mode_ == 0x800u) {
		flags_ |= 0x02u; // synchronous "scan now" mode
	} else if (held || scanLeft_ > 0) {
		flags_ |= 0x02u; // scan complete
		scanLeft_ = held ? 2 : scanLeft_ - 1;
	}
	wasHeld_ = held;
}

void KeyScRegion::serialize(std::vector<uint8_t>& out) const {
	appendPod(out, held_);
	appendPod(out, ctrl_);
	appendPod(out, mode_);
	appendPod(out, ie_);
	appendPod(out, flags_);
	appendPod(out, scanPeriod_);
	appendPod(out, scanNext_);
	const int32_t left = scanLeft_;
	appendPod(out, left);
	appendPod(out, wasHeld_);
	for (const uint32_t r : regs_) appendPod(out, r);
}

bool KeyScRegion::deserialize(const uint8_t* data, size_t len, size_t& used) {
	size_t off = 0;
	if (!takePod(data, len, off, held_)) return false;
	if (!takePod(data, len, off, ctrl_)) return false;
	if (!takePod(data, len, off, mode_)) return false;
	if (!takePod(data, len, off, ie_)) return false;
	if (!takePod(data, len, off, flags_)) return false;
	if (!takePod(data, len, off, scanPeriod_)) return false;
	if (!takePod(data, len, off, scanNext_)) return false;
	int32_t left = 0;
	if (!takePod(data, len, off, left)) return false;
	scanLeft_ = left;
	if (!takePod(data, len, off, wasHeld_)) return false;
	for (uint32_t& r : regs_) if (!takePod(data, len, off, r)) return false;
	used = off;
	return true;
}

// ---- LCD ------------------------------------------------------------------
bool LcdRegion::rs() const { return (pfc_.reg(0x13Cu) & 0x10u) != 0; }

bool LcdRegion::lit() const {
	if (!(reg_[0x007] & 0x0100u)) return false; // display-on bit
	const uint32_t bl = pfc_.reg(0x12Eu);
	if (bl && !(bl & 0x20u)) return false; // backlight bit, unwritten == on
	return true;
}

uint32_t LcdRegion::read(uint32_t off, int size) const {
	if (!rs()) return idx_;
	if (idx_ == 0x202u) return 0; // GRAM read-back not modelled
	return reg_[idx_ & (REG_COUNT - 1)];
}

void LcdRegion::write(uint32_t off, int size, uint32_t value) {
	if (!rs()) {
		idx_ = static_cast<uint16_t>(value & (REG_COUNT - 1));
		return;
	}
	writeData(static_cast<uint16_t>(value));
}

void LcdRegion::setAddress() {
	h_ = reg_[0x200] & 0x1FFu;
	v_ = reg_[0x201] & 0x1FFu;
	if (h_ >= W) h_ = W - 1;
	if (v_ >= H) v_ = H - 1;
}

void LcdRegion::applyBootDefaults() {
	reg_[0x003] = 0x00A0u;            // ORG=1, V-increment, H-decrement
	reg_[0x007] = 0x0100u;            // display on
	reg_[0x210] = 0;
	reg_[0x211] = static_cast<uint16_t>(W - 1);
	reg_[0x212] = 0;
	reg_[0x213] = static_cast<uint16_t>(H - 1);
	setAddress();
}

void LcdRegion::writePixel(uint16_t value) {
	if (h_ < W && v_ < H)
		gram_[v_ * W + h_] = value;
	++h_;
	if (h_ >= W) { h_ = 0; ++v_; if (v_ >= H) v_ = 0; }
	++gen_;
}

void LcdRegion::writeData(uint16_t value) {
	switch (idx_) {
	case 0x202u:
		writePixel(value);
		return;
	case 0x200u:
	case 0x201u:
		reg_[idx_] = value;
		setAddress();
		return;
	default:
		reg_[idx_ & (REG_COUNT - 1)] = value;
		return;
	}
}

void LcdRegion::serialize(std::vector<uint8_t>& out) const {
	appendPod(out, idx_);
	for (uint16_t r : reg_) appendPod(out, r);
	for (uint16_t p : gram_) appendPod(out, p);
	appendPod(out, h_);
	appendPod(out, v_);
	appendPod(out, gen_);
}

bool LcdRegion::deserialize(const uint8_t* data, size_t len, size_t& used) {
	size_t off = 0;
	if (!takePod(data, len, off, idx_)) return false;
	for (uint16_t& r : reg_) if (!takePod(data, len, off, r)) return false;
	for (uint16_t& p : gram_) if (!takePod(data, len, off, p)) return false;
	if (!takePod(data, len, off, h_)) return false;
	if (!takePod(data, len, off, v_)) return false;
	if (!takePod(data, len, off, gen_)) return false;
	used = off;
	return true;
}

// ---- RTC ------------------------------------------------------------------
uint32_t RtcRegion::read(uint32_t off, int size) const {
	if (off < 4) {
		const uint32_t counter = static_cast<uint32_t>(ticks64_);
		return (off & 2) ? (counter >> 16) & 0xFFFFu : counter & 0xFFFFu;
	}
	return SimpleRegion::read(off, size);
}

void RtcRegion::write(uint32_t off, int size, uint32_t value) {
	if (off < 4) {
		const uint32_t low = static_cast<uint32_t>(ticks64_);
		const uint32_t high = static_cast<uint32_t>(ticks64_ >> 32);
		if (off & 2) ticks64_ = (static_cast<uint64_t>(value & 0xFFFFu) << 32) | low;
		else ticks64_ = (static_cast<uint64_t>(high) << 32) | (value & 0xFFFFu);
		return;
	}
	SimpleRegion::write(off, size, value);
}

void RtcRegion::tick(uint64_t cpuCycles, uint64_t ips) {
	if (ips == 0 || cpuCycles <= lastCycle_) { lastCycle_ = cpuCycles; return; }
	const uint64_t delta = cpuCycles - lastCycle_;
	lastCycle_ = cpuCycles;
	ticks64_ += delta * 64u / ips;
}

void RtcRegion::serialize(std::vector<uint8_t>& out) const {
	appendPod(out, ticks64_);
	appendPod(out, lastCycle_);
	for (uint32_t r : regs_) appendPod(out, r);
}

bool RtcRegion::deserialize(const uint8_t* data, size_t len, size_t& used) {
	size_t off = 0;
	if (!takePod(data, len, off, ticks64_)) return false;
	if (!takePod(data, len, off, lastCycle_)) return false;
	for (uint32_t& r : regs_) if (!takePod(data, len, off, r)) return false;
	used = off;
	return true;
}

// ---- Bus ------------------------------------------------------------------
Bus::Bus()
	: pfc_(PFC_BASE, 0x1000u)
	, intc_(INTC_BASE, 0x1000u)
	, ccn_(CCN_BASE, 0x1000u)
	, dmac_(DMAC_BASE, 0x1000u)
	, miscA44C_(A44C_BASE, 0x1000u)
	, miscA44D_(A44D_BASE, 0x1000u)
	, lcd_(pfc_)
{
	dram_.assign(DRAM_SIZE, 0);
	ilram_.assign(ILRAM_SIZE, 0);
	ocram_.assign(OCRAM_SIZE, 0);
	xyram_.assign(XYRAM_SIZE, 0);
	lcd_.applyBootDefaults();
}

bool Bus::loadFlash(const uint8_t* data, size_t len) {
	if (!data || len == 0 || len > FLASH_MUT_TOP) return false;
	flash_.assign(data, data + len);
	flash_.resize(FLASH_MUT_TOP, 0xFFu); // erased NOR beyond the image
	return true;
}

uint32_t Bus::read(uint32_t va, int size) {
	// CG100 area-1 flash mirror at physical 0x0B000000.
	if (va >= FLASH_MIRROR && va < FLASH_MIRROR + FLASH_MUT_TOP)
		return beRead(flash_, va - FLASH_MIRROR, size);

	if (va >= 0xE0000000u) {
		if (va >= ILRAM_BASE && va < ILRAM_BASE + ILRAM_SIZE)
			return beRead(ilram_, (va - ILRAM_BASE) & ILRAM_MASK, size);
		if (va - XYRAM_BASE < XYRAM_SIZE)
			return beRead(xyram_, va - XYRAM_BASE, size);
		if (va >= OCRAM_BASE && va < OCRAM_BASE + OCRAM_SIZE)
			return beRead(ocram_, va - OCRAM_BASE, size);
		return readMmio(va, size); // P4 control registers
	}

	const uint32_t phys = va & 0x1FFFFFFFu;

	// DRAM via ANY mirror (including the uncached 0xAC000000 VRAM mirror).
	// This must precede the on-chip peripheral window check.
	if (phys >= DRAM_BASE && phys < DRAM_BASE + DRAM_SIZE)
		return beRead(dram_, phys - DRAM_BASE, size);

	if (va >= 0xA4000000u && va < 0xC0000000u)
		return readMmio(va, size);

	if (phys < FLASH_SIZE) {
		if (phys < flash_.size()) return beRead(flash_, phys, size);
		return size == 4 ? 0xFFFFFFFFu : (size == 2 ? 0xFFFFu : 0xFFu);
	}

	++unmapped_;
	return 0xFFFFFFFFu;
}

void Bus::write(uint32_t va, int size, uint32_t value) {
	if (va >= FLASH_MIRROR && va < FLASH_MIRROR + FLASH_MUT_TOP) {
		beWrite(flash_, va - FLASH_MIRROR, size, value); // NOR command state machine stubbed
		return;
	}

	if (va >= 0xE0000000u) {
		if (va >= ILRAM_BASE && va < ILRAM_BASE + ILRAM_SIZE) {
			beWrite(ilram_, (va - ILRAM_BASE) & ILRAM_MASK, size, value);
			return;
		}
		if (va - XYRAM_BASE < XYRAM_SIZE) {
			beWrite(xyram_, va - XYRAM_BASE, size, value);
			return;
		}
		if (va >= OCRAM_BASE && va < OCRAM_BASE + OCRAM_SIZE) {
			beWrite(ocram_, va - OCRAM_BASE, size, value);
			return;
		}
		writeMmio(va, size, value);
		return;
	}

	const uint32_t phys = va & 0x1FFFFFFFu;
	if (phys >= DRAM_BASE && phys < DRAM_BASE + DRAM_SIZE) {
		beWrite(dram_, phys - DRAM_BASE, size, value);
		return;
	}
	if (va >= 0xA4000000u && va < 0xC0000000u) {
		writeMmio(va, size, value);
		return;
	}
	if (phys < FLASH_SIZE) {
		if (phys < flash_.size()) beWrite(flash_, phys, size, value);
		return;
	}
	++unmapped_;
}

uint16_t Bus::fetch16(uint32_t pc) {
	const uint32_t p = pc & 0x1FFFFFFFu;
	if (!flash_.empty() && p < flash_.size())
		return static_cast<uint16_t>(beRead(flash_, p, 2));
	if (p - DRAM_BASE < DRAM_SIZE)
		return static_cast<uint16_t>(beRead(dram_, p - DRAM_BASE, 2));
	return static_cast<uint16_t>(read(pc, 2));
}

// Normalise a (possibly mirrored) address into the P2 form used by the MMIO
// decoder. P4 addresses (>= 0xE0000000) are passed through unchanged.
uint32_t Bus::readMmio(uint32_t va, int size) {
	const uint32_t b = va >= 0xE0000000u ? va : ((va & 0x1FFFFFFFu) | 0xA0000000u);
	if (b >= KEYSC_BASE && b < KEYSC_BASE + 0x1000u) return keysc_.read(b - KEYSC_BASE, size);
	if (b >= PFC_BASE && b < PFC_BASE + 0x1000u) return pfc_.read(b - PFC_BASE, size);
	if (b >= INTC_BASE && b < INTC_BASE + 0x1000u) return intc_.read(b - INTC_BASE, size);
	if (b >= LCD_BASE && b < LCD_BASE + 0x20000u) return lcd_.read(b - LCD_BASE, size);
	if (b >= RTC_BASE && b < RTC_BASE + 0x40u) return rtc_.read(b - RTC_BASE, size);
	if (b >= CCN_BASE && b < CCN_BASE + 0x1000u) return ccn_.read(b - CCN_BASE, size);
	if (b >= DMAC_BASE && b < DMAC_BASE + 0x1000u) return dmac_.read(b - DMAC_BASE, size);
	if (b >= A44C_BASE && b < A44C_BASE + 0x1000u) return miscA44C_.read(b - A44C_BASE, size);
	if (b >= A44D_BASE && b < A44D_BASE + 0x1000u) return miscA44D_.read(b - A44D_BASE, size);
	++unmapped_;
	return 0;
}

void Bus::writeMmio(uint32_t va, int size, uint32_t value) {
	++mmioWrites_;
	const uint32_t b = va >= 0xE0000000u ? va : ((va & 0x1FFFFFFFu) | 0xA0000000u);
	if (b >= KEYSC_BASE && b < KEYSC_BASE + 0x1000u) { keysc_.write(b - KEYSC_BASE, size, value); return; }
	if (b >= PFC_BASE && b < PFC_BASE + 0x1000u) { pfc_.write(b - PFC_BASE, size, value); return; }
	if (b >= INTC_BASE && b < INTC_BASE + 0x1000u) { intc_.write(b - INTC_BASE, size, value); return; }
	if (b >= LCD_BASE && b < LCD_BASE + 0x20000u) { lcd_.write(b - LCD_BASE, size, value); return; }
	if (b >= RTC_BASE && b < RTC_BASE + 0x40u) { rtc_.write(b - RTC_BASE, size, value); return; }
	if (b >= CCN_BASE && b < CCN_BASE + 0x1000u) { ccn_.write(b - CCN_BASE, size, value); return; }
	if (b >= DMAC_BASE && b < DMAC_BASE + 0x1000u) { dmac_.write(b - DMAC_BASE, size, value); return; }
	if (b >= A44C_BASE && b < A44C_BASE + 0x1000u) { miscA44C_.write(b - A44C_BASE, size, value); return; }
	if (b >= A44D_BASE && b < A44D_BASE + 0x1000u) { miscA44D_.write(b - A44D_BASE, size, value); return; }
	++unmapped_;
}

void Bus::tickDevices(uint64_t cpuCycles, uint64_t ips) {
	keysc_.tick(cpuCycles);
	rtc_.tick(cpuCycles, ips);
}

void Bus::serialize(std::vector<uint8_t>& out) const {
	static const char magic[8] = {'C','G','1','M','E','M','0','1'};
	out.insert(out.end(), magic, magic + 8);
	appendVec(out, flash_);
	appendVec(out, dram_);
	appendVec(out, ilram_);
	appendVec(out, ocram_);
	appendVec(out, xyram_);
	// Generic regions are fixed-size; dump their register arrays verbatim.
	auto dumpRegion = [&](const SimpleRegion& region) {
		for (uint32_t i = 0; i < 0x400; ++i) appendPod(out, region.reg(i * 4));
	};
	dumpRegion(pfc_);
	dumpRegion(intc_);
	dumpRegion(ccn_);
	dumpRegion(dmac_);
	dumpRegion(miscA44C_);
	dumpRegion(miscA44D_);
	keysc_.serialize(out);
	lcd_.serialize(out);
	rtc_.serialize(out);
}

bool Bus::deserialize(const uint8_t* data, size_t len) {
	static const char magic[8] = {'C','G','1','M','E','M','0','1'};
	if (!data || len < 8 || std::memcmp(data, magic, 8) != 0) return false;
	size_t off = 8;
	if (!takeVec(data, len, off, flash_)) return false;
	if (!takeVec(data, len, off, dram_)) return false;
	if (!takeVec(data, len, off, ilram_)) return false;
	if (!takeVec(data, len, off, ocram_)) return false;
	if (!takeVec(data, len, off, xyram_)) return false;
	auto loadRegion = [&](SimpleRegion& region) {
		for (uint32_t i = 0; i < 0x400; ++i) {
			uint32_t r = 0;
			if (!takePod(data, len, off, r)) return false;
			region.setReg(i * 4, r);
		}
		return true;
	};
	if (!loadRegion(pfc_)) return false;
	if (!loadRegion(intc_)) return false;
	if (!loadRegion(ccn_)) return false;
	if (!loadRegion(dmac_)) return false;
	if (!loadRegion(miscA44C_)) return false;
	if (!loadRegion(miscA44D_)) return false;
	size_t used = 0;
	if (!keysc_.deserialize(data + off, len - off, used)) return false;
	off += used;
	if (!lcd_.deserialize(data + off, len - off, used)) return false;
	off += used;
	if (!rtc_.deserialize(data + off, len - off, used)) return false;
	off += used;
	return off == len;
}

} // namespace cg100
