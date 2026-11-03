// Fixture for the plugin loader tests.  Two configurations are built:
//   - headless  (CASIOEMU_HEADLESS_PLUGIN): no ImGui/UIWindow dependency; used
//     by plugin_loader_smoke to exercise fPluginUnload discovery.
//   - windowed  (PLUGIN_FIXTURE_WINDOW): behaves like a native tool, adds a
//     window and is used by plugin_registry_headless.
// Both write CASIOEMU_PLUGIN_FIXTURE_MARKER when fPluginUnload runs so the
// registry harness can assert unload after the library handle is gone.
#ifdef PLUGIN_FIXTURE_WINDOW
#include "PluginApi.h"
#else
#define CASIOEMU_HEADLESS_PLUGIN
#include "PluginApi.h"
#endif

#include <cstdlib>
#include <fstream>

namespace {

int g_unload_count = 0;

#ifdef PLUGIN_FIXTURE_WINDOW
class FixtureWindow : public UIWindow {
public:
	FixtureWindow() : UIWindow("Fixture Window") {
		inital_size = ImVec2(320, 200);
	}

	void RenderCore() override {
		ImGui::TextUnformatted("fixture");
	}
};
#endif

} // namespace

extern "C" void fPluginLoad(PluginApi* api) {
	api->RegisterPlugin("com.example.fixture.native", "Fixture", "1.0.0", "tests", "loader fixture");
#ifdef PLUGIN_FIXTURE_WINDOW
	api->AddWindow(new FixtureWindow());
#endif
}

extern "C" void fPluginUnload(PluginApi*) {
	++g_unload_count;
	const char* marker = std::getenv("CASIOEMU_PLUGIN_FIXTURE_MARKER");
	if (marker && *marker) {
		std::ofstream output(marker, std::ios::binary | std::ios::trunc);
		output << g_unload_count;
	}
}

extern "C" int fixtureUnloadCount() {
	return g_unload_count;
}

#if defined(PLUGIN_FIXTURE_BADAPI)
extern "C" int fPluginApiVersion() { return 99; }
extern "C" unsigned long fPluginAbiSignature() { return 0; }
#elif defined(PLUGIN_FIXTURE_BADABI)
extern "C" int fPluginApiVersion() { return 3; }
extern "C" unsigned long fPluginAbiSignature() { return 1; }
#elif !defined(PLUGIN_FIXTURE_NO_ABI)
PLUGIN_EXPORT_ABI();
#endif
