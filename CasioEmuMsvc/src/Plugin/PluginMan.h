#pragma once
#include "IArchitectureProvider.h"
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

class PluginApi;  // forward declaration: keep this header safe to include from host TUs

// Global architecture-provider registry (owned by PluginMan.cpp).
casioemu::IArchitectureRegistry* GetArchitectureRegistry();

struct PluginInfo {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string desc;
};

extern std::string g_PluginLoadLog;
extern std::vector<PluginInfo> g_loadedPlugins;
void LoadPlugins();

/// Early, architecture-only plugin pass. Loads plugin modules and calls their
/// OPTIONAL `fPluginLoadArch` export. Must run before Emulator construction.
/// Accepts ONLY *.plugin packages. A .plugin may be either a plain Mach-O dylib
/// (legacy format, still supported) or a ZIP container whose plugin.json
/// manifest points at an embedded native dylib; ZIP containers are extracted
/// into `cacheDir` before being loaded.
void LoadArchitecturePlugins();

/// Set of native modules dlopen'd by the early architecture pass. Declared
/// inline (C++17) so the function-local static is shared by every TU that
/// includes this header -- including the headless registry test, which does
/// not compile PluginMan.cpp. This lets the registry skip a native plugin that
/// the architecture pass already loaded, avoiding a double fPluginLoad.
inline std::vector<std::string>& ArchitectureLoadedModules() {
	static std::vector<std::string> modules;
	return modules;
}

inline std::string ArchitectureModuleKey(const std::filesystem::path& path) {
	std::error_code ec;
	auto canonical = std::filesystem::weakly_canonical(path, ec);
	if (ec)
		canonical = path;
	return canonical.string();
}

/// True when the given native module path was already dlopen'd by the early
/// architecture pass.
inline bool IsArchitectureModuleLoaded(const std::filesystem::path& path) {
	const std::string key = ArchitectureModuleKey(path);
	for (const std::string& loaded : ArchitectureLoadedModules())
		if (loaded == key)
			return true;
	return false;
}

inline void MarkArchitectureModuleLoaded(const std::filesystem::path& path) {
	const std::string key = ArchitectureModuleKey(path);
	if (!IsArchitectureModuleLoaded(path))
		ArchitectureLoadedModules().push_back(key);
}

#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
/// Resolve a .plugin package to a loadable native module.  On Android an
/// embedded AArch64 library is selected by its "-android-arm64-android.so"
/// tag, with "-any" and untagged binaries as fallbacks.
///  - plain Mach-O .plugin -> returns `pluginFile` unchanged (backward compat)
///  - ZIP .plugin          -> extracts into `cacheDir` via the hardened
///                            ExtractPackage() and returns the embedded native
///                            entry (or an embedded *.dylib) from that cache.
/// Returns an empty path and fills `error` when the package cannot be resolved.
std::filesystem::path ResolvePluginNativeModule(
	const std::filesystem::path& pluginFile,
	const std::filesystem::path& cacheDir,
	std::string* error = nullptr);
#endif
