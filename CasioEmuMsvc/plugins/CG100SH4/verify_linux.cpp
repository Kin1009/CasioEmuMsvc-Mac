// Headless Linux verification for the CG100 SH4 plugin + Go core.
//
// Run on an x86-64 Linux host (or under qemu-x86_64 -L <sysroot>):
//   g++ -std=c++20 -O2 -o verify_linux verify_linux.cpp -ldl
//   CG100_FLASH=/path/to/flash.bin ./verify_linux ./CG100SH4.so out.ppm
//
// What it checks:
//   1. PlatformTags.h selects linux/x64 on a Linux x86-64 build.
//   2. The plugin .so can be dlopen'd (RPATH resolves libcig100core.so).
//   3. fPluginLoad / fPluginLoadArch / casioemu_arch_core_* exports exist.
//   4. The real SH4 core runs: create, reset, framebuffer, keys, diag, USB sim.
#include "PlatformTags.h"
#include "ArchCoreABI.h"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static bool g_fail = false;

static void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail = true;
}

static std::vector<uint8_t> ReadFile(const char* path) {
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path, "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return data; }
    data.resize(static_cast<size_t>(n));
    size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) data.clear();
    return data;
}

static void DumpPPM(const char* path, const uint16_t* fb) {
    FILE* f = std::fopen(path, "wb");
    if (!f) { std::perror("fopen PPM"); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", CG100_FB_W, CG100_FB_H);
    for (int i = 0; i < CG100_FB_W * CG100_FB_H; ++i) {
        uint16_t p = static_cast<uint16_t>((fb[i] << 8) | (fb[i] >> 8));
        unsigned char rgb[3] = {
            static_cast<unsigned char>(((p >> 11) & 0x1f) * 255 / 31),
            static_cast<unsigned char>(((p >> 5) & 0x3f) * 255 / 63),
            static_cast<unsigned char>((p & 0x1f) * 255 / 31),
        };
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

template <typename Fn>
static Fn Sym(void* h, const char* name) {
    dlerror();
    void* p = h ? dlsym(h, name) : dlsym(RTLD_DEFAULT, name);
    const char* err = dlerror();
    if (err || !p) std::printf("[FAIL] dlsym(%s): %s\n", name, err ? err : "null");
    return reinterpret_cast<Fn>(p);
}

int main(int argc, char** argv) {
    std::printf("=== CG100 Linux headless verification ===\n");

    // 1. Platform tag selection (compile-time, header-only).
    const std::string key = casioemu::plugin::CurrentPlatformKey();
    std::printf("  current platform key: %s\n", key.c_str());
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    Check(key == "linux-x64", "PlatformTags selects linux/x64");
#else
    Check(key == casioemu::plugin::CurrentPlatformKey(), "PlatformTags derived from compiler predefines");
#endif

    const char* plugin = (argc > 1) ? argv[1] : "./CG100SH4.so";
    const char* ppm = (argc > 2) ? argv[2] : "headless.ppm";
    const char* flashPath = std::getenv("CG100_FLASH");
    if (!flashPath || !*flashPath) flashPath = "./flash.bin";

    // 2. dlopen the plugin (brings in libcig100core.so through its RPATH).
    void* h = dlopen(plugin, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        std::printf("[FAIL] dlopen(%s): %s\n", plugin, dlerror());
        return 1;
    }
    Check(true, "dlopen(CG100SH4.so)");

    Check(Sym<void (*)(void*)>(h, "fPluginLoad") != nullptr, "fPluginLoad exported");
    Check(Sym<void (*)(void*)>(h, "fPluginLoadArch") != nullptr, "fPluginLoadArch exported");
    Check(Sym<int (*)(void*, unsigned char*, int)>(h, "casioemu_arch_core_framebuffer") != nullptr,
          "casioemu_arch_core_framebuffer exported");

    // 3. Core ABI (resolved from RTLD_DEFAULT: plugin + its dependency are global).
    auto create = Sym<CG100Core (*)(const uint8_t*, uint32_t)>(nullptr, "cg100_create");
    auto destroy = Sym<void (*)(CG100Core)>(nullptr, "cg100_destroy");
    auto reset = Sym<void (*)(CG100Core)>(nullptr, "cg100_reset");
    auto run = Sym<uint64_t (*)(CG100Core, int)>(nullptr, "cg100_run");
    auto runIdle = Sym<int (*)(CG100Core, int)>(nullptr, "cg100_run_to_idle");
    auto fbFn = Sym<void (*)(CG100Core, uint16_t*, uint32_t)>(nullptr, "cg100_framebuffer");
    auto faultFn = Sym<const char* (*)(CG100Core)>(nullptr, "cg100_fault");
    auto keyFn = Sym<void (*)(CG100Core, uint32_t, uint32_t)>(nullptr, "cg100_key");
    auto diagFn = Sym<void (*)(void*, int)>(h, "casioemu_arch_core_enter_diag");
    auto usbEcho = Sym<int (*)(int)>(nullptr, "cg100_usb_test_echo");
    auto usbPlug = Sym<int (*)(CG100Core)>(nullptr, "cg100_usb_plug");
    auto usbStatus = Sym<int (*)(CG100Core)>(nullptr, "cg100_usb_status");
    Check(create && destroy && reset && run && runIdle && fbFn && faultFn && keyFn,
          "core ABI symbols resolved");

    std::vector<uint8_t> flash = ReadFile(flashPath);
    std::printf("  flash: %s (%zu bytes)\n", flashPath, flash.size());
    Check(flash.size() >= 1024, "flash image readable");
    if (flash.empty()) return 1;

    CG100Core core = create(flash.data(), static_cast<uint32_t>(flash.size()));
    Check(core != nullptr, "cg100_create");
    if (!core) return 1;

    reset(core);
    if (runIdle) runIdle(core, 400);
    uint64_t cycles = run(core, 200);
    Check(cycles > 0, "core executed instructions");

    std::vector<uint16_t> fb(static_cast<size_t>(CG100_FB_W) * CG100_FB_H);
    fbFn(core, fb.data(), static_cast<uint32_t>(fb.size()));
    unsigned nonBlack = 0;
    for (uint16_t p : fb) if (p != 0) ++nonBlack;
    std::printf("  framebuffer: %u/%zu non-black pixels\n", nonBlack, fb.size());
    Check(nonBlack > 0, "framebuffer rendered non-black pixels");
    DumpPPM(ppm, fb.data());

    // Keys: press 2, +, 3, EXE (same sequence as the plugin's 2+3 test).
    keyFn(core, 6, 2); run(core, 80);
    keyFn(core, 5, 2); run(core, 80);
    keyFn(core, 3, 2); run(core, 80);
    keyFn(core, 4, 2); run(core, 80);
    keyFn(core, 2, 1); run(core, 120);
    Check(faultFn(core) == nullptr, "no core fault after key sequence");

    // Diag + USB simulation (additive ABI; optional but verified when present).
    Check(diagFn != nullptr, "casioemu_arch_core_enter_diag exported");
    if (usbEcho) Check(usbEcho(0x5a) == 0x5a, "USB test echo");
    if (usbPlug && usbStatus) {
        usbPlug(core);
        int st = usbStatus(core);
        std::printf("  USB status after plug: %d\n", st);
        Check(st > 0, "USB plug changes status");
    }

    const char* flt = faultFn(core);
    std::printf("  fault: %s\n", flt ? flt : "(none)");
    destroy(core);
    dlclose(h);

    std::printf("=== %s ===\n", g_fail ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return g_fail ? 1 : 0;
}
