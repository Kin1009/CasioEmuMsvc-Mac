#include "PluginHost.h"

namespace casioemu::plugin {
namespace {
PluginHost* g_host = nullptr;
}

PluginHost& Host() {
	return *g_host;
}

void SetPluginHost(PluginHost* host) {
	g_host = host;
}

} // namespace casioemu::plugin
