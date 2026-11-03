#include "PluginNative.h"

#include <dlfcn.h>

#include <exception>

namespace casioemu::plugin {

UnloadCallResult CallPluginUnload(void* handle, PluginApi* api) {
	UnloadCallResult result;
	if (!handle)
		return result;
	using UnloadFn = void (*)(PluginApi*);
	auto unload = reinterpret_cast<UnloadFn>(dlsym(handle, "fPluginUnload"));
	if (!unload)
		return result;
	result.had_callback = true;
	try {
		unload(api);
	}
	catch (const std::exception& error) {
		result.threw = true;
		result.error = error.what();
	}
	catch (...) {
		result.threw = true;
		result.error = "unknown exception";
	}
	return result;
}

} // namespace casioemu::plugin
