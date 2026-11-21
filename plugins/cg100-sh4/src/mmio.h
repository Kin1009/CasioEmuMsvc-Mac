#pragma once
// mmio.h - CG100 (SH7305) address-space model and placeholder MMIO regions.
//
// Address decode mirrors the Go reference core (emu_go/memory.go):
//   flash 0x00000000 (32 MiB mutable, mirror at 0x0B000000)
//   DRAM  0x0C000000 (8 MiB, visible through every P0/P1/P2 mirror)
//   ILRAM 0xFD800000 (64 KiB window / 16 KiB physical)
//   OCRAM 0xFE200000 (2 MiB)
//   XYRAM 0xE5000000
//   MMIO  0xA4000000..0xBFFFFFFF and 0xE0000000..0xFFFFFFFF
//
// KEYSC / LCD / RTC get behavioural placeholders; other peripherals are
// generic sparse register blocks. See STATUS.md for what is still stubbed.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace cg100 {

// ---- Memory geometry -------------------------------------------------------
constexpr uint32_t FLASH_SIZE       = 0x08000000u; // NOR window (image is a prefix)
constexpr uint32_t FLASH_MUT_TOP    = 0x02000000u; // 32 MiB mutable array
constexpr uint32_t FLASH_MIRROR     = 0x0B000000u; // area-1 CS window
constexpr uint32_t DRAM_BASE        = 0x0C000000u;
constexpr uint32_t DRAM_SIZE        = 0x00800000u; // 8 MiB
constexpr uint32_t ILRAM_BASE       = 0xFD800000u;
constexpr uint32_t ILRAM_SIZE       = 0x00010000u; // 64 KiB window
constexpr uint32_t ILRAM_MASK       = 0x00003FFFu; // 16 KiB physical, mirrored
constexpr uint32_t OCRAM_BASE       = 0xFE200000u;
constexpr uint32_t OCRAM_SIZE       = 0x00200000u; // 2 MiB
constexpr uint32_t XYRAM_BASE       = 0xE5000000u;
constexpr uint32_t XYRAM_SIZE       = 0x00210000u;

// ---- Peripheral base addresses --------------------------------------------
constexpr uint32_t PFC_BASE         = 0xA4050000u;
constexpr uint32_t INTC_BASE        = 0xA4080000u;
constexpr uint32_t KEYSC_BASE       = 0xA44B0000u;
constexpr uint32_t A44C_BASE        = 0xA44C0000u;
constexpr uint32_t A44D_BASE        = 0xA44D0000u;
constexpr uint32_t CCN_BASE         = 0xFF000000u;
constexpr uint32_t LCD_BASE         = 0xB4000000u;
constexpr uint32_t RTC_BASE         = 0xA413FEC0u;
constexpr uint32_t DMAC_BASE        = 0xFE008000u;

// Big-endian load/store helpers (SH4 is big-endian on this part).
uint32_t beRead(const std::vector<uint8_t>& mem, uint32_t off, int size);
void beWrite(std::vector<uint8_t>& mem, uint32_t off, int size, uint32_t value);

// ---- Generic 4 KiB register block -----------------------------------------
class SimpleRegion {
public:
	SimpleRegion() = default;
	explicit SimpleRegion(uint32_t base, uint32_t size = 0x1000u)
		: base_(base), size_(size) {}

	virtual uint32_t read(uint32_t off, int size) const;
	virtual void write(uint32_t off, int size, uint32_t value);
	uint32_t reg(uint32_t off) const { return regs_[(off >> 2) & 0x3FF]; }
	void setReg(uint32_t off, uint32_t value) { regs_[(off >> 2) & 0x3FF] = value; }
	uint32_t base() const { return base_; }
	uint32_t size() const { return size_; }

protected:
	uint32_t base_{0};
	uint32_t size_{0x1000u};
	uint32_t regs_[0x400]{}; // 1024 x 32-bit regs == 4 KiB
};

// ---- KEYSC (key-scan unit, SH7724-style KIU) at 0xA44B0000 ----------------
// Six 16-bit data words: word w low byte = columns 2w, high byte = 2w+1,
// bit n of a byte = row n. Matrix is 8 rows x 12 columns.
class KeyScRegion : public SimpleRegion {
public:
	static constexpr uint32_t INTEVT = 0xBE0u;
	static constexpr uint32_t LEVEL  = 13u;
	static constexpr uint32_t DEFAULT_SCAN_PERIOD = 750000u; // ~33 Hz at host rate

	KeyScRegion() : SimpleRegion(KEYSC_BASE, 0x1000u) {}

	uint32_t read(uint32_t off, int size) const;
	void write(uint32_t off, int size, uint32_t value);

	void press(uint32_t row, uint32_t col);
	void release(uint32_t row, uint32_t col);
	void releaseAll();

	uint64_t scanPeriod() const { return scanPeriod_; }
	void setScanPeriod(uint64_t value) { scanPeriod_ = value; }
	void tick(uint64_t cycles);

	bool pendingIrq() const { return (flags_ & ie_) != 0; }
	void clearFlags(uint32_t mask) { flags_ &= ~mask; }

	void serialize(std::vector<uint8_t>& out) const;
	bool deserialize(const uint8_t* data, size_t len, size_t& used);

private:
	uint32_t dataWord(uint32_t w) const;
	bool anyEnabled() const;

	uint16_t held_[6]{};      // live matrix (word = col>>1, bit = row + 8*(col&1))
	uint32_t ctrl_{0};
	uint32_t mode_{0};
	uint32_t ie_{0};
	uint32_t flags_{0};
	uint64_t scanPeriod_{DEFAULT_SCAN_PERIOD};
	uint64_t scanNext_{0};
	int scanLeft_{0};
	bool wasHeld_{false};
};

// ---- R61524 LCD controller at 0xB4000000 (bus area 5) ---------------------
class LcdRegion {
public:
	static constexpr int W = 396;
	static constexpr int H = 224;
	static constexpr uint32_t REG_COUNT = 0x800u;

	// PFC register 0x13C bit 4 is the register-select line; bit 5 of 0x12E is
	// the backlight enable. Both live in the PFC block owned by Bus.
	explicit LcdRegion(const SimpleRegion& pfc) : pfc_(pfc) {}

	uint32_t read(uint32_t off, int size) const;
	void write(uint32_t off, int size, uint32_t value);

	const uint16_t* framebuffer() const { return gram_; } // big-endian RGB565
	bool lit() const;
	uint64_t frameGen() const { return (gen_ << 1) | (lit() ? 1u : 0u); }

	// Values the OS's LCD init (0x8004dd3a) leaves behind on a fresh machine.
	void applyBootDefaults();

	void serialize(std::vector<uint8_t>& out) const;
	bool deserialize(const uint8_t* data, size_t len, size_t& used);

private:
	bool rs() const;
	void writeData(uint16_t value);
	void writePixel(uint16_t value);
	void setAddress();

	const SimpleRegion& pfc_;
	uint16_t idx_{0};
	uint16_t reg_[REG_COUNT]{};
	uint16_t gram_[W * H]{};
	uint32_t h_{0};
	uint32_t v_{0};
	uint64_t gen_{0};
};

// ---- RTC at 0xA413FEC0 (SH7724-compatible, 0x40 bytes) --------------------
class RtcRegion : public SimpleRegion {
public:
	RtcRegion() : SimpleRegion(RTC_BASE, 0x40u) {}
	uint32_t read(uint32_t off, int size) const override;
	void write(uint32_t off, int size, uint32_t value) override;
	void tick(uint64_t cycles, uint64_t ips);

	void serialize(std::vector<uint8_t>& out) const;
	bool deserialize(const uint8_t* data, size_t len, size_t& used);

private:
	uint64_t ticks64_{0};  // 64 Hz counter driven from CPU cycles
	uint64_t lastCycle_{0}; // last absolute CPU cycle count seen by tick()
};

// ---- The whole bus ---------------------------------------------------------
class Bus {
public:
	Bus();

	bool loadFlash(const uint8_t* data, size_t len);
	size_t flashSize() const { return flash_.size(); }

	// Big-endian 1/2/4-byte accesses at a virtual address.
	uint32_t read(uint32_t va, int size);
	void write(uint32_t va, int size, uint32_t value);

	// Instruction fetch (hot path: avoids the MMIO/page walk for flash & DRAM).
	uint16_t fetch16(uint32_t pc);

	const uint16_t* framebuffer() const { return lcd_.framebuffer(); }
	KeyScRegion& keysc() { return keysc_; }
	const KeyScRegion& keysc() const { return keysc_; }

	// `cpuCycles` is the CPU's absolute cycle counter; regions derive their
	// own deltas from it (so a slice boundary does not disturb their timing).
	void tickDevices(uint64_t cpuCycles, uint64_t ips);
	uint32_t mmioWriteCount() const { return mmioWrites_; }
	uint32_t unmappedCount() const { return unmapped_; }

	// Save-state blob for the memory/peripheral subsystem.
	void serialize(std::vector<uint8_t>& out) const;
	bool deserialize(const uint8_t* data, size_t len);

private:
	uint32_t readMmio(uint32_t va, int size);
	void writeMmio(uint32_t va, int size, uint32_t value);

	std::vector<uint8_t> flash_;
	std::vector<uint8_t> dram_;
	std::vector<uint8_t> ilram_;
	std::vector<uint8_t> ocram_;
	std::vector<uint8_t> xyram_;

	SimpleRegion pfc_;
	SimpleRegion intc_;
	SimpleRegion ccn_;
	SimpleRegion dmac_;
	SimpleRegion miscA44C_;
	SimpleRegion miscA44D_;
	KeyScRegion keysc_;
	LcdRegion lcd_;
	RtcRegion rtc_;

	uint32_t mmioWrites_{0};
	uint32_t unmapped_{0};
};

} // namespace cg100
