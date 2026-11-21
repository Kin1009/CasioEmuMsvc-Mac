// mock_plugin.cpp - minimal ABI-conformant plugin used ONLY to self-test the
// validation harness and prove the build produces a loadable image. NOT the
// real SH4 plugin; never ship this. It mirrors the Mac host ABI in sh4_abi.h.

#ifndef SH4_ABI_HEADER
#define SH4_ABI_HEADER "sh4_abi.h"
#endif
#include SH4_ABI_HEADER

#include <cstdint>

namespace {

class MockCore final : public casioemu::IEmulatorCore {
public:
    const char* GetId() const override { return "cg100sh4.mock.core"; }
    bool Initialize(const casioemu::ArchInitInfo&) override { return true; }
    void Reset() override {}
    void Tick() override {}
    void RenderFrame(const casioemu::ArchFrameInfo&) override {}
    void Key(casioemu::ArchKey, bool) override {}
    void ReleaseAll() override {}
    const char* GetStatusText() const override {
        return "CG100 SH4 mock core (harness self-test, OS not booted)";
    }
};

class MockProvider final : public casioemu::IArchitectureProvider {
public:
    const char* GetId() const override { return "cg100sh4"; }
    const char* GetName() const override { return "SH4 / Casio CG100 (mock)"; }
    uint32_t GetVersion() const override { return 1; }
    const uint16_t* GetSupportedHardwareIds(size_t* count) const override {
        static const uint16_t ids[] = { SH4_HW_CG100 };
        if (count) *count = sizeof(ids) / sizeof(ids[0]);
        return ids;
    }
    casioemu::IEmulatorCore* CreateCore() override { return new MockCore(); }
    void DestroyCore(casioemu::IEmulatorCore* core) override { delete core; }
};

MockProvider g_provider;

} // namespace

extern "C" SH4_PLUGIN_EXPORT int fPluginApiVersion() {
    return PLUGIN_API_VERSION;
}
extern "C" SH4_PLUGIN_EXPORT unsigned long fPluginAbiSignature() {
    return kPluginUiWindowAbiSignature;
}

extern "C" SH4_PLUGIN_EXPORT void fPluginLoadArch(PluginApi* api) {
    if (!api) return;
    api->RegisterPlugin("cg100sh4", "SH4 Architecture (mock)", 1);
    api->RegisterArchitectureProvider(&g_provider);
}

extern "C" SH4_PLUGIN_EXPORT void fPluginLoad(PluginApi* api) {
    fPluginLoadArch(api);
}
