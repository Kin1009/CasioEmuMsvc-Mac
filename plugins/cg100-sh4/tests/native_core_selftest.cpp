// native_core_selftest.cpp - isolated checks for the native SH4 core.
// Deliberately does NOT boot the CG100 OS. Linked against sh4_cpu.cpp + mmio.cpp.

#include "sh4_cpu.h"
#include "mmio.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace cg100;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
	if (!condition) {
		std::printf("FAIL: %s\n", what);
		++g_failures;
	} else {
		std::printf("ok  : %s\n", what);
	}
}

void put16(std::vector<uint8_t>& image, uint32_t off, uint16_t value) {
	image[off] = static_cast<uint8_t>(value >> 8);
	image[off + 1] = static_cast<uint8_t>(value & 0xFF);
}

void put32(std::vector<uint8_t>& image, uint32_t off, uint32_t value) {
	for (int i = 0; i < 4; ++i)
		image[off + i] = static_cast<uint8_t>(value >> (8 * (3 - i)));
}

} // namespace

int main() {
	// --- Register reset state ------------------------------------------------
	{
		Bus bus;
		Sh4Cpu cpu(bus);
		cpu.reset();
		check(cpu.state().pc == 0x80000000u, "reset PC = 0x80000000");
		check(cpu.state().sr == (SR_MD | SR_RB | SR_BL | SR_IMASK),
		      "reset SR = MD|RB|BL|IMASK");
		check(cpu.state().r[0] == 0 && cpu.state().r[15] == 0, "reset GPRs cleared");
	}

	// --- Big-endian fetch + a few decoded instructions -----------------------
	{
		Bus bus;
		std::vector<uint8_t> image(0x1000, 0xFF);
		put16(image, 0x0000, 0xE005); // mov #5,R0
		put16(image, 0x0002, 0x6103); // mov R0,R1
		put16(image, 0x0004, 0x310C); // add R0,R1  -> R1 = 10
		put32(image, 0x000C, 0x12345678);
		put16(image, 0x0006, 0xD201); // mov.l @(1,PC),R2 -> EA = ((PC+2)&~3) + 1*4 = 0xC
		if (!bus.loadFlash(image.data(), image.size())) {
			std::printf("FAIL: loadFlash\n");
			++g_failures;
			return g_failures;
		}
		check(bus.fetch16(0x80000000u) == 0xE005u, "big-endian instruction fetch");

		Sh4Cpu cpu(bus);
		cpu.reset();
		cpu.step();
		check(cpu.state().r[0] == 5, "mov #imm,Rn");
		cpu.step();
		check(cpu.state().r[1] == 5, "mov Rm,Rn");
		cpu.step();
		check(cpu.state().r[1] == 10, "add Rm,Rn");
		cpu.step();
		check(cpu.state().r[2] == 0x12345678u, "mov.l @(disp,PC),Rn");
		check(cpu.state().pc == 0x80000008u, "PC advanced by 2 per instruction");
		check(cpu.illegalCount() == 0, "no illegal opcodes in the decoded subset");
	}

	// --- Delayed branch ------------------------------------------------------
	{
		Bus bus;
		std::vector<uint8_t> image(0x1000, 0xFF);
		put16(image, 0x0000, 0xA002); // bra +2 (target = 4+2*2 = 8)
		put16(image, 0x0002, 0xE001); // delay slot: mov #1,R0
		put16(image, 0x0004, 0xE002); // skipped: mov #2,R0
		put16(image, 0x0008, 0xE003); // target: mov #3,R0
		bus.loadFlash(image.data(), image.size());
		Sh4Cpu cpu(bus);
		cpu.reset();
		cpu.step(); // bra: executes delay slot, lands at 0x...08
		check(cpu.state().pc == 0x80000008u, "bra target after delay slot");
		check(cpu.state().r[0] == 1, "delay slot executed, skipped instruction not");
		cpu.step();
		check(cpu.state().r[0] == 3, "instruction at branch target executes");
	}

	// --- KEYSC matrix + LCD --------------------------------------------------
	{
		Bus bus;
		bus.keysc().press(0, 0); // AC/ON
		check((bus.keysc().read(0x0, 2) & 0xFFu) == 1u, "KEYSC (0,0) sets data word0 bit0");
		bus.keysc().release(0, 0);
		check((bus.keysc().read(0x0, 2) & 0xFFu) == 0u, "KEYSC release clears the bit");
		bus.keysc().press(6, 9); // ON / F1
		check((bus.keysc().read(0x8, 2) & 0x4000u) != 0u, "KEYSC (6,9) sets word4 bit14");
		check(bus.framebuffer() != nullptr, "LCD GRAM allocated");
	}

	// --- Save-state round trip -----------------------------------------------
	{
		Bus bus;
		std::vector<uint8_t> image(0x100, 0xA5);
		bus.loadFlash(image.data(), image.size());
		Bus restored;
		std::vector<uint8_t> blob;
		bus.serialize(blob);
		check(restored.deserialize(blob.data(), blob.size()), "bus save-state round trip");
		// DRAM is reached through P1/P2 mirrors; P0 0x0C000000 falls inside the
		// CG100 flash-mirror window (0x0B000000..0x0D000000).
		check(restored.read(0xAC000000u, 1) == 0, "restored DRAM accessible via P2 mirror");
	}

	if (g_failures == 0)
		std::printf("\nAll native core self-tests passed.\n");
	else
		std::printf("\n%d self-test(s) FAILED.\n", g_failures);
	return g_failures;
}
