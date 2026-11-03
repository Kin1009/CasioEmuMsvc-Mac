#pragma once

#include <string>

class PluginApi;

namespace casioemu::plugin {

struct UnloadCallResult {
	bool had_callback = false;
	bool threw = false;
	std::string error;
};

// Resolves and calls fPluginUnload on an open library handle.  Kept free of
// GUI dependencies so the loader smoke test can exercise the unload path.
UnloadCallResult CallPluginUnload(void* handle, PluginApi* api);

} // namespace casioemu::plugin
