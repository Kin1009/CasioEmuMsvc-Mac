#pragma once
// Minimal, ABI-COMPATIBLE copy of the host PluginApi.h for an architecture-only
// plugin. It reproduces the host's virtual method order exactly (see below)
// but drops the ImGui / debugger / model headers this plugin never uses, so the
// build stays standalone.
//
//   Host: /Volumes/Code/_imports/Desktop/CasioEmuMsvc-Mac/CasioEmuMsvc/src/Plugin/PluginApi.h
//
// DO NOT reorder or insert virtual methods. Slot N below == host slot N.
// Attributes like [[nodiscard]] are omitted: they do not affect layout.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "IArchitectureProvider.h"

namespace casioemu { struct ModelInfo; }

// Forward declaration only. This plugin registers no UI windows, so the
// complete host UIWindow definition (and its ImGui dependency) is not needed.
class UIWindow;

inline constexpr int PLUGIN_API_VERSION = 3;
inline constexpr unsigned long kPluginUiWindowAbiRevision = 2;
using IArchitectureProviderRegistry = casioemu::IArchitectureRegistry;

// Derived from the host's UIWindow definition:
//   (sizeof(UIWindow) << 8) ^ kPluginUiWindowAbiRevision
//   = (32 << 8) ^ 2 = 8194 (0x2002).
// Cross-checked by dlopen'ing the known-good reference plugin and calling its
// fPluginAbiSignature(): it returns 8194. Keep in sync if the host changes.
inline constexpr unsigned long kPluginUiWindowAbiSignature = 8194UL;

class PluginApi {
public:
	// NOTE: the host class has NO virtual destructor. Do not add one: it would
	// insert a vtable slot and shift every method below. We never delete it.
	virtual int GetVersion() = 0;                                                       // slot 0
	virtual void AddWindow(UIWindow* wnd) = 0;                                          // slot 1
	virtual bool RegisterPlugin(const char* id, const char* name, int version) = 0;      // slot 2
	virtual void AssertFundamentalSTL(size_t a, size_t b, size_t c, size_t d) = 0;       // slot 3
	virtual void* QueryInterface(const char* name) = 0;                                 // slot 4
	virtual void* GetImGuiContext() = 0;                                                // slot 5
	virtual bool RegisterPlugin(const char* id, const char* name, const char* version,
	                            const char* author = nullptr,
	                            const char* desc = nullptr) = 0;                        // slot 6
	virtual bool RegisterArchitectureProvider(casioemu::IArchitectureProvider* provider) = 0; // slot 7
};

// STL-size handshake: must match the host's exact expression.
#define PLUGINASSERTSTL(x) x->AssertFundamentalSTL( \
	sizeof(std::string), sizeof(std::vector<int>), \
	sizeof(std::map<int, int>), sizeof(std::mutex))

using PluginLoad     = void (*)(PluginApi* api);
using PluginLoadArch = void (*)(PluginApi* api);
using PluginUnload   = void (*)(PluginApi* api);

// Export the ABI handshake. The host calls these during load and rejects a
// mismatch rather than risking a vtable/layout crash.
#define PLUGIN_EXPORT_ABI() \
	extern "C" int fPluginApiVersion() { return PLUGIN_API_VERSION; } \
	extern "C" unsigned long fPluginAbiSignature() { return kPluginUiWindowAbiSignature; }
