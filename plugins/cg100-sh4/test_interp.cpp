// test_interp.cpp -- standalone unit tests for the CG100 SH4 interpreter + MMIO.
// Build: c++ -std=c++17 -O2 -Wall -o test_interp test_interp.cpp sh4_interp.cpp cg100_mmio.cpp
// Run:   ./test_interp

#include "sh4_interp.h"
#include "cg100_mmio.h"

#include <cstdio>
#include <cstdlib>

using namespace cg100;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    ++checks; \
    if (!(cond)) { std::printf("FAIL: %s (line %d)\n", msg, __LINE__); ++failures; } \
} while (0)

// Program the OS image directly (bypassing the NOR command state machine).
static void put16(Memory& m, uint32_t addr, uint16_t v) {
    const uint32_t p = addr & 0x1FFFFFFFu;
    if (p + 1 < Memory::FLASH_MUT_TOP) {
        m.flash[p] = static_cast<uint8_t>(v >> 8);
        m.flash[p + 1] = static_cast<uint8_t>(v);
    } else m.w16(addr, v);
}
// Build a machine with a tiny program in DRAM at 0x80000000 (phys flash alias).
struct Machine {
    CG100MMIO mmio{nullptr};
    Memory mem{&mmio};
    Sh4CPU cpu{mem};
    Machine() { mmio.attachCPU(&cpu); cpu.compatHooks = false; }
};

static void testBigEndianFetch() {
    Machine x;
    put16(x.mem, 0x80000000, 0x1234);
    put16(x.mem, 0x80000002, 0xABCD);
    CHECK(x.mem.fetch16(0x80000000) == 0x1234, "fetch16 big-endian word 0");
    CHECK(x.mem.fetch16(0x80000002) == 0xABCD, "fetch16 big-endian word 1");
    CHECK(x.mem.read(0x80000000, 4) == 0x1234ABCDu, "read32 big-endian");
}

static void testMovImmAndAddImm() {
    Machine x;
    // mov #-3,r1 (0xE1FD) ; add #5,r1 (0x7105) ; nop
    put16(x.mem, 0x80000000, 0xE1FD);
    put16(x.mem, 0x80000002, 0x7105);
    put16(x.mem, 0x80000004, 0x0009);
    x.cpu.step();
    CHECK(x.cpu.r[1] == static_cast<uint32_t>(-3), "mov #-3,r1");
    x.cpu.step();
    CHECK(x.cpu.r[1] == 2, "add #5,r1 -> 2");
}

static void testAddReg() {
    Machine x;
    // mov #10,r2 ; mov #7,r1 ; add r2,r1 (0x312C) ; nop
    put16(x.mem, 0x80000000, 0xE20A);
    put16(x.mem, 0x80000002, 0xE107);
    put16(x.mem, 0x80000004, 0x312C);
    put16(x.mem, 0x80000006, 0x0009);
    x.cpu.step(); x.cpu.step(); x.cpu.step();
    CHECK(x.cpu.r[1] == 17, "add r2,r1 -> 17");
}

static void testMovW() {
    Machine x;
    // mov.w @(1,PC),r3  => opcode 0x9301 ; EA = instr+4+1*2 = 0x80000006
    put16(x.mem, 0x80000000, 0x9301);
    put16(x.mem, 0x80000002, 0x0009); // unused
    put16(x.mem, 0x80000004, 0x0000);
    put16(x.mem, 0x80000006, 0xFFFE); // -2 sign-extended
    x.cpu.step();
    CHECK(x.cpu.r[3] == static_cast<uint32_t>(-2), "mov.w @(disp,PC),r3 sign-extends");
}

static void testBraDelaySlot() {
    Machine x;
    // bra +2 at 0x80000000: target = 0x80000000+4+2*2 = 0x80000008
    put16(x.mem, 0x80000000, 0xA002);
    put16(x.mem, 0x80000002, 0x0009); // delay slot
    put16(x.mem, 0x80000004, 0x0009); // skipped
    put16(x.mem, 0x80000006, 0x0009); // skipped
    put16(x.mem, 0x80000008, 0x7001); // add #1,r0
    x.cpu.step(); // executes bra AND its delay slot in one step
    CHECK(x.cpu.pc == 0x80000008u, "bra lands at bra+4+disp*2");
    x.cpu.step();
    CHECK(x.cpu.r[0] == 1, "instruction after bra executes");
}

static void testBtBf() {
    Machine x;
    // sett ; bt +1 at 0x80000002 -> target 0x80000002+4+2 = 0x80000008
    put16(x.mem, 0x80000000, 0x0018); // sett
    put16(x.mem, 0x80000002, 0x8901); // bt +1
    put16(x.mem, 0x80000004, 0x0009); // delay slot
    put16(x.mem, 0x80000006, 0x0009); // skipped
    put16(x.mem, 0x80000008, 0x7009); // add #9,r0 (executed)
    x.cpu.step(); // sett
    x.cpu.step(); // bt + slot
    CHECK(x.cpu.pc == 0x80000008u, "bt taken lands at bt+4+disp*2");
}

static void testMemoryMapping() {
    Machine x;
    // DRAM via any mirror (P2 alias 0xAC...).
    x.mem.write(0xAC000000u, 4, 0xDEADBEEFu);
    CHECK(x.mem.read(0x8C000000u, 4) == 0xDEADBEEFu, "DRAM P0 alias maps to phys 0x0C000000");
    // ILRAM mirrored every 16 KB.
    x.mem.write(0xFD800000u, 4, 0x11223344u);
    CHECK(x.mem.read(0xFD804000u, 4) == 0x11223344u, "ILRAM 16 KB mirror");
    // Flash beyond the image reads as erased 0xFF.
    CHECK(x.mem.read(0x01000000u, 4) == 0xFFFFFFFFu, "erased flash tail reads 0xFF");
    // CCN model strap.
    CHECK(x.mem.read(0xFF000024u, 4) == 0x0A02u, "CCN EXPEVT/model strap 0x0A02");
}

static void testKeyscIrq() {
    Machine x;
    x.cpu.setSR(x.cpu.sr & ~SR_BL); // unmask interrupts for the test
    // Program KEYSC like the OS init does.
    x.mem.write(0xA44B0000u + 0x0C, 2, 0x8000); // enable
    x.mem.write(0xA44B0000u + 0x10, 2, 0x0200); // normal mode
    x.mem.write(0xA44B0000u + 0x14, 2, 0x4800); // IE flags 3,6
    x.mmio.setKeyScanPeriod(1);
    x.mmio.keyPress(0, 0); // AC/ON
    x.mmio.tick(1, 0);
    CHECK((x.mmio.keyFlags() & 0x08) != 0, "KEYSC press edge sets detect flag");
    CHECK((x.mmio.keyFlags() & 0x02) != 0, "KEYSC held key sets scan-complete flag");
    CHECK(!x.cpu.pending.empty(), "KEYSC raised an interrupt");
    CHECK(x.mem.read(0xA44B0000u, 2) == 0x0001u, "KEYSC data word: AC/ON -> bit0");
    x.mmio.keyReleaseAll();
}

int main() {
    testBigEndianFetch();
    testMovImmAndAddImm();
    testAddReg();
    testMovW();
    testBraDelaySlot();
    testBtBf();
    testMemoryMapping();
    testKeyscIrq();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
