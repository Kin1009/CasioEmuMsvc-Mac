#include "PluginResources.h"

#include <algorithm>
#include <mutex>

namespace casioemu::plugin {
namespace {

struct TrackedWindow {
	std::string pluginId;
	void* window;
};

struct PluginFlags {
	std::string pluginId;
	bool hooks = false;
};

std::mutex g_mutex;
std::vector<TrackedWindow> g_windows;
std::vector<PluginFlags> g_flags;

PluginFlags& FlagsFor(const std::string& pluginId) {
	auto it = std::find_if(g_flags.begin(), g_flags.end(),
		[&](const PluginFlags& flags) { return flags.pluginId == pluginId; });
	if (it != g_flags.end())
		return *it;
	g_flags.push_back({ pluginId, false });
	return g_flags.back();
}

} // namespace

void TrackPluginWindow(const std::string& pluginId, void* window) {
	if (!window)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = std::find_if(g_windows.begin(), g_windows.end(),
		[&](const TrackedWindow& tracked) { return tracked.window == window; });
	if (it != g_windows.end()) {
		it->pluginId = pluginId;
		return;
	}
	g_windows.push_back({ pluginId, window });
}

std::vector<void*> TakePluginWindows(const std::string& pluginId) {
	std::lock_guard<std::mutex> lock(g_mutex);
	std::vector<void*> taken;
	for (const auto& tracked : g_windows) {
		if (tracked.pluginId == pluginId)
			taken.push_back(tracked.window);
	}
	g_windows.erase(std::remove_if(g_windows.begin(), g_windows.end(),
		[&](const TrackedWindow& tracked) { return tracked.pluginId == pluginId; }), g_windows.end());
	return taken;
}

bool IsTrackedPluginWindow(const void* window) {
	std::lock_guard<std::mutex> lock(g_mutex);
	return std::any_of(g_windows.begin(), g_windows.end(),
		[&](const TrackedWindow& tracked) { return tracked.window == window; });
}

void ForgetPluginWindow(const void* window) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_windows.erase(std::remove_if(g_windows.begin(), g_windows.end(),
		[&](const TrackedWindow& tracked) { return tracked.window == window; }), g_windows.end());
}

void MarkPluginHookRegistration(const std::string& pluginId) {
	std::lock_guard<std::mutex> lock(g_mutex);
	FlagsFor(pluginId).hooks = true;
}

bool PluginHasHookRegistrations(const std::string& pluginId) {
	std::lock_guard<std::mutex> lock(g_mutex);
	const auto it = std::find_if(g_flags.begin(), g_flags.end(),
		[&](const PluginFlags& flags) { return flags.pluginId == pluginId; });
	return it != g_flags.end() && it->hooks;
}

void ClearPluginResources(const std::string& pluginId) {
	std::lock_guard<std::mutex> lock(g_mutex);
	g_windows.erase(std::remove_if(g_windows.begin(), g_windows.end(),
		[&](const TrackedWindow& tracked) { return tracked.pluginId == pluginId; }), g_windows.end());
	g_flags.erase(std::remove_if(g_flags.begin(), g_flags.end(),
		[&](const PluginFlags& flags) { return flags.pluginId == pluginId; }), g_flags.end());
}

} // namespace casioemu::plugin
