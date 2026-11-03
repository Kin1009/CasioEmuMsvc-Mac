#pragma once

#include <string>
#include <vector>

namespace casioemu::plugin {

// Resources a native plugin acquired through PluginApi, tracked per plugin id
// so Unload can hide/remove its windows and decide whether the dylib can be
// closed safely.  Pointers are void* to keep this module independent of the
// host/SDK UIWindow definitions (which differ between translation units by
// design).
void TrackPluginWindow(const std::string& pluginId, void* window);
[[nodiscard]] std::vector<void*> TakePluginWindows(const std::string& pluginId);
[[nodiscard]] bool IsTrackedPluginWindow(const void* window);
void ForgetPluginWindow(const void* window);

// Hooks registered through the Hooks interface cannot be removed with the
// current API (there is no unregister call).  If a plugin registered any, the
// registry keeps the dylib mapped after unloading its windows instead of
// calling dlclose and leaving dangling callbacks.
void MarkPluginHookRegistration(const std::string& pluginId);
[[nodiscard]] bool PluginHasHookRegistrations(const std::string& pluginId);

void ClearPluginResources(const std::string& pluginId);

} // namespace casioemu::plugin
