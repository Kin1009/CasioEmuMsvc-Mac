// validate.cpp - load/registration validation harness for the C++ SH4 plugin.
//
// Build:  CMake target `sh4_validate` (see CMakeLists.txt).
// Run:    sh4_validate [--plugin <path>] [--hw <10|14>] [--verbose]
//
// Proves (NO CG100 OS boot, NO hardware writes, NO mouse control):
//   1. The plugin image dlopen()/LoadLibrary()s successfully.
//   2. It exports fPluginLoadArch() (falls back to fPluginLoad()).
//   3. Calling it with a stub PluginApi registers an IArchitectureProvider,
//      either via PluginApi::RegisterArchitectureProvider() or via
//      QueryInterface(CASIOEMU_ARCH_REGISTRY_NAME)->RegisterProvider().
//   4. The provider reports hardware_id 10 (HW_CG100) or 14 (alt host) through
//      GetSupportedHardwareIds() - the exact method the Mac host calls.
//   5. provider->CreateCore() returns a non-null IEmulatorCore whose GetId()
//      and GetStatusText() are callable; it is released with DestroyCore().
//      The core is constructed but NEVER Initialized/Ticked/Rendered.
//
// Exit 0 = PASS, 1 = FAIL. Every check prints symbol/pointer evidence.

#ifndef SH4_ABI_HEADER
#define SH4_ABI_HEADER "sh4_abi.h"
#endif
#include SH4_ABI_HEADER

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <typeinfo>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace {

struct Evidence {
    bool loaded = false, symbolFound = false, registered = false;
    bool hwOk = false, idOk = false, coreOk = false;
    int  hwSeen = -1;
    std::string idSeen, nameSeen, registeredVia, pluginId;
    void* imageBase = nullptr;
    void* entry = nullptr;
    void* corePtr = nullptr;
    std::string coreId, coreStatus;
    // Authoritative Mac host tree here uses HW_CG100 == 14; 10 is accepted too.
    unsigned short expectedHw = SH4_HW_CG100_ALT;
    bool verbose = false;
};
Evidence g;

// ---------------------------------------------------------------------------
// Stub registry + stub PluginApi. Both registration routes are supported so
// the harness works regardless of which one the plugin chooses.
// ---------------------------------------------------------------------------
class StubRegistry final : public casioemu::IArchitectureRegistry {
public:
    bool RegisterProvider(casioemu::IArchitectureProvider* p) override {
        g.registered = (p != nullptr);
        g.registeredVia = "QueryInterface(CASIOEMU_ARCH_REGISTRY_NAME)->RegisterProvider";
        provider = p;
        return p != nullptr;
    }
    casioemu::IArchitectureProvider* FindProvider(uint16_t hw) override {
        if (!provider) return nullptr;
        size_t n = 0;
        const uint16_t* ids = provider->GetSupportedHardwareIds(&n);
        for (size_t i = 0; i < n && ids; ++i)
            if (ids[i] == hw) return provider;
        return nullptr;
    }
    size_t GetProviderCount() const override { return provider ? 1u : 0u; }
    casioemu::IArchitectureProvider* GetProvider(size_t i) const override {
        return (i == 0) ? provider : nullptr;
    }
    casioemu::IArchitectureProvider* provider = nullptr;
} g_registry;

class StubApi final : public PluginApi {
public:
    int GetVersion() override { return 2; }
    void AddWindow(UIWindow*) override { ++windowsAdded; }
    bool RegisterPlugin(const char* id, const char*, int) override {
        g.pluginId = id ? id : "";
        return true;
    }
    void AssertFundamentalSTL(size_t, size_t, size_t, size_t) override {}
    void* QueryInterface(const char* name) override {
        if (!name) return nullptr;
        if (std::strcmp(name, CASIOEMU_ARCH_REGISTRY_NAME) == 0 ||
            std::strcmp(name, typeid(casioemu::IArchitectureRegistry).name()) == 0)
            return &g_registry;
        return nullptr;
    }
    void* GetImGuiContext() override { return nullptr; }
    bool RegisterPlugin(const char* id, const char*, const char*,
                        const char*, const char*) override {
        g.pluginId = id ? id : "";
        return true;
    }
    bool RegisterArchitectureProvider(casioemu::IArchitectureProvider* p) override {
        g.registered = (p != nullptr);
        g.registeredVia = "PluginApi::RegisterArchitectureProvider";
        g_registry.provider = p;
        return p != nullptr;
    }
    int windowsAdded = 0;
};

// ---------------------------------------------------------------------------
// Dynamic loading.
// ---------------------------------------------------------------------------
void* OpenImage(const char* path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* FindSymbol(void* image, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(image), name));
#else
    return dlsym(image, name);
#endif
}

const char* LoadError() {
#if defined(_WIN32)
    static char buf[160];
    std::snprintf(buf, sizeof(buf), "GetLastError=%lu",
                  static_cast<unsigned long>(GetLastError()));
    return buf;
#else
    return dlerror() ? dlerror() : "unknown dlopen error";
#endif
}

void CloseImage(void* image) {
    if (!image) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(image));
#else
    dlclose(image);
#endif
}

std::string DefaultPluginPath() {
#if defined(_WIN32)
    const char* env = std::getenv("SH4_PLUGIN");
    return env ? env : "artifacts/CasioEmuMsvc.Plugin.CG100SH4.dll";
#else
    const char* env = std::getenv("SH4_PLUGIN");
    return env ? env : "artifacts/libcg100sh4.plugin";
#endif
}

void Check(const char* label, bool ok, const char* detail = "") {
    std::printf("  [%s] %s%s%s\n", ok ? "PASS" : "FAIL", label,
                detail ? " -- " : "", detail ? detail : "");
}

} // namespace

int main(int argc, char** argv) {
    std::printf("=== CG100/SH4 C++ plugin: load + registration validation ===\n");

    std::string pluginPath = DefaultPluginPath();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--plugin" && i + 1 < argc) pluginPath = argv[++i];
        else if (a == "--hw" && i + 1 < argc)
            g.expectedHw = static_cast<unsigned short>(std::atoi(argv[++i]));
        else if (a == "--verbose") g.verbose = true;
        else if (a == "--help") {
            std::printf("usage: sh4_validate [--plugin PATH] [--hw 10|14] [--verbose]\n");
            return 0;
        }
    }

    std::printf("  target: %s\n", pluginPath.c_str());
    std::printf("  expected HW id: %u (SH4_HW_CG100=%d, alt=%d)\n",
                g.expectedHw, SH4_HW_CG100, SH4_HW_CG100_ALT);

    void* image = OpenImage(pluginPath.c_str());
    if (!image) {
        std::printf("  [FAIL] dlopen/LoadLibrary -- %s\n", LoadError());
        std::printf("RESULT: FAIL (image did not load)\n");
        return 1;
    }
    g.loaded = true;
    g.imageBase = image;
    std::printf("  [PASS] image loaded -- base=%p\n", image);

    g.entry = FindSymbol(image, "fPluginLoadArch");
    const char* entryName = "fPluginLoadArch";
    if (!g.entry) {
        g.entry = FindSymbol(image, "fPluginLoad");
        entryName = "fPluginLoad";
    }
    if (!g.entry) {
        std::printf("  [FAIL] no fPluginLoadArch / fPluginLoad export\n");
        std::printf("RESULT: FAIL (entry point missing)\n");
        CloseImage(image);
        return 1;
    }
    g.symbolFound = true;
    std::printf("  [PASS] symbol %s -- address=%p\n", entryName, g.entry);

    // ABI handshake exports (host may reject the plugin without these).
    // Informational: their absence does not fail the load/registration check.
    if (auto* pfv = reinterpret_cast<int (*)()>(FindSymbol(image, "fPluginApiVersion"))) {
        std::printf("  handshake fPluginApiVersion = %d (address=%p)\n", pfv(),
                    reinterpret_cast<void*>(pfv));
    } else {
        std::printf("  handshake fPluginApiVersion: not exported (informational)\n");
    }
    if (auto* pfs = reinterpret_cast<unsigned long (*)()>(
            FindSymbol(image, "fPluginAbiSignature"))) {
        std::printf("  handshake fPluginAbiSignature = %lu (address=%p)\n", pfs(),
                    reinterpret_cast<void*>(pfs));
    }

    StubApi api;
    reinterpret_cast<PluginLoadArch>(g.entry)(&api);

    Check("architecture provider registered", g.registered, g.registeredVia.c_str());
    if (!g.registered) {
        std::printf("RESULT: FAIL (no provider registration)\n");
        CloseImage(image);
        return 1;
    }

    casioemu::IArchitectureProvider* provider = g_registry.provider;

    const char* id = provider->GetId();
    g.idSeen = id ? id : "";
    Check("provider GetId()", id && id[0] != '\0', g.idSeen.c_str());
    g.idOk = id && id[0] != '\0';

    const char* name = provider->GetName();
    g.nameSeen = name ? name : "";
    Check("provider GetName()", name && name[0] != '\0', g.nameSeen.c_str());
    std::printf("  provider version = %u\n", provider->GetVersion());

    // This is the exact call the host makes to route a model to this provider.
    size_t hwCount = 0;
    const uint16_t* hwIds = provider->GetSupportedHardwareIds(&hwCount);
    bool hasHw = false;
    std::string hwList;
    for (size_t i = 0; i < hwCount && hwIds; ++i) {
        g.hwSeen = hwIds[i];
        hwList += std::to_string(hwIds[i]) + (i + 1 < hwCount ? "," : "");
        if (hwIds[i] == SH4_HW_CG100 || hwIds[i] == SH4_HW_CG100_ALT ||
            hwIds[i] == g.expectedHw)
            hasHw = true;
    }
    Check("GetSupportedHardwareIds()", hasHw,
          (std::string("ids=[") + hwList + "] count=" + std::to_string(hwCount)).c_str());
    g.hwOk = hasHw;

    // CreateCore() must yield a live core. We only call non-destructive
    // accessors: no Initialize(), Tick(), RenderFrame() or Key().
    casioemu::IEmulatorCore* core = provider->CreateCore();
    g.corePtr = core;
    Check("CreateCore()", core != nullptr,
          (std::string("core=") + (core ? "non-null" : "null")).c_str());
    if (core) {
        const char* cid = core->GetId();
        g.coreId = cid ? cid : "";
        Check("core GetId()", cid != nullptr, g.coreId.c_str());
        const char* status = core->GetStatusText();
        g.coreStatus = status ? status : "";
        std::printf("  core status text: '%s'\n", g.coreStatus.c_str());
        provider->DestroyCore(core);
        g.coreOk = true;
    }

    bool pass = g.loaded && g.symbolFound && g.registered && g.hwOk &&
                g.idOk && g.coreOk;
    std::printf("  plugin metadata: id='%s', via=%s\n",
                g.pluginId.c_str(), g.registeredVia.c_str());
    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    CloseImage(image);
    return pass ? 0 : 1;
}
