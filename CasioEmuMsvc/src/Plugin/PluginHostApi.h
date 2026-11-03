#pragma once

#include "PluginManifest.h"

#include <string>

class PluginApi;

namespace casioemu::plugin {

// Creates a permission-scoped PluginApi facade for one native plugin.  The
// facade is what fPluginLoad receives; it filters AddWindow/QueryInterface and
// records denials for the manager.  Destroy it after fPluginUnload returns.
PluginApi* CreatePluginApi(const std::string& pluginId, const Manifest& manifest);
void DestroyPluginApi(PluginApi* api);

// ABI handshake.  Plugins may export
//   extern "C" unsigned long fPluginAbiSignature();
//   extern "C" int fPluginApiVersion();
// and the loader compares both against these values.  Older plugins without
// the exports still load (v1 compatibility) but are logged as unverified.
unsigned long HostUiWindowAbiSignature();
int HostPluginApiVersion();

// Optional export names, shared by the loader and the SDK examples.
inline constexpr const char* kPluginAbiSignatureSymbol = "fPluginAbiSignature";
inline constexpr const char* kPluginApiVersionSymbol = "fPluginApiVersion";
inline constexpr const char* kPluginUnloadSymbol = "fPluginUnload";

} // namespace casioemu::plugin
